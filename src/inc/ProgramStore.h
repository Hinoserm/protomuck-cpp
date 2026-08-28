#ifndef MUCK_PROGRAMSTORE_H
#define MUCK_PROGRAMSTORE_H

/* MUF program text storage: programs live outside the database proper as
 * muf/<ref>.m files, loaded on demand and written back from the editor.
 * Extracted from Database.cpp.
 */

#include <cstdio>

namespace MUCK {

class ProgramStore {
  public:
    struct line *read(dbref i);
    void write(struct line *first, dbref i);
    void logText(struct line *first, dbref player, dbref i);
    struct line *newLine();
};

extern ProgramStore g_programStore;

inline ProgramStore &programs() { return g_programStore; }

} /* namespace MUCK */

#endif /* MUCK_PROGRAMSTORE_H */
