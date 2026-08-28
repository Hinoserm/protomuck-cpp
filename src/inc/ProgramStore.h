#ifndef MUCK_PROGRAMSTORE_H
#define MUCK_PROGRAMSTORE_H

/* MUF program text storage: programs live outside the database proper as
 * muf/<ref>.m files, loaded on demand and written back from the editor.
 * Extracted from Database.cpp.
 */

#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

namespace MUCK {

class ProgramStore {
  public:
    struct line *read(dbref i);
    void write(struct line *first, dbref i);
    void logText(struct line *first, dbref player, dbref i);
    struct line *newLine();

    /* Source cache: the authoritative in-memory copy of program text.
     * Populated from the object store at load (or lazily from legacy
     * muf/<ref>.m files), updated by write(), serialized by the
     * ObjectStore. Returns null when a program has no source. */
    const std::vector<std::string> *sourceLines(dbref i);
    void setSourceLines(dbref i, std::vector<std::string> lines);
    void dropSource(dbref i);

  private:
    std::unordered_map<dbref, std::vector<std::string> > source_;
};

extern ProgramStore g_programStore;

inline ProgramStore &programs() { return g_programStore; }

} /* namespace MUCK */

#endif /* MUCK_PROGRAMSTORE_H */
