

#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "MacroTable.h"
#include "Modules.h"
#include "ProgramStore.h"
#include "strutils.h"

int db_load_format = 0;

#ifndef DB_INITIAL_SIZE
#define DB_INITIAL_SIZE 10000
#endif /* DB_INITIAL_SIZE */

#ifdef DB_DOUBLING

dbref db_size = DB_INITIAL_SIZE;

#endif /* DB_DOUBLING */


#ifndef MALLOC_PROFILING
extern char *alloc_string(const char *);
#endif

int number(const char *s);
int ifloat(const char *s);
void getproperties(FILE * f, int obj);

#ifdef DBDEBUG
/* This function is a total quickhack, mostly because of how */
/*  DBFETCH works.  -Hinoserm.                               */
short
dbcheck(const char *file, int line, dbref item)
{
    if (!OkObj(item)) {
        log_status("DB FATAL ERROR! Attempt to access a bad object at %s:%d. Object was %d.\n", file, line, item);
        abort();
    }

    return 0;
}
#endif /* DBDEBUG */

dbref
MUCK::Database::parent(dbref obj)
{
    int limit = 88;

    if (!OkObj(obj))
        return GLOBAL_ENVIRONMENT;
    do {
        if (Typeof(obj) == TYPE_THING && (FLAGS(obj) & VEHICLE)
            && limit-- > 0) {
            obj = DBFETCH(obj)->sp.thing.home;
            if (obj == NIL)
                obj = GLOBAL_ENVIRONMENT;
            if (obj != NOTHING && Typeof(obj) == TYPE_PLAYER)
                obj = DBFETCH(obj)->sp.player.home;
        } else {
            obj = getloc(obj);
        }
    } while (obj != NOTHING && Typeof(obj) == TYPE_THING);
    if (!limit)
        return GLOBAL_ENVIRONMENT;
    return obj;
}


void
free_line(struct line *l)
{
    delete[]l->this_line;
    delete l;
}

void
free_prog_text(struct line *l)
{
    struct line *next;

    while (l) {
        next = l->next;
        free_line(l);
        l = next;
    }
}

#ifdef DB_DOUBLING

void
MUCK::Database::grow(dbref newtop)
{
    struct object *newdb;

    if (newtop > db_top) {
        db_top = newtop;
        if (!db) {
            /* make the initial one */
            db_size = DB_INITIAL_SIZE;
            while (db_top > db_size)
                db_size += 1000;
            if ((db = (struct object *)
                 malloc(db_size * sizeof(struct object))) == 0) {
                fprintf(stderr, "PANIC: Unable to allocate new object.\n");
                abort();
            }
        }
        /* maybe grow it */
        if (db_top > db_size) {
            /* make sure it's big enough */
            while (db_top > db_size)
                db_size += 1000;
            if ((newdb = (struct object *)
                 realloc((void *) db, db_size * sizeof(struct object))) == 0) {
                fprintf(stderr, "PANIC: Unable to reallocate object.\n");
                abort();
            }
            db = newdb;
        }
    }
}

#else /* DB_DOUBLING */

void
MUCK::Database::grow(dbref newtop)
{
    struct object *newdb;

    if (newtop > db_top) {
        db_top = newtop + (newtop / 10); /*CrT */
        if (db) {
            if ((newdb = (struct object *)
                 realloc((void *) db, db_top * sizeof(struct object))) == 0) {
                fprintf(stderr, "PANIC: Unable to reallocate object.\n");
                abort();
            }
            db = newdb;
        } else {
            /* make the initial one */
            int startsize = (newtop >= DB_INITIAL_SIZE) ? newtop : DB_INITIAL_SIZE;

            if ((db = (struct object *)
                 malloc(startsize * sizeof(struct object))) == 0) {
                fprintf(stderr, "PANIC: Unable to allocate new object.\n");
                abort();
            }
        }
    }
}

#endif /* DB_DOUBLING */

void
MUCK::Database::clearObject(dbref player, dbref i)
{
    struct object *o = DBFETCH(i);

    bzero(o, sizeof(struct object));
    NAME(i) = 0;
    ts_newobject(player, o);
    o->location = NOTHING;
    o->contents = NOTHING;
    o->exits = NOTHING;
    o->next = NOTHING;
    o->properties = 0;

    /* DBDIRTY(i); */
    /* flags you must initialize yourself */
    /* type-specific fields you must also initialize */
}

