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
 * A layer maps an entry key to the value it took during that era, or
 * to a removal mark. Latest value per key wins, so a property ticked
 * ten thousand times in one era occupies one slot.
 *
 * Only the TOP layer is mutable, and only the game thread touches it.
 * Sealed layers are frozen forever, which is what lets the dump thread
 * write them with no locks and no copy of the world.
 */

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace MUCK {

/* One recorded change. A removal carries no value. */
struct JournalEntry {
    nlohmann::json value;
    bool removed = false;
};

/* Every change an object saw during one era. */
class JournalLayer {
  public:
    explicit JournalLayer(long era) : era_(era) {}

    long era() const { return era_; }
    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }

    /* Record the latest value for a key; overwrites within the era. */
    void set(const std::string &key, nlohmann::json value) {
        JournalEntry &e = entries_[key];
        e.value = std::move(value);
        e.removed = false;
    }

    void remove(const std::string &key) {
        JournalEntry &e = entries_[key];
        e.value = nullptr;
        e.removed = true;
    }

    const std::unordered_map<std::string, JournalEntry> &entries() const {
        return entries_;
    }

    /* Serialized form for the .hist sidecar: one JSON object holding
     * the era and its keys. */
    nlohmann::json toJson() const;
    static JournalLayer fromJson(const nlohmann::json &j);

  private:
    long era_;
    std::unordered_map<std::string, JournalEntry> entries_;
};

/* The stack on one object. */
class Journal {
  public:
    /* The layer currently accepting writes; created on demand so an
     * untouched object carries no journal weight at all. */
    JournalLayer &top(long era) {
        if (!top_ || top_->era() != era)
            top_ = std::make_unique<JournalLayer>(era);
        return *top_;
    }

    bool hasUnsaved() const { return top_ && !top_->empty(); }
    size_t unsavedCount() const { return top_ ? top_->size() : 0; }

    /* Freeze the top layer and hand it over; the object starts a fresh
     * one on its next write. Null when there was nothing to seal. */
    std::unique_ptr<JournalLayer> seal() {
        if (!top_ || top_->empty())
            return nullptr;
        return std::move(top_);
    }

    /* Layers sealed but not yet written, oldest first. The dump thread
     * owns these once handed over; they are frozen. */
    const std::vector<std::shared_ptr<const JournalLayer>> &pending() const {
        return pending_;
    }
    void addPending(std::shared_ptr<const JournalLayer> layer) {
        pending_.push_back(std::move(layer));
    }
    void clearPending() { pending_.clear(); }

  private:
    std::unique_ptr<JournalLayer> top_;
    std::vector<std::shared_ptr<const JournalLayer>> pending_;
};

} /* namespace MUCK */

#endif /* MUCK_JOURNAL_H */
