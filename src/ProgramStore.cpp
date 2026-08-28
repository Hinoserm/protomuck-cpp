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
     * with the object. */
    std::vector<std::string> lines;

    for (struct line *l = first; l; l = l->next)
        if (l->this_line)
            lines.push_back(l->this_line);
    setSourceLines(i, std::move(lines));
    DBDIRTY(i);
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
    /* Serve from the source cache only; the cache is populated by the
     * object store at load and by write(). No file fallback. */
    auto it = source_.find(i);

    if (it == source_.end())
        return 0;

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
        /* one more look through read() for symmetry; no fallback */
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
