/* Standard headers FIRST: db.h defines function-like macros (getloc
 * among them) that clobber identically named members inside libstdc++
 * headers included after it. */
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "MacroTable.h"
#include "ProgramStore.h"
#include "strutils.h"
#include "FlatFileConverter.h"
#include "Modules.h"

/* Flat chain refs captured during the read; materializeLists() turns
 * them into the owning containment vectors after the whole database
 * is in, applying the same corruption rules the old save-side chain
 * sanitizer used. Import-only state. */
static std::unordered_map<int, dbref> rawContents;
static std::unordered_map<int, dbref> rawExits;
static std::unordered_map<int, dbref> rawNext;

/* Reading helpers that remain in Database.cpp (shared with the macro
 * file loader) */
extern dbref getref(FILE *);
extern int number(const char *);
extern int ifloat(const char *);
extern void autostart_progs(void);
#ifndef MALLOC_PROFILING
extern char *alloc_string(const char *);
#endif
#ifdef COMPRESS
extern const char *pcompress(const char *);
#ifdef ARCHAIC_DATABASES
extern const char *old_uncompress(const char *);
#endif
#endif

#define getstring(x) alloc_string(getstring_noalloc(x))

namespace MUCK {

FlatFileConverter g_flatConverter;

} /* namespace MUCK */

/* ---------------------------------------------------------------
 * Flat-format reading helpers, moved here with the readers. Only
 * this converter understands the old format.
 * --------------------------------------------------------------- */

int db_load_format = 0;


