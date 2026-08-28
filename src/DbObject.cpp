#include "copyright.h"
#include "config.h"
#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "strutils.h"
#include "DbObject.h"
#include "Modules.h"
#include "ProgramStore.h"
#include "PasswordHash.h"

/* Views over the legacy storage: every accessor resolves through the
 * classic macros so the two worlds stay coherent during migration. */

/* check_password and set_password come from externs.h */

namespace MUCK {

/* --------------------------------------------------------------- */
/* DbObject core                                                   */
/* --------------------------------------------------------------- */

DbObject::DbObject(dbref ref) : ref_(ref)
{
    /* garbage-typed shell until a loader or creator fills it */
    memset(&legacy_, 0, sizeof(legacy_));
    legacy_.name = NULL;
    legacy_.flags = TYPE_GARBAGE;
    legacy_.location = NOTHING;
    legacy_.owner = NOTHING;
    legacy_.contents = NOTHING;
    legacy_.exits = NOTHING;
    legacy_.next = NOTHING;
    legacy_.properties = NULL;
}

void
DbObject::ensureModules()
{
    int bits = legacy_.flags & TYPE_MASK;

    if (moduleTypeBits_ != bits)
        rebuildModules();
}

void
DbObject::rebuildModules()
{
    modules_.clear();
    typeModule_ = nullptr;
    moduleTypeBits_ = legacy_.flags & TYPE_MASK;

    switch (moduleTypeBits_) {
        case TYPE_ROOM:
            typeModule_ = attach(std::make_unique<Room>());
            break;
        case TYPE_THING:
            typeModule_ = attach(std::make_unique<Thing>());
            break;
        case TYPE_PLAYER:
            typeModule_ = attach(std::make_unique<Player>());
            break;
        case TYPE_EXIT:
            typeModule_ = attach(std::make_unique<Exit>());
            break;
        case TYPE_PROGRAM:
            typeModule_ = attach(std::make_unique<MufProgram>());
            break;
        default:               /* garbage: no type module */
            break;
    }
    /* PROPERTIES is a global feature module: every object has it */
    attach(std::make_unique<Properties>());
}

const char *
DbObject::name() const
{
    return NAME(ref_);
}

void
DbObject::setName(const char *newname)
{
    delete[] DBFETCH(ref_)->name;
    NAME(ref_) = alloc_string(newname);
    DBDIRTY(ref_);
}

DbObject *
DbObject::location() const
{
    return database().get(DBFETCH(ref_)->location);
}

DbObject *
DbObject::owner() const
{
    return database().get(OWNER(ref_));
}

void
DbObject::setOwner(DbObject *o)
{
    OWNER(ref_) = o ? o->ref() : NOTHING;
    DBDIRTY(ref_);
}

const char *
DbObject::typeName()
{
    ensureModules();
    return typeModule_ ? typeModule_->moduleName() : "garbage";
}

Module *
DbObject::attach(std::unique_ptr<Module> m)
{
    m->owner_ = this;
    modules_.push_back(std::move(m));
    return modules_.back().get();
}

/* --------------------------------------------------------------- */
/* Container                                                       */
/* --------------------------------------------------------------- */

static std::vector<DbObject *>
chainVector(dbref first)
{
    std::vector<DbObject *> out;
    dbref guard = database().top();

    for (dbref i = first; i != NOTHING && guard-- > 0; i = DBFETCH(i)->next)
        if (DbObject *o = database().get(i))
            out.push_back(o);
    return out;
}

std::vector<DbObject *>
Container::contents() const
{
    return chainVector(DBFETCH(object()->ref())->contents);
}

std::vector<DbObject *>
Container::exits() const
{
    return chainVector(DBFETCH(object()->ref())->exits);
}

/* --------------------------------------------------------------- */
/* Type modules                                                    */
/* --------------------------------------------------------------- */

DbObject *
Room::dropTo() const
{
    return database().get(DBFETCH(object()->ref())->sp.room.dropto);
}

void
Room::setDropTo(DbObject *where)
{
    DBSTORE(object()->ref(), sp.room.dropto, where ? where->ref() : NOTHING);
}

DbObject *
Thing::home() const
{
    return database().get(DBFETCH(object()->ref())->sp.thing.home);
}

void
Thing::setHome(DbObject *where)
{
    DBSTORE(object()->ref(), sp.thing.home, where ? where->ref() : NOTHING);
}

int
Thing::value() const
{
    return DBFETCH(object()->ref())->sp.thing.value;
}

void
Thing::setValue(int v)
{
    DBSTORE(object()->ref(), sp.thing.value, v);
}

DbObject *
Player::home() const
{
    return database().get(DBFETCH(object()->ref())->sp.player.home);
}

void
Player::setHome(DbObject *where)
{
    DBSTORE(object()->ref(), sp.player.home, where ? where->ref() : NOTHING);
}

int
Player::pennies() const
{
    return DBFETCH(object()->ref())->sp.player.pennies;
}

void
Player::setPennies(int v)
{
    DBSTORE(object()->ref(), sp.player.pennies, v);
}

bool
Player::checkPassword(const char *plaintext) const
{
    return ::check_password(object()->ref(), plaintext);
}

bool
Player::setPassword(const char *plaintext)
{
    return ::set_password(object()->ref(), plaintext);
}

int
Exit::destCount() const
{
    return DBFETCH(object()->ref())->sp.exit.ndest;
}

dbref
Exit::destRef(int i) const
{
    struct object *o = DBFETCH(object()->ref());

    if (i < 0 || i >= o->sp.exit.ndest || !o->sp.exit.dest)
        return NOTHING;
    return o->sp.exit.dest[i];
}

int
exitDestCount(dbref ref)
{
    DbObject *o = database().get(ref);
    Exit *e = o ? o->As<Exit>() : nullptr;

    return e ? e->destCount() : 0;
}

dbref
exitDestRef(dbref ref, int i)
{
    DbObject *o = database().get(ref);
    Exit *e = o ? o->As<Exit>() : nullptr;

    return e ? e->destRef(i) : NOTHING;
}

dbref
playerHomeRef(dbref ref)
{
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    return (p && p->home()) ? p->home()->ref() : NOTHING;
}

int
playerPennies(dbref ref)
{
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    return p ? p->pennies() : 0;
}

void
playerAddPennies(dbref ref, int delta)
{
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    if (p)
        p->setPennies(p->pennies() + delta);
}

PlayerSession &
playerSession(dbref ref)
{
    static PlayerSession dummy;
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    if (!p) {
        dummy = PlayerSession();
        return dummy;
    }
    return p->session();
}

ProgramRuntime &
programRuntime(dbref ref)
{
    static ProgramRuntime dummy;
    DbObject *o = database().get(ref);
    MufProgram *p = o ? o->As<MufProgram>() : nullptr;

    if (!p) {
        dummy = ProgramRuntime();
        return dummy;
    }
    return p->runtime();
}

void
Exit::setDestRefs(const dbref *refs, int n)
{
    struct object *o = DBFETCH(object()->ref());

    delete[] o->sp.exit.dest;
    o->sp.exit.ndest = n;
    o->sp.exit.dest = n > 0 ? new dbref[n] : NULL;
    for (int i = 0; i < n; i++)
        o->sp.exit.dest[i] = refs[i];
    DBDIRTY(object()->ref());
}

std::vector<DbObject *>
Exit::destinations() const
{
    std::vector<DbObject *> out;
    struct object *o = DBFETCH(object()->ref());

    for (int i = 0; i < o->sp.exit.ndest; i++)
        if (DbObject *d = database().get(o->sp.exit.dest[i]))
            out.push_back(d);
    return out;
}

void
Exit::setDestinations(const std::vector<DbObject *> &dests)
{
    struct object *o = DBFETCH(object()->ref());

    delete[] o->sp.exit.dest;
    o->sp.exit.ndest = (int) dests.size();
    o->sp.exit.dest = dests.empty() ? NULL : new dbref[dests.size()];
    for (size_t i = 0; i < dests.size(); i++)
        o->sp.exit.dest[i] = dests[i]->ref();
    DBDIRTY(object()->ref());
}

const std::vector<std::string> *
MufProgram::source() const
{
    return programs().sourceLines(object()->ref());
}

void
MufProgram::setSource(std::vector<std::string> lines)
{
    programs().setSourceLines(object()->ref(), std::move(lines));
    DBDIRTY(object()->ref());
}

/* --------------------------------------------------------------- */
/* Properties                                                      */
/* --------------------------------------------------------------- */

const char *
Properties::getString(const char *path) const
{
    return get_property_class(object()->ref(), path);
}

int
Properties::getInt(const char *path) const
{
    return get_property_value(object()->ref(), path);
}

double
Properties::getFloat(const char *path) const
{
    return get_property_fvalue(object()->ref(), path);
}

dbref
Properties::getRef(const char *path) const
{
    return get_property_dbref(object()->ref(), path);
}

void
Properties::setString(const char *path, const char *value)
{
    add_property(object()->ref(), path, value, 0);
}

void
Properties::setInt(const char *path, int value)
{
    add_property(object()->ref(), path, NULL, value);
}

void
Properties::remove(const char *path)
{
    remove_property(object()->ref(), path);
}

bool
Properties::exists(const char *path) const
{
    return get_property(object()->ref(), path) != NULL;
}

} /* namespace MUCK */