dbref
MUCK::Database::newObject(dbref player)
{
    dbref newobj;

    if (recyclable != NOTHING) {
        newobj = recyclable;
        if (TYPEOF(newobj) != TYPE_GARBAGE) {
            log_status("DB FATAL ERROR! Attempted to reuse non-garbage object (%d)!\n", newobj);
            abort();
        }
 
        recyclable = DBFETCH(newobj)->next;
        freeObject(newobj);
    } else {
        newobj = db_top;
        grow(db_top + 1);
    }

    /* clear it out */
    clearObject(player, newobj);

    /* fresh identity, even on a recycled slot: a reused dbref is a NEW
     * object and must never inherit the old uuid */
    assignUuid(newobj, Uuid::generate());
    DBDIRTY(newobj);
    return newobj;
}


dbref
MUCK::Database::newProgram(dbref player, const char *name)
{
    unsigned char mlvl;
    dbref newprog;
    char buf[BUFFER_LEN];

    newprog = newObject(player);
    player = OWNER(player);

    NAME(newprog) = alloc_string(name);
    sprintf(buf, "A scroll containing a spell called %s", name);
    SETDESC(newprog, buf);
    DBFETCH(newprog)->location = player;
    FLAGS(newprog) = TYPE_PROGRAM;

    mlvl = MLevel(player);
    if (mlvl < 1)
        mlvl = 2;
    else if (mlvl > 3)
        mlvl = 3;
    SetMLevel(newprog, mlvl);

    OWNER(newprog) = player;
    DBFETCH(newprog)->sp.program.first = 0;
    DBFETCH(newprog)->sp.program.curr_line = 0;
    DBFETCH(newprog)->sp.program.siz = 0;
    DBFETCH(newprog)->sp.program.code = 0;
    DBFETCH(newprog)->sp.program.start = 0;
    DBFETCH(newprog)->sp.program.pubs = 0;
    DBFETCH(newprog)->sp.program.fprofile = NULL;
    DBFETCH(newprog)->sp.program.proftime.tv_sec = 0;
    DBFETCH(newprog)->sp.program.proftime.tv_usec = 0;
    DBFETCH(newprog)->sp.program.profstart = 0;
    DBFETCH(newprog)->sp.program.profuses = 0;
    DBFETCH(newprog)->sp.program.instances = 0;
    PUSH(newprog, DBFETCH(player)->contents);
    DBDIRTY(newprog);
    DBDIRTY(player);

    return newprog;
}


void
putref(FILE * f, dbref ref)
{
    if (fprintf(f, "%d\n", ref) < 0) {
        fprintf(stderr, "PANIC: Unable to write to db file.\n");
        abort();
    }
}

void
putfref(FILE * f, dbref ref, dbref ref2, dbref ref3, dbref ref4, dbref pow1, dbref pow2)
{
    if (fprintf(f, "%d %d %d %d %d %d\n", ref, ref2, ref3, ref4, pow1, pow2)
        == EOF) {
        fprintf(stderr, "PANIC: Unable write to db file.\n");
        abort();
    }
}

void
puttimestampEx(FILE * f, int ref, dbref ref2)
{
    if (fprintf(f, "%d %d\n", ref, ref2) == EOF) {
        fprintf(stderr, "PANIC: Unable write to db file.\n");
        abort();
    }
}


static void
putstring(FILE * f, const char *s)
{
    if (s) {
        if (fputs(s, f) == EOF) {
            fprintf(stderr, "PANIC: Unable to write to db file.\n");
            abort();
        }
    }
    if (putc('\n', f) == EOF) {
        fprintf(stderr, "PANIC: Unable to write to db file.\n");
        abort();
    }
}

extern FILE *input_file;
extern FILE *delta_infile;
extern FILE *delta_outfile;



dbref
parse_dbref(const char *s)
{
    const char *p;
    int x;

    x = atol(s);
    if (x > 0) {
        return x;
    } else if (x == 0) {
        /* check for 0 */
        for (p = s; *p; p++) {
            if (*p == '0')
                return 0;
            if (!isspace(*p))
                break;
        }
    }
    /* else x < 0 or s != 0 */
    return NOTHING;
}

static int
do_peek(FILE * f)
{
    int peekch;

    ungetc((peekch = getc(f)), f);

    return (peekch);
}

dbref
getref(FILE * f)
{
    char buf[BUFFER_LEN];
    int peekch;

    /*
     * Compiled in with or without timestamps, Sep 1, 1990 by Fuzzy, added to
     * Muck by Kinomon.  Thanks Kino!
     */
    if ((peekch = do_peek(f)) == NUMBER_TOKEN || peekch == LOOKUP_TOKEN) {
        return (0);
    }
    fgets(buf, sizeof(buf), f);
    return (atol(buf));
}


