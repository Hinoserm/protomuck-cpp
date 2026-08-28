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

int
DbObject::muckerLevel() const
{
    switch (CheckMWLevel(ref_)) {
        case LBOY:
            return (tp_multi_wizlevels ? LBOY : LARCH);
        case LWIZ:
            return (tp_multi_wizlevels ? LWIZ : LARCH);
        case LMAGE:
            return (tp_multi_wizlevels ? LMAGE : LM3);
        default:
            return CheckMWLevel(ref_);
    }
}

int
DbObject::wizLevel() const
{
    int mlev = MLevel(ref_);

    return mlev >= LMAGE ? mlev : 0;
}

} /* namespace MUCK */

/* Legacy shims: the MLevel macro family in db.h lands here. The OkObj
 * guard preserves the historical 0 for invalid and recycled refs. */
int
RawMWLevel(dbref thing, const char *file, int line)
{
    (void) file;
    (void) line;

    if (!OkObj(thing))
        return 0;
    return MUCK::database().get(thing)->muckerLevel();
}

int
WLevel(dbref player)
{
    if (!OkObj(player))
        return 0;
    return MUCK::database().get(player)->wizLevel();
}

namespace MUCK {

const char *
DbObject::typeName()
{
    ensureModules();
    if (typeModule_)
        return typeModule_->moduleName();
    return (legacy_.flags & TYPE_MASK) == TYPE_UNSUPPORTED
        ? "unsupported" : "garbage";
}

Module *
DbObject::attach(std::unique_ptr<Module> m)
{
    m->owner_ = this;
    modules_.push_back(std::move(m));
    return modules_.back().get();
}

/* --------------------------------------------------------------- */
/* Containment                                                     */
/* --------------------------------------------------------------- */

/* Shared inert list for invalid refs so blanket conversions stay
 * safe; it is cleared on every access in case someone mutated it. */
static std::vector<DbObject *> &
emptyList()
{
    static std::vector<DbObject *> dummy;

    dummy.clear();
    return dummy;
}

std::vector<DbObject *> &
contentsOf(dbref ref)
{
    DbObject *o = database().get(ref);

    return o ? o->contentsVec() : emptyList();
}

std::vector<DbObject *> &
exitsOf(dbref ref)
{
    DbObject *o = database().get(ref);

    return o ? o->exitsVec() : emptyList();
}

dbref
firstContentRef(dbref loc)
{
    std::vector<DbObject *> &v = contentsOf(loc);

    if (v.empty())
        return NOTHING;
    v.front()->listHint = 0;
    return v.front()->ref();
}

dbref
firstExitRef(dbref loc)
{
    std::vector<DbObject *> &v = exitsOf(loc);

    if (v.empty())
        return NOTHING;
    v.front()->listHint = 0;
    return v.front()->ref();
}

dbref
nextSiblingRef(dbref obj)
{
    DbObject *o = database().get(obj);

    if (!o)
        return NOTHING;

    dbref loc = DBFETCH(obj)->location;

    if (loc == NOTHING)
        return NOTHING;

    std::vector<DbObject *> &v =
        (Typeof(obj) == TYPE_EXIT) ? exitsOf(loc) : contentsOf(loc);

    /* trust the hint when it still points at us; a mutation since the
     * last step just means one rescan */
    size_t idx = v.size();

    if (o->listHint < v.size() && v[o->listHint] == o) {
        idx = o->listHint;
    } else {
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == o) {
                o->listHint = i;
                idx = i;
                break;
            }
    }
    if (idx + 1 >= v.size())
        return NOTHING;
    v[idx + 1]->listHint = idx + 1;
    return v[idx + 1]->ref();
}

bool
listContains(const std::vector<DbObject *> &v, dbref obj)
{
    for (DbObject *o : v)
        if (o->ref() == obj)
            return true;
    return false;
}

void
attachContent(dbref loc, dbref obj)
{
    DbObject *l = database().get(loc);
    DbObject *o = database().get(obj);

    if (!l || !o)
        return;
    if (!listContains(l->contentsVec(), obj))
        l->contentsVec().insert(l->contentsVec().begin(), o);
}

void
detachContent(dbref loc, dbref obj)
{
    DbObject *l = database().get(loc);

    if (!l)
        return;

    std::vector<DbObject *> &v = l->contentsVec();

    for (size_t i = 0; i < v.size(); i++)
        if (v[i]->ref() == obj) {
            v.erase(v.begin() + i);
            return;
        }
}

