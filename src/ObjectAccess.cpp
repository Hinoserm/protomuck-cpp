/* Core field access. See ObjectAccess.h and docs/DATABASE.txt
 * sections 2 and 7: every setter here is a journal recording point,
 * and nothing outside this file touches a core field directly. */

#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "ObjectAccess.h"
#include "Modules.h"

#ifndef MALLOC_PROFILING
extern char *alloc_string(const char *);
#endif

namespace MUCK {

/* An invalid ref reads a default and writes nothing. Deleted shells
 * are still readable: stale holders inspect them on purpose. */
static inline struct object *
rec(dbref ref)
{
    if (ref < 0 || !database().valid(ref))
        return nullptr;
    return database().object(ref);
}

/* Every mutation funnels through here, so there is exactly one place
 * that marks an object changed. The journal (docs section 7) records
 * its entry here when it lands. */
static inline void
touched(dbref ref)
{
    struct object *o = rec(ref);

    if (o)
        o->flags |= OBJECT_CHANGED;
}

/* --- name ------------------------------------------------------- */

const char *
getName(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->name : nullptr;
}

void
setName(dbref ref, const char *name)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    /* a dead shell's name is the static "<garbage>" literal */
    if (o->name && (o->flags & TYPE_MASK) != TYPE_GARBAGE)
        delete[](char *) o->name;
    o->name = alloc_string(name);
    touched(ref);
}

/* --- links ------------------------------------------------------ */

dbref
getLocation(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->location : NOTHING;
}

void
setLocation(dbref ref, dbref loc)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->location = loc;
    touched(ref);
}

dbref
getOwner(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->owner : NOTHING;
}

void
setOwner(dbref ref, dbref owner)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->owner = owner;
    touched(ref);
}

/* --- flag words ------------------------------------------------- */

#define FLAGWORD(N, FIELD)                                              \
    object_flag_type getFlags##N(dbref ref)                             \
    {                                                                   \
        struct object *o = rec(ref);                                    \
        return o ? o->FIELD : 0;                                        \
    }                                                                   \
    void setFlags##N(dbref ref, object_flag_type v)                     \
    {                                                                   \
        struct object *o = rec(ref);                                    \
        if (!o)                                                         \
            return;                                                     \
        o->FIELD = v;                                                   \
        touched(ref);                                                   \
    }                                                                   \
    void addFlags##N(dbref ref, object_flag_type bits)                  \
    {                                                                   \
        struct object *o = rec(ref);                                    \
        if (!o)                                                         \
            return;                                                     \
        o->FIELD |= bits;                                               \
        touched(ref);                                                   \
    }                                                                   \
    void clearFlags##N(dbref ref, object_flag_type bits)                \
    {                                                                   \
        struct object *o = rec(ref);                                    \
        if (!o)                                                         \
            return;                                                     \
        o->FIELD &= ~bits;                                              \
        touched(ref);                                                   \
    }

object_flag_type
getFlags(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->flags : 0;
}

void
setFlags(dbref ref, object_flag_type v)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->flags = v;
    touched(ref);
}

void
addFlags(dbref ref, object_flag_type bits)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->flags |= bits;
    touched(ref);
}

void
clearFlags(dbref ref, object_flag_type bits)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->flags &= ~bits;
    touched(ref);
}

FLAGWORD(2, flag2)
FLAGWORD(3, flag3)
FLAGWORD(4, flag4)

#undef FLAGWORD

int
typeOf(dbref ref)
{
    if (ref == HOME)
        return TYPE_ROOM;
    return getFlags(ref) & TYPE_MASK;
}

int
rawTypeOf(dbref ref)
{
    return getFlags(ref) & TYPE_MASK;
}

/* --- powers ----------------------------------------------------- */

object_power_type
getPowers(dbref ref)
{
    return getOwnPowers(getOwner(ref));
}

object_power_type
getPowers2(dbref ref)
{
    return getOwnPowers2(getOwner(ref));
}

object_power_type
getOwnPowers(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->powers : 0;
}

object_power_type
getOwnPowers2(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->power2 : 0;
}

void
setOwnPowers(dbref ref, object_power_type v)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->powers = v;
    touched(ref);
}

void
setOwnPowers2(dbref ref, object_power_type v)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->power2 = v;
    touched(ref);
}

void
addOwnPowers(dbref ref, object_power_type bits)
{
    setOwnPowers(ref, getOwnPowers(ref) | bits);
}

void
clearOwnPowers(dbref ref, object_power_type bits)
{
    setOwnPowers(ref, getOwnPowers(ref) & ~bits);
}

void
addOwnPowers2(dbref ref, object_power_type bits)
{
    setOwnPowers2(ref, getOwnPowers2(ref) | bits);
}

void
clearOwnPowers2(dbref ref, object_power_type bits)
{
    setOwnPowers2(ref, getOwnPowers2(ref) & ~bits);
}

/* --- containment ------------------------------------------------ */

static std::vector<dbref>
refsOf(const std::vector<DbObject *> &v)
{
    std::vector<dbref> out;

    out.reserve(v.size());
    for (DbObject *o : v)
        if (o)
            out.push_back(o->ref());
    return out;
}

std::vector<dbref>
getContents(dbref ref)
{
    return refsOf(contentsOf(ref));
}

std::vector<dbref>
getExits(dbref ref)
{
    return refsOf(exitsOf(ref));
}

/* --- timestamps ------------------------------------------------- */

time_t
getCreated(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.created : 0;
}

time_t
getModified(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.modified : 0;
}

time_t
getLastUsed(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.lastused : 0;
}

int
getUseCount(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.usecount : 0;
}

dbref
getCreatedBy(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.dcreated : NOTHING;
}

dbref
getModifiedBy(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.dmodified : NOTHING;
}

dbref
getLastUsedBy(dbref ref)
{
    struct object *o = rec(ref);

    return o ? o->ts.dlastused : NOTHING;
}

void
setCreated(dbref ref, time_t when, dbref who)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->ts.created = when;
    o->ts.dcreated = who;
    touched(ref);
}

void
setModified(dbref ref, time_t when, dbref who)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->ts.modified = when;
    o->ts.dmodified = who;
    touched(ref);
}

void
setLastUsed(dbref ref, time_t when, dbref who)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->ts.lastused = when;
    o->ts.dlastused = who;
    touched(ref);
}

void
setUseCount(dbref ref, int n)
{
    struct object *o = rec(ref);

    if (!o)
        return;
    o->ts.usecount = n;
    touched(ref);
}

/* --- descriptions ----------------------------------------------- */

const char *
getDesc(dbref ref)
{
    return get_property_class(ref, "_/de");
}

void
setDesc(dbref ref, const char *text)
{
    add_property(ref, "_/de", text, 0);
}

} /* namespace MUCK */