dbref
getfref(FILE * f, dbref *f2, dbref *f3, dbref *f4, dbref *p1, dbref *p2)
{
    char buf[BUFFER_LEN];
    dbref f1;
    int got, peekch;

    if ((peekch = do_peek(f)) == NUMBER_TOKEN || peekch == LOOKUP_TOKEN) {
        return (0);
    }
    fgets(buf, sizeof(buf), f);

    got = sscanf(buf, "%d %d %d %d %d %d", &f1, f2, f3, f4, p1, p2);

    if (got < 6)
        (*p2) = 0;
    if (got < 5)
        (*p1) = 0;
    if (got < 4)
        (*f4) = 0;
    if (got < 3)
        (*f3) = 0;
    if (got < 2)
        (*f2) = 0;
    if (got < 1) {
        fprintf(stderr, "getfref: scanf failed\n");
        return 0;
    }
    return (f1);
}

dbref
gettimestampEx(FILE * f, dbref *f2)
{
    char buf[BUFFER_LEN];
    dbref f1;
    int got, peekch;

    if ((peekch = do_peek(f)) == NUMBER_TOKEN || peekch == LOOKUP_TOKEN) {
        return (0);
    }
    fgets(buf, sizeof(buf), f);

    got = sscanf(buf, "%d %d", &f1, f2);

    if (got < 2)
        (*f2) = -1;
    if (got < 1) {
        fprintf(stderr, "getfref: scanf failed\n");
        return 0;
    }
    return (f1);
}


static char xyzzybuf[BUFFER_LEN];

/* shared with FlatFileConverter and the macro loader; no longer static */
const char *
getstring_noalloc(FILE * f)
{
    char *p;

    char c;

    if (fgets(xyzzybuf, sizeof(xyzzybuf), f) == NULL) {
        xyzzybuf[0] = '\0';
        return xyzzybuf;
    }

    if (strlen(xyzzybuf) == BUFFER_LEN - 1) {
        /* ignore whatever comes after */
        if (xyzzybuf[BUFFER_LEN - 2] != '\n')
            while ((c = fgetc(f)) != '\n') ;
    }
    for (p = xyzzybuf; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            break;
        }
    }

    return xyzzybuf;
}

#define getstring(x) alloc_string(getstring_noalloc(x))

#ifdef COMPRESS
extern const char *pcompress(const char *);

#ifdef ARCHAIC_DATABASES
extern const char *old_uncompress(const char *);
#endif /* ARCHAIC_DATABASES */
#endif

#define alloc_compressed(x) alloc_string(x)

