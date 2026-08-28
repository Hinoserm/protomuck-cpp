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

/* Reading helpers that remain in Database.cpp (shared with the macro
 * file loader) */
extern dbref getref(FILE *);
extern int getfref(FILE *, dbref *, dbref *, dbref *, dbref *, dbref *);
extern int gettimestampEx(FILE *, dbref *);
extern const char *getstring_noalloc(FILE *);
extern void getproperties(FILE *, dbref);
extern int number(const char *);
extern int ifloat(const char *);
extern void autostart_progs(void);
extern int db_load_format;
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
    o->contents = getref(f);
    exits = getref(f);
    o->next = getref(f);
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
            o->exits = NOTHING;
            o->sp.thing.value = pennies;
            break;
        case TYPE_ROOM:
            o->sp.room.dropto = o->location;
            o->location = NOTHING;
            o->exits = exits;
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
            o->exits = NOTHING;
            o->sp.player.pennies = pennies;
            o->sp.player.password = password;
            o->sp.player.curr_prog = NOTHING;
            o->sp.player.insert_mode = 0;
            o->sp.player.descrs = NULL;
            o->sp.player.descr_count = 0;
            break;
        case TYPE_GARBAGE:
            OWNER(objno) = NOTHING;
            o->next = recyclable;
            recyclable = objno;

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
    o->contents = getref(f);
    o->next = getref(f);
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
            o->sp.player.curr_prog = NOTHING;
            o->sp.player.insert_mode = 0;
            o->sp.player.descrs = NULL;
            o->sp.player.descr_count = 0;
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
    o->contents = getref(f);
    o->next = getref(f);

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
            o->exits = getref(f);
            OWNER(objno) = getref(f);
            o->sp.thing.value = getref(f);
            break;
        case TYPE_ROOM:
            if (verboseload)
                fprintf(stderr, "[type: ROOM] ");
            o->sp.room.dropto = prop_flag ? getref(f) : j;
            o->exits = getref(f);
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
            o->exits = getref(f);
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
            o->sp.player.curr_prog = NOTHING;
            o->sp.player.insert_mode = 0;
            o->sp.player.descrs = NULL;
            o->sp.player.descr_count = 0;
#ifdef IGNORE_SUPPORT
            o->sp.player.ignoretime = 0;
#endif /* IGNORE_SUPPORT */
            break;
        case TYPE_PROGRAM:
            if (verboseload)
                fprintf(stderr, "[type: PROGRAM] ");
            OWNER(objno) = getref(f);
            FLAGS(objno) &= ~INTERNAL;
            o->sp.program.curr_line = 0;
            o->sp.program.first = 0;
            o->sp.program.code = 0;
            o->sp.program.siz = 0;
            o->sp.program.start = 0;
            o->sp.program.pubs = 0;
            o->sp.program.proftime.tv_sec = 0;
            o->sp.program.proftime.tv_usec = 0;
            o->sp.program.profstart = 0;
            o->sp.program.profuses = 0;
            o->sp.program.fprofile = NULL;
            o->sp.program.instances = 0;

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
            MUCK::database().growTo(i);
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

            MUCK::database().growTo(i);
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
        if (doing_deltas && !MUCK::database().rawArray()) {
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
                MUCK::database().growTo(thisref + 1);

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
                                            DBFETCH(i)->next = MUCK::database().recycleHead();
                                            MUCK::database().setRecycleHead(i);
                                        }
                                    }
                                    if (MUCK::PasswordHash::enabled)
                                        MUCK::PasswordHash::version = HVER_CURRENT;
                                    else
                                        MUCK::PasswordHash::version = HVER_NONE;
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
