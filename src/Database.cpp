#include "copyright.h"
#include "config.h"

#include <algorithm>

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "MacroTable.h"
#include "Modules.h"
#include "ProgramStore.h"
#include "ObjectStore.h"
#include "ObjectAccess.h"
#include "Journal.h"
#include "strutils.h"

#ifndef MALLOC_PROFILING
extern char *alloc_string(const char *);
#endif

namespace MUCK {

Database g_database;

/* ================================================================ */
/* Identity                                                         */
/* ================================================================ */

static const UUID nilUUID;

const UUID &
Database::UUIDOf(dbref ref) const
{
    DbObject *o = get(ref);

    return o ? o->uuid() : nilUUID;
}

dbref
Database::refOf(const UUID &u) const
{
    DbObject *o = get(u);

    return o ? o->ref() : NOTHING;
}

DbObject *
Database::get(const UUID &u) const
{
    if (u.isNil())
        return nullptr;

    std::shared_lock<std::shared_mutex> hold(indexMutex_);
    auto it = byUUID_.find(u);

    return it == byUUID_.end() ? nullptr : it->second;
}

void
Database::assignUUID(dbref ref, const UUID &u)
{
    DbObject *o = get(ref);

    if (!o)
        return;

    std::unique_lock<std::shared_mutex> hold(indexMutex_);

    if (!o->uuid_.isNil())
        byUUID_.erase(o->uuid_);
    o->uuid_ = u;
    if (!u.isNil())
        byUUID_[u] = o;
}

dbref
Database::resolveUUIDPrefix(const char *prefix) const
{
    std::shared_lock<std::shared_mutex> hold(indexMutex_);
    dbref found = NOTHING;

    for (const auto &pair : byUUID_) {
        if (!pair.first.matchesPrefix(prefix))
            continue;
        if (found != NOTHING)
            return AMBIGUOUS;
        found = pair.second->ref();
    }
    return found;
}

/* ================================================================ */
/* Index and storage                                                */
/* ================================================================ */

DbObject *
Database::makeObject(dbref ref)
{
    return new DbObject(ref);
}

void
Database::ensureTop(dbref newTop)
{
    if (newTop <= allocated_ && newTop <= top())
        return;

    std::unique_lock<std::shared_mutex> hold(indexMutex_);

    for (dbref i = allocated_; i < newTop; i++) {
        int c = i >> CHUNK_BITS;

        if (!chunks_[c])
            chunks_[c] = new DbObject *[CHUNK_SIZE]();
        chunks_[c][slotFor(i)] = makeObject(i);
    }
    if (newTop > allocated_)
        allocated_ = newTop;
    if (newTop > top())
        top_.store(newTop, std::memory_order_release);
}

/* ================================================================ */
/* Lifecycle                                                        */
/* ================================================================ */

void
Database::clearObject(dbref player, dbref i)
{
    struct object *o = DBFETCH(i);

    /* The bzero wipes the flags word, and the type bits mirrored in it
     * go with it, leaving the mirror disagreeing with the type field.
     * Put the object back in a coherent state explicitly rather than
     * leaving two sources of truth pointing different ways. */
    bzero(o, sizeof(struct object));
    if (DbObject *sh = get(i))
        sh->setType(ObjectType::Garbage);
    MUCK::setName(i, 0);
    ts_newobject(player, o);
    MUCK::setLocation(i, NOTHING);
    contentsOf(i).clear();
    journalRecord(i, "$type/contents");
    exitsOf(i).clear();
    journalRecord(i, "$type/exits");
    if (DbObject *sh = get(i)) {
        if (Properties *pp = sh->As<Properties>()) {
            journalRecordPropTree(i, "");
            pp->root().clear();
        }
    }

    /* flags and type-specific fields are the caller's to initialize */
}

dbref
Database::newObject(dbref player)
{
    /* dbrefs are monotonic and NEVER reused: the recycling that let a
     * stale #500 silently retarget to a stranger's new object is gone
     * for good. Deleted slots stay dead shells forever. */
    dbref newobj = top();

    ensureTop(newobj + 1);
    clearObject(player, newobj);
    assignUUID(newobj, UUID::generate());
    DBDIRTY(newobj);
    return newobj;
}

dbref
Database::newProgram(dbref player, const char *name)
{
    unsigned char mlvl;
    char buf[BUFFER_LEN];

    player = MUCK::getOwner(player);

    MufProgram *mp = Create<MufProgram>(name, player);

    if (!mp)
        return NOTHING;

    dbref newprog = mp->object()->ref();

    sprintf(buf, "A scroll containing a spell called %s", name);
    MUCK::setDesc(newprog, buf);
    MUCK::setLocation(newprog, player);   /* chain wiring flips later */

    mlvl = MLevel(player);
    if (mlvl < 1)
        mlvl = 2;
    else if (mlvl > 3)
        mlvl = 3;
    SetMLevel(newprog, mlvl);

    attachContent(player, newprog);
    DBDIRTY(newprog);
    DBDIRTY(player);

    return newprog;
}

void
Database::freeObject(dbref i)
{
    struct object *o;

    o = DBFETCH(i);
    /* names are always heap-allocated, dead shells included */
    if (MUCK::getName(i))
        delete[](char *) MUCK::getName(i);

    /* properties, exit destinations, and the player password hash are
     * owned by their modules now and freed with the module objects */
#ifndef SANITY
    if (Typeof(i) == TYPE_PROGRAM) {
        uncompile_program(i);
    }
#endif
    /* DBDIRTY(i); */
}

void
Database::freeAll()
{
    for (dbref i = 0; i < allocated_; i++) {
        freeObject(i);
        delete chunkFor(i)[slotFor(i)];
        chunkFor(i)[slotFor(i)] = nullptr;
    }

    {
        std::unique_lock<std::shared_mutex> hold(indexMutex_);

        for (int c = 0; c < TOP_CHUNKS; c++) {
            delete[] chunks_[c];
            chunks_[c] = nullptr;
        }
        byUUID_.clear();
        tombstones_.clear();
        allocated_ = 0;
        top_.store(0, std::memory_order_release);
    }

    clear_players();
    clear_primitives();
}

void
Database::noteHole(dbref ref)
{
    DbObject *o = get(ref);

    if (!o)
        return;
    o->deleted_ = true;
    if (!NAME(ref))
        MUCK::setName(ref, "<garbage>");
}

void
Database::deleteObject(dbref victim, dbref deleter)
{
    DbObject *o = get(victim);

    if (!o)
        return;

    /* Flush the final state while the uuid still resolves. The file
     * and its history stay on disk as long as a retained snapshot
     * marker covers the object's lifetime, so @rollback can restore
     * it; the manifest sweep reclaims it once coverage ages out. */
    long drev = 0;

    if (store().active())
        drev = store().retireObject(victim);

    Tombstone t;
    t.uuid = o->uuid();
    t.ref = victim;
    t.deletedAt = (long) current_systime;
    t.deletedBy = UUIDOf(deleter);
    t.deletedRev = drev < 0 ? 0 : drev;

    {
        std::unique_lock<std::shared_mutex> hold(indexMutex_);

        tombstones_.push_back(t);
        if (!o->uuid_.isNil())
            byUUID_.erase(o->uuid_);
    }
    o->deleted_ = true;

    /* OBJECT_DELETED broadcast disabled pending design discussion.
    struct inst temp;

    temp.type = PROG_OBJECT;
    temp.data.objref = victim;
    broadcast_muf_event((char *) "OBJECT_DELETED", &temp, 1, 0);
    */
}

void
Database::setTombstones(std::vector<Tombstone> list)
{
    std::unique_lock<std::shared_mutex> hold(indexMutex_);

    tombstones_ = std::move(list);
}

bool
Database::findTombstone(dbref ref, Tombstone *out) const
{
    std::shared_lock<std::shared_mutex> hold(indexMutex_);

    /* newest first: a slot is never reused, but be explicit anyway */
    for (auto it = tombstones_.rbegin(); it != tombstones_.rend(); ++it)
        if (it->ref == ref) {
            if (out)
                *out = *it;
            return true;
        }
    return false;
}

void
Database::removeTombstone(const UUID &u)
{
    std::unique_lock<std::shared_mutex> hold(indexMutex_);

    tombstones_.erase(
        std::remove_if(tombstones_.begin(), tombstones_.end(),
                       [&u](const Tombstone &t) { return t.uuid == u; }),
        tombstones_.end());
}

void
Database::reviveHole(dbref ref)
{
    DbObject *o = get(ref);

    if (o)
        o->deleted_ = false;
}

dbref
Database::parent(dbref obj)
{
    int limit = 88;

    if (!OkObj(obj))
        return GLOBAL_ENVIRONMENT;
    do {
        if (Typeof(obj) == TYPE_THING && (FLAGS(obj) & VEHICLE)
            && limit-- > 0) {
            obj = thingHomeRef(obj);
            if (obj == NIL)
                obj = GLOBAL_ENVIRONMENT;
            if (obj != NOTHING && Typeof(obj) == TYPE_PLAYER)
                obj = playerHomeRef(obj);
        } else {
            obj = MUCK::getLocation(obj);
        }
    } while (obj != NOTHING && Typeof(obj) == TYPE_THING);
    if (!limit)
        return GLOBAL_ENVIRONMENT;
    return obj;
}

/* ================================================================ */
/* Typed creation                                                   */
/* ================================================================ */

static ObjectType moduleObjectType(Room *) { return ObjectType::Room; }
static ObjectType moduleObjectType(Thing *) { return ObjectType::Thing; }
static ObjectType moduleObjectType(Player *) { return ObjectType::Player; }
static ObjectType moduleObjectType(Exit *) { return ObjectType::Exit; }
static ObjectType moduleObjectType(MufProgram *) { return ObjectType::Program; }

/* Type payload initialization happens in the type modules' own
 * constructors now (NOTHING homes, zero pennies, empty destinations,
 * null password). Nothing is left to do here; the function remains as
 * the documented hook for any future construction-time setup. */
static void
typeInit(dbref r)
{
    (void) r;
}

template <class T>
T *
Database::Create(const char *name, dbref owner)
{
    /* An excluded type module cannot mint new objects. */
    if (ObjectStore::typeExcluded(T::staticName()))
        return nullptr;

    dbref r = newObject(owner);

    MUCK::setType(r, moduleObjectType((T *) nullptr));
    MUCK::setName(r, name);
    MUCK::setOwner(r, owner);
    typeInit(r);
    DBDIRTY(r);
    return get(r)->template As<T>();
}

template Room *Database::Create<Room>(const char *, dbref);
template Thing *Database::Create<Thing>(const char *, dbref);
template Player *Database::Create<Player>(const char *, dbref);
template Exit *Database::Create<Exit>(const char *, dbref);
template MufProgram *Database::Create<MufProgram>(const char *, dbref);

} /* namespace MUCK */
