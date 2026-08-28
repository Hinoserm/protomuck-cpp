/* The Adaptive Radix Tree behind every propdir. Self-contained: no
 * muck headers, no dependencies beyond the standard library. See
 * docs/PROPERTIES.txt for the design and the ordering proof.
 *
 * Implementation notes:
 *  - Child pointers are tagged: low bit set means the pointer is a
 *    PropNode leaf, clear means an internal ArtNode.
 *  - Hybrid path compression per the ART paper: an internal node
 *    records the full compressed-prefix LENGTH but stores only the
 *    first MAX_PREFIX bytes; when more is needed (deep splits,
 *    ordering against long prefixes) the bytes are recovered from
 *    any leaf in the subtree, whose folded key contains them.
 *  - Keys are produced by foldKey and are prefix-free (FOLD_TERM
 *    appears exactly once, at the end).
 */

#include <cctype>
#include <cstring>
#include <string>

#include "inc/PropertyTree.h"
#include "inc/PropNode.h"

namespace MUCK {

/* ------------------------------------------------------------------ */
/* Key folding                                                        */
/* ------------------------------------------------------------------ */

uint8_t
PropertyTree::foldByte(unsigned char c)
{
    /* The legacy comparator runs tolower on the SIGNED char. Measured
     * on glibc: the negative aliases of 0x80..0xfe map to their
     * unsigned selves (so high latin-1 sorts AFTER ascii), but 0xff
     * is signed -1, which is EOF, and tolower(EOF) returns -1, so a
     * 0xff byte sorts before EVERYTHING, the end of the name
     * included. Bug-compatible by mandate (docs/PROPERTIES.txt).
     *
     * Fold map, order-isomorphic to that arithmetic:
     *   0xff        -> 0x00                (the EOF alias, first)
     *   (terminator -> 0x01, see FOLD_TERM; no input produces it)
     *   0x01..0x7f  -> ascii-tolower + 1   (0x02..0x80)
     *   0x80..0xfe  -> value + 1           (0x81..0xff)
     */
    return foldTableInstance.t[c];
}

PropertyTree::FoldTable::FoldTable()
{
    for (int c = 0; c < 256; c++) {
        if (c == 0xff)
            t[c] = 0x00;
        else if (c >= 0x80)
            t[c] = (uint8_t) (c + 1);
        else if (c >= 'A' && c <= 'Z')
            t[c] = (uint8_t) (c + 32 + 1);
        else
            t[c] = (uint8_t) (c + 1);
    }
}

const PropertyTree::FoldTable PropertyTree::foldTableInstance;

size_t
PropertyTree::foldKey(const char *name, uint8_t *out, size_t max)
{
    size_t n = 0;

    if (name)
        for (const unsigned char *p = (const unsigned char *) name;
             *p && n + 1 < max; p++)
            out[n++] = foldByte(*p);
    out[n++] = FOLD_TERM;
    return n;
}

/* A folded key with small-buffer optimization. */
struct FoldedKey {
    uint8_t small[256];
    std::string big;
    const uint8_t *data;
    size_t len;

