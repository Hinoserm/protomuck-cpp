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

    /* --- whole-database serialization --- */
    dbref load(FILE *f);
    dbref save(FILE *f);
    int saveThreaded();

    /* internal: reached from legacy helpers inside Database.cpp */
    void grow(dbref newtop);
    struct object *rawArray() { return db; }

  private:
    struct object *db = 0;
    dbref db_top = 0;
    dbref recyclable = -3;      /* NOTHING; db.h not yet parsed here */
    dbref db_size = 0;          /* allocation high-water for DB_DOUBLING */

    friend Database &database();
};

extern Database g_database;

inline Database &database() { return g_database; }

} /* namespace MUCK */

#endif /* MUCK_DATABASE_H */
