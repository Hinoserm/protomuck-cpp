#ifndef MUCK_PROPNODE_H
#define MUCK_PROPNODE_H

/* One property: the leaf of a PropertyTree and the object behind the
 * legacy PropPtr handle. Owns its value, its flags, and its child
 * tree (a property can carry a value and children at once). See
 * docs/PROPERTIES.txt.
 *
 * The flags word keeps the exact legacy encoding from props.h: the
 * low bits are the type (PROP_DIRTYP and friends), the rest are the
 * permission and bookkeeping bits. It is a public member so the
 * legacy flag macros keep working verbatim.
 *
 * RESIDENCY: metadata (name, flags, children) is always resident.
 * The value can be RESIDENT or UNLOADED; every accessor calls
 * ensureResident(), which is a no-op today and becomes the on-demand
 * store fetch when the optional lazy-value mode arrives. No code may
 * read a value except through these accessors. */

#include <string>

#include "PropertyTree.h"

struct boolexp;

namespace MUCK {

class PropNode {
  public:
    explicit PropNode(const char *name) : name_(name ? name : "") {}
    ~PropNode();
    PropNode(const PropNode &) = delete;
    PropNode &operator=(const PropNode &) = delete;

    const char *name() const { return name_.c_str(); }

    /* legacy flags word: type in the low bits, perms above */
    unsigned short flags = 0;

    int type() const;           /* PROP_*TYP from the flags word */
    void setType(int t);

    /* --- value accessors (residency-aware) --- */
    const char *strValue();     /* null when not a string */
    int intValue();
    double fltValue();
    int refValue();             /* dbref */
    struct boolexp *lokValue();

    void setStr(const char *s); /* copies; sets type */
    void setInt(int v);
    void setFlt(double v);
    void setRef(int r);
    void setLok(struct boolexp *l); /* takes ownership */

    /* Drop the value (frees a lock), leaving type DIRTYP. */
    void clearValue();

    /* --- children --- */
    PropertyTree &children() { return children_; }
    bool isDir() const { return !children_.empty(); }

    PropNode *parent = nullptr; /* enclosing dir's node, or null at
                                 * an object's root level */

    /* --- residency (docs/PROPERTIES.txt section 4) --- */
    enum Residency : unsigned char { RESIDENT = 0, UNLOADED = 1 };
    Residency residency = RESIDENT;

  private:
    void ensureResident();

    std::string name_;          /* original spelling, case preserved */
    std::string sval_;
    union {
        int ival_;
        double fval_;
        int ref_;
        struct boolexp *lok_;
    };
    PropertyTree children_;
};

} /* namespace MUCK */

#endif /* MUCK_PROPNODE_H */
