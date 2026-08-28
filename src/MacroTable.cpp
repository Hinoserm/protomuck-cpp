#include "copyright.h"
#include "config.h"
#include "db.h"
#include "externs.h"
#include "interface.h"
#include "interp.h"
#include "MacroTable.h"

namespace MUCK {

/* --- node primitives ------------------------------------------------- */

MacroTable::Node *
MacroTable::newNode(const char *name, const char *definition, dbref player)
{
    Node *newmacro = new Node;
    char buf[BUFFER_LEN];
    int i;

    for (i = 0; name[i]; i++)
        buf[i] = DOWNCASE(name[i]);
    buf[i] = '\0';
    newmacro->name = alloc_string(buf);
    newmacro->definition = alloc_string(definition);
    newmacro->implementor = player;
    newmacro->left = NULL;
    newmacro->right = NULL;
    return newmacro;
}

void
MacroTable::freeNode(Node *node)
{
    delete[] node->name;
    delete[] node->definition;
    delete node;
}

bool
MacroTable::grow(Node *node, Node *newmacro)
{
    int value = strcmp(newmacro->name, node->name);

    if (!value)
        return false;
    else if (value < 0) {
        if (node->left)
            return grow(node->left, newmacro);
        else {
            node->left = newmacro;
            return true;
        }
    } else if (node->right)
        return grow(node->right, newmacro);
    else {
        node->right = newmacro;
        return true;
    }
}

/* --- public interface ------------------------------------------------ */

bool
MacroTable::insert(const char *name, const char *definition, dbref player)
{
    Node *newmacro = newNode(name, definition, player);

    if (!top_) {
        top_ = newmacro;
        return true;
    }
    if (!grow(top_, newmacro)) {
        freeNode(newmacro);
        return false;
    }
    return true;
}

char *
MacroTable::lookup(const Node *node, const char *match)
{
    if (!node)
        return NULL;
    else {
        int value = string_compare(match, node->name);

        if (value < 0)
            return lookup(node->left, match);
        else if (value > 0)
            return lookup(node->right, match);
        else
            return alloc_string(node->definition);
    }
}

char *
MacroTable::expansion(const char *name) const
{
    return lookup(top_, name);
}

bool
MacroTable::eraseNode(Node *oldnode, Node *node, const char *killname, Node *mtop)
{
    if (!node)
        return false;
    else if (strcmp(killname, node->name) < 0)
        return eraseNode(node, node->left, killname, mtop);
    else if (strcmp(killname, node->name))
        return eraseNode(node, node->right, killname, mtop);
    else {
        if (node == oldnode->left) {
            oldnode->left = node->left;
            if (node->right)
                grow(mtop, node->right);
        } else {
            oldnode->right = node->right;
            if (node->left)
                grow(mtop, node->left);
        }
        freeNode(node);
        return true;
    }
}

bool
MacroTable::remove(const char *name)
{
    if (!top_) {
        return false;
    } else if (!string_compare(name, top_->name)) {
        Node *macrotemp = top_;
        bool whichway = top_->left ? true : false;

        top_ = whichway ? top_->left : top_->right;
        if (top_ && (whichway ? macrotemp->right : macrotemp->left))
            grow(top_, whichway ? macrotemp->right : macrotemp->left);
        freeNode(macrotemp);
        return true;
    } else if (eraseNode(top_, top_, name, top_))
        return true;
    else
        return false;
}

void
MacroTable::listTree(const Node *node, const char *first, const char *last,
                     bool verbose, dbref player, char *buf) const
{
    if (!node)
        return;
    if (strncmp(node->name, first, strlen(first)) >= 0)
        listTree(node->left, first, last, verbose, player, buf);
    if ((strncmp(node->name, first, strlen(first)) >= 0)
        && (strncmp(node->name, last, strlen(last)) <= 0)) {
        if (verbose) {
            sprintf(buf, "%-16s %-16s  %s", node->name,
                    NAME(node->implementor), node->definition);
            notify(player, buf);
            buf[0] = '\0';
        } else {
            sprintf(buf + strlen(buf), "%-16s", node->name);
            if (strlen(buf) > 70) {
                notify(player, buf);
                buf[0] = '\0';
            }
        }
    }
    if (strncmp(last, node->name, strlen(last)) >= 0)
        listTree(node->right, first, last, verbose, player, buf);
}

void
MacroTable::list(const char *word[], int wordCount, dbref player, bool verbose) const
{
    char buf[BUFFER_LEN];

    buf[0] = '\0';
    if (!wordCount--) {
        listTree(top_, "a", "z", verbose, player, buf);
    } else {
        listTree(top_, word[0], word[wordCount], verbose, player, buf);
    }
    if (!verbose && buf[0])
        notify(player, buf);
    anotify_nolisten(player, CINFO "End of list.", 1);
}

void
MacroTable::forEach(Visitor fn, void *ctx) const
{
    visitTree(top_, fn, ctx);
}

void
MacroTable::visitTree(const Node *node, Visitor fn, void *ctx)
{
    if (!node)
        return;
    visitTree(node->left, fn, ctx);
    fn(ctx, node->name, node->definition, node->implementor);
    visitTree(node->right, fn, ctx);
}

void
MacroTable::purgeTree(Node *node)
{
    if (!node)
        return;
    purgeTree(node->left);
    purgeTree(node->right);
    freeNode(node);
}

void
MacroTable::purge()
{
    purgeTree(top_);
    top_ = NULL;
}

/* --- muf/macros file serialization ----------------------------------- */

static void
putString(FILE *f, const char *s)
{
    if (s) {
        if (fputs(s, f) == EOF) {
            fprintf(stderr, "PANIC: Unable to write to macro file.\n");
            abort();
        }
    }
    if (putc('\n', f) == EOF) {
        fprintf(stderr, "PANIC: Unable to write to macro file.\n");
        abort();
    }
}

static char *
fileLine(FILE *f)
{
    char buf[BUFFER_LEN];
    int len;

    if (!fgets(buf, sizeof(buf), f))
        return NULL;
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
    return alloc_string(buf);
}

void
MacroTable::dumpTree(const Node *node, FILE *f)
{
    if (!node)
        return;
    dumpTree(node->left, f);
    putString(f, node->name);
    putString(f, node->definition);
    putref(f, node->implementor);
    dumpTree(node->right, f);
}

void
MacroTable::dump(FILE *f) const
{
    dumpTree(top_, f);
}

/* Rebalance a chain built by chainLoad into a tree, exactly as the
 * historical foldtree did. */
void
MacroTable::foldTree(Node *center)
{
    int count = 0;
    Node *nextcent = center;

    for (; nextcent; nextcent = nextcent->left)
        count++;
    if (count > 1) {
        for (nextcent = center, count /= 2; count--; nextcent = nextcent->left) ;
        if (center->left)
            center->left->right = NULL;
        center->left = nextcent;
        foldTree(center->left);
    }
    for (count = 0, nextcent = center; nextcent; nextcent = nextcent->right)
        count++;
    if (count > 1) {
        for (nextcent = center, count /= 2; count--; nextcent = nextcent->right) ;
        if (center->right)
            center->right->left = NULL;
        foldTree(center->right);
    }
}

int
MacroTable::chainLoad(Node *lastnode, FILE *f)
{
    char *line, *line2;
    Node *newmacro;

    if (!(line = fileLine(f)))
        return 0;
    line2 = fileLine(f);

    newmacro = newNode(line, line2 ? line2 : "", getref(f));
    delete[] line;
    delete[] line2;

    if (!top_)
        top_ = newmacro;
    else {
        newmacro->left = lastnode;
        lastnode->right = newmacro;
    }
    return 1 + chainLoad(newmacro, f);
}

void
MacroTable::load(FILE *f)
{
    int count;

    top_ = NULL;
    count = chainLoad(NULL, f);
    for (count /= 2; count--; top_ = top_->right) ;
    foldTree(top_);
}

MacroTable &
macros()
{
    static MacroTable instance;
    return instance;
}

} /* namespace MUCK */
