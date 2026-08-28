#ifndef MUCK_UUID_H
#define MUCK_UUID_H

/* UUID value type. Objects are identified by version 7 UUIDs: the top 48
 * bits are a millisecond Unix timestamp, so ids sort by creation time and
 * short prefixes of contemporaneous objects stay distinct.
 *
 * See docs/DATABASE.txt section 1.
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <functional>

namespace MUCK {

class Uuid {
  public:
    Uuid() { memset(b_, 0, sizeof(b_)); }

    /* Mint a fresh version 7 UUID from the current time and entropy. */
    static Uuid generate();

    /* Parse canonical 36-character form (hyphens required). Returns the
     * nil UUID on malformed input; check isNil() or use tryParse. */
    static Uuid parse(const char *text);
    static bool tryParse(const char *text, Uuid &out);

    /* "018f3a2b-7c41-7e39-9f2a-5b8c1d0e4f77" */
    std::string toString() const;

    /* First n hex digits (default 8), the display short form. */
    std::string shortString(int digits = 8) const;

    /* True if this UUID's hex form starts with the given prefix; used
     * for git-style short-form resolution. Prefix is hex digits only,
     * hyphens in the input are ignored. */
    bool matchesPrefix(const char *prefix) const;

    bool isNil() const;

    const unsigned char *bytes() const { return b_; }
    void setBytes(const unsigned char *src) { memcpy(b_, src, 16); }

    bool operator==(const Uuid &o) const { return memcmp(b_, o.b_, 16) == 0; }
    bool operator!=(const Uuid &o) const { return !(*this == o); }
    bool operator<(const Uuid &o) const { return memcmp(b_, o.b_, 16) < 0; }

  private:
    unsigned char b_[16];
};

} /* namespace MUCK */

/* Hash support so Uuid can key unordered_map. */
template <>
struct std::hash<MUCK::Uuid> {
    size_t operator()(const MUCK::Uuid &u) const {
        size_t h;
        memcpy(&h, u.bytes(), sizeof(h));
        return h;
    }
};

#endif /* MUCK_UUID_H */
