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

const Uuid &
DbObject::uuid() const
{
    return database().uuidOf(ref_);
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
DbObject::typeName() const
{
    return typeModule_ ? typeModule_->moduleName() : "garbage";
}

Module *
DbObject::attach(std::unique_ptr<Module> m)
{
    m->owner_ = this;
    modules_.push_back(std::move(m));
    return modules_.back().get();
}

void
DbObject::setTypeModule(std::unique_ptr<Module> m)
{
    typeModule_ = attach(std::move(m));
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
