#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "MacroTable.h"
#include "Modules.h"
#include "ProgramStore.h"
#include "strutils.h"

#ifndef MALLOC_PROFILING
extern char *alloc_string(const char *);
#endif

namespace MUCK {

Database g_database;

/* ================================================================ */
/* Identity                                                         */
/* ================================================================ */

static const Uuid nilUuid;

const Uuid &
Database::uuidOf(dbref ref) const
{
    DbObject *o = get(ref);

    return o ? o->uuid() : nilUuid;
}

dbref
Database::refOf(const Uuid &u) const
{
    DbObject *o = get(u);

    return o ? o->ref() : NOTHING;
}

DbObject *
Database::get(const Uuid &u) const
{
    if (u.isNil())
        return nullptr;

    std::shared_lock<std::shared_mutex> hold(indexMutex_);
    auto it = byUuid_.find(u);

    return it == byUuid_.end() ? nullptr : it->second;
}

void
Database::assignUuid(dbref ref, const Uuid &u)
{
    DbObject *o = get(ref);

    if (!o)
        return;

    std::unique_lock<std::shared_mutex> hold(indexMutex_);

    if (!o->uuid_.isNil())
        byUuid_.erase(o->uuid_);
    o->uuid_ = u;
    if (!u.isNil())
        byUuid_[u] = o;
}

dbref
Database::resolveUuidPrefix(const char *prefix) const
{
    std::shared_lock<std::shared_mutex> hold(indexMutex_);
    dbref found = NOTHING;

    for (const auto &pair : byUuid_) {
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

    bzero(o, sizeof(struct object));
    NAME(i) = 0;
    ts_newobject(player, o);
    o->location = NOTHING;
    o->contents = NOTHING;
    o->exits = NOTHING;
    o->next = NOTHING;
    o->properties = 0;

    /* flags and type-specific fields are the caller's to initialize */
}

dbref
Database::newObject(dbref player)
{
    dbref newobj;

    if (recyclable_ != NOTHING) {
        newobj = recyclable_;
        if (TYPEOF(newobj) != TYPE_GARBAGE) {
            log_status("DB FATAL ERROR! Attempted to reuse non-garbage object (%d)!\n", newobj);
            abort();
        }
        recyclable_ = DBFETCH(newobj)->next;
        freeObject(newobj);
    } else {
        newobj = top();
        ensureTop(newobj + 1);
    }

    clearObject(player, newobj);

    /* fresh identity, even on a recycled slot: a reused dbref is a NEW
     * object and must never inherit the old uuid */
    assignUuid(newobj, Uuid::generate());
    DBDIRTY(newobj);
    return newobj;
}

dbref
Database::newProgram(dbref player, const char *name)
{
    unsigned char mlvl;
    dbref newprog;
    char buf[BUFFER_LEN];

    newprog = newObject(player);
    player = OWNER(player);

    NAME(newprog) = alloc_string(name);
    sprintf(buf, "A scroll containing a spell called %s", name);
    SETDESC(newprog, buf);
    DBFETCH(newprog)->location = player;
    FLAGS(newprog) = TYPE_PROGRAM;

    mlvl = MLevel(player);
    if (mlvl < 1)
        mlvl = 2;
    else if (mlvl > 3)
        mlvl = 3;
    SetMLevel(newprog, mlvl);

    OWNER(newprog) = player;
    DBFETCH(newprog)->sp.program.first = 0;
    DBFETCH(newprog)->sp.program.curr_line = 0;
    DBFETCH(newprog)->sp.program.siz = 0;
    DBFETCH(newprog)->sp.program.code = 0;
    DBFETCH(newprog)->sp.program.start = 0;
    DBFETCH(newprog)->sp.program.pubs = 0;
    DBFETCH(newprog)->sp.program.fprofile = NULL;
    DBFETCH(newprog)->sp.program.proftime.tv_sec = 0;
    DBFETCH(newprog)->sp.program.proftime.tv_usec = 0;
    DBFETCH(newprog)->sp.program.profstart = 0;
    DBFETCH(newprog)->sp.program.profuses = 0;
    DBFETCH(newprog)->sp.program.instances = 0;
    PUSH(newprog, DBFETCH(player)->contents);
    DBDIRTY(newprog);
    DBDIRTY(player);

    return newprog;
}

void
Database::freeObject(dbref i)
{
    struct object *o;

    o = DBFETCH(i);
    if (NAME(i) && Typeof(i) != TYPE_GARBAGE)
        delete[]NAME(i);

    if (o->properties) {
        delete_proplist(o->properties);
    }

    if (Typeof(i) == TYPE_EXIT && o->sp.exit.dest) {
        delete[]o->sp.exit.dest;
    } else if (Typeof(i) == TYPE_PLAYER) {
        if (o->sp.player.password) {
            delete[]o->sp.player.password;
        }
        if (o->sp.player.descrs) {
            delete[]o->sp.player.descrs;
            o->sp.player.descrs = NULL;
            o->sp.player.descr_count = 0;
        }
    }
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
        byUuid_.clear();
        allocated_ = 0;
        top_.store(0, std::memory_order_release);
        recyclable_ = NOTHING;
    }

    clear_players();
    clear_primitives();
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
            obj = DBFETCH(obj)->sp.thing.home;
            if (obj == NIL)
                obj = GLOBAL_ENVIRONMENT;
            if (obj != NOTHING && Typeof(obj) == TYPE_PLAYER)
                obj = DBFETCH(obj)->sp.player.home;
        } else {
            obj = getloc(obj);
        }
    } while (obj != NOTHING && Typeof(obj) == TYPE_THING);
    if (!limit)
        return GLOBAL_ENVIRONMENT;
    return obj;
}

/* ================================================================ */
/* Typed creation                                                   */
/* ================================================================ */

static int moduleTypeBits(Room *) { return TYPE_ROOM; }
static int moduleTypeBits(Thing *) { return TYPE_THING; }
static int moduleTypeBits(Player *) { return TYPE_PLAYER; }
static int moduleTypeBits(Exit *) { return TYPE_EXIT; }
static int moduleTypeBits(MufProgram *) { return TYPE_PROGRAM; }

template <class T>
T *
Database::Create(const char *name, dbref owner)
{
    dbref r = newObject(owner);

    FLAGS(r) = moduleTypeBits((T *) nullptr);
    NAME(r) = alloc_string(name);
    OWNER(r) = owner;
    DBDIRTY(r);
    return get(r)->template As<T>();
}

template Room *Database::Create<Room>(const char *, dbref);
template Thing *Database::Create<Thing>(const char *, dbref);
template Player *Database::Create<Player>(const char *, dbref);
template Exit *Database::Create<Exit>(const char *, dbref);
template MufProgram *Database::Create<MufProgram>(const char *, dbref);

} /* namespace MUCK */
