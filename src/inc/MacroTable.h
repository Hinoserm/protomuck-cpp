#ifndef MUCK_MACROTABLE_H
#define MUCK_MACROTABLE_H

/* The MUF editor's macro table: a name-to-definition binary search tree,
 * persisted to the muf/macros file alongside the database dumps.
 *
 * Consolidates what previously lived across Database.cpp (load, dump,
 * rebalance), edit.cpp (insert, remove, lookup, listing), and p_muf.cpp
 * (tree walking for the MACROS_ARRAY primitive).
 */

#include <cstdio>
#include "db.h"

namespace MUCK {

class MacroTable {
  public:
    MacroTable() : top_(0) {}
    ~MacroTable() { purge(); }

    /* Returns false if a macro by that name already exists. */
    bool insert(const char *name, const char *definition, dbref player);

    /* Returns false if no macro by that name exists. */
    bool remove(const char *name);

    /* Returns a fresh alloc_string copy of the definition, or NULL. */
    char *expansion(const char *name) const;

    /* In-order visit of every macro, for building MUF arrays and the like. */
    typedef void (*Visitor)(void *ctx, const char *name,
                            const char *definition, dbref implementor);
    void forEach(Visitor fn, void *ctx) const;

    /* Range listing to a player, [first, last] by name prefix.
     * verbose lists one macro per line with implementor and definition. */
    void list(const char *word[], int wordCount, dbref player, bool verbose) const;

    /* muf/macros file serialization. load() replaces nothing: matching the
     * historical behavior, it assumes an empty table and rebalances after. */
    void load(FILE *f);
    void dump(FILE *f) const;

    void purge();

  private:
    struct Node {
        char *name;
        char *definition;
        dbref implementor;
        Node *left;
        Node *right;
    };

    static Node *newNode(const char *name, const char *definition, dbref player);
    static void freeNode(Node *node);
    static bool grow(Node *node, Node *newmacro);
    static char *lookup(const Node *node, const char *match);
    static void purgeTree(Node *node);
    static void dumpTree(const Node *node, FILE *f);
    static void visitTree(const Node *node, Visitor fn, void *ctx);
    static void foldTree(Node *center);
    bool eraseNode(Node *oldnode, Node *node, const char *killname, Node *mtop);
    int chainLoad(Node *lastnode, FILE *f);
    void listTree(const Node *node, const char *first, const char *last,
                  bool verbose, dbref player, char *buf) const;

    Node *top_;
};

/* The single global macro table. */
MacroTable &macros();

} /* namespace MUCK */

#endif /* MUCK_MACROTABLE_H */
