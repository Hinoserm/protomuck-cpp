#ifndef MUCK_DATABASE_H
#define MUCK_DATABASE_H

/* The object database: owns the object array, its growth, object
 * lifecycle, and whole-database serialization.
 *
 * The hot accessors (object, top, valid) are inline: the DBFETCH macro
 * family in db.h routes through them from the interpreter's innermost
 * loops, and they must compile down to the same array indexing the old
 * bare globals produced.
 *
 * Storage note: the members are named db and db_top so the method bodies
 * in Database.cpp, which carry decades of history, read unchanged.
 */

#include <cstdio>
#include <vector>
#include <unordered_map>

#include "Uuid.h"

namespace MUCK {

class Database {
  public:
    /* --- hot path: keep inline, zero-cost --- */
    dbref top() const { return db_top; }
    struct object *object(dbref ref) { return &db[ref]; }
    const struct object *object(dbref ref) const { return &db[ref]; }
    bool valid(dbref ref) const {
        return ref >= 0 && ref < db_top;
    }

    /* --- object lifecycle --- */
    dbref newObject(dbref player);
    dbref newProgram(dbref player, const char *name);
    void clearObject(dbref player, dbref ref);
    void freeObject(dbref ref);
    void freeAll();
    dbref parent(dbref obj);

    /* --- recycle list (garbage chain reused by newObject) --- */
    dbref recycleHead() const { return recyclable; }
    void setRecycleHead(dbref ref) { recyclable = ref; }

    /* Whole-database serialization lives in ObjectStore (the JSON
     * store, the only write path) and FlatFileConverter (one-way
     * import of the legacy flat format). */

    /* --- identity (see docs/DATABASE.txt section 1) --- */
    /* Every live object carries a UUIDv7 minted at creation or import
     * and never changed. uuidOf returns the nil uuid for out-of-range
     * refs; refOf returns NOTHING for unknown uuids. */
    const Uuid &uuidOf(dbref ref) const;
    dbref refOf(const Uuid &u) const;
    void assignUuid(dbref ref, const Uuid &u);

    /* Resolve a short-form hex prefix (git style). Returns NOTHING if
     * no match, AMBIGUOUS if more than one object matches. */
    dbref resolveUuidPrefix(const char *prefix) const;

    /* internal: reached from legacy helpers inside Database.cpp */
    void grow(dbref newtop);
    void growTo(dbref newtop) { grow(newtop); }
    struct object *rawArray() { return db; }

  private:
    struct object *db = 0;
    dbref db_top = 0;
    dbref recyclable = -3;      /* NOTHING; db.h not yet parsed here */
    dbref db_size = 0;          /* allocation high-water for DB_DOUBLING */

    std::vector<Uuid> uuids;                    /* by dbref */
    std::unordered_map<Uuid, dbref> byUuid;

    friend Database &database();
};

extern Database g_database;

inline Database &database() { return g_database; }

} /* namespace MUCK */

#endif /* MUCK_DATABASE_H */