    explicit FoldedKey(const char *name)
    {
        size_t need = (name ? strlen(name) : 0) + 1;

        if (need <= sizeof(small)) {
            len = PropertyTree::foldKey(name, small, sizeof(small));
            data = small;
        } else {
            big.resize(need);
            len = PropertyTree::foldKey(name, (uint8_t *) &big[0], need);
            data = (const uint8_t *) big.data();
        }
    }
};

/* ------------------------------------------------------------------ */
/* Node representations                                               */
/* ------------------------------------------------------------------ */

static const int MAX_PREFIX = 10;

enum { N4, N16, N48, N256 };

struct ArtNode {
    uint8_t type;
    uint16_t num;               /* child count */
    uint32_t prefixLen;         /* FULL compressed-prefix length */
    uint8_t prefix[MAX_PREFIX]; /* first bytes of it */
};

struct Node4 : ArtNode {
    uint8_t keys[4];
    void *child[4];
};
struct Node16 : ArtNode {
    uint8_t keys[16];
    void *child[16];
};
struct Node48 : ArtNode {
    uint8_t idx[256];           /* 0 = empty, else slot + 1 */
    void *child[48];
};
struct Node256 : ArtNode {
    void *child[256];
};

static inline bool
isLeaf(void *p)
{
    return ((uintptr_t) p & 1) != 0;
}

static inline PropNode *
asLeaf(void *p)
{
    return (PropNode *) ((uintptr_t) p & ~(uintptr_t) 1);
}

static inline void *
tagLeaf(PropNode *l)
{
    return (void *) ((uintptr_t) l | 1);
}

static inline ArtNode *
asNode(void *p)
{
    return (ArtNode *) p;
}

static ArtNode *
makeNode(int type)
{
    ArtNode *n;

    switch (type) {
        case N4:   n = new Node4();   break;
        case N16:  n = new Node16();  break;
        case N48:  n = new Node48();  break;
        default:   n = new Node256(); break;
    }
    n->type = (uint8_t) type;
    n->num = 0;
    n->prefixLen = 0;
    return n;
}

static void
freeNodeOnly(ArtNode *n)
{
    switch (n->type) {
        case N4:   delete (Node4 *) n;   break;
        case N16:  delete (Node16 *) n;  break;
        case N48:  delete (Node48 *) n;  break;
        default:   delete (Node256 *) n; break;
    }
}

/* ------------------------------------------------------------------ */
/* Child access                                                       */
/* ------------------------------------------------------------------ */

static void **
findChild(ArtNode *n, uint8_t b)
{
    switch (n->type) {
        case N4: {
            Node4 *p = (Node4 *) n;

            for (int i = 0; i < n->num; i++)
                if (p->keys[i] == b)
                    return &p->child[i];
            return nullptr;
        }
        case N16: {
            Node16 *p = (Node16 *) n;

            for (int i = 0; i < n->num; i++)
                if (p->keys[i] == b)
                    return &p->child[i];
            return nullptr;
        }
        case N48: {
            Node48 *p = (Node48 *) n;

            if (p->idx[b])
                return &p->child[p->idx[b] - 1];
            return nullptr;
        }
        default: {
            Node256 *p = (Node256 *) n;

            if (p->child[b])
                return &p->child[b];
            return nullptr;
        }
    }
}

/* smallest child slot, with its key byte */
static void *
minChild(ArtNode *n)
{
    switch (n->type) {
        case N4: {
            Node4 *p = (Node4 *) n;
            int best = 0;

            for (int i = 1; i < n->num; i++)
                if (p->keys[i] < p->keys[best])
                    best = i;
            return p->child[best];
        }
        case N16: {
            Node16 *p = (Node16 *) n;
            int best = 0;

            for (int i = 1; i < n->num; i++)
                if (p->keys[i] < p->keys[best])
                    best = i;
            return p->child[best];
        }
        case N48: {
            Node48 *p = (Node48 *) n;

            for (int b = 0; b < 256; b++)
                if (p->idx[b])
                    return p->child[p->idx[b] - 1];
            return nullptr;
        }
        default: {
            Node256 *p = (Node256 *) n;

            for (int b = 0; b < 256; b++)
                if (p->child[b])
                    return p->child[b];
            return nullptr;
        }
    }
}

/* smallest child with key byte strictly greater than b, or null */
static void *
nextGreaterChild(ArtNode *n, int b)
{
    void *best = nullptr;
    int bestKey = 256;

    switch (n->type) {
        case N4: {
            Node4 *p = (Node4 *) n;

            for (int i = 0; i < n->num; i++)
                if (p->keys[i] > b && p->keys[i] < bestKey) {
                    bestKey = p->keys[i];
                    best = p->child[i];
                }
            return best;
        }
        case N16: {
            Node16 *p = (Node16 *) n;

            for (int i = 0; i < n->num; i++)
                if (p->keys[i] > b && p->keys[i] < bestKey) {
                    bestKey = p->keys[i];
                    best = p->child[i];
                }
            return best;
        }
        case N48: {
            Node48 *p = (Node48 *) n;

            for (int k = b + 1; k < 256; k++)
                if (p->idx[k])
                    return p->child[p->idx[k] - 1];
            return nullptr;
        }
        default: {
            Node256 *p = (Node256 *) n;

            for (int k = b + 1; k < 256; k++)
                if (p->child[k])
                    return p->child[k];
            return nullptr;
        }
    }
}

/* leftmost leaf of a subtree */
static PropNode *
minLeaf(void *p)
{
    while (p && !isLeaf(p))
        p = minChild(asNode(p));
    return p ? asLeaf(p) : nullptr;
}

/* the byte of the compressed prefix at position i, recovering bytes
 * beyond MAX_PREFIX from a representative leaf. depth is where this
 * node's prefix begins in the full key. */
static uint8_t
prefixByte(ArtNode *n, uint32_t i, uint32_t depth)
{
    if (i < (uint32_t) MAX_PREFIX)
        return n->prefix[i];

    PropNode *repr = minLeaf(minChild(n));
    FoldedKey rk(repr->name());

    return rk.data[depth + i];
}

/* ------------------------------------------------------------------ */
/* Insertion machinery                                                */
/* ------------------------------------------------------------------ */

static void
addChild(void **slot, ArtNode *n, uint8_t b, void *child);

static void
growInsert(void **slot, ArtNode *n, uint8_t b, void *child)
{
    if (n->type == N4) {
        Node4 *o = (Node4 *) n;
        Node16 *nw = (Node16 *) makeNode(N16);

        *(ArtNode *) nw = *(ArtNode *) o;
        nw->type = N16;
        for (int i = 0; i < o->num; i++) {
            nw->keys[i] = o->keys[i];
            nw->child[i] = o->child[i];
        }
        delete o;
        *slot = nw;
        addChild(slot, nw, b, child);
    } else if (n->type == N16) {
        Node16 *o = (Node16 *) n;
        Node48 *nw = (Node48 *) makeNode(N48);

        *(ArtNode *) nw = *(ArtNode *) o;
        nw->type = N48;
        for (int i = 0; i < o->num; i++) {
            nw->child[i] = o->child[i];
            nw->idx[o->keys[i]] = (uint8_t) (i + 1);
        }
        delete o;
        *slot = nw;
        addChild(slot, nw, b, child);
    } else {
        Node48 *o = (Node48 *) n;
        Node256 *nw = (Node256 *) makeNode(N256);

        *(ArtNode *) nw = *(ArtNode *) o;
        nw->type = N256;
        for (int k = 0; k < 256; k++)
            if (o->idx[k])
                nw->child[k] = o->child[o->idx[k] - 1];
        delete o;
        *slot = nw;
        addChild(slot, nw, b, child);
    }
}

static void
addChild(void **slot, ArtNode *n, uint8_t b, void *child)
{
    switch (n->type) {
        case N4: {
            Node4 *p = (Node4 *) n;

            if (n->num < 4) {
                p->keys[n->num] = b;
                p->child[n->num] = child;
                n->num++;
            } else {
                growInsert(slot, n, b, child);
            }
            break;
        }
        case N16: {
            Node16 *p = (Node16 *) n;

            if (n->num < 16) {
                p->keys[n->num] = b;
                p->child[n->num] = child;
                n->num++;
            } else {
                growInsert(slot, n, b, child);
            }
            break;
        }
        case N48: {
            Node48 *p = (Node48 *) n;

            if (n->num < 48) {
                int s = 0;

                while (p->child[s])
                    s++;
                p->child[s] = child;
                p->idx[b] = (uint8_t) (s + 1);
                n->num++;
            } else {
                growInsert(slot, n, b, child);
            }
            break;
        }
        default: {
            Node256 *p = (Node256 *) n;

            p->child[b] = child;
            n->num++;
            break;
        }
    }
}

/* length of the match between the node's compressed prefix and
 * key[depth..], up to prefixLen */
static uint32_t
prefixMatch(ArtNode *n, const uint8_t *key, size_t len, uint32_t depth)
{
    uint32_t i;

    for (i = 0; i < n->prefixLen; i++) {
        if (depth + i >= len)
            return i;
        if (prefixByte(n, i, depth) != key[depth + i])
            return i;
    }
    return i;
}

struct ArtOps {
    static PropNode *
    insertRec(void **slot, const uint8_t *key, size_t len, uint32_t depth,
              const char *name, size_t *count)
    {
        void *p = *slot;

        if (!p) {
            PropNode *leaf = new PropNode(name);

            *slot = tagLeaf(leaf);
            (*count)++;
            return leaf;
        }

        if (isLeaf(p)) {
            PropNode *leaf = asLeaf(p);
            FoldedKey lk(leaf->name());

            if (lk.len == len && memcmp(lk.data, key, len) == 0)
                return leaf; /* same identity; spelling preserved */

            /* split: common prefix from depth, then two leaves */
            uint32_t common = 0;

            while (depth + common < len && depth + common < lk.len
                   && key[depth + common] == lk.data[depth + common])
                common++;

            Node4 *nw = (Node4 *) makeNode(N4);

            nw->prefixLen = common;
            for (uint32_t i = 0; i < common && i < (uint32_t) MAX_PREFIX; i++)
                nw->prefix[i] = key[depth + i];

            PropNode *fresh = new PropNode(name);

            addChild((void **) &nw, nw, lk.data[depth + common], p);
            addChild((void **) &nw, nw, key[depth + common], tagLeaf(fresh));
            *slot = nw;
            (*count)++;
            return fresh;
        }

        ArtNode *n = asNode(p);
        uint32_t match = prefixMatch(n, key, len, depth);

        if (match < n->prefixLen) {
            /* split the prefix */
            Node4 *nw = (Node4 *) makeNode(N4);

            nw->prefixLen = match;
            for (uint32_t i = 0; i < match && i < (uint32_t) MAX_PREFIX; i++)
                nw->prefix[i] = prefixByte(n, i, depth);

            uint8_t oldByte = prefixByte(n, match, depth);

            /* advance the old node's prefix past match+1 */
            {
                uint32_t newLen = n->prefixLen - match - 1;
                uint8_t tmp[MAX_PREFIX];

                for (uint32_t i = 0;
                     i < newLen && i < (uint32_t) MAX_PREFIX; i++)
                    tmp[i] = prefixByte(n, match + 1 + i, depth);
                n->prefixLen = newLen;
                memcpy(n->prefix, tmp, MAX_PREFIX);
            }

            PropNode *fresh = new PropNode(name);

            addChild((void **) &nw, nw, oldByte, p);
            addChild((void **) &nw, nw, key[depth + match], tagLeaf(fresh));
            *slot = nw;
            (*count)++;
            return fresh;
        }

        depth += n->prefixLen;

        uint8_t b = key[depth];
        void **childSlot = findChild(n, b);

        if (childSlot)
            return insertRec(childSlot, key, len, depth + 1, name, count);

        PropNode *fresh = new PropNode(name);

        addChild(slot, asNode(*slot), b, tagLeaf(fresh));
        (*count)++;
        return fresh;
    }