void
attachExit(dbref loc, dbref ex)
{
    DbObject *l = database().get(loc);
    DbObject *o = database().get(ex);

    if (!l || !o)
        return;
    if (!listContains(l->exitsVec(), ex))
        l->exitsVec().insert(l->exitsVec().begin(), o);
}

void
detachExit(dbref loc, dbref ex)
{
    DbObject *l = database().get(loc);

    if (!l)
        return;

    std::vector<DbObject *> &v = l->exitsVec();

    for (size_t i = 0; i < v.size(); i++)
        if (v[i]->ref() == ex) {
            v.erase(v.begin() + i);
            return;
        }
}

std::vector<DbObject *>
Container::contents() const
{
    return object()->contentsVec();
}

std::vector<DbObject *>
Container::exits() const
{
    return object()->exitsVec();
}

/* --------------------------------------------------------------- */
/* Type modules                                                    */
/* --------------------------------------------------------------- */

DbObject *
Room::dropTo() const
{
    return database().get(dropTo_);
}

void
Room::setDropTo(DbObject *where)
{
    setDropToRef(where ? where->ref() : NOTHING);
}

void
Room::setDropToRef(dbref d)
{
    dropTo_ = d;
    DBDIRTY(object()->ref());
}

DbObject *
Thing::home() const
{
    return database().get(home_);
}

void
Thing::setHome(DbObject *where)
{
    setHomeRef(where ? where->ref() : NOTHING);
}

void
Thing::setHomeRef(dbref d)
{
    home_ = d;
    DBDIRTY(object()->ref());
}

int
Thing::value() const
{
    return value_;
}

void
Thing::setValue(int v)
{
    value_ = v;
    DBDIRTY(object()->ref());
}

DbObject *
Player::home() const
{
    return database().get(home_);
}

void
Player::setHome(DbObject *where)
{
    setHomeRef(where ? where->ref() : NOTHING);
}

void
Player::setHomeRef(dbref d)
{
    home_ = d;
    DBDIRTY(object()->ref());
}

int
Player::pennies() const
{
    return pennies_;
}

void
Player::setPennies(int v)
{
    pennies_ = v;
    DBDIRTY(object()->ref());
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
    return (int) dests_.size();
}

dbref
Exit::destRef(int i) const
{
    if (i < 0 || (size_t) i >= dests_.size())
        return NOTHING;
    return dests_[i];
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

void
playerSetHomeRef(dbref ref, dbref where)
{
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    if (p)
        p->setHomeRef(where);
}

void
playerSetPennies(dbref ref, int v)
{
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    if (p)
        p->setPennies(v);
}

dbref
roomDropToRef(dbref ref)
{
    DbObject *o = database().get(ref);
    Room *r = o ? o->As<Room>() : nullptr;

    return r ? r->dropToRef() : NOTHING;
}

void
roomSetDropToRef(dbref ref, dbref where)
{
    DbObject *o = database().get(ref);
    Room *r = o ? o->As<Room>() : nullptr;

    if (r)
        r->setDropToRef(where);
}

dbref
thingHomeRef(dbref ref)
{
    DbObject *o = database().get(ref);
    Thing *t = o ? o->As<Thing>() : nullptr;

    return t ? t->homeRef() : NOTHING;
}

void
thingSetHomeRef(dbref ref, dbref where)
{
    DbObject *o = database().get(ref);
    Thing *t = o ? o->As<Thing>() : nullptr;

    if (t)
        t->setHomeRef(where);
}

int
thingValue(dbref ref)
{
    DbObject *o = database().get(ref);
    Thing *t = o ? o->As<Thing>() : nullptr;

    return t ? t->value() : 0;
}

void
thingSetValue(dbref ref, int v)
{
    DbObject *o = database().get(ref);
    Thing *t = o ? o->As<Thing>() : nullptr;

    if (t)
        t->setValue(v);
}

const char *&
playerPasswordSlot(dbref ref)
{
    static const char *dummy;
    DbObject *o = database().get(ref);
    Player *p = o ? o->As<Player>() : nullptr;

    if (!p) {
        dummy = nullptr;
        return dummy;
    }
    return p->passwordSlot();
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
    dests_.assign(refs, refs + (n > 0 ? n : 0));
    DBDIRTY(object()->ref());
}

std::vector<DbObject *>
Exit::destinations() const
{
    std::vector<DbObject *> out;

    for (dbref d : dests_)
        if (DbObject *o = database().get(d))
            out.push_back(o);
    return out;
}

void
Exit::setDestinations(const std::vector<DbObject *> &dests)
{
    dests_.clear();
    for (DbObject *o : dests)
        dests_.push_back(o->ref());
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
