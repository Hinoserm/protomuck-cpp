#ifndef MUCK_DBOBJECT_H
#define MUCK_DBOBJECT_H

/* The modernized object model: one concrete DbObject with an exclusive
 * type module and attachable feature modules. See docs/DATABASE.txt
 * sections 2 and 3.
 *
 * TRANSITION NOTE (step 2 in progress): DbObject and the modules are
 * currently VIEWS over the legacy struct object storage, so new-style
 * call sites and legacy code can coexist while call sites migrate.
 * When the last direct user of the legacy struct is gone, storage
 * flips inside these classes and the struct dies. Code written against
 * this API does not change when that happens.
 */

#include <cstdio>
#include <string>
#include <vector>
#include <memory>

#include "Uuid.h"

namespace MUCK {

class DbObject;

/* --------------------------------------------------------------- */
/* Module: base of every attachable behavior.                      */
/* --------------------------------------------------------------- */

class Module {
  public:
    virtual ~Module() {}

    /* Namespace tag, unique per module class: "player", "muf_program",
     * "properties", "~vehicle". Type modules use their type name. */
    virtual const char *moduleName() const = 0;

    DbObject *object() const { return owner_; }

  protected:
    friend class DbObject;
    DbObject *owner_ = nullptr;
};

/* --------------------------------------------------------------- */
/* DbObject core: identity plus what every object has.             */
/* --------------------------------------------------------------- */

class DbObject {
  public:
    /* --- identity (immutable) --- */
    const Uuid &uuid() const;
    dbref ref() const { return ref_; }

    /* --- core fields --- */
    const char *name() const;
    void setName(const char *name);
    DbObject *location() const;
    DbObject *owner() const;
    void setOwner(DbObject *owner);

    bool isDeleted() const { return deleted_; }

    /* --- module access --- */
    /* Get<T> returns the attached module of that class, or null.
     * As<T> is the idiomatic type test at boundaries:
     *     if (Player *p = obj->As<Player>()) { ... }              */
    template <class T> T *Get() const {
        for (const auto &m : modules_)
            if (T *t = dynamic_cast<T *>(m.get()))
                return t;
        return nullptr;
    }
    template <class T> T *As() const { return Get<T>(); }

    Module *typeModule() const { return typeModule_; }
    const char *typeName() const;

    /* Attach a feature module (PROPERTIES is auto-attached). The
     * object takes ownership. */
    Module *attach(std::unique_ptr<Module> m);

  private:
    friend class Database;

    explicit DbObject(dbref ref) : ref_(ref) {}
    DbObject(const DbObject &) = delete;
    DbObject &operator=(const DbObject &) = delete;

    void setTypeModule(std::unique_ptr<Module> m);

    dbref ref_;
    bool deleted_ = false;
    Module *typeModule_ = nullptr;
    std::vector<std::unique_ptr<Module> > modules_;
};

} /* namespace MUCK */

#endif /* MUCK_DBOBJECT_H */
