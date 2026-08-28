#ifndef __INCL_PROPS_H
#define __INCL_PROPS_H

/* Property struct */

union pdata_u {
    const char *str;
    struct boolexp *lok;
    int val;
    double fval;
    dbref ref;
    long pos;
};

/* data struct for setting data. */
struct pdata {
    unsigned short flags;
    union pdata_u data;
};
typedef struct pdata PData;

/* The property node is MUCK::PropNode (inc/PropNode.h), a leaf of
 * the per-propdir adaptive radix tree. The legacy handle names keep
 * working: PropPtr is a node, PropDirPtr is a directory (one tree).
 * docs/PROPERTIES.txt. */
#include "PropNode.h"

typedef MUCK::PropNode *PropPtr;
typedef MUCK::PropertyTree *PropDirPtr;

/* propload queue types */
#define PROPS_UNLOADED 0x0
#define PROPS_LOADED   0x1
#define PROPS_PRIORITY 0x2
#define PROPS_CHANGED  0x3

/* property value types */
#define PROP_DIRTYP   0x0
#define PROP_STRTYP   0x2
#define PROP_INTTYP   0x3
#define PROP_FLTTYP   0x6
#define PROP_LOKTYP   0x4
#define PROP_REFTYP   0x5
#define PROP_TYPMASK  0x7

/* Property flags.  Unimplemented as yet. */
#define PROP_UREAD      0x0010
#define PROP_UWRITE     0x0020
#define PROP_WREAD      0x0040
#define PROP_WWRITE     0x0080

/* half implemented.  Will be used for stuff like password props. */
#define PROP_SYSPERMS   0x0100

/* If set, this prop's string value uses Dr.Cat's compression code. */
#define PROP_COMPRESSED 0x0008

/* Do not compress this property. */
#define PROP_NOCOMPRESS 0x0800

/* Internally used prop flags.  Never stored on disk. */
#define PROP_NOASCIICHK 0x1000
#define PROP_ISUNLOADED 0x0200
#define PROP_TOUCHED    0x0400


/* Macros over the modern node. PropDir yields the child tree when
 * the node has children (matching the legacy truthiness), else null.
 * The Set* macros that took ownership of alloc_string results now
 * copy and free, so every legacy caller stays correct. */
#define PropDir(x) ((x)->isDir() ? &(x)->children() : (PropDirPtr) 0)

#define SetPDataStr(x,z) { const char *zz_ = (z); (x)->setStr(zz_); \
                           delete[] (char *) zz_; }
#define SetPDataVal(x,z) {(x)->setInt(z);}
#define SetPDataRef(x,z) {(x)->setRef(z);}
#define SetPDataLok(x,z) {(x)->setLok(z);}
#define SetPDataFVal(x,z) {(x)->setFlt(z);}

#define PropDataUNCStr(x) ((x)->strValue())
#define PropDataStr(x) ((x)->strValue())
#define PropDataVal(x) ((x)->intValue())
#define PropDataRef(x) ((dbref) (x)->refValue())
#define PropDataLok(x) ((x)->lokValue())
#define PropDataFVal(x) ((x)->fltValue())

#define PropName(x) ((x)->name())

#define SetPFlags(x,y) {(x)->flags = ((x)->flags & PROP_TYPMASK) | (short)y;}
#define PropFlags(x) ((x)->flags & ~PROP_TYPMASK)

#define SetPType(x,y) {(x)->flags = ((x)->flags & ~PROP_TYPMASK) | (short)y;}
#define PropType(x) ((x)->flags & PROP_TYPMASK)

#define SetPFlagsRaw(x,y) {(x)->flags = (short)y;}
#define PropFlagsRaw(x) ((x)->flags)

/* property access macros */
#define Prop_ReadOnly(name) \
    (Prop_Check(name, PROP_RDONLY) || Prop_Check(name, PROP_RDONLY2))
#define Prop_Private(name) 1 //Prop_Check(name, PROP_PRIVATE)
#define Prop_SeeOnly(name) Prop_Check(name, PROP_SEEONLY)
#define Prop_Hidden(name) Prop_Check(name, PROP_HIDDEN)

/* Routines as they need to be:

     PropPtr locate_prop(PropPtr list, char *name)
       if list is NULL, return NULL.

     PropPtr new_prop(PropPtr *list, char *name)
       if *list is NULL, create a new propdir, then insert the prop

     PropPtr delete_prop(PropPtr *list, char *name)
       when last prop in dir is deleted, destroy propdir & change *list to NULL

     PropPtr first_node(PropPtr list)
       if list is NULL, return NULL

     PropPtr next_node(PropPtr list, char *name)
       if list is NULL, return NULL

 */

