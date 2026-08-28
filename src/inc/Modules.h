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
#include "PropertyTree.h"

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
    static const char *staticName() { return "room"; }
    const char *moduleName() const override { return staticName(); }
    DbObject *dropTo() const;
    void setDropTo(DbObject *where);

    /* Raw ref access: dropto may hold the HOME sentinel, which the
     * pointer API cannot express. */
    dbref dropToRef() const { return dropTo_; }
    void setDropToRef(dbref d);

  private:
    dbref dropTo_ = -3;         /* NOTHING */
};

class Thing : public Container {
  public:
    static const char *staticName() { return "thing"; }
    const char *moduleName() const override { return staticName(); }
    DbObject *home() const;
    void setHome(DbObject *where);
    int value() const;
    void setValue(int v);

    dbref homeRef() const { return home_; }
    void setHomeRef(dbref d);

  private:
    dbref home_ = -3;           /* NOTHING */
    int value_ = 0;
};

/* Per-player transient state: everything about the LIVE presence of a
 * player. Lives on the Player module, structurally unserializable, gone
 * at restart. docs/DATABASE.txt section 2. */
struct PlayerSession {
    std::vector<int> descrs;    /* descriptor table indexes */
    int lastDescr = -1;
    dbref currProg = -1;        /* NOTHING; editor target */
    short insertMode = 0;
    short block = 0;
    std::vector<dbref> ignore;  /* ignore-list cache */
    long ignoreTime = 0;
};

class Player : public Container {
  public:
    static const char *staticName() { return "player"; }
    const char *moduleName() const override { return staticName(); }
    ~Player() override { delete[] password_; }
    DbObject *home() const;
    void setHome(DbObject *where);
    int pennies() const;
    void setPennies(int v);

    dbref homeRef() const { return home_; }
    void setHomeRef(dbref d);

    /* Password interface; hashing handled by PasswordHash. */
    bool checkPassword(const char *plaintext) const;
    bool setPassword(const char *plaintext);

    /* Raw stored hash slot for the legacy password plumbing in
     * player.cpp; owned here, alloc_string allocated. */
    const char *&passwordSlot() { return password_; }

    PlayerSession &session() { return session_; }

  private:
    dbref home_ = -3;           /* NOTHING */
    int pennies_ = 0;
    const char *password_ = nullptr;
    PlayerSession session_;
};

class Exit : public Module {
  public:
    static const char *staticName() { return "exit"; }
    const char *moduleName() const override { return staticName(); }
    std::vector<DbObject *> destinations() const;
    void setDestinations(const std::vector<DbObject *> &dests);

    /* Zero-copy transitional accessors for the legacy index-loop call
     * sites: no allocation on the movement hot path, raw refs so the
     * NIL and HOME sentinels survive. destRef returns NOTHING out of
     * range. These go away when destinations become owned vectors. */
    int destCount() const;
    dbref destRef(int i) const;

    /* Raw setter: replaces the whole destination list, sentinels
     * preserved, dirty flag set. n of 0 clears. */
    void setDestRefs(const dbref *refs, int n);

  private:
    std::vector<dbref> dests_;  /* may hold HOME and NIL sentinels */
};

/* Containment helpers over the owning vectors on DbObject. Blanket
 * safe: a bad ref yields a shared empty list and no-op mutations.
 * attachContent/attachExit prepend (legacy PUSH order); the next
 * sibling of an object is the element after it in its location's
 * exits list for exits, contents list for everything else. */
std::vector<DbObject *> &contentsOf(dbref ref);
std::vector<DbObject *> &exitsOf(dbref ref);
dbref firstContentRef(dbref loc);
dbref firstExitRef(dbref loc);
dbref nextSiblingRef(dbref obj);
bool listContains(const std::vector<DbObject *> &v, dbref obj);
void attachContent(dbref loc, dbref obj);
void detachContent(dbref loc, dbref obj);
void attachExit(dbref loc, dbref ex);
void detachExit(dbref loc, dbref ex);

/* Blanket-safe free helpers for mechanical call-site conversion:
 * tolerate refs that are not exits (count 0, ref NOTHING), so guarded
 * legacy expressions convert one for one. */
int exitDestCount(dbref ref);
dbref exitDestRef(dbref ref, int i);
dbref playerHomeRef(dbref ref);
int playerPennies(dbref ref);
void playerAddPennies(dbref ref, int delta);
void playerSetHomeRef(dbref ref, dbref where);
void playerSetPennies(dbref ref, int v);
dbref roomDropToRef(dbref ref);
void roomSetDropToRef(dbref ref, dbref where);
dbref thingHomeRef(dbref ref);
void thingSetHomeRef(dbref ref, dbref where);
int thingValue(dbref ref);
void thingSetValue(dbref ref, int v);

/* Session of a player ref; a shared inert dummy for non-players so
 * blanket conversions stay safe. Do not hold across type changes. */
PlayerSession &playerSession(dbref ref);

/* Raw password-hash slot of a player ref; a shared inert dummy slot
 * for non-players. Owned by the Player module. */
const char *&playerPasswordSlot(dbref ref);

/* Per-program transient interpreter and editor state: compiled code,
 * running-instance bookkeeping, the editor's working text, profiling.
 * Lives on the MufProgram module, memory-only, rebuilt from stored
 * source on demand; the persisted source itself goes through the
 * program text store. docs/DATABASE.txt section 2. */
struct ProgramRuntime {
    short currLine = 0;             /* editor current line */
    unsigned short instances = 0;   /* running instances of this program */
    int codeSize = 0;               /* length of compiled code */
    struct inst *code = nullptr;    /* byte-compiled code */
    struct inst *start = nullptr;   /* entry point within code */
    struct line *first = nullptr;   /* editor working text */
    struct publics *pubs = nullptr; /* public subroutine addresses */
    struct timeval profTime = {};   /* profiling time spent in program */
    time_t profStart = 0;           /* when profiling started */
    unsigned int profUses = 0;      /* calls while profiling */
    struct funcprof *fprofile = nullptr;
    struct inst *staticVars = nullptr;
    int staticVarCnt = 0;
};

class MufProgram : public Module {
  public:
    static const char *staticName() { return "muf_program"; }
    const char *moduleName() const override { return staticName(); }
    const std::vector<std::string> *source() const;
    void setSource(std::vector<std::string> lines);

    ProgramRuntime &runtime() { return runtime_; }

  private:
    ProgramRuntime runtime_;
};

/* Runtime of a program ref; a shared inert dummy for non-programs so
 * blanket conversions stay safe. Do not hold across type changes. */
ProgramRuntime &programRuntime(dbref ref);

/* --------------------------------------------------------------- */
/* Feature modules.                                                */
/* --------------------------------------------------------------- */

class Properties : public Module {
  public:
    static const char *staticName() { return "properties"; }
    const char *moduleName() const override { return staticName(); }

    /* The object's root propdir. docs/PROPERTIES.txt. */
    PropertyTree &root() { return root_; }

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

  private:
    PropertyTree root_;
};

/* Root propdir of an object, or null when the Properties module is
 * not attached (excluded build): every legacy entry point then reads
 * empty and refuses writes. docs/PROPERTIES.txt section 5. */
PropertyTree *propRoot(dbref ref);

} /* namespace MUCK */

#endif /* MUCK_MODULES_H */
