/* Journal recording. See Journal.h and docs/DATABASE.txt section 7.
 *
 * This file only NOTES what changed. Materializing a layer's values,
 * and writing it, belong to the store, which owns the one definition
 * of what an entry looks like on disk. */

#include "copyright.h"
#include "config.h"

#include <set>

#include <nlohmann/json.hpp>

#include "db.h"
#include "Journal.h"
#include "DbObject.h"
#include "Database.h"
#include "ObjectStore.h"
#include "props.h"
#include "PropertyTree.h"
#include "PropNode.h"
#include "Modules.h"

namespace MUCK {

/* The era a change belongs to is the store's current revision: a
 * snapshot advances it, so everything written after a snapshot lands
 * in the next layer and a read at the marker excludes it. */
static inline long
currentEra()
{
    return store().rev();
}

/* Every object with a non-empty top layer. */
static std::set<dbref> g_dirty;

/* Load-mode suppression: the loader's setters would journal thirteen
 * million entries only for the end-of-load pass to discard them all,
 * and the dirty index is shared state the parallel phase-two workers
 * must not race on. What was just read from disk is not a change. */
static bool g_suppress = false;

void
journalSuppress(bool on)
{
    g_suppress = on;
}

const std::set<dbref> &
dirtyObjects()
{
    return g_dirty;
}

void
forgetDirty(dbref ref)
{
    g_dirty.erase(ref);
}

void
clearDirtyObjects()
{
    g_dirty.clear();
}

void
journalRecord(dbref ref, const char *key)
{
    if (g_suppress)
        return;

    DbObject *o = database().get(ref);

    if (!o || !key)
        return;
    o->journal().top(currentEra()).touch(key);
    g_dirty.insert(ref);
}

void
journalRecord(dbref ref, const std::string &key)
{
    if (g_suppress)
        return;

    DbObject *o = database().get(ref);

    if (!o)
        return;
    o->journal().top(currentEra()).touch(key);
    g_dirty.insert(ref);
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

/* Walk the live prop tree under path and record every node, so a
 * subtree removal leaves no child behind in the base. */
static void
recordSubtree(dbref ref, const std::string &prefix, PropertyTree *dir)
{
    if (!dir)
        return;

    for (PropNode *p = dir->first(); p; p = dir->nextAfter(p->name())) {
        std::string child = prefix + p->name();

        journalRecord(ref, propEntryKey(child.c_str()));
        if (p->isDir())
            recordSubtree(ref, child + "/", &p->children());
    }
}

void
journalRecordPropTree(dbref ref, const char *path)
{
    /* An empty path means the whole tree: used when a rollback wipes
     * an object's properties before rebuilding them. */
    if (!path || !*path) {
        DbObject *o = database().get(ref);
        Properties *pp = o ? o->propsModule() : nullptr;

        if (pp)
            recordSubtree(ref, "", &pp->root());
        return;
    }

    journalRecordProp(ref, path);

    PropPtr p = get_property(ref, path);

    if (p && p->isDir()) {
        std::string prefix = propEntryKey(path);

        if (prefix.size() && prefix[prefix.size() - 1] != '/')
            prefix += '/';
        recordSubtree(ref, prefix.substr(1), &p->children());
    }
}

bool
hasUnsavedChanges(dbref ref)
{
    DbObject *o = database().get(ref);

    return o && o->journal().hasUnsaved();
}

} /* namespace MUCK */
