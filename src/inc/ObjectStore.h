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
#include <vector>

namespace MUCK {

class ObjectStore {
  public:
    /* True if path is a directory containing a manifest.json. */
    static bool isStore(const char *path);

    /* --db-exclude-type: deliberately boot without a type module.
     * Objects of an excluded (or unknown) type load as UNSUPPORTED
     * placeholders; their stored type name, type bits, and $type
     * entries persist verbatim. Returns false with *err set when the
     * name cannot be excluded (container types, until the storage
     * flip). docs/DATABASE.txt section 4. */
    static bool excludeType(const char *name, std::string *err);
    static bool typeExcluded(const std::string &name);

    /* --db-exclude-module: boot without a FEATURE module (properties
     * included). Its namespace entries ride dormant; the module's
     * API surface goes blanket-inert. docs/PROPERTIES.txt 5. */
    static bool excludeModule(const char *name, std::string *err);
    static bool moduleExcluded(const std::string &name);

    /* Stored type name of an UNSUPPORTED placeholder, empty string
     * for normal objects. */
    static std::string placeholderTypeName(dbref ref);

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

    /* --- versioning (docs/DATABASE.txt section 7) --- */
    struct Marker {
        long rev;
        long when;
        std::string label;
        bool locked;
    };

    long rev() const { return rev_; }

    /* Take a snapshot: bump the global rev by one and record a marker,
     * in the manifest (global) or in one object's file (scoped).
     * Returns the new rev, or -1 on error. */
    long snapshotGlobal(const char *label, bool locked);
    long snapshotObject(dbref i, const char *label, bool locked);

    /* Reconstruct one object's entries as of a revision (from its
     * current file plus its history) and restore the parts that are
     * safe to restore in a running world: name, properties, program
     * source. Containment and location are not rolled back. Returns
     * false if the object or revision cannot be resolved. */
    bool rollbackObject(dbref i, long rev);

    /* Marker listings for the @snapshot command. */
    const std::vector<Marker> &globalMarkers() const { return markers_; }
    std::vector<Marker> objectMarkers(dbref i) const;

    /* Offline maintenance (the -storegc flag): drop history entries no
     * retained marker can see and delete chunk files nothing
     * references. Walks the whole store; run it at maintenance time,
     * not from the dump path. Returns entries + chunks removed. */
    long gcStore();

  private:
    std::string objectPath(dbref i) const;
    std::string histPath(dbref i) const;
    bool writeManifest();

    std::string root_;
    long rev_ = 0;
    std::vector<Marker> markers_;    /* global; per-object live in files */
};

extern ObjectStore g_objectStore;

inline ObjectStore &store() { return g_objectStore; }

} /* namespace MUCK */

#endif /* MUCK_OBJECTSTORE_H */
