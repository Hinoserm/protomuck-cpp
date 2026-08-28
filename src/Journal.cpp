/* Journal recording. See Journal.h and docs/DATABASE.txt section 7.
 *
 * This file only NOTES what changed. Materializing a layer's values,
 * and writing it, belong to the store, which owns the one definition
 * of what an entry looks like on disk. */

#include "copyright.h"
#include "config.h"

#include <nlohmann/json.hpp>

#include "db.h"
#include "Journal.h"
#include "DbObject.h"
#include "Database.h"
#include "ObjectStore.h"

namespace MUCK {

/* The era a change belongs to is the store's current revision: a
 * snapshot advances it, so everything written after a snapshot lands
 * in the next layer and a read at the marker excludes it. */
static inline long
currentEra()
{
    return store().rev();
}

void
journalRecord(dbref ref, const char *key)
{
    DbObject *o = database().get(ref);

    if (!o || !key)
        return;
    o->journal().top(currentEra()).touch(key);
}

void
journalRecord(dbref ref, const std::string &key)
{
    DbObject *o = database().get(ref);

    if (!o)
        return;
    o->journal().top(currentEra()).touch(key);
}

std::string
propEntryKey(const char *path)
{
    if (!path || !*path)
        return "/";
    return (*path == '/') ? std::string(path) : "/" + std::string(path);
}

void
journalRecordProp(dbref ref, const char *path)
{
    journalRecord(ref, propEntryKey(path));
}

bool
hasUnsavedChanges(dbref ref)
{
    DbObject *o = database().get(ref);

    return o && o->journal().hasUnsaved();
}

} /* namespace MUCK */
