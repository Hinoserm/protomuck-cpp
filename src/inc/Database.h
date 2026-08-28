#ifndef MUCK_DATABASE_H
#define MUCK_DATABASE_H

/* The object database engine. See docs/DATABASE.txt.
 *
 * STORAGE MODEL: objects are individually heap-allocated DbObjects,
 * stable in memory for the life of the process. The dbref index is a
 * two-level page table: a fixed top-level array of chunk pointers,
 * each chunk holding 64K object pointers. Chunks are allocated on
 * demand and NEVER move or shrink, so a growing database never
 * invalidates a concurrent reader and there is no realloc of anything
 * an object lives in. Sized for tens of millions of objects: the top
 * level is 256KB of pointers; everything else is allocated as used.
 * The uuid index is a hash map to the same pointers.
 *
 * THREADING CONTRACT:
 *   - Index reads (object(), get(), DBFETCH) are lock-free; they rely
 *     on chunks never moving.
 *   - Index growth and uuid-map writes serialize on indexMutex_.
 *   - Object payloads are protected by striped object locks, reached
 *     through DbObject::lockShared / lockExclusive. New threaded code
 *     MUST hold the object lock to mutate a payload; the legacy
 *     single-threaded main loop is exempt by convention.
 */

#include <cstdio>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "UUID.h"
#include "DbObject.h"

namespace MUCK {

class Database {
  public:
    /* ============================================================ */
    /* The designed API. This part survives the migration.          */
    /* ============================================================ */

    /* --- lookup --- */
    dbref top() const { return top_.load(std::memory_order_acquire); }

    bool valid(dbref ref) const {
        return ref >= 0 && ref < top();
    }

    /* The object for a dbref or uuid; null if invalid. Pointers are
     * stable for the life of the process. */
    DbObject *get(dbref ref) const {
        if (!valid(ref))
            return nullptr;
        return chunkFor(ref)[slotFor(ref)];
    }
    DbObject *get(const UUID &u) const;

    /* --- identity --- */
    const UUID &UUIDOf(dbref ref) const;
    dbref refOf(const UUID &u) const;
    void assignUUID(dbref ref, const UUID &u);

    /* Unique-prefix lookup (git style): NOTHING or AMBIGUOUS on
     * failure. */
    dbref resolveUUIDPrefix(const char *prefix) const;

    /* --- typed creation: the single creation gatekeeper --- */
    template <class T>
    T *Create(const char *name, dbref owner);

    /* --- lifecycle --- */
    dbref newObject(dbref player);
    dbref newProgram(dbref player, const char *name);
    void clearObject(dbref player, dbref ref);
    void freeObject(dbref ref);
    void freeAll();
    dbref parent(dbref obj);

    /* --- deletion (the modern gatekeeper) --- */
    /* Records a tombstone, removes the object's store file, retires
     * its uuid from the index, marks the shell deleted, and raises the
     * OBJECT_DELETED mufevent. dbrefs are NEVER reused; the slot stays
     * a dead shell so stale holders resolve to something inspectable
     * instead of someone else's object. Callers (recycle_object) have
     * already emptied and unlinked the object. */
    void deleteObject(dbref victim, dbref deleter);

    struct Tombstone {
        UUID uuid;
        dbref ref;
        long deletedAt;
        UUID deletedBy;
        /* store revision era at deletion: a snapshot marker with
         * rev < deletedRev was taken while the object was alive and
         * pins its store file until the marker ages out */
        long deletedRev = 0;
    };
    const std::vector<Tombstone> &tombstones() const { return tombstones_; }
    void setTombstones(std::vector<Tombstone> list);
    bool findTombstone(dbref ref, Tombstone *out) const;
    void removeTombstone(const UUID &u);
    /* Clear the deleted mark on a shell being resurrected. */
    void reviveHole(dbref ref);

    /* ============================================================ */
    /* LEGACY BRIDGE. Everything below exists only while the old    */
    /* struct object payload and its call sites survive; it shrinks */
    /* with every step 2 section and dies with the last one.        */
    /* ============================================================ */

    /* Raw payload accessor behind DBFETCH. Hot path: lock-free. */
    struct object *object(dbref ref) const {
        return chunkFor(ref)[slotFor(ref)]->legacyData();
    }

    /* Ensure objects [current top, newTop) exist as garbage-typed
     * shells; used by the loaders. Serializes on indexMutex_. */
    void ensureTop(dbref newTop);

    /* Mark a loader-created hole (a dbref with no stored object) as a
     * dead shell: legacy code sees TYPE_GARBAGE, modern code sees
     * isDeleted(). */
    void noteHole(dbref ref);

  private:
    friend class DbObject;      /* striped object locks */

    /* --- page table --- */
    static const int CHUNK_BITS = 16;
    static const int CHUNK_SIZE = 1 << CHUNK_BITS;          /* 64K */
    static const int TOP_CHUNKS = 32768;    /* 2^31 refs max */

    DbObject **chunkFor(dbref ref) const {
        return chunks_[ref >> CHUNK_BITS];
    }
    static int slotFor(dbref ref) { return ref & (CHUNK_SIZE - 1); }

    DbObject *makeObject(dbref ref);

    /* --- striped object locks --- */
    static const int LOCK_STRIPES = 1024;
    std::shared_mutex &stripeFor(dbref ref) const {
        return objectLocks_[(unsigned) ref % LOCK_STRIPES];
    }

    DbObject **chunks_[TOP_CHUNKS] = {};
    std::atomic<dbref> top_{0};
    dbref allocated_ = 0;       /* shells exist for [0, allocated_) */

    std::vector<Tombstone> tombstones_;
    std::unordered_map<UUID, DbObject *> byUUID_;
    mutable std::shared_mutex indexMutex_;
    mutable std::shared_mutex objectLocks_[LOCK_STRIPES];
};

extern Database g_database;

inline Database &database() { return g_database; }

/* DbObject lock methods stripe through the database. Defined here so
 * they inline. */
inline void DbObject::lockShared() const { database().stripeFor(ref_).lock_shared(); }
inline void DbObject::unlockShared() const { database().stripeFor(ref_).unlock_shared(); }
inline void DbObject::lockExclusive() const { database().stripeFor(ref_).lock(); }
inline void DbObject::unlockExclusive() const { database().stripeFor(ref_).unlock(); }

} /* namespace MUCK */

#endif /* MUCK_DATABASE_H */
