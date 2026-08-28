#ifndef MUCK_DBOBJECT_H
#define MUCK_DBOBJECT_H

/* The object: one concrete DbObject with an exclusive type module and
 * attachable feature modules. See docs/DATABASE.txt sections 2 and 3.
 *
 * STORAGE: each DbObject is individually heap-allocated and owns its
 * data for the life of the process; the database's indexes hold
 * pointers, never copies, so a DbObject pointer is stable forever.
 * During the step 2 migration the persistent payload is still the
 * legacy struct object, embedded here as a member; module accessors
 * are views over it. When the last direct user of the struct is gone,
 * its fields dissolve into the modules and the struct dies. Code
 * written against this API does not change when that happens.
 *
 * THREADING: mutate an object's payload from concurrent contexts only
 * under its lock (see lockExclusive / lockShared below; locks are
 * striped by ref inside the Database, so 20M objects do not carry 20M
 * mutexes). The main server loop is single-threaded today and is
 * exempt by convention, but all NEW threaded code must take the lock.
 */

#include <cstdio>
#include <string>
#include <vector>
#include <memory>

#include <nlohmann/json_fwd.hpp>

#include "UUID.h"
#include "ObjectType.h"
#include "Journal.h"

namespace MUCK {

class DbObject;
class Database;
class Properties;

/* --------------------------------------------------------------- */
/* Module: base of every attachable behavior.                      */
/* --------------------------------------------------------------- */

class Module {
  public:
    virtual ~Module() {}

    /* Namespace tag, unique per module class: "player", "muf_program",
     * "properties", "~vehicle". Type modules use their type name. */
    virtual const char *moduleName() const = 0;

    /* The value-model persist contract (docs section 3): a feature
     * module writes its state as entries in its own namespace and
     * rebuilds from its slice at load. Type modules and Properties
     * serialize through dedicated paths and leave these alone. */
    virtual void saveEntries(nlohmann::json &entries) const { (void) entries; }
    virtual void loadEntries(const nlohmann::json &entries) { (void) entries; }

    DbObject *object() const { return owner_; }

  protected:
    friend class DbObject;
    DbObject *owner_ = nullptr;
};

/* --------------------------------------------------------------- */
/* DbObject                                                        */
/* --------------------------------------------------------------- */

class DbObject {
  public:
    /* --- identity (immutable once assigned) --- */
    const UUID &uuid() const { return uuid_; }
    dbref ref() const { return ref_; }

    /* --- type: a real field, not bits in the flags word --- */
    /* Reads never go through the ref lookup, so this is the cheap form;
     * MUCK::typeOf(ref) is the sentinel-aware one (it knows HOME). */
    ObjectType type() const { return type_; }
    void setType(ObjectType t);

    /* --- core fields (views over the legacy payload for now) --- */
    const char *name() const;
    void setName(const char *name);
    DbObject *location() const;
    DbObject *owner() const;
    void setOwner(DbObject *owner);

    bool isDeleted() const { return deleted_; }

    /* --- permissions --- */
    /* Effective MUF/wizard level from the W-bits, folded by the
     * multi_wizlevels tune. The MLevel macro family resolves here. */
    int muckerLevel() const;
    /* Wizard level: muckerLevel at LMAGE or above, else 0. */
    int wizLevel() const;

    /* --- containment (owning storage; every object has the lists,
     * most stay empty). Order is MUF-visible: front of the vector is
     * the head of the old chain, arrivals prepend. --- */
    std::vector<DbObject *> &contentsVec() { return contents_; }
    std::vector<DbObject *> &exitsVec() { return exits_; }

    /* Cached index of this object inside its container's list: a walk
     * accelerator only. nextSiblingRef validates it before trusting
     * it, so stale hints cost one rescan, never a wrong answer. Keeps
     * classic head/next walks (the MUF CONTENTS...NEXT idiom) at the
     * old chain cost of O(1) per step. */
    size_t listHint = 0;

    /* --- module access --- */
    /* As<T> is the idiomatic type test at boundaries:
     *     if (Player *p = obj->As<Player>()) { ... }
     * Modules attach lazily from the type bits and re-attach if legacy
     * code changes the object's type under us (newProgram, recycling);
     * that rebuild vanishes with the legacy struct. */
    template <class T> T *Get() {
        ensureModules();
        for (const auto &m : modules_)
            if (T *t = dynamic_cast<T *>(m.get()))
                return t;
        return nullptr;
    }
    template <class T> T *As() { return Get<T>(); }

    Module *typeModule() { ensureModules(); return typeModule_; }
    const char *typeName();

    /* Attach a feature module; the object takes ownership. */
    Module *attach(std::unique_ptr<Module> m);

    /* Cached Properties module, set at attach: property access is the
     * hottest module lookup and skips the dynamic_cast scan. */
    Properties *propsModule() { ensureModules(); return propsCache_; }

    /* Iterate attached modules (type module included). */
    template <class F> void eachModule(F f) {
        ensureModules();
        for (const auto &m : modules_)
            f(m.get());
    }

    /* --- journal (docs/DATABASE.txt section 7) --- */
    /* The object's stack of layers. Only the top one is mutable and
     * only the game thread touches it; sealed layers are frozen, which
     * is what lets the dump thread write with no locks. */
    Journal &journal() { return journal_; }

    /* --- object-level locking (striped; see Database) --- */
    void lockShared() const;
    void unlockShared() const;
    void lockExclusive() const;
    void unlockExclusive() const;

    class SharedLock {
      public:
        explicit SharedLock(const DbObject *o) : o_(o) { o_->lockShared(); }
        ~SharedLock() { o_->unlockShared(); }
      private:
        const DbObject *o_;
    };
    class ExclusiveLock {
      public:
        explicit ExclusiveLock(DbObject *o) : o_(o) { o_->lockExclusive(); }
        ~ExclusiveLock() { o_->unlockExclusive(); }
      private:
        DbObject *o_;
    };

    /* --- legacy payload (TRANSITIONAL: dies with step 2) --- */
    /* The embedded struct object. DBFETCH resolves here; nothing new
     * should touch it directly, use the modules. */
    struct object *legacyData() { return &legacy_; }

  private:
    friend class Database;

    explicit DbObject(dbref ref);
    DbObject(const DbObject &) = delete;
    DbObject &operator=(const DbObject &) = delete;

    void ensureModules();
    void rebuildModules();

    dbref ref_;
    UUID uuid_;
    /* The object's type: a real field, not bits inside the flags word.
     * Reached through MUCK::typeOf/setType (ObjectAccess.h). */
    ObjectType type_ = ObjectType::Garbage;
    std::vector<DbObject *> contents_;
    std::vector<DbObject *> exits_;
    bool deleted_ = false;
    /* the type the attached modules were built for; Invalid forces a
     * rebuild on first access */
    ObjectType moduleType_ = ObjectType::Invalid;
    Module *typeModule_ = nullptr;
    Properties *propsCache_ = nullptr;
    std::vector<std::unique_ptr<Module> > modules_;

    Journal journal_;
    struct object legacy_;      /* TRANSITIONAL payload */
};

} /* namespace MUCK */

#endif /* MUCK_DBOBJECT_H */
