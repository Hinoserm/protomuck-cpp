/* Path walking over the modern property trees. The path-parsing
 * rules (leading delimiters skipped, empty components collapsed,
 * a trailing delimiter naming the directory itself) are the legacy
 * ones, byte for byte. docs/PROPERTIES.txt.
 *
 * These take a PropDirPtr (a PropertyTree) where the legacy code
 * took a chain head, and mutate in place instead of returning a new
 * root, which is why the legacy "returns the updated root" contract
 * collapses to returning the same directory.
 */

#include "copyright.h"
#include "config.h"
#include "params.h"

#include "db.h"
#include "tune.h"
#include "props.h"
#include "externs.h"
#include <utility>
#include <vector>

#include "interface.h"

/* Split a path at the first delimiter run. Returns the head
 * component in comp (up to BUFFER_LEN) and the remainder, or null
 * when this was the last component. */
static const char *
splitPath(const char *path, char *comp)
{
    const char *n;
    size_t len;

    while (*path && *path == PROPDIR_DELIMITER)
        path++;
    if (!*path) {
        *comp = '\0';
        return NULL;
    }
    n = index(path, PROPDIR_DELIMITER);
    if (n) {
        len = (size_t) (n - path);
        while (*n == PROPDIR_DELIMITER)
            n++;
        if (!*n)
            n = NULL;           /* trailing delimiters only */
    } else {
        len = strlen(path);
    }
    if (len >= BUFFER_LEN)
        len = BUFFER_LEN - 1;
    memcpy(comp, path, len);
    comp[len] = '\0';
    return n;
}

/* Exported component splitter so the fold-once path cache in
 * property.cpp parses exactly the way these walkers do. */
const char *
propdir_split(const char *path, char *comp)
{
    return splitPath(path, comp);
}

PropPtr
propdir_new_elem(PropDirPtr l, char *path)
{
    /* ITERATIVE, and it must stay that way. These walkers descend one
     * level per path component, and a component buffer is BUFFER_LEN
     * (64KB). Recursing meant 64KB of stack per component, so an
     * ordinary player storing "a/a/a/.../a" (thousands of components
     * fit inside one property name) overflowed the game thread's
     * stack and took the whole server down. */
    char comp[BUFFER_LEN];
    const char *rest = path;
    PropPtr p = NULL;

    if (!l)
        return NULL;
    for (;;) {
        rest = splitPath(rest, comp);
        if (!*comp)
            return NULL;
        p = l->insert(comp);
        if (!rest)
            return p;
        p->children().parentNode = p;
        l = &p->children();
    }
}

PropDirPtr
propdir_delete_elem(PropDirPtr l, char *path)
{
    /* Iterative for the same stack reason as propdir_new_elem. The
     * descent is recorded so the post-order cleanup (an emptied pure
     * directory disappears along with its child) still runs from the
     * deepest level back up, exactly as the recursion did. */
    char comp[BUFFER_LEN];
    const char *rest = path;
    PropDirPtr top = l;
    std::vector<std::pair<PropDirPtr, PropPtr> > descent;

    if (!l)
        return NULL;
    for (;;) {
        rest = splitPath(rest, comp);
        if (!*comp)
            return top;
        if (!rest) {
            l->erase(comp);     /* erases the subtree with it */
            break;
        }

        PropPtr p = l->find(comp);

        if (!p || !p->isDir())
            return top;         /* nothing to delete down this path */
        descent.push_back(std::make_pair(l, p));
        l = &p->children();
    }
    /* unwind: same cleanup the recursion performed on the way out */
    for (size_t i = descent.size(); i-- > 0;) {
        PropDirPtr parent = descent[i].first;
        PropPtr node = descent[i].second;

        if (!node->isDir() && node->type() == PROP_DIRTYP)
            parent->erase(node->name());
        else
            break;              /* a surviving level stops the unwind */
    }
    return top;
}

PropPtr
propdir_get_elem(PropDirPtr l, char *path)
{
    char comp[BUFFER_LEN];
    const char *rest;

    if (!l)
        return NULL;
    rest = splitPath(path, comp);
    if (!*comp)
        return NULL;

    for (;;) {
        PropPtr p = l->find(comp);

        if (!p)
            return NULL;
        if (!rest)
            return p;
        if (!p->isDir())
            return NULL;
        l = &p->children();
        rest = splitPath(rest, comp);
        if (!*comp)
            return NULL;
    }
}

PropPtr
propdir_first_elem(PropDirPtr l, char *path)
{
    if (!l)
        return NULL;
    while (*path && *path == PROPDIR_DELIMITER)
        path++;
    if (!*path)
        return l->first();

    PropPtr p = propdir_get_elem(l, path);

    if (p && p->isDir())
        return p->children().first();
    return NULL;
}

PropPtr
propdir_next_elem(PropDirPtr l, char *path)
{
    char comp[BUFFER_LEN];
    const char *rest;

    if (!l)
        return NULL;
    rest = splitPath(path, comp);
    if (!*comp)
        return NULL;

    for (;;) {
        if (!rest)
            return l->nextAfter(comp);

        PropPtr p = l->find(comp);

        if (!p || !p->isDir())
            return NULL;
        l = &p->children();
        rest = splitPath(rest, comp);
        if (!*comp)
            return NULL;
    }
}

int
propdir_check(PropDirPtr l, char *path)
{
    PropPtr p = propdir_get_elem(l, path);

    return p ? (p->isDir() ? 1 : 0) : 0;
}
