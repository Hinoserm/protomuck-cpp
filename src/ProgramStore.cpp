#include "copyright.h"
#include "config.h"
#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "ProgramStore.h"

namespace MUCK {

ProgramStore g_programStore;

} /* namespace MUCK */

void
MUCK::ProgramStore::logText(struct line *first, dbref player, dbref i)
{
    FILE *f;
    char fname[BUFFER_LEN], buf1[BUFFER_LEN], buf2[BUFFER_LEN];
    time_t lt = current_systime;

#ifndef SANITY
    strcpy(fname, PROGRAM_LOG);
    f = fopen(fname, "ab");
    if (!f) {
        log_status("Couldn't open file %s!\n", fname);
        return;
    }

    fprintf(f, "{{{ PROGRAM %s, SAVED AT %s BY %s\n", strcpy(buf1, unparse_object(player, i)), ctime(&lt), strcpy(buf2, unparse_object(player, player))
        );

    while (first) {
        if (!first->this_line)
            continue;
        fputs(first->this_line, f);
        fputc('\n', f);
        first = first->next;
    }
    fputs("\n\n\n", f);
    fclose(f);
#endif
}

void
MUCK::ProgramStore::write(struct line *first, dbref i)
{
    FILE *f;
    char fname[BUFFER_LEN];

    sprintf(fname, "muf/%d.m", (int) i);
    f = fopen(fname, "wb");
    if (!f) {
        log_status("Couldn't open file %s!\n", fname);
        return;
    }
    while (first) {
        if (!first->this_line)
            continue;
        if (fputs(first->this_line, f) == EOF) {
            fprintf(stderr, "PANIC: Unable to write to db file.\n");
            abort();
        }
        if (fputc('\n', f) == EOF) {
            fprintf(stderr, "PANIC: Unable to write to db file.\n");
            abort();
        }
        first = first->next;
    }
    fclose(f);
}

struct line *
MUCK::ProgramStore::newLine()
{
    struct line *nw;

    nw = new line;

    if (!nw) {
        fprintf(stderr, "get_new_line(): Out of memory!\n");
        abort();
    }
    nw->this_line = NULL;
    nw->next = NULL;
    nw->prev = NULL;

    return nw;
}

struct line *
MUCK::ProgramStore::read(dbref i)
{
    char buf[BUFFER_LEN];
    struct line *first;
    struct line *prev = NULL;
    struct line *nw;
    FILE *f;
    int len;

    first = NULL;
    sprintf(buf, "muf/%d.m", (int) i);
    f = fopen(buf, "rb");
    if (!f)
        return 0;

    while (fgets(buf, BUFFER_LEN, f)) {
        nw = newLine();
        len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
        }
        if (!*buf)
            strcpy(buf, " ");
        nw->this_line = alloc_string(buf);
        if (!first) {
            prev = nw;
            first = nw;
        } else {
            prev->next = nw;
            nw->prev = prev;
            prev = nw;
        }
    }

    fclose(f);
    return first;
}
