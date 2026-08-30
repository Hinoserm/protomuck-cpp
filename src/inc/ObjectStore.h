#ifndef MUCK_OBJECTSTORE_H
#define MUCK_OBJECTSTORE_H

/* The sharded JSON object store: one file per object named by UUID,
 * an append-only JSONL history sidecar beside it, and a manifest at
 * the data root. This is the only write path for the database; the
 * old flat-file format is import-only through FlatFileConverter.
 *
 * Layout:
 *     <root>/manifest.json
 *     <root>/objects/aa/bb/<uuid>.json
 *     <root>/objects/aa/bb/<uuid>.hist
 *
 * See docs/DATABASE.txt sections 6 and the API notes appended there.
 */

#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "Journal.h"

namespace MUCK {

class ObjectStore {
  public:
    /* True if path is a directory containing a manifest.json. */
    static bool isStore(const char *path);

    /* --db-exclude-type: deliberately boot without a type module.
     * Objects of an excluded (or unknown) type load as UNSUPPORTED
     * placeholders; their stored type name, type bits, and $type
     * entries persist verbatim. Returns false with *err set when the
     * name cannot be excluded (container types, until the storage
     * flip). docs/DATABASE.txt section 4. */
    static bool excludeType(const char *name, std::string *err);
    static bool typeExcluded(const std::string &name);

    /* --db-exclude-module: boot without a FEATURE module (properties
     * included). Its namespace entries ride dormant; the module's
     * API surface goes blanket-inert. docs/PROPERTIES.txt 5. */
    static bool excludeModule(const char *name, std::string *err);
    static bool moduleExcluded(const std::string &name);

    /* Stored type name of an UNSUPPORTED placeholder, empty string
     * for normal objects. */
    static std::string placeholderTypeName(dbref ref);

    /* --force-load: boot even when the store fails its load-time
     * integrity checks (corrupt files, missing objects, damaged
     * history). Every problem is still reported; the damaged data is
     * skipped. Without the flag, any problem refuses the boot:
     * silently regressing objects to older state is worse than not
     * starting. */
    static void setForceLoad(bool v);
    static bool forceLoad();

    /* Bind the store to its data root. Creates the directory tree on
     * first save if needed. */
    void setRoot(const char *path) { root_ = path; }
    const std::string &root() const { return root_; }
    bool active() const { return !root_.empty(); }

    /* Write every object plus the manifest. When dirtyOnly is true,
     * objects without OBJECT_CHANGED are skipped (their files are
     * already current). Returns the number of objects written, or -1
     * on a filesystem error. */
    int saveAll(bool dirtyOnly);

    /* Load an entire store into the database. Returns the resulting
     * top dbref, or -1 on error. */
    dbref loadAll();

    /* Write or rewrite one object's file. */
    bool saveObject(dbref i);

    /* Remove one object's file and history unconditionally. Deletion
     * no longer calls this: retained files age out in the manifest
     * sweep instead. */
    bool removeObject(dbref i);

    /* --- versioning (docs/DATABASE.txt section 7) --- */
    struct Marker {
        long rev;
        long when;
        std::string label;
        bool locked;
    };

    long rev() const { return rev_; }

    /* Take a snapshot: bump the global rev by one and record a marker,
     * in the manifest (global) or in one object's file (scoped).
     * Returns the new rev, or -1 on error. */
    long snapshotGlobal(const char *label, bool locked);
    long snapshotObject(dbref i, const char *label, bool locked);

    /* Reconstruct one object's entries as of a revision (from its
     * current file plus its history) and restore the parts that are
     * safe to restore in a running world: name, properties, program
     * source. Containment and location are not rolled back. Returns
     * false if the object or revision cannot be resolved; a revision
     * compaction has coalesced away is refused, with err (when given)
     * naming the nearest revisions that still exist. */
    bool rollbackObject(dbref i, long rev, std::string *err = nullptr);

