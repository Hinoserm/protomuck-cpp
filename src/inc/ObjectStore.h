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
        bool hasMarker = false;
        std::vector<CompactOrder> compactions;
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
    void syncNow();

    /* Block until the dump thread has written everything queued.
     * Rollback, resurrection, marker reads, shutdown and @restart all
     * take this barrier: reading a marker whose set has not landed
     * would find no stored state. */
    void drain();

    /* Stop the dump thread after draining. */
    void stopDumpThread();

    /* Ask the dump thread to stop without waiting for it. For panic:
     * the world is coming down and cannot block on a worker that may
     * be the thing that died. */
    void requestDumpStop();

    /* True while the dump thread has work outstanding. */
    bool persistPending();

    /* True once since the last call if a dump's manifest committed.
     * The game loop polls this and posts the dump-done message; the
     * dump thread itself must not wall (walling walks the descriptor
     * list). So "Done." means the disk is caught up, not merely that
     * the fire returned. */
    bool takeDumpLanded() { return dumpLanded_.exchange(false); }

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
    bool dumpThreadStop_ = false;
    bool persisting_ = false;
    std::atomic<bool> dumpLanded_{false};

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