    static void
    removeChild(void **slot, ArtNode *n, uint8_t b, uint32_t depth)
    {
        switch (n->type) {
            case N4: {
                Node4 *p = (Node4 *) n;

                for (int i = 0; i < n->num; i++)
                    if (p->keys[i] == b) {
                        p->keys[i] = p->keys[n->num - 1];
                        p->child[i] = p->child[n->num - 1];
                        n->num--;
                        break;
                    }
                if (n->num == 1) {
                    /* collapse: single child absorbs this node */
                    void *only = p->child[0];
                    uint8_t onlyByte = p->keys[0];

                    if (isLeaf(only)) {
                        *slot = only;
                    } else {
                        ArtNode *c = asNode(only);
                        uint32_t addLen = n->prefixLen + 1 + c->prefixLen;
                        uint8_t merged[MAX_PREFIX];
                        uint32_t w = 0;

                        for (uint32_t i = 0;
                             i < n->prefixLen && w < (uint32_t) MAX_PREFIX;
                             i++)
                            merged[w++] = prefixByte(n, i, depth);
                        if (w < (uint32_t) MAX_PREFIX)
                            merged[w++] = onlyByte;
                        for (uint32_t i = 0;
                             i < c->prefixLen && w < (uint32_t) MAX_PREFIX;
                             i++)
                            merged[w++] = prefixByte(c, i,
                                                     depth + n->prefixLen + 1);
                        c->prefixLen = addLen;
                        memcpy(c->prefix, merged, MAX_PREFIX);
                        *slot = c;
                    }
                    delete p;
                }
                break;
            }
            case N16: {
                Node16 *p = (Node16 *) n;

                for (int i = 0; i < n->num; i++)
                    if (p->keys[i] == b) {
                        p->keys[i] = p->keys[n->num - 1];
                        p->child[i] = p->child[n->num - 1];
                        n->num--;
                        break;
                    }
                if (n->num <= 4) {
                    Node4 *nw = (Node4 *) makeNode(N4);

                    *(ArtNode *) nw = *(ArtNode *) p;
                    nw->type = N4;
                    for (int i = 0; i < n->num; i++) {
                        nw->keys[i] = p->keys[i];
                        nw->child[i] = p->child[i];
                    }
                    nw->num = n->num;
                    delete p;
                    *slot = nw;
                }
                break;
            }
            case N48: {
                Node48 *p = (Node48 *) n;

                if (p->idx[b]) {
                    p->child[p->idx[b] - 1] = nullptr;
                    p->idx[b] = 0;
                    n->num--;
                }
                if (n->num <= 16) {
                    Node16 *nw = (Node16 *) makeNode(N16);

                    *(ArtNode *) nw = *(ArtNode *) p;
                    nw->type = N16;
                    int w = 0;

                    for (int k = 0; k < 256; k++)
                        if (p->idx[k]) {
                            nw->keys[w] = (uint8_t) k;
                            nw->child[w] = p->child[p->idx[k] - 1];
                            w++;
                        }
                    nw->num = (uint16_t) w;
                    delete p;
                    *slot = nw;
                }
                break;
            }
            default: {
                Node256 *p = (Node256 *) n;

                if (p->child[b]) {
                    p->child[b] = nullptr;
                    n->num--;
                }
                if (n->num <= 48) {
                    Node48 *nw = (Node48 *) makeNode(N48);

                    *(ArtNode *) nw = *(ArtNode *) p;
                    nw->type = N48;
                    int w = 0;

                    for (int k = 0; k < 256; k++)
                        if (p->child[k]) {
                            nw->child[w] = p->child[k];
                            nw->idx[k] = (uint8_t) (w + 1);
                            w++;
                        }
                    nw->num = (uint16_t) w;
                    delete p;
                    *slot = nw;
                }
                break;
            }
        }
    }