    /* Marker listings for the @snapshot command. */
    const std::vector<Marker> &globalMarkers() const { return markers_; }
    std::vector<Marker> objectMarkers(dbref i) const;

    /* Rollback points available for one object (global markers plus
     * its own), for the examine display. Returns the count and sets
     * oldest to the earliest marker's timestamp (0 when none). */
    int snapshotSummary(dbref i, long *oldest) const;

    /* Offline maintenance (the -storegc flag): drop history entries no
     * retained marker can see. Walks the whole store; run it at
     * maintenance time, not from the dump path. Returns the number of
     * entries removed. */
    long gcStore();

    /* --- the journal persist path (docs/DATABASE.txt 7.1) --- */

    /* One object's compaction work order (docs/DATABASE.txt 7.2):
     * decided on the game thread at fire time, executed on the dump
     * thread AFTER the set's manifest lands, so a reclaimed object
     * leaves the committed index before its files leave the disk.
     * survivors is the sorted union of the global ladder survivors
     * and the object's own surviving scoped markers; scopedKeep is
     * what the file's marker list is rewritten to. reclaim means the
     * object is a recycled shell no survivor predates: its files are
     * removed outright and only its tombstone remains. */
    struct CompactOrder {
        dbref ref = -1;
        std::string uuid;
        std::vector<long> survivors;
        std::vector<Marker> scopedKeep;
        bool reclaim = false;
    };

    /* One fire's worth of frozen work: the layers sealed from every
     * object that changed, plus the manifest as of that instant.
     * Nothing in here points at a live object, which is what lets the
     * dump thread write it without locks. */
    struct CaptureSet {
        long rev = 0;
        std::vector<SealedLayer> layers;
        std::string manifest;   /* serialized, ready to write */
        std::vector<CompactOrder> compactions;
        /* when fire() sealed this set (steady clock, ms): the dump
         * thread computes fire-to-commit duration from it */
        long long firedAtMs = 0;
        /* this set's journal segment number (0 = no layers), the
         * committed watermark its manifest records (everything at or
         * below is commanded durable once the manifest lands), and
         * the distributed watermark it records: segments at or below
         * THAT are removed once the manifest lands */
        long journalSeq = 0;
        long committedAtBuild = 0;
        long distributedAtBuild = 0;
        /* a sync barrier's set: distribute every pending segment
         * before returning, so file readers see everything */
        bool distributeAll = false;
    };

    /* Game thread: seal every object's top layer, materializing only
     * the entries that changed, and hand back the frozen result.
     * With compact set (the dump path), this also runs the retention
     * ladder over the global markers, decides this dump's batch of
     * compaction work, and reclaims recycled shells no retained
     * snapshot can revive; the frozen orders ride in the set. */
    CaptureSet fire(bool compact = false);

    /* Hold a frozen set until the next dump. Sealing and WRITING are
     * separate concerns: a snapshot or a deletion has to seal, because
     * a layer captures values at its era boundary, but neither needs
     * to touch the disk. Only @dump and the dump interval write, so a
     * mass recycle cannot turn into one manifest rewrite per object.
     * A crash before the next dump loses those changes, exactly as it
     * loses any other unsaved change. */
    void hold(CaptureSet set);

    /* Hand everything held, plus the set just fired, to the dump
     * thread and return. This is the dump. */
    void flushHeld(CaptureSet set);

    /* Hand a frozen set to the dump thread and return immediately. */
    void enqueue(CaptureSet set);

    /* Put everything held on disk and wait for it. Any read of stored
     * state takes this: a held set is not on disk, and reading without
     * it would see a world missing its most recent changes. */
    /* returns false if the folder barrier did not complete, meaning
     * per-object files may be stale or mid-write; callers that read
     * those files must refuse rather than trust them */
    bool syncNow();

    /* Block until the dump thread has written everything queued.
     * Rollback, resurrection, marker reads, shutdown and @restart all
     * take this barrier: reading a marker whose set has not landed
     * would find no stored state. */
    void drain();

