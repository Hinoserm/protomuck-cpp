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
    /* The source cache is authoritative; the object store persists it
     * with the object. The legacy muf/<ref>.m file is refreshed too so
     * a flat-era installation stays consistent until conversion. */
    std::vector<std::string> lines;

    for (struct line *l = first; l; l = l->next)
        if (l->this_line)
            lines.push_back(l->this_line);
    setSourceLines(i, std::move(lines));
    DBDIRTY(i);

    FILE *f;
    char fname[BUFFER_LEN];

    sprintf(fname, "muf/%d.m", (int) i);
    f = fopen(fname, "wb");
    if (!f) {
        log_status("Couldn't open file %s!\n", fname);
        return;
    }
    for (const auto &ln : source_[i]) {
        if (fputs(ln.c_str(), f) == EOF || fputc('\n', f) == EOF) {
            fprintf(stderr, "PANIC: Unable to write program text.\n");
            abort();
        }
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
    /* Serve from the source cache, filling it from the legacy
     * muf/<ref>.m file on first miss (flat-era installations). */
    auto it = source_.find(i);

    if (it == source_.end()) {
        char buf[BUFFER_LEN];
        std::vector<std::string> lines;
        FILE *f;
        int len;

        sprintf(buf, "muf/%d.m", (int) i);
        f = fopen(buf, "rb");
        if (!f)
            return 0;
        while (fgets(buf, BUFFER_LEN, f)) {
            len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            lines.push_back(*buf ? buf : " ");
        }
        fclose(f);
        it = source_.emplace(i, std::move(lines)).first;
    }

    struct line *first = NULL;
    struct line *prev = NULL;

    for (const auto &ln : it->second) {
        struct line *nw = newLine();

        nw->this_line = alloc_string(ln.empty() ? " " : ln.c_str());
        if (!first) {
            prev = nw;
            first = nw;
        } else {
            prev->next = nw;
            nw->prev = prev;
            prev = nw;
        }
    }
    return first;
}

const std::vector<std::string> *
MUCK::ProgramStore::sourceLines(dbref i)
{
    auto it = source_.find(i);

    if (it == source_.end()) {
        /* try the legacy file through read(), which fills the cache */
        struct line *l = read(i);

        free_prog_text(l);
        it = source_.find(i);
        if (it == source_.end())
            return NULL;
    }
    return &it->second;
}

void
MUCK::ProgramStore::setSourceLines(dbref i, std::vector<std::string> lines)
{
    source_[i] = std::move(lines);
}

void
MUCK::ProgramStore::dropSource(dbref i)
{
    source_.erase(i);
}