    static bool
    eraseRec(void **slot, const uint8_t *key, size_t len, uint32_t depth)
    {
        void *p = *slot;

        if (!p)
            return false;

        if (isLeaf(p)) {
            PropNode *leaf = asLeaf(p);
            FoldedKey lk(leaf->name());

            if (lk.len == len && memcmp(lk.data, key, len) == 0) {
                delete leaf;
                *slot = nullptr;
                return true;
            }
            return false;
        }

        ArtNode *n = asNode(p);
        uint32_t match = prefixMatch(n, key, len, depth);

        if (match < n->prefixLen)
            return false;
        depth += n->prefixLen;

        uint8_t b = key[depth];
        void **childSlot = findChild(n, b);

        if (!childSlot)
            return false;

        if (isLeaf(*childSlot)) {
            PropNode *leaf = asLeaf(*childSlot);
            FoldedKey lk(leaf->name());

            if (lk.len == len && memcmp(lk.data, key, len) == 0) {
                delete leaf;
                removeChild(slot, n, b, depth - n->prefixLen);
                return true;
            }
            return false;
        }
        return eraseRec(childSlot, key, len, depth + 1);
    }

    /* smallest leaf with folded key strictly greater than key */
    static PropNode *
    succRec(void *p, const uint8_t *key, size_t len, uint32_t depth)
    {
        if (!p)
            return nullptr;

        if (isLeaf(p)) {
            PropNode *leaf = asLeaf(p);
            FoldedKey lk(leaf->name());
            size_t n = lk.len < len ? lk.len : len;
            int c = memcmp(lk.data, key, n);

            if (c > 0 || (c == 0 && lk.len > len))
                return leaf;
            return nullptr;
        }

        ArtNode *n = asNode(p);

        /* compare the compressed prefix against key[depth..] */
        for (uint32_t i = 0; i < n->prefixLen; i++) {
            if (depth + i >= len)
                return minLeaf(p); /* key exhausted: subtree greater */

            uint8_t pb = prefixByte(n, i, depth);

            if (pb > key[depth + i])
                return minLeaf(p);
            if (pb < key[depth + i])
                return nullptr; /* whole subtree smaller */
        }
        depth += n->prefixLen;
        if (depth >= len)
            return minLeaf(p);

        uint8_t b = key[depth];
        void **childSlot = findChild(n, b);

        if (childSlot) {
            PropNode *r = succRec(*childSlot, key, len, depth + 1);

            if (r)
                return r;
        }
        return minLeaf(nextGreaterChild(n, b));
    }

