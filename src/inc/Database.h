#ifndef MUCK_DATABASE_H
#define MUCK_DATABASE_H

/* The modernized face of the object database.
 *
 * This is currently a facade: storage still lives in the legacy globals
 * (db, db_top, recyclable) defined in Database.cpp, because the DBFETCH
 * family of macros in db.h indexes the db array directly from every
 * corner of the codebase. Callers should migrate to this interface; once
 * nothing touches the globals directly, storage moves inside the class.
 */

#include <cstdio>
#include "db.h"

namespace muck {

class Database {
  public:
    /* Object accounting */
    dbref top() const;
    struct object *object(dbref ref);
    bool valid(dbref ref) const;

    /* Object lifecycle */
    dbref newObject(dbref player);
    dbref newProgram(dbref player, const char *name);
    void clearObject(dbref player, dbref ref);
    void freeObject(dbref ref);
    void freeAll();
    dbref parent(dbref obj);

    /* Whole-database serialization */
    dbref load(FILE *f);
    void save(FILE *f);
    int saveThreaded();
};

/* The single global database instance. */
Database &database();

} /* namespace muck */

#endif /* MUCK_DATABASE_H */