static int
do_peek(FILE * f)
{
    int peekch;

    ungetc((peekch = getc(f)), f);

    return (peekch);
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


#ifdef ARCHAIC_DATABASES

#ifdef COMPRESS
# define getstring_oldcomp_noalloc(foo) old_uncompress(getstring_noalloc(foo))
#else
# define getstring_oldcomp_noalloc(foo) getstring_noalloc(foo)
#endif

void
db_read_object_old(FILE * f, struct object *o, dbref objno)
{
    dbref exits, f2, f3, f4, p1, p2;
    int pennies;
    const char *password;

    MUCK::database().clearObject(-1, objno);
    FLAGS(objno) = 0;
    FLAG2(objno) = 0;
    FLAG3(objno) = 0;
    FLAG4(objno) = 0;
    POWERSDB(objno) = 0;
    POWER2DB(objno) = 0;
    NAME(objno) = getstring(f);
    LOADDESC(objno, getstring_oldcomp_noalloc(f));
    o->location = getref(f);
    rawContents[objno] = getref(f);
    exits = getref(f);
    rawNext[objno] = getref(f);
    LOADLOCK(objno, getboolexp(f));
    LOADFAIL(objno, getstring_oldcomp_noalloc(f));
    LOADSUCC(objno, getstring_oldcomp_noalloc(f));
    LOADOFAIL(objno, getstring_oldcomp_noalloc(f));
    LOADOSUCC(objno, getstring_oldcomp_noalloc(f));
    OWNER(objno) = getref(f);
    pennies = getref(f);

    /* timestamps mods */
    o->ts.created = current_systime;
    o->ts.lastused = current_systime;
    o->ts.usecount = 0;
    o->ts.modified = current_systime;
    o->ts.dcreated = -1;
    o->ts.dlastused = -1;
    o->ts.dmodified = -1;


    FLAGS(objno) |= getfref(f, &f2, &f3, &f4, &p1, &p2);
    FLAG2(objno) |= f2;
    FLAG3(objno) |= f3;
    FLAG4(objno) |= f4;
    POWERSDB(objno) |= p1;
    POWER2DB(objno) |= p2;
    /*
     * flags have to be checked for conflict --- if they happen to coincide
     * with chown_ok flags and jump_ok flags, we bump them up to the
     * corresponding HAVEN and ABODE flags
     */
    if (FLAGS(objno) & CHOWN_OK) {
        FLAGS(objno) &= ~CHOWN_OK;
        FLAGS(objno) |= HAVEN;
    }
    if (FLAGS(objno) & JUMP_OK) {
        FLAGS(objno) &= ~JUMP_OK;
        FLAGS(objno) |= ABODE;
    }
    password = getstring(f);
    /* convert GENDER flag to property */
    switch ((FLAGS(objno) & GENDER_MASK) >> GENDER_SHIFT) {
        case GENDER_NEUTER:
            add_property(objno, tp_sex_prop, "neuter", 0);
            break;
        case GENDER_FEMALE:
            add_property(objno, tp_sex_prop, "female", 0);
            break;
        case GENDER_MALE:
            add_property(objno, tp_sex_prop, "male", 0);
            break;
        default:
            break;
    }
    FLAGS(objno) &= ~GENDER_MASK;
    /* For downward compatibility with databases using the */
    /* obsolete ANTILOCK flag. */
    if (FLAGS(objno) & ANTILOCK) {
        LOADLOCK(objno, negate_boolexp(copy_bool(GETLOCK(objno))))
            FLAGS(objno) &= ~ANTILOCK;
    }
    switch (FLAGS(objno) & TYPE_MASK) {
        case TYPE_THING:
            o->sp.thing.home = exits;
            o->sp.thing.value = pennies;
            break;
        case TYPE_ROOM:
            o->sp.room.dropto = o->location;
            o->location = NOTHING;
            rawExits[objno] = exits;
            break;
        case TYPE_EXIT:
            if (o->location == NOTHING) {
                o->sp.exit.ndest = 0;
                o->sp.exit.dest = NULL;
            } else {
                o->sp.exit.ndest = 1;
                o->sp.exit.dest = new dbref[1];

                (o->sp.exit.dest)[0] = o->location;
            }
            o->location = NOTHING;
            break;
        case TYPE_PLAYER:
            o->sp.player.home = exits;
            o->sp.player.pennies = pennies;
            o->sp.player.password = password;
            break;
        case TYPE_GARBAGE:
            OWNER(objno) = NOTHING;

            delete[]NAME(objno);
            NAME(objno) = "<garbage>";
            SETDESC(objno, "<recyclable>");
            break;
    }
}

void
db_read_object_new(FILE * f, struct object *o, dbref objno)
{
    dbref f2, f3, f4, p1, p2;

    int j;

    db_clear_object(-1, objno);
    FLAGS(objno) = 0;
    FLAG2(objno) = 0;
    FLAG3(objno) = 0;
    FLAG4(objno) = 0;
    POWERSDB(objno) = 0;
    POWER2DB(objno) = 0;
    NAME(objno) = getstring(f);
    LOADDESC(objno, getstring_noalloc(f));
    o->location = getref(f);
    rawContents[objno] = getref(f);
    rawNext[objno] = getref(f);
    LOADLOCK(objno, getboolexp(f));
    LOADFAIL(objno, getstring_oldcomp_noalloc(f));
    LOADSUCC(objno, getstring_oldcomp_noalloc(f));
    LOADOFAIL(objno, getstring_oldcomp_noalloc(f));
    LOADOSUCC(objno, getstring_oldcomp_noalloc(f));

    /* timestamps mods */
    o->ts.created = current_systime;
    o->ts.lastused = current_systime;
    o->ts.usecount = 0;
    o->ts.modified = current_systime;
    o->ts.dcreated = -1;
    o->ts.dlastused = -1;
    o->ts.dmodified = -1;

    /* OWNER(objno) = getref(f); */
    /* o->pennies = getref(f); */
    FLAGS(objno) |= getfref(f, &f2, &f3, &f4, &p1, &p2);
    FLAG2(objno) |= f2;
    FLAG3(objno) |= f3;
    FLAG4(objno) |= f4;
    POWERSDB(objno) |= p1;
    POWER2DB(objno) |= p2;
    /*
     * flags have to be checked for conflict --- if they happen to coincide
     * with chown_ok flags and jump_ok flags, we bump them up to the
     * corresponding HAVEN and ABODE flags
     */
    if (FLAGS(objno) & CHOWN_OK) {
        FLAGS(objno) &= ~CHOWN_OK;
        FLAGS(objno) |= HAVEN;
    }
    if (FLAGS(objno) & JUMP_OK) {
        FLAGS(objno) &= ~JUMP_OK;
        FLAGS(objno) |= ABODE;
    }
    /* convert GENDER flag to property */
    switch ((FLAGS(objno) & GENDER_MASK) >> GENDER_SHIFT) {
        case GENDER_NEUTER:
            add_property(objno, tp_sex_prop, "neuter", 0);
            break;
        case GENDER_FEMALE:
            add_property(objno, tp_sex_prop, "female", 0);
            break;
        case GENDER_MALE:
            add_property(objno, tp_sex_prop, "male", 0);
            break;
        default:
            break;
    }
    FLAGS(objno) &= ~GENDER_MASK;

    /* o->password = getstring(f); */
    /* For downward compatibility with databases using the */
    /* obsolete ANTILOCK flag. */
    if (FLAGS(objno) & ANTILOCK) {
        LOADLOCK(objno, negate_boolexp(copy_bool(GETLOCK(objno))))
            FLAGS(objno) &= ~ANTILOCK;
    }
    switch (FLAGS(objno) & TYPE_MASK) {
        case TYPE_THING:
            o->sp.thing.home = getref(f);
            o->exits = getref(f);
            OWNER(objno) = getref(f);
            o->sp.thing.value = getref(f);
            break;
        case TYPE_ROOM:
            o->sp.room.dropto = getref(f);
            o->exits = getref(f);
            OWNER(objno) = getref(f);
            break;
        case TYPE_EXIT:
            o->sp.exit.ndest = getref(f);
            o->sp.exit.dest = new dbref[o->sp.exit.ndest];

            for (j = 0; j < o->sp.exit.ndest; j++) {
                (o->sp.exit.dest)[j] = getref(f);
            }
            OWNER(objno) = getref(f);
            break;
        case TYPE_PLAYER:
            o->sp.player.home = getref(f);
            o->exits = getref(f);
            o->sp.player.pennies = getref(f);
            o->sp.player.password = getstring(f);
            break;
    }
}

#endif /* ARCHAIC_DATABASES */

/* Reads in Foxen, Foxen[234], WhiteFire, Mage or Lachesis DB Formats */
void
db_read_object_foxen(FILE * f, struct object *o, dbref objno, int dtype, int read_before)
{
    dbref f2, f3, f4, p1, p2;

    int tmp, c, prop_flag = 0;

    int j = 0;

    if (read_before) {
        MUCK::database().freeObject(objno);
    }
    MUCK::database().clearObject(-1, objno);

    FLAGS(objno) = 0;
    FLAG2(objno) = 0;
    FLAG3(objno) = 0;
    FLAG4(objno) = 0;
    POWERSDB(objno) = 0;
    POWER2DB(objno) = 0;

    if (verboseload)
        fprintf(stderr, "#%d [object_info] ", objno);

    NAME(objno) = getstring(f);
#ifdef ARCHAIC_DATABASES
    if (dtype <= 3)
        LOADDESC(objno, getstring_oldcomp_noalloc(f));
#endif /* ARCHAIC_DATABASES */

    o->location = getref(f);
    rawContents[objno] = getref(f);
    rawNext[objno] = getref(f);

#ifdef ARCHAIC_DATABASES
    if (dtype < 6)
        LOADLOCK(objno, getboolexp(f));

    if (dtype == 3) {
        if (verboseload)
            fprintf(stderr, "[timestamps v3] ");
        /* Mage timestamps */
        o->ts.created = getref(f);
        o->ts.modified = getref(f);
        o->ts.lastused = getref(f);
        o->ts.usecount = 0;
    }

    if (dtype <= 3) {
        /* Lachesis, WhiteFire, and Mage messages */
        LOADFAIL(objno, getstring_oldcomp_noalloc(f));
        LOADSUCC(objno, getstring_oldcomp_noalloc(f));
        LOADDROP(objno, getstring_oldcomp_noalloc(f));
        LOADOFAIL(objno, getstring_oldcomp_noalloc(f));
        LOADOSUCC(objno, getstring_oldcomp_noalloc(f));
        LOADODROP(objno, getstring_oldcomp_noalloc(f));
    }
#endif /* ARCHAIC_DATABASES */

    if (verboseload)
        fprintf(stderr, "[flags] ");

    tmp = getfref(f, &f2, &f3, &f4, &p1, &p2);

    if (dtype >= 4) {
        tmp &= ~DUMP_MASK;
        f2 &= ~DUM2_MASK;
        f3 &= ~DUM3_MASK;
        f4 &= ~DUM4_MASK;
        p1 &= ~POWERS_DUMP_MASK;
        p2 &= ~POWER2_DUMP_MASK;
    }

    FLAGS(objno) |= tmp;
    FLAG2(objno) |= f2;
    FLAG3(objno) |= f3;
    FLAG4(objno) |= f4;
    POWERSDB(objno) |= p1;
    POWER2DB(objno) |= p2;

    FLAGS(objno) &= ~SAVED_DELTA;

    if (dtype != 3) {
        if (verboseload)
            fprintf(stderr, "[timestamps v4] ");
        /* Foxen and WhiteFire timestamps */
        o->ts.created = gettimestampEx(f, &f2);
        o->ts.dcreated = f2;
        o->ts.lastused = gettimestampEx(f, &f2);
        o->ts.dlastused = f2;
        o->ts.usecount = getref(f);
        o->ts.modified = gettimestampEx(f, &f2);
        o->ts.dmodified = f2;
    }

    c = getc(f);
    if (c == '*') {
        if (verboseload)
            fprintf(stderr, "[properties] ");
        getproperties(f, objno);

        prop_flag++;
    } else {
        /* do our own getref */
        int sign = 0;

        char buf[BUFFER_LEN];

        int i = 0;

        if (c == '-')
            sign = 1;
        else if (c != '+') {
            buf[i] = c;
            i++;
        }
        while ((c = getc(f)) != '\n') {
            buf[i] = c;
            i++;
        }
        buf[i] = '\0';
        j = atol(buf);
        if (sign)
            j = -j;

        /* set gender stuff */
        /* convert GENDER flag to property */
        switch ((FLAGS(objno) & GENDER_MASK) >> GENDER_SHIFT) {
            case GENDER_NEUTER:
                add_property(objno, tp_sex_prop, "neuter", 0);
                break;
            case GENDER_FEMALE:
                add_property(objno, tp_sex_prop, "female", 0);
                break;
            case GENDER_MALE:
                add_property(objno, tp_sex_prop, "male", 0);
                break;
            default:
                break;
        }
    }
    FLAGS(objno) &= ~GENDER_MASK;

    /* o->password = getstring(f); */
    /* For downward compatibility with databases using the */
    /* obsolete ANTILOCK flag. */
    if (FLAGS(objno) & ANTILOCK) {
        LOADLOCK(objno, negate_boolexp(copy_bool(GETLOCK(objno))))
            FLAGS(objno) &= ~ANTILOCK;
    }

    switch (FLAGS(objno) & TYPE_MASK) {
        case TYPE_THING:
            if (verboseload)
                fprintf(stderr, "[type: THING] ");
            o->sp.thing.home = prop_flag ? getref(f) : j;
            rawExits[objno] = getref(f);
            OWNER(objno) = getref(f);
            o->sp.thing.value = getref(f);
            break;
        case TYPE_ROOM:
            if (verboseload)
                fprintf(stderr, "[type: ROOM] ");
            o->sp.room.dropto = prop_flag ? getref(f) : j;
            rawExits[objno] = getref(f);
            OWNER(objno) = getref(f);
            break;
        case TYPE_EXIT:
            if (verboseload)
                fprintf(stderr, "[type: EXIT] ");
            o->sp.exit.ndest = prop_flag ? getref(f) : j;
            if (o->sp.exit.ndest) /* only allocate space for linked exits */
                o->sp.exit.dest = new dbref[o->sp.exit.ndest];

            for (j = 0; j < o->sp.exit.ndest; j++) {
                (o->sp.exit.dest)[j] = getref(f);
            }
            OWNER(objno) = getref(f);
            break;
        case TYPE_PLAYER:
            if (verboseload)
                fprintf(stderr, "[type: PLAYER] ");
            o->sp.player.home = prop_flag ? getref(f) : j;
            rawExits[objno] = getref(f);
            o->sp.player.pennies = getref(f);
            if (MUCK::PasswordHash::enabled) {
                if (db_hash_convert) {
                    // Update legacy untagged raw plaintext to new tagged hex encoded best algorithm
                    char hashbuf[BUFFER_LEN];

                    hashbuf[0] = '\0';
                    const char *p = getstring_noalloc(f);

                    if (!p || !*p) {
                        // Convert blank legacy untagged raw plaintext password to new tagged NONE indicator
                        MUCK::PasswordHash::hash(HTYPE_NONE, hashbuf, NULL, NULL);
                    } else {
                        // Convert legacy untagged raw plaintext password to new tagged hex encoded best algorithm
                        MUCK::PasswordHash::hash(HTYPE_CURRENT, hashbuf, p, NULL);
                    }
                    o->sp.player.password = alloc_string(hashbuf);
                } else {
                    if (MUCK::PasswordHash::version == HVER_NONE) {
                        // Update legacy untagged base64 encoded md5 to new tagged hex encoded unsalted MD5 algorithm
                        char hashbuf[BUFFER_LEN];

                        hashbuf[0] = '\0';
                        const char *p = getstring_noalloc(f);

                        MUCK::PasswordHash::oldConvert(hashbuf, p);
                        o->sp.player.password = alloc_string(hashbuf);
                    } else {
                        // Handle new tagged methods
                        const char *p = getstring_noalloc(f);

                        if (MUCK::PasswordHash::tagToVal(p) == HTYPE_PLAIN) {
                            // Update new tagged plaintext to new tagged hex encoded best algorithm
                            char hashbuf[BUFFER_LEN];

                            hashbuf[0] = '\0';
                            MUCK::PasswordHash::split(p, NULL, hashbuf, NULL);
                            MUCK::PasswordHash::hash(HTYPE_CURRENT, hashbuf, hashbuf, NULL);
                            o->sp.player.password = alloc_string(hashbuf);
                        } else {
                            // Preserve new tagged methods
                            o->sp.player.password = alloc_string(p);
                        }
                    }
                }
            } else {
                if (db_hash_convert) { // This section doesn't need to be here, but is included for robustness
                    // Update legacy untagged raw plaintext to new tagged hex encoded best algorithm
                    char hashbuf[BUFFER_LEN];

                    hashbuf[0] = '\0';
                    const char *p = getstring_noalloc(f);

                    if (!p || !*p) {
                        // Convert blank legacy untagged raw plaintext password to new tagged NONE indicator
                        MUCK::PasswordHash::hash(HTYPE_NONE, hashbuf, NULL, NULL);
                    } else {
                        // Convert legacy untagged raw plaintext password to new tagged hex encoded best algorithm
                        MUCK::PasswordHash::hash(HTYPE_CURRENT, hashbuf, p, NULL);
                    }
                    o->sp.player.password = alloc_string(hashbuf);
                } else {
                    // Preserve legacy untagged raw plaintext
                    o->sp.player.password = getstring(f);
                }
            }
            break;
        case TYPE_PROGRAM:
            if (verboseload)
                fprintf(stderr, "[type: PROGRAM] ");
            OWNER(objno) = getref(f);
            FLAGS(objno) &= ~INTERNAL;
#ifdef ARCHAIC_DATABASES
            if (dtype < 5 && MLevel(objno) == 0)
                SetMLevel(objno, 2);
#endif /* ARCHAIC_DATABASES */
            break;
        case TYPE_GARBAGE:
            delete[]NAME(objno);
            NAME(objno) = "<garbage>";

            if (verboseload)
                fprintf(stderr, "[type: GARBAGE] ");
            break;
    }
    if (verboseload)
        fprintf(stderr, "OK\n");
}

/* Turn the captured flat chains into the owning containment vectors.
 * Same corruption rules as the old save-side chain sanitizer: a
 * member must be valid, must not be its own container, must think it
 * is located in the container, and may appear in only one list; the
 * rest of a chain is dropped at the first bad member, matching how
 * the chain could not be followed further anyway. */
static void
materializeLists(void)
{
    std::unordered_set<int> claimed;
    long dropped = 0;

    for (dbref i = 0; i < MUCK::database().top(); i++) {
        if (Typeof(i) == TYPE_GARBAGE)
            continue;

        for (int pass = 0; pass < 2; pass++) {
            bool exitsList = (pass == 1);
            auto headIt = exitsList ? rawExits.find((int) i)
                                    : rawContents.find((int) i);
            dbref head = (headIt == (exitsList ? rawExits.end()
                                               : rawContents.end()))
                ? NOTHING : headIt->second;
            std::vector<MUCK::DbObject *> &out =
                exitsList ? MUCK::exitsOf(i) : MUCK::contentsOf(i);
            long guard = MUCK::database().top();

            for (dbref m = head; m != NOTHING && guard-- > 0;) {
                if (!MUCK::database().valid(m) || m == i
                    || DBFETCH(m)->location != i
                    || !claimed.insert((int) m).second) {
                    dropped++;
                    break;
                }
                out.push_back(MUCK::database().get(m));

                auto nx = rawNext.find((int) m);

                m = (nx == rawNext.end()) ? NOTHING : nx->second;
            }
        }
    }
    if (dropped)
        log_status("IMPORT: dropped %ld corrupt chain members\n", dropped);
    rawContents.clear();
    rawExits.clear();
    rawNext.clear();
}

/* First-time import of program text from the flat era's muf/<ref>.m
 * files into the source cache, which the object store then persists.
 * This is the ONLY place that reads those files; the server itself
 * has no muf-file fallback. */
static void
importProgramSources(void)
{
    char buf[BUFFER_LEN];
    int loaded = 0;

    for (dbref i = 0; i < MUCK::database().top(); i++) {
        if (Typeof(i) != TYPE_PROGRAM)
            continue;

        FILE *f;

        sprintf(buf, "muf/%d.m", (int) i);
        f = fopen(buf, "rb");
        if (!f)
            continue;

        std::vector<std::string> lines;

        while (fgets(buf, BUFFER_LEN, f)) {
            int len = strlen(buf);

            if (len > 0 && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            lines.push_back(*buf ? buf : " ");
        }
        fclose(f);
        MUCK::programs().setSourceLines(i, std::move(lines));
        loaded++;
    }
    log_status("IMPORT: program text for %d programs from muf/\n", loaded);
}

dbref
MUCK::FlatFileConverter::import(FILE * f)
{
    dbref i, thisref;
    struct object *o;
    const char *special;
    int doing_deltas = 0;
    int main_db_format = 0;
    int parmcnt;
    int dbflags = 0;
    char c;

    db_load_format = 0;

    if ((c = getc(f)) == '*') {
        special = getstring(f);
#ifdef ARCHAIC_DATABASES
        if (!strcmp(special, "**TinyMUCK DUMP Format***")) {
            db_load_format = 1;
        } else if (!strcmp(special, "**Lachesis TinyMUCK DUMP Format***") || !strcmp(special, "**WhiteFire TinyMUCK DUMP Format***")) {
            db_load_format = 2;
        } else if (!strcmp(special, "**Mage TinyMUCK DUMP Format***")) {
            db_load_format = 3;
        } else if (!strcmp(special, "**Foxen TinyMUCK DUMP Format***")) {
            db_load_format = 4;
        } else if (!strcmp(special, "**Foxen2 TinyMUCK DUMP Format***")) {
            db_load_format = 5;
        } else if (!strcmp(special, "**Foxen3 TinyMUCK DUMP Format***")) {
            db_load_format = 6;
        } else if (!strcmp(special, "**Foxen4 TinyMUCK DUMP Format***")) {
            db_load_format = 6;
            i = getref(f);
            MUCK::database().ensureTop(i);
        } else
#endif /* ARCHAIC_DATABASES */
        if (!strcmp(special, "**Foxen5 TinyMUCK DUMP Format***") ||
                !strcmp(special, "**Foxen6 TinyMUCK DUMP Format***") || !strcmp(special, "**Foxen7 TinyMUCK DUMP Format***") || !strcmp(special, "**NeonMuck V2 DUMP Format***")) {
            db_load_format = !strcmp(special, "**Foxen7 TinyMUCK DUMP Format***") ? 8 : 7;
            i = getref(f);
            dbflags = getref(f);
            if (dbflags & DB_PARMSINFO) {
                parmcnt = getref(f);
                tune_load_parms_from_file(f, NOTHING, parmcnt);
            }
            if (dbflags & DB_COMPRESSED) {
#ifdef COMPRESS
                init_compress_from_file(f);
#else
                fprintf(stderr, "This server is not compiled to read compressed databases.\n");
                return -1;
#endif
            }

            if ((MUCK::PasswordHash::enabled = (dbflags & DB_NEWPASSES || db_load_format == 8)))
                db_hash_convert = 0;
            else if (db_hash_convert)
                MUCK::PasswordHash::enabled = 1;
            MUCK::PasswordHash::version = MUCK::PasswordHash::enabled ? ((dbflags & HVER_MASK) >> HVER_SHIFT) : HVER_NONE;

            MUCK::database().ensureTop(i);
#ifdef ARCHAIC_DATABASES
        } else if (!strcmp(special, "***Foxen Deltas Dump Extention***")) {
            db_load_format = 4;
            doing_deltas = 1;
        } else if (!strcmp(special, "***Foxen2 Deltas Dump Extention***")) {
            db_load_format = 5;
            doing_deltas = 1;
        } else if (!strcmp(special, "***Foxen4 Deltas Dump Extention***")) {
            db_load_format = 6;
            doing_deltas = 1;
#endif /* ARCHAIC_DATABASES */
        } else if (!strcmp(special, "***Foxen5 Deltas Dump Extention***") ||
                   !strcmp(special, "***Foxen6 Deltas Dump Extention***") ||
                   !strcmp(special, "***Foxen7 Deltas Dump Extention***") || !strcmp(special, "***NeonMuck V2 Deltas Dump Format***")) {
            db_load_format = !strcmp(special, "***Foxen7 Deltas Dump Extention***") ? 8 : 7;
            doing_deltas = 1;
        }
        if (doing_deltas && MUCK::database().top() == 0) {
            fprintf(stderr, "Can't read a deltas file without a dbfile.\n");
            return -1;
        }
        delete[]special;
        if (!doing_deltas)
            main_db_format = db_load_format;
        c = getc(f);            /* get next char */
    }

    for (i = 0;; i++) {
        switch (c) {
            case NUMBER_TOKEN:
                /* another entry, yawn */
                thisref = getref(f);

#ifndef SANITY
                if (thisref < MUCK::database().top()) {
                    if (doing_deltas && Typeof(thisref) == TYPE_PLAYER) {
                        delete_player(thisref);
                    }
                }
#endif

                /* make space */
                MUCK::database().ensureTop(thisref + 1);

                /* read it in */
                o = DBFETCH(thisref);
#ifdef ARCHAIC_DATABASES
                switch (db_load_format) {
                    case 0:
                        db_read_object_old(f, o, thisref);
                        break;
                    case 1:
                        db_read_object_new(f, o, thisref);
                        break;
                    case 2:
                    case 3:
                    case 4:
                    case 5:
                    case 6:
                    case 7:
                    case 8:
                        db_read_object_foxen(f, o, thisref, db_load_format, doing_deltas);
                        break;
                }
#else /* !ARCHAIC_DATABASES */
                db_read_object_foxen(f, o, thisref, db_load_format, doing_deltas);
#endif /* !ARCHAIC_DATABASES */

                /* every imported object gets a fresh identity */
                MUCK::database().assignUuid(thisref, MUCK::Uuid::generate());

                if (Typeof(thisref) == TYPE_PLAYER) {
                    OWNER(thisref) = thisref;
                    add_player(thisref);
                }
                break;
            case LOOKUP_TOKEN:
                special = getstring(f);
                if (strcmp(special, "**END OF DUMP***")) {
                    delete[]special;
                    return -1;
                } else {
                    delete[]special;
                    special = getstring(f);
#ifdef ARCHAIC_DATABASES
                    if (!special || strcmp(special, "***Foxen Deltas Dump Extention***")) {
                        if (!special || strcmp(special, "***Foxen2 Deltas Dump Extention***")) {
                            if (!special || strcmp(special, "***Foxen4 Deltas Dump Extention***")) {
#endif /* ARCHAIC_DATABASES */
                                if (!special || strcmp(special, "***Foxen5 Deltas Dump Extention***")
                                    || strcmp(special, "***Foxen6 Deltas Dump Extention***")
                                    || strcmp(special, "***Foxen7 Deltas Dump Extention***")
                                    || strcmp(special, "***NeonMuck V2 Deltas Dump Format***")) {
                                    if (special)
                                        delete[]special;
                                    if ((main_db_format == 7 || main_db_format == 8)
                                        && (dbflags & DB_PARMSINFO)) {
                                        rewind(f);
                                        delete[]getstring(f);
                                        getref(f);
                                        getref(f);
                                        parmcnt = getref(f);
                                        tune_load_parms_from_file(f, NOTHING, parmcnt);
                                    }
                                    for (i = 0; i < MUCK::database().top(); i++) {
                                        if (Typeof(i) == TYPE_GARBAGE) {
                                            MUCK::database().noteHole(i);
                                        }
                                    }
                                    if (MUCK::PasswordHash::enabled)
                                        MUCK::PasswordHash::version = HVER_CURRENT;
                                    else
                                        MUCK::PasswordHash::version = HVER_NONE;
                                    materializeLists();
                                    importProgramSources();
                                    autostart_progs();
                                    return MUCK::database().top();
                                } else {
                                    delete[]special;
                                    db_load_format = !strcmp(special, "***Foxen7 Deltas Dump Extention***")
                                        ? 8 : 7;
                                    doing_deltas = 1;
                                }
#ifdef ARCHAIC_DATABASES
                            } else {
                                delete[]special;
                                db_load_format = 6;
                                doing_deltas = 1;
                            }
                        } else {
                            delete[]special;
                            db_load_format = 5;
                            doing_deltas = 1;
                        }
                    } else {
                        delete[]special;
                        db_load_format = 4;
                        doing_deltas = 1;
                    }
#endif /* ARCHAIC_DATABASES */
                }
                break;
            default:
                return -1;
                /* break; */
        }
        c = getc(f);
    }                           /* for */
}                               /* db_read */