    static void
    eachRec(void *p, void (*fn)(PropNode *, void *), void *arg)
    {
        if (!p)
            return;
        if (isLeaf(p)) {
            fn(asLeaf(p), arg);
            return;
        }

        ArtNode *n = asNode(p);

        switch (n->type) {
            case N4:
            case N16: {
                /* keys are unsorted in storage; visit ascending */
                uint8_t *keys = (n->type == N4) ? ((Node4 *) n)->keys
                                                : ((Node16 *) n)->keys;
                void **child = (n->type == N4) ? ((Node4 *) n)->child
                                               : ((Node16 *) n)->child;
                int last = -1;

                for (int done = 0; done < n->num; done++) {
                    int best = -1;

                    for (int i = 0; i < n->num; i++)
                        if (keys[i] > last
                            && (best < 0 || keys[i] < keys[best]))
                            best = i;
                    last = keys[best];
                    eachRec(child[best], fn, arg);
                }
                break;
            }
            case N48: {
                Node48 *q = (Node48 *) n;

                for (int k = 0; k < 256; k++)
                    if (q->idx[k])
                        eachRec(q->child[q->idx[k] - 1], fn, arg);
                break;
            }
            default: {
                Node256 *q = (Node256 *) n;

                for (int k = 0; k < 256; k++)
                    if (q->child[k])
                        eachRec(q->child[k], fn, arg);
                break;
            }
        }
    }