    /* Stop the dump thread after draining. */
    void stopDumpThread();

    /* Panic coordination.
     *
     * panic() runs on whichever thread faulted, which is not
     * necessarily the GAME thread, and saveAll() walks every LIVE
     * object. If the game thread keeps running commands underneath
     * that walk it mutates objects mid-read, so the panic store
     * records a world that never existed. Two threads panicking at
     * once is worse still: both walk and both write.
     *
     * markGameThread() stamps the identity once at startup.
     * beginPanic() lets exactly one thread through. panicPark() is
     * the game thread's cooperative stop, called from the poll points
     * it already passes through often (the command loop and the MUF
     * instruction loop); once panicking it never returns.
     * waitGameParked() gives the panicking thread a bounded wait for
     * that to happen: bounded because a game thread wedged in a
     * syscall would otherwise hang the crash dump forever, and a
     * slightly torn store beats no store at all. */
    void markGameThread();
    bool onGameThread() const;
    bool beginPanic();
    bool waitGameParked(int millis);

    /* Called once per MUF instruction, so the overwhelmingly common
     * not-panicking case has to cost one relaxed load and nothing
     * else; the parking itself is out of line. */
    void panicPark()
    {
        if (panicking_.load(std::memory_order_relaxed))
            panicParkSlow();
    }

    /* Ask the dump thread to stop without waiting for it. For panic:
     * the world is coming down and cannot block on a worker that may
     * be the thing that died. */
    void requestDumpStop();

    /* True while the dump thread has work outstanding. */
    bool persistPending();

    /* The committed-index blob cached inside buildManifest is stale;
     * rebuilt on the next manifest build. Game thread only. */
    void invalidateIndexBlob() { indexBlobDirty_ = true; }

    /* Operator-visible store health: failures on the dump and folder
     * threads are otherwise invisible (their stderr is detached), so
     * every failure site bumps one of these and @stats surfaces them.
     * Atomic, relaxed, incremented from any thread. */
    struct Health {
        unsigned long failedPersists;
        unsigned long failedFolds;
        unsigned long damagedSegments;
        unsigned long damagedObjectFiles;
        unsigned long barrierTimeouts;
        unsigned long workerExceptions;
    };
    Health healthSnapshot() const {
        return { hFailedPersists_.load(), hFailedFolds_.load(),
                 hDamagedSegments_.load(), hDamagedObjectFiles_.load(),
                 hBarrierTimeouts_.load(), hWorkerExceptions_.load() };
    }

    /* A full-object layer that never reached the disk. fire() flips
     * baseWritten true when it seals a full layer, so a persist that
     * fails for good would leave the object claiming a base that does
     * not exist: later deltas fold into nothing and the committed
     * index names a missing file, which refuses the next boot. The
     * dump thread cannot repair live objects, so it records the refs
     * here and the game loop reconciles them. */
    void noteFailedFullLayer(dbref ref) {
        std::unique_lock<std::mutex> lk(failedMutex_);

        failedFullLayers_.push_back(ref);
        hasFailedFullLayers_.store(true);
    }
    bool anyFailedFullLayers() const { return hasFailedFullLayers_.load(); }
    std::vector<dbref> takeFailedFullLayers() {
        std::unique_lock<std::mutex> lk(failedMutex_);
        std::vector<dbref> out;

        out.swap(failedFullLayers_);
        hasFailedFullLayers_.store(false);
        return out;
    }

    /* True once since the last call if a dump's manifest committed.
     * The game loop polls this and posts the dump-done message; the
     * dump thread itself must not wall (walling walks the descriptor
     * list). So "Done." means the disk is caught up, not merely that
     * the fire returned. */
    bool takeDumpLanded() { return dumpLanded_.exchange(false); }