extern void clear_propnode(PropPtr p);
extern void delete_proplist(PropDirPtr p);
extern PropPtr alloc_propnode(const char *name);
extern void free_propnode(PropPtr node);
extern PropPtr first_node(PropDirPtr p);
extern PropPtr next_node(PropDirPtr p, const char *c);
extern void putprop(FILE * f, PropPtr p);
extern int Prop_Check(const char *name, const char what);
extern PropPtr locate_prop(PropDirPtr l, char *path);
extern PropPtr new_prop(PropDirPtr l, char *path);
extern void delete_prop(PropDirPtr list, char *name);
extern int size_proplist(PropDirPtr dir);

extern void set_property_nofetch(dbref object, const char *pname, PData *dat, bool pure);
extern void set_property(dbref object, const char *pname, PData *dat);

extern void add_prop_nofetch(dbref player, const char *type, const char *pclass,
                             int value);
extern void add_property(dbref player, const char *type, const char *pclass,
                         int value);
extern void remove_property_list(dbref player, int all);
extern void remove_property_nofetch(dbref player, const char *type);
extern void remove_property(dbref player, const char *type);
extern int has_property(int descr, dbref player, dbref what, const char *type,
                        const char *pclass, int value);
extern int has_property_strict(int descr, dbref player, dbref what,
                               const char *type, const char *pclass, int value);
extern PropPtr get_property(dbref player, const char *type);

extern const char *get_property_class(dbref player, const char *type);
extern double get_property_fvalue(dbref player, const char *type);
extern int get_property_value(dbref player, const char *type);
extern dbref get_property_dbref(dbref player, const char *pclass);
extern struct boolexp *get_property_lock(dbref player, const char *type);
extern int genderof(int descr, dbref player);
extern void copy_prop(dbref old, dbref nw);
extern void copy_proplist(dbref obj, PropDirPtr new2, PropDirPtr old);
extern PropPtr first_prop(dbref player, const char *dir, PropDirPtr *list,
                          char *name);
extern PropPtr first_prop_nofetch(dbref player, const char *dir, PropDirPtr *list,
                                  char *name);
extern PropPtr next_prop(PropDirPtr list, PropPtr prop, char *name);
extern char *next_prop_name(dbref player, char *outbuf, char *name);
extern int is_propdir(dbref player, const char *dir);
extern const char *envpropstr(dbref *where, const char *propname);
extern PropPtr envprop(dbref *where, const char *propname, int typ);
extern PropPtr envprop_cmds(dbref *where, const char *propname, int typ);
extern PropPtr regenvprop(dbref *where, const char *propname, int typ);
extern void delete_proplist(PropDirPtr p);

#ifdef DISKBASE
extern int fetchprops_priority(dbref obj, int mode);
extern int fetchprops_nostamp(dbref obj);
extern void fetchprops(dbref obj);
extern void unloadprops_with_prejudice(dbref obj);
extern int disposeprops_notime(dbref obj);
extern int disposeprops(dbref obj);
extern void dirtyprops(dbref obj);
extern void undirtyprops(dbref obj);
extern int propfetch(dbref obj, PropPtr p);
#endif /* DISKBASE */

extern const char *propdir_split(const char *path, char *comp);
extern PropPtr propdir_new_elem(PropDirPtr l, char *path);
extern PropDirPtr propdir_delete_elem(PropDirPtr l, char *path);
extern PropPtr propdir_get_elem(PropDirPtr l, char *path);
extern PropPtr propdir_first_elem(PropDirPtr l, char *path);
extern PropPtr propdir_next_elem(PropDirPtr l, char *path);
extern int propdir_check(PropDirPtr l, char *path);

extern void db_putprop(FILE * f, const char *dir, PropPtr p);
extern int db_get_single_prop(FILE * f, dbref obj, int pos);
extern void db_getprops(FILE * f, dbref obj);
extern void db_dump_props(FILE * f, dbref obj);


/* From property.c */

extern void db_putprop(FILE * f, const char *dir, PropPtr p);
extern void db_dump_props_rec(FILE * f, const char *dir, PropDirPtr d);
extern void db_getprops(FILE * f, dbref obj);
extern char *displayprop(dbref player, dbref obj, const char *name, char *buf);
extern int size_properties(dbref player, int load);
extern void untouchprops_incremental(int limit);
extern int Prop_SysPerms(dbref obj, const char *type);
extern void reflist_add(dbref obj, const char *propname, dbref toadd);
extern void reflist_del(dbref obj, const char *propname, dbref todel);
extern int reflist_find(dbref obj, const char *propname, dbref tofind);


#endif