    static void
    clearRec(void *p)
    {
        if (!p)
            return;
        if (isLeaf(p)) {
            delete asLeaf(p);
            return;
        }

        ArtNode *n = asNode(p);

        switch (n->type) {
            case N4: {
                Node4 *q = (Node4 *) n;

                for (int i = 0; i < n->num; i++)
                    clearRec(q->child[i]);
                break;
            }
            case N16: {
                Node16 *q = (Node16 *) n;

                for (int i = 0; i < n->num; i++)
                    clearRec(q->child[i]);
                break;
            }
            case N48: {
                Node48 *q = (Node48 *) n;

                for (int k = 0; k < 256; k++)
                    if (q->idx[k])
                        clearRec(q->child[q->idx[k] - 1]);
                break;
            }
            default: {
                Node256 *q = (Node256 *) n;

                for (int k = 0; k < 256; k++)
                    if (q->child[k])
                        clearRec(q->child[k]);
                break;
            }
        }
        freeNodeOnly(n);
    }
};

/* ------------------------------------------------------------------ */
/* Public interface                                                   */
/* ------------------------------------------------------------------ */

PropNode *
PropertyTree::find(const char *name) const
{
    FoldedKey k(name);
    void *p = root_;
    uint32_t depth = 0;

    while (p) {
        if (isLeaf(p)) {
            PropNode *leaf = asLeaf(p);
            FoldedKey lk(leaf->name());

            if (lk.len == k.len && memcmp(lk.data, k.data, k.len) == 0)
                return leaf;
            return nullptr;
        }

        ArtNode *n = asNode(p);
        uint32_t match = prefixMatch(n, k.data, k.len, depth);

        if (match < n->prefixLen)
            return nullptr;
        depth += n->prefixLen;
        if (depth >= k.len)
            return nullptr;

        void **slot = findChild(n, k.data[depth]);

        if (!slot)
            return nullptr;
        p = *slot;
        depth++;
    }
    return nullptr;
}

PropNode *
PropertyTree::insert(const char *name)
{
    FoldedKey k(name);

    return ArtOps::insertRec(&root_, k.data, k.len, 0, name, &count_);
}

bool
PropertyTree::erase(const char *name)
{
    FoldedKey k(name);

    if (ArtOps::eraseRec(&root_, k.data, k.len, 0)) {
        count_--;
        return true;
    }
    return false;
}

PropNode *
PropertyTree::first() const
{
    return minLeaf(root_);
}

PropNode *
PropertyTree::nextAfter(const char *name) const
{
    FoldedKey k(name);

    return ArtOps::succRec(root_, k.data, k.len, 0);
}

void
PropertyTree::each(void (*fn)(PropNode *, void *), void *arg) const
{
    ArtOps::eachRec(root_, fn, arg);
}

void
PropertyTree::clear()
{
    ArtOps::clearRec(root_);
    root_ = nullptr;
    count_ = 0;
}

} /* namespace MUCK */
