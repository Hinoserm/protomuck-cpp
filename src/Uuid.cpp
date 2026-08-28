#include <cstdio>
#include <ctime>
#include <sys/random.h>

#include "inc/Uuid.h"

namespace MUCK {

Uuid
Uuid::generate()
{
    Uuid u;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    /* 48-bit big-endian millisecond timestamp */
    u.b_[0] = (ms >> 40) & 0xff;
    u.b_[1] = (ms >> 32) & 0xff;
    u.b_[2] = (ms >> 24) & 0xff;
    u.b_[3] = (ms >> 16) & 0xff;
    u.b_[4] = (ms >> 8) & 0xff;
    u.b_[5] = ms & 0xff;

    /* 74 bits of entropy; getrandom cannot meaningfully fail here, but
     * fall back to clock jitter rather than mint a low-entropy id. */
    if (getrandom(u.b_ + 6, 10, 0) != 10) {
        for (int i = 6; i < 16; i++)
            u.b_[i] = (unsigned char) (ts.tv_nsec >> ((i - 6) * 3));
    }

    u.b_[6] = (u.b_[6] & 0x0f) | 0x70;  /* version 7 */
    u.b_[8] = (u.b_[8] & 0x3f) | 0x80;  /* RFC 4122 variant */
    return u;
}

static int
hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool
Uuid::tryParse(const char *text, Uuid &out)
{
    /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    static const int hyphens[] = { 8, 13, 18, 23 };
    unsigned char buf[16];
    int i = 0, nibble = 0, h = 0;

    if (!text)
        return false;
    for (const char *p = text; *p; p++, i++) {
        if (h < 4 && i == hyphens[h]) {
            if (*p != '-')
                return false;
            h++;
            continue;
        }
        int v = hexval(*p);
        if (v < 0 || nibble >= 32)
            return false;
        if (nibble % 2 == 0)
            buf[nibble / 2] = (unsigned char) (v << 4);
        else
            buf[nibble / 2] |= (unsigned char) v;
        nibble++;
    }
    if (i != 36 || nibble != 32)
        return false;
    out.setBytes(buf);
    return true;
}

Uuid
Uuid::parse(const char *text)
{
    Uuid u;

    if (!tryParse(text, u))
        return Uuid();
    return u;
}

std::string
Uuid::toString() const
{
    char buf[37];

    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b_[0], b_[1], b_[2], b_[3], b_[4], b_[5], b_[6], b_[7],
             b_[8], b_[9], b_[10], b_[11], b_[12], b_[13], b_[14], b_[15]);
    return std::string(buf);
}

std::string
Uuid::shortString(int digits) const
{
    std::string full = toString();
    std::string out;

    for (char c : full) {
        if (c == '-')
            continue;
        out += c;
        if ((int) out.size() >= digits)
            break;
    }
    return out;
}

bool
Uuid::matchesPrefix(const char *prefix) const
{
    std::string full = toString();
    size_t fi = 0;

    if (!prefix || !*prefix)
        return false;
    for (const char *p = prefix; *p; p++) {
        if (*p == '-')
            continue;
        while (fi < full.size() && full[fi] == '-')
            fi++;
        if (fi >= full.size())
            return false;
        int a = hexval(*p), b = hexval(full[fi]);
        if (a < 0 || a != b)
            return false;
        fi++;
    }
    return true;
}

bool
Uuid::isNil() const
{
    for (int i = 0; i < 16; i++)
        if (b_[i])
            return false;
    return true;
}

} /* namespace MUCK */