    /* Stats of the most recently committed dump, for the completion
     * message: how many object layers it wrote and how long it took
     * from fire to manifest commit. Written by the dump thread,
     * read by the game loop; atomics, no lock. */
    long lastDumpLayers() const { return lastDumpLayers_.load(); }
    long lastDumpMillis() const { return lastDumpMillis_.load(); }

    /* Who ran @dump, so the completion message can go to them and
     * not only the wizard wall. Game thread only. A queue, not a
     * slot: two wizards dumping back to back both deserve their
     * notice, and the second must not silently eat the first's. */
    void addDumpRequester(dbref who) { dumpRequesters_.push_back(who); }
    std::vector<dbref> takeDumpRequesters() {
        std::vector<dbref> out;

        out.swap(dumpRequesters_);
        return out;
    }

    /* Dump thread (or inline): write a frozen set. Appends each
     * layer to its object's .hist, or writes a full base when the
     * object has none yet, then commits with the manifest. */
    bool persist(const CaptureSet &set);

    /* Audit: for every object and every entry it has, check that the
     * per-key materializer used to seal journal layers agrees exactly
     * with the full serializer used to write the base. A disagreement
     * would mean a journalled change persists differently from the
     * same change captured in a full save, which is the one way this
     * design can silently corrupt data. Returns the mismatch count. */
    long verifyEntrySerialization();

  private:
    std::string objectPath(dbref i) const;
    std::string histPath(dbref i) const;
    bool writeManifest();
    std::string buildManifest();
    bool buildCompactOrder(dbref i, const std::vector<long> &globalRevs,
                           long now, CompactOrder *out);
    /* wall clock for the ladder, clamped against clock jumps */
    long ladderNow();

    /* --- the dump thread (docs/DATABASE.txt 7.1) --- */
    void dumpThreadMain();
    void ensureDumpThread();

    std::thread dumpThread_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::condition_variable idleCv_;
    std::deque<CaptureSet> queue_;
    /* sealed but deliberately not yet written; drained by a dump */
    std::vector<CaptureSet> held_;
    bool dumpThreadRunning_ = false;
    /* ATOMIC: panic runs from a signal handler and must be able to
     * raise the stop flags without taking a mutex the interrupted
     * thread may already hold. */
    std::atomic<bool> dumpThreadStop_{false};
    bool persisting_ = false;
    std::atomic<bool> dumpLanded_{false};
    std::atomic<long> lastDumpLayers_{0};
    std::atomic<long> lastDumpMillis_{0};
    std::atomic<long> layersSinceCommit_{0};
    std::vector<dbref> dumpRequesters_;     /* game thread only */

    /* --- the dump journal (docs/DATABASE.txt 7.1) ---
     *
     * A dump appends every sealed layer as one line to a single
     * journal segment: one sequential write and one fsync commit a
     * dump of any size. Distribution of those lines into the
     * per-object files happens later on the dump thread, unsynced
     * (the segment already guarantees durability) and coalesced, so
     * the per-object scatter never sits on the dump's critical path.
     *
     * journalSeq_ is the last segment number assigned (game thread,
     * at fire). journalDistributed_ is the last segment fully folded
     * into per-object files (dump thread advances it; the game
     * thread's next manifest records it, and only segments a LANDED
     * manifest calls distributed are removed). */
    long journalSeq_ = 0;
    std::atomic<long> journalDistributed_{0};
    /* claimed one segment at a time: panic's saveAll and an
     * in-flight persist can both be retiring segments at once */
    std::atomic<long> journalUnlinked_{0};
    /* highest segment a LANDED manifest has committed: distribution
     * must never run ahead of this, or uncommitted changes would leak
     * into the per-object files and survive the crash that was
     * supposed to discard them. Written by the dump thread, read by
     * the folder thread. */
    std::atomic<long> journalLandedCommitted_{0};

    std::string journalSegmentPath(long seq) const;
    long distributeOneSegment(long seq);
    void distributeSegments(long keepAtMost);
    void unlinkDistributedSegments(long upTo);

