#include <dlfcn.h>

#include "copyright.h"
#include "config.h"
#include "db.h"
#include "externs.h"
#include "ModuleRegistry.h"

namespace MUCK {

ModuleRegistry &
moduleRegistry()
{
    static ModuleRegistry instance;

    return instance;
}

bool
ModuleRegistry::loadShared(const char *path, std::string *err)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);

    if (!h) {
        if (err)
            *err = dlerror();
        return false;
    }

    void (*reg)(void) = (void (*)(void)) dlsym(h, "muck_module_register");

    if (!reg) {
        if (err)
            *err = "no muck_module_register symbol";
        dlclose(h);
        return false;
    }
    reg();
    log_status("MODULE: loaded %s\n", path);
    /* the handle is intentionally kept open for the process lifetime;
     * unloading code whose objects may be live is never safe. Data
     * durability on unload is the store's dormancy, across restarts. */
    return true;
}

} /* namespace MUCK */