/* returns true for numbers of form [ + | - ] <series of digits> */
int
number(const char *s)
{
    if (!s)
        return 0;
    while (isspace(*s))
        s++;
    if (*s == '+' || *s == '-')
        s++;
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

/* returns true for floats of form  [+|-]<digits>.<digits>[E[+|-]<digits>] */
int
ifloat(const char *s)
{
    const char *hold = NULL;
    int decFound = 0;           /* bool to indicate if a decimal is found yet */
    int expFound = 0;           /* bool to indicate if exponent is found yet */

    if (!s)
        return 0;               /* no string at all */
    while (isspace(*s))
        s++;                    /* remove leading spaces */
    if (*s == '+' || *s == '-')
        s++;
    /* inf or nan */
    if (*s == 'i' || *s == 'n' || *s == 'I' || *s == 'N') {
        s++;
        if (*s == 'n' || *s == 'a' || *s == 'N' || *s == 'A') {
            s++;
            if (*s == 'f' || *s == 'n' || *s == 'F' || *s == 'N') {
                s++;
                if (!*s) {
                    return 1;
                } else {
                    return 0;
                }
            } else {
                return 0;
            }
        } else {
            return 0;
        }
    }
    if (*s == '.') {
        decFound = 1;
        s++;                    /* valid format = .#e# and .# */
    }
    hold = s;
    while ((*s) && (*s >= '0' && *s <= '9'))
        s++;
    if (s == hold)              /* Blank or non-numbers at start. Boo */
        return 0;
    if (!*s)                    /* means it was a # or a .# number */
        return 1;
    if (*s == '.' && decFound)
        return 0;               /* prevent 2 decimal marks */
    if (*s == '.')
        s++;                    /* skip valid decimal point */
    if (*s == 'e' || *s == 'E') {
        expFound = 1;
        s++;                    /* could be #.#e# or .#e# */
    }
    if (expFound && (*s == '+' || *s == '-'))
        s++;                    /* skip + or - for exponent */
    hold = s;
    while ((*s) && (*s >= '0' && *s <= '9'))
        s++;                    /* eat more numbers */
    if (s == hold)
        return 0;               /* no numbers or non-numbers after token */
    if (!*s)
        return 1;               /* normal ending at this point */
    if (expFound)
        return 0;               /* more characters following exponent already */
    if (*s != 'e' && *s != 'E')
        return 0;               /* Invalid token following numbers */
    s++;                        /* skip 'e' or 'E' */
    if (*s == '+' || *s == '-')
        s++;                    /* Alynna: skip + or - for this exponent */
    hold = s;
    while ((*s) && (*s >= '0' && *s <= '9'))
        s++;                    /* eat final numbers */
    if (s == hold)
        return 0;               /* no numbers after token */
    if (*s)
        return 0;               /* more characters after numbers = bad */
    return 1;
}

/*** CHANGED:
was: PropPtr getproperties(FILE *f)
now: void getproperties(FILE *f, dbref obj)
***/
void
getproperties(FILE * f, dbref obj)
{
    char buf[BUFFER_LEN], *p;
    int datalen;

    /* get rid of first line */
    fgets(buf, sizeof(buf), f);

    if (strcmp(buf, "Props*\n")) {
        /* initialize first line stuff */
        fgets(buf, sizeof(buf), f);
        while (1) {
            /* fgets reads in \n too! */
            if (!strcmp(buf, "***Property list end ***\n") || !strcmp(buf, "*End*\n"))
                break;
            p = index(buf, PROP_DELIMITER);
            *(p++) = '\0';
            datalen = strlen(p);
            p[datalen - 1] = '\0';

            if ((p - buf) >= BUFFER_LEN)
                buf[BUFFER_LEN - 1] = '\0';
            if (datalen >= BUFFER_LEN)
                buf[BUFFER_LEN - 1] = '\0';

            if ((*p == '^') && (number(p + 1))) {
                add_prop_nofetch(obj, buf, NULL, atol(p + 1));
            } else {
                if (*buf) {
                    add_prop_nofetch(obj, buf, p, 0);
                }
            }
            fgets(buf, sizeof(buf), f);
        }
    } else {
        db_getprops(f, obj);
    }
}

void
MUCK::Database::freeObject(dbref i)
{
    struct object *o;

    o = DBFETCH(i);
    if (NAME(i) && Typeof(i) != TYPE_GARBAGE)
        delete[]NAME(i);

    if (o->properties) {
        delete_proplist(o->properties);
    }

    if (Typeof(i) == TYPE_EXIT && o->sp.exit.dest) {
        delete[]o->sp.exit.dest;
    } else if (Typeof(i) == TYPE_PLAYER) {
        if (o->sp.player.password) {
            delete[]o->sp.player.password;
        }
        if (o->sp.player.descrs) {
            delete[]o->sp.player.descrs;
            o->sp.player.descrs = NULL;
            o->sp.player.descr_count = 0;
        }
    }
#ifndef SANITY
    if (Typeof(i) == TYPE_PROGRAM) {
        uncompile_program(i);
    }
#endif
    /* DBDIRTY(i); */
}

void
MUCK::Database::freeAll()
{
    dbref i;

    if (db) {
        for (i = 0; i < db_top; i++)
            freeObject(i);
        free((void *) db);      //TODO: Update this later for C++. -hinoserm
        db = 0;
        db_top = 0;
    }

    clear_players();
    clear_primitives();
    recyclable = NOTHING;
}





void
autostart_progs(void)
{
    dbref i;
    struct object *o;
    struct line *tmp;

    if (db_conversion_flag) {
        return;
    }

    for (i = 0; i < MUCK::database().top(); i++) {
        if (Typeof(i) == TYPE_PROGRAM) {
            if ((FLAGS(i) & ABODE) && TMage(OWNER(i))) {
                /* pre-compile AUTOSTART programs. */
                /* They queue up when they finish compiling. */
                o = DBFETCH(i);
                tmp = o->sp.program.first;
                o->sp.program.first = (struct line *) MUCK::programs().read(i);
                do_compile(-1, OWNER(i), i, 0);
                free_prog_text(o->sp.program.first);
                o->sp.program.first = tmp;
            }
        }
    }
}


int
RawMWLevel(dbref thing, const char *file, int line)
{
    if (!OkObj(thing))
        return 0;

    switch (CheckMWLevel(thing)) {
        case LBOY:
            return (tp_multi_wizlevels ? LBOY : LARCH);
        case LWIZ:
            return (tp_multi_wizlevels ? LWIZ : LARCH);
        case LMAGE:
            return (tp_multi_wizlevels ? LMAGE : LM3);
        default:
            return CheckMWLevel(thing);
    }
}

int
WLevel(dbref player)
{
    int mlev = MLevel(player);

    return mlev >= LMAGE ? mlev : 0;
}

/* ---------------------------------------------------------------------
 * The single global database instance. Constant-initialized: the inline
 * accessors compile to direct member loads with no init guard.
 * --------------------------------------------------------------------- */

namespace MUCK {

Database g_database;

/* --- identity ----------------------------------------------------- */

static const Uuid nilUuid;

const Uuid &
Database::uuidOf(dbref ref) const
{
    if (ref < 0 || ref >= (dbref) uuids.size())
        return nilUuid;
    return uuids[ref];
}

dbref
Database::refOf(const Uuid &u) const
{
    if (u.isNil())
        return NOTHING;
    auto it = byUuid.find(u);
    return it == byUuid.end() ? NOTHING : it->second;
}

void
Database::assignUuid(dbref ref, const Uuid &u)
{
    if (ref < 0)
        return;
    if ((dbref) uuids.size() <= ref)
        uuids.resize(ref + 1);
    if (!uuids[ref].isNil())
        byUuid.erase(uuids[ref]);
    uuids[ref] = u;
    if (!u.isNil())
        byUuid[u] = ref;
}

dbref
Database::resolveUuidPrefix(const char *prefix) const
{
    dbref found = NOTHING;

    for (dbref i = 0; i < (dbref) uuids.size(); i++) {
        if (uuids[i].isNil() || !uuids[i].matchesPrefix(prefix))
            continue;
        if (found != NOTHING)
            return AMBIGUOUS;
        found = i;
    }
    return found;
}

/* --- the modernized object model (step 2) ------------------------- */

void
Database::attachTypeModule(DbObject *obj)
{
    switch (FLAGS(obj->ref()) & TYPE_MASK) {
        case TYPE_ROOM:
            obj->setTypeModule(std::make_unique<MUCK::Room>());
            break;
        case TYPE_THING:
            obj->setTypeModule(std::make_unique<MUCK::Thing>());
            break;
        case TYPE_PLAYER:
            obj->setTypeModule(std::make_unique<MUCK::Player>());
            break;
        case TYPE_EXIT:
            obj->setTypeModule(std::make_unique<MUCK::Exit>());
            break;
        case TYPE_PROGRAM:
            obj->setTypeModule(std::make_unique<MUCK::MufProgram>());
            break;
        default:               /* garbage: no type module */
            break;
    }
    /* PROPERTIES is a global feature module: every object has it */
    obj->attach(std::make_unique<MUCK::Properties>());
}

DbObject *
Database::get(dbref ref)
{
    if (!valid(ref))
        return nullptr;
    if ((dbref) shells.size() <= ref)
        shells.resize(ref + 1);
    if (!shells[ref]) {
        shells[ref].reset(new DbObject(ref));
        attachTypeModule(shells[ref].get());
    }
    return shells[ref].get();
}

static int moduleTypeBits(Room *) { return TYPE_ROOM; }
static int moduleTypeBits(Thing *) { return TYPE_THING; }
static int moduleTypeBits(Player *) { return TYPE_PLAYER; }
static int moduleTypeBits(Exit *) { return TYPE_EXIT; }
static int moduleTypeBits(MufProgram *) { return TYPE_PROGRAM; }

template <class T>
T *
Database::Create(const char *name, dbref owner)
{
    dbref r = newObject(owner);

    FLAGS(r) = moduleTypeBits((T *) nullptr);
    NAME(r) = alloc_string(name);
    OWNER(r) = owner;
    DBDIRTY(r);
    return get(r)->template As<T>();
}

template Room *Database::Create<Room>(const char *, dbref);
template Thing *Database::Create<Thing>(const char *, dbref);
template Player *Database::Create<Player>(const char *, dbref);
template Exit *Database::Create<Exit>(const char *, dbref);
template MufProgram *Database::Create<MufProgram>(const char *, dbref);

} /* namespace MUCK */
