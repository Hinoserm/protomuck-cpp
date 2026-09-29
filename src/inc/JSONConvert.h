#ifndef MUCK_JSONCONVERT_H
#define MUCK_JSONCONVERT_H

/* JSON text <-> MUF stack values, for the websocket sideband and the
 * JSON_TO_ARRAY / ARRAY_TO_JSON primitives.
 *
 * The conversion is strict: a value with no faithful counterpart on
 * the other side throws rather than being guessed at. Two mappings are
 * deliberate exceptions, because they are the conventions MUF already
 * lives by: JSON true/false become the integers 1/0, and an integer
 * dictionary key becomes its decimal string on the way out (JSON
 * object keys can only be strings).
 *
 *   JSON -> MUF                     MUF -> JSON
 *   object   dictionary             dictionary  object (string and
 *   array    list                                integer keys only)
 *   string   string                 list        array
 *   integer  integer (64-bit;       string      string
 *            anything wider throws) integer     number
 *   float    float                  float       number (NaN and
 *   bool     1 / 0                               infinity throw)
 *   null     throws                 anything else throws (dbref,
 *                                   lock, address, variable, ...)
 *
 * Everything that crosses to or from a user is 7-bit ASCII. Non-ASCII
 * text arriving in JSON becomes '?' (one per character, not one per
 * byte), and so does any high-bit byte in a MUF string on its way out.
 * An embedded NUL becomes '?' too: MUF strings are NUL-terminated at
 * too many call sites for one to survive.
 *
 * Limits, all checked while the work is being done and never after:
 * the byte limit passed in (callers use json_max_len), 32 levels of
 * nesting, 10000 values in total, and BUFFER_LEN for any single string
 * that becomes a MUF string. Every error names the path to the value
 * that caused it, e.g. "$.items[3]: null has no MUF equivalent".
 */

#include <cstddef>
#include <stdexcept>
#include <string>

#include "config.h"             /* json */

struct inst;

namespace MUCK {

class JSONConvert {
  public:
    class Error : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };

    static const int kMaxDepth = 32;
    static const size_t kMaxValues = 10000;

    /* json_max_len, in bytes */
    static size_t maxBytes();

    /* Parse JSON text within the limits above. The byte limit is
     * checked before parsing begins; depth, value count and integer
     * range are checked as each value is read, so a hostile document
     * is refused before it has been built in memory. */
    static json parse(const std::string &text, size_t limit);

    /* JSON value -> one MUF stack value. The caller owns *out and must
     * CLEAR it. On a throw, *out is left cleared and nothing leaks. */
    static void toInst(const json &j, struct inst *out);

    /* One MUF stack value -> JSON value. The byte limit bounds the
     * text it will serialize to, counted as the value is walked. */
    static json fromInst(struct inst *in, size_t limit);

    /* Serialize, and refuse the result if it exceeds the limit. */
    static std::string dump(const json &j, size_t limit);
};

/* 7-bit ASCII filters for anything that crosses to or from a user.
 * fromBytes replaces every high-bit byte with '?' (MUCK-side text,
 * which may hold legacy 8-bit data). fromUTF8 replaces every non-ASCII
 * UTF-8 character with a single '?' (client-side text), and any byte
 * that is not part of a valid UTF-8 sequence with its own '?'. */
std::string ASCIIFromBytes(const std::string &in);
std::string ASCIIFromUTF8(const std::string &in);

} /* namespace MUCK */

#endif /* MUCK_JSONCONVERT_H */
