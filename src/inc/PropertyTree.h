#ifndef MUCK_PROPERTYTREE_H
#define MUCK_PROPERTYTREE_H

/* One ordered index of property names: an Adaptive Radix Tree (Leis
 * et al., ICDE 2013) over case-folded keys. Every propdir owns one.
 * See docs/PROPERTIES.txt.
 *
 * ORDERING IS STRUCTURAL. Keys are folded byte-by-byte with
 * foldByte() below, then terminated with FOLD_TERM. Unsigned radix
 * order over folded keys is exactly the order of the legacy
 * string_compare comparator as it actually behaves on glibc,
 * including the 0xff byte aliasing EOF inside tolower and therefore
 * sorting before everything. No input byte folds to FOLD_TERM, so
 * every key is prefix-free.
 *
 * STABILITY. Leaves are individually heap-allocated PropNodes; the
 * internal radix nodes reorganize freely but a leaf address never
 * changes until that property is erased. The legacy PropPtr is a
 * PropNode pointer.
 */

#include <cstddef>
#include <cstdint>

namespace MUCK {

class PropNode;

class PropertyTree {
  public:
    PropertyTree() {}
    ~PropertyTree() { clear(); }
    PropertyTree(const PropertyTree &) = delete;
    PropertyTree &operator=(const PropertyTree &) = delete;

    /* Exact-name lookup (folded identity, so "Foo" finds "foo"). */
    PropNode *find(const char *name) const;

    /* Lookup with a pre-folded key (foldKey output). Lets a scan over
     * many objects fold its path once. docs/PROPERTIES.txt 3a. */
    PropNode *findFolded(const uint8_t *key, size_t len) const;

    /* Find-or-create. A new node preserves the given spelling; an
     * existing node keeps its original spelling. */
    PropNode *insert(const char *name);

    /* Remove and delete the node (and its subtree). False if the
     * name is absent. */
    bool erase(const char *name);

    /* Smallest key, or null when empty. */
    PropNode *first() const;

    /* Strict successor of a name in legacy order; the name itself
     * need not exist. Null at the end. */
    PropNode *nextAfter(const char *name) const;

    /* Ordered visit of every leaf in this tree (one level only). */
    void each(void (*fn)(PropNode *, void *), void *arg) const;

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    /* The node this directory hangs under, null at an object root.
     * Set by the path walker when it descends into a child tree. */
    PropNode *parentNode = nullptr;

    /* Delete every leaf (recursively freeing their subtrees). */
    void clear();

    /* The legacy comparator's byte mapping, measured empirically
     * against glibc's signed-char tolower (see PropertyTree.cpp for
     * the full map, including the 0xff/EOF quirk). A single table
     * load: the compatibility quirks cost nothing at runtime. */
    struct FoldTable {
        uint8_t t[256];
        FoldTable();
    };
    static const FoldTable foldTableInstance;
    static uint8_t foldByte(unsigned char c);
    static const uint8_t FOLD_TERM = 0x01; /* end of name; no input
                                            * byte folds to it */

    /* Fold name into out (must hold len+1 bytes); returns key length
     * including the terminator. */
    static size_t foldKey(const char *name, uint8_t *out, size_t max);

  private:
    void *root_ = nullptr;      /* tagged pointer: leaf or ArtNode */
    size_t count_ = 0;

    friend struct ArtOps;       /* the implementation lives in
                                 * PropertyTree.cpp */
};

} /* namespace MUCK */

#endif /* MUCK_PROPERTYTREE_H */
