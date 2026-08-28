#ifndef MUCK_MODULEREGISTRY_H
#define MUCK_MODULEREGISTRY_H

/* The feature-module registry: maps module names to factories so
 * objects loaded from disk can re-attach their modules, and so modules
 * can arrive from shared objects at runtime (docs/DATABASE.txt
 * sections 2 and 4).
 *
 * A shared-object module exports:
 *
 *     extern "C" void muck_module_register(void);
 *
 * and inside it calls MUCK::moduleRegistry().add("vehicle", []{
 * return std::make_unique<VehicleModule>(); }). Data belonging to a
 * module that is not currently registered stays dormant in the store,
 * so loading and unloading can never destroy anything.
 */

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace MUCK {

class Module;

class ModuleRegistry {
  public:
    typedef std::function<std::unique_ptr<Module>()> Factory;

    void add(const std::string &name, Factory f) { factories_[name] = f; }
    bool knows(const std::string &name) const {
        return factories_.count(name) != 0;
    }
    std::unique_ptr<Module> make(const std::string &name) const {
        auto it = factories_.find(name);
        return it == factories_.end() ? nullptr : it->second();
    }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto &p : factories_)
            out.push_back(p.first);
        return out;
    }

    /* dlopen a shared-object module and run its registration hook.
     * Returns false with the error in *err (if given). */
    bool loadShared(const char *path, std::string *err);

  private:
    std::map<std::string, Factory> factories_;
};

ModuleRegistry &moduleRegistry();

} /* namespace MUCK */

#endif /* MUCK_MODULEREGISTRY_H */
