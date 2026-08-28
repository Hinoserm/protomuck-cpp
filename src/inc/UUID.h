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

class UUID {
  public:
    /* Wire size in bytes. */
    static const size_t SIZE = 16;

    UUID() { memset(b_, 0, sizeof(b_)); }

    /* Mint a fresh version 7 UUID from the current time and entropy. */
    static UUID generate();

    /* The all-zeros UUID. */
    static UUID nil() { return UUID(); }

    /* --- from strings --- */

    /* Parse the canonical 36-character hyphenated form or the bare
     * 32-hex form. Returns the nil UUID on malformed input; check
     * isNil() or use tryParse. */
    static UUID parse(const char *text);
    static UUID parse(const std::string &text) { return parse(text.c_str()); }
    static bool tryParse(const char *text, UUID &out);
    static bool tryParse(const std::string &text, UUID &out) {
        return tryParse(text.c_str(), out);
    }

    /* --- to strings --- */

    /* "018f3a2b-7c41-7e39-9f2a-5b8c1d0e4f77" */
    std::string toString() const;

    /* First n hex digits (default 8), the display short form. */
    std::string shortString(int digits = 8) const;

    /* True if this UUID's hex form starts with the given prefix; used
     * for git-style short-form resolution. Prefix is hex digits only,
     * hyphens in the input are ignored. */
    bool matchesPrefix(const char *prefix) const;

    /* --- from/to binary --- */

    static UUID fromBytes(const void *src) {
        UUID u;
        memcpy(u.b_, src, SIZE);
        return u;
    }
    void toBytes(void *dst) const { memcpy(dst, b_, SIZE); }
    const unsigned char *bytes() const { return b_; }
    void setBytes(const unsigned char *src) { memcpy(b_, src, SIZE); }

    /* --- state and comparison --- */

    bool isNil() const;
    explicit operator bool() const { return !isNil(); }

    bool operator==(const UUID &o) const { return memcmp(b_, o.b_, SIZE) == 0; }
    bool operator!=(const UUID &o) const { return !(*this == o); }
    bool operator<(const UUID &o) const { return memcmp(b_, o.b_, SIZE) < 0; }

  private:
    unsigned char b_[SIZE];
};

} /* namespace MUCK */

/* Hash support so UUID can key unordered_map. */
template <>
struct std::hash<MUCK::UUID> {
    size_t operator()(const MUCK::UUID &u) const {
        size_t h;
        memcpy(&h, u.bytes(), sizeof(h));
        return h;
    }
};

#endif /* MUCK_UUID_H */