    /* --- the folder thread ---
     *
     * Housekeeping (folding segments into per-object files, sweep
     * compaction) lives on its own worker so a commit NEVER queues
     * behind it: one segment line can be a hundred-megabyte whale,
     * and no quantum trick makes folding that yield mid-object. The
     * folder is the only thread that touches per-object files during
     * normal operation; the dump thread touches only segments and
     * the manifest. Sync barriers lower folderLagTarget_ to zero and
     * wait on folderIdleCv_ until everything is folded. */
    void folderThreadMain();
    void ensureFolderThread();

    std::thread folderThread_;
    std::mutex folderMutex_;
    void panicParkSlow();

    std::thread::id gameThread_{};
    std::atomic<bool> gameThreadKnown_{false};
    std::atomic<bool> panicking_{false};
    std::atomic<bool> gameParked_{false};

    std::condition_variable folderCv_;      /* work available */
    std::condition_variable folderIdleCv_;  /* caught up */
    std::deque<CompactOrder> folderOrders_;
    bool folderRunning_ = false;
    std::atomic<bool> folderStop_{false};   /* see dumpThreadStop_ */
    bool folderBusy_ = false;
    std::atomic<long> folderLagTarget_{8};

    /* serialized committed-index, spliced into the manifest; rebuilt
     * only when membership changes (see storeIndexInvalidate).
     * Atomic: parallel seal workers flip it via setBaseWritten. */
    /* highest journal_distributed watermark whose folded bytes have
     * been forced to the platter; a manifest must never advertise a
     * watermark past this, or a crash loses folded-but-unwritten data
     * that the loader will then refuse to replay */
    long lastDurableDistributed_ = 0;

    std::string indexBlob_;
    std::atomic<bool> indexBlobDirty_{true};

    /* Last wall-clock value the retention ladder ran against. A
     * forward clock jump (NTP correction, hypervisor resume, admin
     * error) would otherwise age every unlocked marker out in a
     * single fire, merging the whole rollback history away
     * irreversibly. Game thread only. */
    long lastLadderNow_ = 0;

    /* full layers whose write failed for good; drained by the game
     * loop, which resets baseWritten and re-dirties them */
    mutable std::mutex failedMutex_;
    std::vector<dbref> failedFullLayers_;
    std::atomic<bool> hasFailedFullLayers_{false};

    /* store-health counters (see Health above) */
    std::atomic<unsigned long> hFailedPersists_{0};
    std::atomic<unsigned long> hFailedFolds_{0};
    std::atomic<unsigned long> hDamagedSegments_{0};
    std::atomic<unsigned long> hBarrierTimeouts_{0};
    std::atomic<unsigned long> hDamagedObjectFiles_{0};
    std::atomic<unsigned long> hWorkerExceptions_{0};

  public:
    void healthFailedPersist() { hFailedPersists_.fetch_add(1); }
    void healthFailedFold() { hFailedFolds_.fetch_add(1); }
    void healthDamagedSegment() { hDamagedSegments_.fetch_add(1); }
    void healthDamagedObjectFile() { hDamagedObjectFiles_.fetch_add(1); }
    void healthBarrierTimeout() { hBarrierTimeouts_.fetch_add(1); }
    void healthWorkerException() { hWorkerExceptions_.fetch_add(1); }

  private:

    std::string root_;
    long rev_ = 0;
    std::vector<Marker> markers_;    /* global; per-object live in files */
    /* round-robin position of the incremental compaction sweep: each
     * dump compacts the next batch of objects, so the whole store is
     * revisited every few dozen dumps without one giant pass */
    dbref sweepCursor_ = 0;
};

extern ObjectStore g_objectStore;

inline ObjectStore &store() { return g_objectStore; }

} /* namespace MUCK */

#endif /* MUCK_OBJECTSTORE_H */
