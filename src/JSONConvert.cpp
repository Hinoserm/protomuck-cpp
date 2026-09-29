#include "copyright.h"
#include "config.h"
#include "db.h"
#include "interp.h"
#include "tune.h"
#include "JSONConvert.h"

#include <cmath>
#include <cstdint>

/* See JSONConvert.h for the mapping and the limits. */

namespace MUCK {

size_t
JSONConvert::maxBytes()
{
    long kb = tp_json_max_len;

    /* a zero or negative @tune would otherwise refuse every packet,
     * including the one an admin needs to fix it from a web client */
    if (kb < 1)
        kb = 1;
    return (size_t) kb * 1024;
}

/* ------------------------------------------------------------------ */
/* 7-bit ASCII filters                                                */
/* ------------------------------------------------------------------ */

std::string
ASCIIFromBytes(const std::string &in)
{
    std::string out(in);

    for (char &c : out)
        if ((unsigned char) c >= 0x80 || c == '\0')
            c = '?';
    return out;
}

std::string
ASCIIFromUTF8(const std::string &in)
{
    std::string out;
    size_t i = 0, n = in.size();

    out.reserve(n);
    while (i < n) {
        unsigned char c = (unsigned char) in[i];

        if (c < 0x80) {
            out.push_back(c == '\0' ? '?' : (char) c);
            i++;
            continue;
        }

        /* One '?' per CHARACTER, not per byte, so "cafe" with an
         * accent reads "caf?" rather than "caf??". A byte that does not
         * start a well-formed sequence stands alone and gets its own. */
        size_t len = (c >= 0xC2 && c <= 0xDF) ? 2
            : (c >= 0xE0 && c <= 0xEF) ? 3
            : (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
        bool whole = len > 1 && i + len <= n;

        for (size_t k = 1; whole && k < len; k++)
            if (((unsigned char) in[i + k] & 0xC0) != 0x80)
                whole = false;
        out.push_back('?');
        i += whole ? len : 1;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

namespace {

/* A SAX front end for the library's own DOM builder: the same result
 * as json::parse, but the depth, value-count and integer-range limits
 * are enforced as each value arrives. Checking them on the finished
 * DOM would mean a 1MB document of "[[[[..." or "[0,0,0,..." had
 * already been built in memory before being refused. */
class LimitedSAX {
  public:
    using number_integer_t = json::number_integer_t;
    using number_unsigned_t = json::number_unsigned_t;
    using number_float_t = json::number_float_t;
    using string_t = json::string_t;

    explicit LimitedSAX(json &root) : dom_(root, true) {}

    bool null() { value(); return dom_.null(); }
    bool boolean(bool v) { value(); return dom_.boolean(v); }
    bool number_integer(number_integer_t v) { value(); return dom_.number_integer(v); }
    bool string(string_t &v) { value(); return dom_.string(v); }
    bool key(string_t &k) { return dom_.key(k); }

    bool number_unsigned(number_unsigned_t v)
    {
        if (v > (number_unsigned_t) INT64_MAX)
            throw JSONConvert::Error("integer " + std::to_string(v)
                                     + " does not fit a MUF integer");
        value();
        return dom_.number_unsigned(v);
    }

    bool number_float(number_float_t v, const string_t &raw)
    {
        /* An integer literal too wide for 64 bits reaches us as a
         * float. Accepting it would quietly change both its type and
         * its value, so an integer that does not fit is an error. */
        if (raw.find_first_of(".eE") == string_t::npos)
            throw JSONConvert::Error("integer " + raw
                                     + " does not fit a MUF integer");
        value();
        return dom_.number_float(v, raw);
    }

    bool start_object(std::size_t n) { value(); enter(); return dom_.start_object(n); }
    bool end_object() { depth_--; return dom_.end_object(); }
    bool start_array(std::size_t n) { value(); enter(); return dom_.start_array(n); }
    bool end_array() { depth_--; return dom_.end_array(); }

    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &ex)
    {
        throw JSONConvert::Error(std::string("invalid JSON (") + ex.what()
                                 + ")");
    }

  private:
    void value()
    {
        if (++values_ > JSONConvert::kMaxValues)
            throw JSONConvert::Error("JSON holds more than "
                                     + std::to_string(JSONConvert::kMaxValues)
                                     + " values");
    }

    void enter()
    {
        if (++depth_ > JSONConvert::kMaxDepth)
            throw JSONConvert::Error("JSON is nested deeper than "
                                     + std::to_string(JSONConvert::kMaxDepth)
                                     + " levels");
    }

    nlohmann::detail::json_sax_dom_parser<json> dom_;
    size_t values_ = 0;
    int depth_ = 0;
};

} /* namespace */

json
JSONConvert::parse(const std::string &text, size_t limit)
{
    if (text.size() > limit)
        throw Error("JSON of " + std::to_string(text.size())
                    + " bytes exceeds the limit of " + std::to_string(limit));

    json root;
    LimitedSAX sax(root);

    if (!json::sax_parse(text, &sax))
        throw Error("invalid JSON");
    return root;
}

/* ------------------------------------------------------------------ */
/* JSON -> MUF                                                        */
/* ------------------------------------------------------------------ */

namespace {

void
makeString(struct inst *out, const std::string &utf8, const std::string &path)
{
    std::string s = ASCIIFromUTF8(utf8);

    if (s.size() > BUFFER_LEN - 1)
        throw JSONConvert::Error(path + ": a string of "
                                 + std::to_string(s.size())
                                 + " bytes exceeds the MUF string limit");
    out->type = PROG_STRING;
    out->data.string = alloc_prog_string(s.c_str());    /* NULL if empty */
}

void
toInstAt(const json &j, struct inst *out, std::string &path)
{
    switch (j.type()) {
        case json::value_t::null:
            throw JSONConvert::Error(path + ": null has no MUF equivalent");

        case json::value_t::boolean:
            out->type = PROG_INTEGER;
            out->data.number = j.get<bool>() ? 1 : 0;
            return;

        case json::value_t::number_integer:
            out->type = PROG_INTEGER;
            out->data.number = (MUFINT) j.get<int64_t>();
            return;

        case json::value_t::number_unsigned: {
            uint64_t u = j.get<uint64_t>();

            if (u > (uint64_t) INT64_MAX)
                throw JSONConvert::Error(path + ": integer "
                                         + std::to_string(u)
                                         + " does not fit a MUF integer");
            out->type = PROG_INTEGER;
            out->data.number = (MUFINT) u;
            return;
        }

        case json::value_t::number_float:
            out->type = PROG_FLOAT;
            out->data.fnumber = j.get<double>();
            return;

        case json::value_t::string:
            makeString(out, j.get_ref<const std::string &>(), path);
            return;

        case json::value_t::array: {
            struct inst holder;

            holder.type = PROG_ARRAY;
            holder.data.array = new_array_packed(0, 0);
            try {
                size_t i = 0;

                for (const auto &el : j) {
                    struct inst v;
                    size_t mark = path.size();

                    path += "[" + std::to_string(i++) + "]";
                    toInstAt(el, &v, path);
                    path.resize(mark);
                    array_appenditem(&holder.data.array, &v);
                    CLEAR(&v);
                }
            } catch (...) {
                CLEAR(&holder);
                throw;
            }
            *out = holder;
            return;
        }

        case json::value_t::object: {
            struct inst holder;

            holder.type = PROG_ARRAY;
            holder.data.array = new_array_dictionary();
            try {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    struct inst k, v;
                    size_t mark = path.size();

                    /* the path ends up in an error a user reads, so it
                     * obeys the 7-bit rule like everything else */
                    path += "." + ASCIIFromUTF8(it.key());
                    /* keys stay strings: "1" is the string "1" */
                    makeString(&k, it.key(), path);
                    try {
                        toInstAt(it.value(), &v, path);
                    } catch (...) {
                        CLEAR(&k);
                        throw;
                    }
                    path.resize(mark);
                    array_setitem(&holder.data.array, &k, &v);
                    CLEAR(&k);
                    CLEAR(&v);
                }
            } catch (...) {
                CLEAR(&holder);
                throw;
            }
            *out = holder;
            return;
        }

        default:
            throw JSONConvert::Error(path + ": value has no MUF equivalent");
    }
}

} /* namespace */

void
JSONConvert::toInst(const json &j, struct inst *out)
{
    std::string path = "$";

    toInstAt(j, out, path);
}

/* ------------------------------------------------------------------ */
/* MUF -> JSON                                                        */
/* ------------------------------------------------------------------ */

namespace {

/* Running totals while a MUF value is walked. bytes approximates the
 * serialized size closely enough to stop a runaway build early; the
 * exact size is checked again on the dumped text. */
struct Budget {
    size_t limit;
    size_t bytes = 0;
    size_t values = 0;

    void add(size_t n, const std::string &path)
    {
        bytes += n;
        if (bytes > limit)
            throw JSONConvert::Error(path + ": JSON would exceed the limit of "
                                     + std::to_string(limit) + " bytes");
    }

    void value(const std::string &path)
    {
        if (++values > JSONConvert::kMaxValues)
            throw JSONConvert::Error(path + ": more than "
                                     + std::to_string(JSONConvert::kMaxValues)
                                     + " values");
    }
};

/* serialized length of a string, quotes and escapes included */
size_t
quotedLength(const std::string &s)
{
    size_t n = 2;

    for (unsigned char c : s)
        n += (c < 0x20) ? 6 : (c == '"' || c == '\\') ? 2 : 1;
    return n;
}

const char *
typeWord(int type)
{
    switch (type) {
        case PROG_OBJECT:
            return "a dbref";
        case PROG_LOCK:
            return "a lock";
        case PROG_ADD:
            return "a function address";
        case PROG_VAR:
        case PROG_LVAR:
        case PROG_SVAR:
            return "a variable";
        case PROG_MARK:
            return "a stack marker";
        case PROG_SOCKET:
            return "a socket";
        default:
            return "this MUF type";
    }
}

json
fromInstAt(struct inst *in, std::string &path, int depth, Budget &b)
{
    b.value(path);

    switch (in->type) {
        case PROG_INTEGER:
            b.add(20, path);
            return json((int64_t) in->data.number);

        case PROG_FLOAT:
            if (!std::isfinite(in->data.fnumber))
                throw JSONConvert::Error(path + ": NaN and infinity have no "
                                         "JSON equivalent");
            b.add(24, path);
            return json(in->data.fnumber);

        case PROG_STRING: {
            std::string s = in->data.string
                ? ASCIIFromBytes(in->data.string->data) : std::string();

            b.add(quotedLength(s), path);
            return json(s);
        }

        case PROG_ARRAY: {
            stk_array *a = in->data.array;
            bool dict = a && a->type == ARRAY_DICTIONARY;
            json out = dict ? json::object() : json::array();
            array_iter idx;

            if (depth >= JSONConvert::kMaxDepth)
                throw JSONConvert::Error(path + ": nested deeper than "
                                         + std::to_string(JSONConvert::kMaxDepth)
                                         + " levels");
            b.add(2, path);
            if (!a || !array_first(a, &idx))
                return out;
            try {
                size_t i = 0;

                do {
                    size_t mark = path.size();
                    array_data *v = array_getitem(a, &idx);

                    if (dict) {
                        std::string k;

                        if (idx.type == PROG_STRING)
                            k = idx.data.string
                                ? ASCIIFromBytes(idx.data.string->data)
                                : std::string();
                        else if (idx.type == PROG_INTEGER)
                            k = std::to_string((int64_t) idx.data.number);
                        else
                            throw JSONConvert::Error(
                                path + ": a dictionary key must be a string "
                                "or an integer to become a JSON key");
                        /* 1 and "1" both become "1"; keeping one would
                         * silently drop the other */
                        if (out.contains(k))
                            throw JSONConvert::Error(
                                path + ": keys 1 and \"1\" would both become "
                                "the JSON key \"" + k + "\"");
                        path += "." + k;
                        b.add(quotedLength(k) + 2, path);
                        out[k] = fromInstAt(v, path, depth + 1, b);
                    } else {
                        path += "[" + std::to_string(i++) + "]";
                        b.add(1, path);
                        out.push_back(fromInstAt(v, path, depth + 1, b));
                    }
                    path.resize(mark);
                } while (array_next(a, &idx));
            } catch (...) {
                /* array_next clears the iterator only when it runs off
                 * the end; leaving the loop any other way must do it */
                CLEAR(&idx);
                throw;
            }
            return out;
        }

        default:
            throw JSONConvert::Error(path + ": " + typeWord(in->type)
                                     + " has no JSON equivalent");
    }
}

} /* namespace */

json
JSONConvert::fromInst(struct inst *in, size_t limit)
{
    Budget b;
    std::string path = "$";

    b.limit = limit;
    return fromInstAt(in, path, 0, b);
}

std::string
JSONConvert::dump(const json &j, size_t limit)
{
    /* every string in j is already 7-bit, so ensure_ascii is a second
     * guarantee rather than the first */
    std::string s = j.dump(-1, ' ', true);

    if (s.size() > limit)
        throw Error("JSON of " + std::to_string(s.size())
                    + " bytes exceeds the limit of " + std::to_string(limit));
    return s;
}

} /* namespace MUCK */
