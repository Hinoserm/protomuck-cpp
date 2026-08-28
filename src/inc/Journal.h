#ifndef MUCK_JOURNAL_H
#define MUCK_JOURNAL_H

/* The per-object journal: base plus stacked layers.
 *
 * docs/DATABASE.txt section 7. An object's stored state is its base
 * (the .json on disk, never resident) plus a stack of layers, one per
 * era in which the object changed. The live in-memory object is the
 * cache: base with every layer applied. Readers never touch the
 * journal at all.
 *
 * A layer names the entries that changed during its era. Values are
 * materialized when the layer is SEALED, through the same serializer
 * that writes the base, which is what keeps the two encodings from
 * drifting: there is one definition of what an entry looks like.
 * Sealing therefore costs one serialization per entry that actually
 * changed, never per object and never per database.
 *
 * A key recorded ten thousand times in one era is one set member, so
 * a property ticked in a loop costs one entry. A write that does not
 * change the value records nothing at all: the setters compare first
 * and return early.
 *
 * Only the TOP layer is mutable, and only the game thread touches it.
 * Sealed layers are frozen forever, which is what lets the dump thread
 * write them with no locks and no copy of the world.
 */

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace MUCK {

typedef int dbref;

/* A sealed layer: one era's changes, with values already materialized.
 * Frozen; the dump thread owns it. */
struct SealedLayer {
    long era = 0;
    dbref ref = -1;
    /* The object's uuid as a string: the dump thread needs the file
     * path and must not reach into the live database to get it. */
    std::string uuid;
    /* entry key -> value, or null for a removal */
    nlohmann::json entries;
    /* Set once this layer has been written. A retry after a partial
     * failure skips what already landed; re-appending would duplicate
     * the record and replay it twice at load. */
    mutable bool landed = false;
    /* True when this is the object's whole state rather than a delta:
     * an object with no base file yet has nothing for a layer to sit
     * on, so its first persist writes the base. */
    bool full = false;
};

/* The mutable top layer: which entries changed this era. */
class JournalLayer {
  public:
    explicit JournalLayer(long era) : era_(era) {}

    long era() const { return era_; }
    bool empty() const { return keys_.empty(); }
    size_t size() const { return keys_.size(); }

    void touch(const std::string &key) { keys_.insert(key); }

    const std::set<std::string> &keys() const { return keys_; }

  private:
    long era_;
    /* ordered so a layer serializes deterministically */
    std::set<std::string> keys_;
};

/* The stack on one object. */
class Journal {
  public:
    /* The layer accepting writes, created on demand so an untouched
     * object carries no journal weight at all. */
    JournalLayer &top(long era) {
        if (!top_ || top_->era() != era)
            top_ = std::unique_ptr<JournalLayer>(new JournalLayer(era));
        return *top_;
    }

    bool hasUnsaved() const { return top_ && !top_->empty(); }
    size_t unsavedCount() const { return top_ ? top_->size() : 0; }

    JournalLayer *peek() { return top_.get(); }

    /* Drop the top layer; the object starts a fresh one on its next
     * write. Called once its contents have been materialized. */
    void discardTop() { top_.reset(); }

  private:
    std::unique_ptr<JournalLayer> top_;
};

/* ------------------------------------------------------------------ */
/* Recording. Every setter in the tree lands here, which is how a
 * change becomes persistent at all: an entry with no journal record is
 * never written. See docs/DATABASE.txt sections 2 and 7.
 * ------------------------------------------------------------------ */

/* Note that one entry on one object changed. */
void journalRecord(dbref ref, const char *key);
void journalRecord(dbref ref, const std::string &key);

/* Entry key for a property path (docs section 3: properties keep
 * their traditional slash paths). */
std::string propEntryKey(const char *path);

/* Note that a property changed, by path. */
void journalRecordProp(dbref ref, const char *path);

/* Note that a property AND everything under it changed. Removing a
 * propdir takes its whole subtree with it, and every one of those
 * entries exists separately in the base on disk, so each needs its own
 * removal record or the children come back at load. */
void journalRecordPropTree(dbref ref, const char *path);

/* True when the object has unsaved changes. This is what @stats
 * counts; it replaces the OBJECT_CHANGED dirty flag. */
bool hasUnsavedChanges(dbref ref);

/* The objects with something in their top layer. Firing walks this,
 * not the whole database, so a dump costs what changed rather than
 * what exists: the difference between nothing and seconds once a game
 * has millions of objects. */
const std::set<dbref> &dirtyObjects();
void forgetDirty(dbref ref);
void clearDirtyObjects();

} /* namespace MUCK */

#endif /* MUCK_JOURNAL_H */
