#ifndef MUCK_FLATFILECONVERTER_H
#define MUCK_FLATFILECONVERTER_H

/* One-way importer for the legacy NeonMuck V2 flat-file database (and
 * the archaic formats before it). This is the only code that can read
 * the old format, and nothing can write it: the object store is the
 * sole write path. Every imported object is minted a fresh UUID.
 *
 * See docs/DATABASE.txt sections 6 and 8.
 */

#include <cstdio>

namespace MUCK {

class FlatFileConverter {
  public:
    /* Read an entire flat dump into the database. Returns the top
     * dbref, or -1 on parse failure. */
    dbref import(FILE *f);
};

extern FlatFileConverter g_flatConverter;

inline FlatFileConverter &flatConverter() { return g_flatConverter; }

} /* namespace MUCK */

#endif /* MUCK_FLATFILECONVERTER_H */
