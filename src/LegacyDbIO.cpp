#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "Modules.h"
#include "strutils.h"

/* LEGACY DATABASE HELPERS.
 *
 * Free functions that predate the class model, evicted here from
 * Database.cpp so the engine file holds only the engine. Users: the
 * macro file loader (getref, putref), the MUF editor and interpreter
 * (line lists), player input parsing (parse_dbref), property loading
 * (number, ifloat), and boot (autostart_progs). Each migrates into
 * its proper class in a later step 2 section; nothing NEW may call
 * these. The permission checks (RawMWLevel, WLevel) moved to
 * DbObject::muckerLevel and DbObject::wizLevel.
 */

#ifndef MALLOC_PROFILING
extern char *alloc_string(const char *);
#endif

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


void
autostart_progs(void)
{
    dbref i;
    struct line *tmp;

    if (db_conversion_flag) {
        return;
    }

    for (i = 0; i < MUCK::database().top(); i++) {
        if (Typeof(i) == TYPE_PROGRAM) {
            if ((FLAGS(i) & ABODE) && TMage(OWNER(i))) {
                /* pre-compile AUTOSTART programs. */
                /* They queue up when they finish compiling. */
                MUCK::ProgramRuntime &rt = MUCK::programRuntime(i);
                tmp = rt.first;
                rt.first = (struct line *) MUCK::programs().read(i);
                do_compile(-1, OWNER(i), i, 0);
                free_prog_text(rt.first);
                rt.first = tmp;
            }
        }
    }
}


