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
    char comp[BUFFER_LEN];
    const char *rest;

    if (!l)
        return NULL;
    rest = splitPath(path, comp);
    if (!*comp)
        return NULL;

    PropPtr p = l->insert(comp);

    if (rest) {
        p->children().parentNode = p;
        return propdir_new_elem(&p->children(), (char *) rest);
    }
    return p;
}

PropDirPtr
propdir_delete_elem(PropDirPtr l, char *path)
{
    char comp[BUFFER_LEN];
    const char *rest;

    if (!l)
        return NULL;
    rest = splitPath(path, comp);
    if (!*comp)
        return l;

    if (rest) {
        PropPtr p = l->find(comp);

        if (p && p->isDir()) {
            propdir_delete_elem(&p->children(), (char *) rest);
            /* an emptied pure directory disappears with its child */
            if (!p->isDir() && p->type() == PROP_DIRTYP)
                l->erase(p->name());
        }
        return l;
    }
    l->erase(comp);             /* erases the subtree with it */
    return l;
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

    PropPtr p = l->find(comp);

    if (!p)
        return NULL;
    if (rest)
        return p->isDir() ? propdir_get_elem(&p->children(), (char *) rest)
                          : NULL;
    return p;
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

    if (rest) {
        PropPtr p = l->find(comp);

        if (p && p->isDir())
            return propdir_next_elem(&p->children(), (char *) rest);
        return NULL;
    }
    return l->nextAfter(comp);
}

int
propdir_check(PropDirPtr l, char *path)
{
    PropPtr p = propdir_get_elem(l, path);

    return p ? (p->isDir() ? 1 : 0) : 0;
}
