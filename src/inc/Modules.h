#ifndef MUCK_MODULES_H
#define MUCK_MODULES_H

/* The built-in type modules (exactly one per object, attached by
 * Database::Create) and the first feature module, Properties.
 * See docs/DATABASE.txt section 2.
 *
 * These are views over the legacy struct object during step 2; their
 * accessors are the migration target for every call site that touches
 * the sp union or the raw containment chains today.
 */

#include "DbObject.h"

namespace MUCK {

/* --------------------------------------------------------------- */
/* Container: shared base of the type modules that can hold things. */
/* --------------------------------------------------------------- */

class Container : public Module {
  public:
    /* Materialized from the legacy contents/exits chains for now;
     * become the owning vectors when storage flips. */
    std::vector<DbObject *> contents() const;
    std::vector<DbObject *> exits() const;
};

/* --------------------------------------------------------------- */
/* Type modules.                                                   */
/* --------------------------------------------------------------- */

class Room : public Container {
  public:
    const char *moduleName() const override { return "room"; }
    DbObject *dropTo() const;
    void setDropTo(DbObject *where);
};

class Thing : public Container {
  public:
    const char *moduleName() const override { return "thing"; }
    DbObject *home() const;
    void setHome(DbObject *where);
    int value() const;
    void setValue(int v);
};

class Player : public Container {
  public:
    const char *moduleName() const override { return "player"; }
    DbObject *home() const;
    void setHome(DbObject *where);
    int pennies() const;
    void setPennies(int v);

    /* Password interface; hashing handled by PasswordHash. */
    bool checkPassword(const char *plaintext) const;
    bool setPassword(const char *plaintext);

    /* Transient session state (descriptors) migrates here from the
     * sp union and interface.cpp in a later integration pass. */
};

class Exit : public Module {
  public:
    const char *moduleName() const override { return "exit"; }
    std::vector<DbObject *> destinations() const;
    void setDestinations(const std::vector<DbObject *> &dests);

    /* Zero-copy transitional accessors for the legacy index-loop call
     * sites: no allocation on the movement hot path, raw refs so the
     * NIL and HOME sentinels survive. destRef returns NOTHING out of
     * range. These go away when destinations become owned vectors. */
    int destCount() const;
    dbref destRef(int i) const;

    /* Transitional raw setter: replaces the whole destination array,
     * sentinels preserved, dirty flag set. n of 0 clears. */
    void setDestRefs(const dbref *refs, int n);
};

/* Blanket-safe free helpers for mechanical call-site conversion:
 * tolerate refs that are not exits (count 0, ref NOTHING), so guarded
 * legacy expressions convert one for one. */
int exitDestCount(dbref ref);
dbref exitDestRef(dbref ref, int i);
dbref playerHomeRef(dbref ref);
int playerPennies(dbref ref);
void playerAddPennies(dbref ref, int delta);

class MufProgram : public Module {
  public:
    const char *moduleName() const override { return "muf_program"; }
    const std::vector<std::string> *source() const;
    void setSource(std::vector<std::string> lines);
};

/* --------------------------------------------------------------- */
/* Feature modules.                                                */
/* --------------------------------------------------------------- */

class Properties : public Module {
  public:
    const char *moduleName() const override { return "properties"; }

    /* Thin typed veneer over the legacy prop tree; call sites migrate
     * here from get_property_class and friends. */
    const char *getString(const char *path) const;
    int getInt(const char *path) const;
    double getFloat(const char *path) const;
    dbref getRef(const char *path) const;
    void setString(const char *path, const char *value);
    void setInt(const char *path, int value);
    void remove(const char *path);
    bool exists(const char *path) const;
};

} /* namespace MUCK */

#endif /* MUCK_MODULES_H */
