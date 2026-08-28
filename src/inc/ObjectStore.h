#ifndef MUCK_OBJECTSTORE_H
#define MUCK_OBJECTSTORE_H

/* The sharded JSON object store: one file per object named by UUID,
 * plus a manifest at the data root. This is the only write path for
 * the database; the old flat-file format is import-only through
 * FlatFileConverter.
 *
 * Layout:
 *     <root>/manifest.json
 *     <root>/objects/aa/bb/<uuid>.json
 *
 * See docs/DATABASE.txt sections 6 and the API notes appended there.
 */

#include <cstdio>
#include <string>

namespace MUCK {

class ObjectStore {
  public:
    /* True if path is a directory containing a manifest.json. */
    static bool isStore(const char *path);

    /* Bind the store to its data root. Creates the directory tree on
     * first save if needed. */
    void setRoot(const char *path) { root_ = path; }
    const std::string &root() const { return root_; }
    bool active() const { return !root_.empty(); }

    /* Write every object plus the manifest. When dirtyOnly is true,
     * objects without OBJECT_CHANGED are skipped (their files are
     * already current). Returns the number of objects written, or -1
     * on a filesystem error. */
    int saveAll(bool dirtyOnly);

    /* Load an entire store into the database. Returns the resulting
     * top dbref, or -1 on error. */
    dbref loadAll();

    /* Write or rewrite one object's file. */
    bool saveObject(dbref i);

    /* Remove one object's file (deletion support; unused until the
     * garbage type dies in step 2). */
    bool removeObject(dbref i);

  private:
    std::string objectPath(dbref i) const;
    bool writeManifest();

    std::string root_;
};

extern ObjectStore g_objectStore;

inline ObjectStore &store() { return g_objectStore; }

} /* namespace MUCK */

#endif /* MUCK_OBJECTSTORE_H */
