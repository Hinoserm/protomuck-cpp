/* Node and directory primitives over the modern property trees. The
 * AVL implementation this file used to hold is gone; the ordering is
 * now structural (see docs/PROPERTIES.txt and PropertyTree.cpp).
 */

#include "copyright.h"
#include "config.h"
#include "params.h"

#include "db.h"
#include "tune.h"
#include "props.h"
#include "interface.h"
#include "externs.h"

PropPtr
alloc_propnode(const char *name)
{
    return new MUCK::PropNode(name);
}

void
free_propnode(PropPtr node)
{
    delete node;
}

/* Drop a node's value, keeping its name, flags, and children. */
void
clear_propnode(PropPtr p)
{
    if (p)
        p->clearValue();
}

void
delete_proplist(PropDirPtr dir)
{
    if (dir)
        dir->clear();
}

PropPtr
locate_prop(PropDirPtr list, char *name)
{
    return list ? list->find(name) : NULL;
}

PropPtr
new_prop(PropDirPtr list, char *name)
{
    return list ? list->insert(name) : NULL;
}

void
delete_prop(PropDirPtr list, char *name)
{
    if (list)
        list->erase(name);
}

PropPtr
first_node(PropDirPtr list)
{
    return list ? list->first() : NULL;
}

PropPtr
next_node(PropDirPtr list, const char *name)
{
    return list ? list->nextAfter(name) : NULL;
}

/* Deep copy of one directory into another, values and children. */
void
copy_proplist(dbref obj, PropDirPtr nw, PropDirPtr old)
{
    if (!nw || !old)
        return;

    for (PropPtr o = old->first(); o; o = old->nextAfter(o->name())) {
        PropPtr p = nw->insert(o->name());

        SetPFlagsRaw(p, PropFlagsRaw(o));
        switch (PropType(o)) {
            case PROP_STRTYP:
                p->setStr(o->strValue());
                break;
            case PROP_LOKTYP:
                p->setLok(copy_bool(o->lokValue()));
                break;
            case PROP_FLTTYP:
                p->setFlt(o->fltValue());
                break;
            case PROP_REFTYP:
                p->setRef(o->refValue());
                break;
            case PROP_INTTYP:
                p->setInt(o->intValue());
                break;
            case PROP_DIRTYP:
            default:
                break;
        }
        if (o->isDir()) {
            p->children().parentNode = p;
            copy_proplist(obj, &p->children(), &o->children());
        }
    }
}

static void
sizeVisit(MUCK::PropNode *p, void *arg)
{
    int *acc = (int *) arg;

    *acc += sizeof(MUCK::PropNode) + strlen(p->name()) + 1;
    if (PropType(p) == PROP_STRTYP && p->strValue())
        *acc += strlen(p->strValue()) + 1;
    if (p->isDir())
        *acc += size_proplist(&p->children());
}

int
size_proplist(PropDirPtr dir)
{
    int total = 0;

    if (dir)
        dir->each(sizeVisit, &total);
    return total;
}

int
Prop_Check(const char *name, const char what)
{
    if (*name == what)
        return (1);
    while ((name = (char *) index(name, PROPDIR_DELIMITER))) {
        if (name[1] == what)
            return (1);
        name++;
    }
    return (0);
}
