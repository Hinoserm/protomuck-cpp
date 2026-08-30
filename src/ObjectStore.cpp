/* Standard headers FIRST: db.h defines function-like macros (getloc
 * among them) that clobber identically named members inside libstdc++
 * headers included after it. */
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cctype>
#include <fstream>
#include <sstream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <nlohmann/json.hpp>

#include "copyright.h"
#include "config.h"
#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "strutils.h"
#include "ObjectStore.h"
#include "ObjectAccess.h"
#include "Modules.h"
#include "ProgramStore.h"
#include "PasswordHash.h"
#include "ModuleRegistry.h"

using json = nlohmann::json;

extern void tune_save_parms_to_file(FILE *);
extern void tune_load_parms_from_file(FILE *, dbref, int);

namespace MUCK {

ObjectStore g_objectStore;

/* The one and only store format: the entry model, with everything an
 * object owns, program source included, inline in its uuid.json. A
 * store stamped with any other format number does not load; rebuild
 * it by re-importing the legacy flat database. */
static const int STORE_FORMAT = 1;

/* Dormant module data (docs section 4): entries in namespaces no
 * loaded module claims, plus unrecognized attached-module names, are
 * carried verbatim across load and save so unloading a module can
 * never destroy its data. Keyed by UUID. */
static std::unordered_map<UUID, json> dormantEntries;
static std::unordered_map<UUID, json> dormantModules;

/* UNSUPPORTED placeholders: uuid -> {"name": stored type name,
 * "bits": original TYPE_MASK bits}. Populated at load for objects
 * whose type module is excluded or unknown; consulted at save so the
 * file keeps saying what the object would have been. */
static std::unordered_map<UUID, json> dormantTypeInfo;
static std::set<std::string> excludedTypes;


static bool
builtinTypeName(const std::string &n)
{
    return n == "room" || n == "thing" || n == "exit"
        || n == "player" || n == "muf_program";
}

bool
ObjectStore::excludeType(const char *name, std::string *err)
{
    std::string n = name ? name : "";

    for (auto &c : n)
        c = (char) tolower(c);
    if (n == "program")
        n = "muf_program";
    if (n == "room" || n == "thing" || n == "player") {
        if (err)
            *err = "cannot exclude container type '" + n
                + "' until the storage flip; contents chains would break";
        return false;
    }
    if (n.empty()) {
        if (err)
            *err = "empty type name";
        return false;
    }
    excludedTypes.insert(n);
    return true;
}

bool
ObjectStore::typeExcluded(const std::string &name)
{
    return excludedTypes.count(name) != 0;
}

static bool forceLoadFlag = false;

void
ObjectStore::setForceLoad(bool v)
{
    forceLoadFlag = v;
}

bool
ObjectStore::forceLoad()
{
    return forceLoadFlag;
}

static std::set<std::string> excludedModules;

bool
ObjectStore::excludeModule(const char *name, std::string *err)
{
    std::string n = name ? name : "";

    for (auto &c : n)
        c = (char) tolower(c);
    if (n.empty()) {
        if (err)
            *err = "empty module name";
        return false;
    }
    excludedModules.insert(n);
    return true;
}

bool
ObjectStore::moduleExcluded(const std::string &name)
{
    return excludedModules.count(name) != 0;
}

/* A stored type name is supported when it is one of the built-ins and
 * has not been excluded for this boot. "garbage" passes through the
 * legacy path untouched. */
static bool
storedTypeSupported(const std::string &n)
{
    if (n == "garbage")
        return true;
    return builtinTypeName(n) && !ObjectStore::typeExcluded(n);
}

std::string
ObjectStore::placeholderTypeName(dbref ref)
{
    auto it = dormantTypeInfo.find(MUCK::database().UUIDOf(ref));

    if (it == dormantTypeInfo.end())
        return "";
    return it->second.value("name", "");
}

static bool
knownNamespace(const std::string &k)
{
    if (k.empty())
        return false;
    if (k[0] == '/')            /* properties: dormant when excluded */
        return !ObjectStore::moduleExcluded("properties");
    if (k.rfind("$core/", 0) == 0 || k.rfind("$type/", 0) == 0)
        return true;
    return false;
}

/* forward declarations: marker helpers are defined with the versioned
 * save path below but used by the manifest writer above it */
static std::vector<ObjectStore::Marker> markersFromJson(const json &arr);
static json markersToJson(const std::vector<ObjectStore::Marker> &list);
static bool markerInWindow(long from, long to,
                           const std::vector<ObjectStore::Marker> &globals,
                           const std::vector<ObjectStore::Marker> &own);

/* ------------------------------------------------------------------ */
/* String escaping. MUCK strings are raw 8-bit (latin-1 by long       */
/* convention); JSON requires valid UTF-8. Encode losslessly on save, */
/* decode on load. A byte over 0x7f becomes a two-byte UTF-8 sequence */
/* and comes back identical.                                          */
/* ------------------------------------------------------------------ */

/* JSON cannot represent a non-finite double: nlohmann silently dumps
 * infinity and NaN as the literal null, and reading that back throws
 * type_error inside whatever thread is loading. Ordinary MUF float
 * math produces infinity without any error flag (1e308 1e308 F+), so
 * a single unprivileged property set would otherwise bake a value
 * into the store that makes every later boot abort. Non-finite values
 * therefore travel as strings, which round-trip exactly; finite ones
 * keep their plain JSON number, so existing files are unchanged. */
static json
floatToJson(double d)
{
    if (std::isfinite(d))
        return json(d);
    if (std::isnan(d))
        return json("nan");
    return json(d > 0 ? "inf" : "-inf");
}

static double
jsonToFloat(const json &v, bool *ok)
{
    if (ok)
        *ok = true;
    if (v.is_number())
        return v.get<double>();
    if (v.is_string()) {
        const std::string s = v.get<std::string>();

        if (s == "inf")
            return HUGE_VAL;
        if (s == "-inf")
            return -HUGE_VAL;
        if (s == "nan")
            return std::nan("");
    }
    /* null: a non-finite value written before this encoding existed */
    if (ok)
        *ok = false;
    return 0.0;
}

static std::string
jstr(const char *s)
{
    std::string out;

    if (!s)
        return out;
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if (*p < 0x80) {
            out += (char) *p;
        } else {
            out += (char) (0xc0 | (*p >> 6));
            out += (char) (0x80 | (*p & 0x3f));
        }
    }
    return out;
}

static std::string
junstr(const std::string &s)
{
    std::string out;

    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char) s[i];

        if (c < 0x80) {
            out += (char) c;
            i++;
        } else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) {
            unsigned char c2 = (unsigned char) s[i + 1];
            unsigned int cp = ((c & 0x1f) << 6) | (c2 & 0x3f);

            out += (char) (cp <= 0xff ? cp : '?');
            i += 2;
        } else if ((c & 0xf0) == 0xe0 && i + 2 < s.size()) {
            out += '?';         /* beyond latin-1; cannot round-trip */
            i += 3;
        } else if ((c & 0xf8) == 0xf0 && i + 3 < s.size()) {
            out += '?';
            i += 4;
        } else {
            out += '?';
            i++;
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* ref encoding: objects serialize as UUID strings, the negative      */
/* sentinels (NOTHING, AMBIGUOUS, HOME, NIL) as integers.             */
/* ------------------------------------------------------------------ */

static json
refToJson(dbref ref)
{
    if (ref < 0 || !MUCK::database().valid(ref))
        return json((int) ref);
    return json(MUCK::database().UUIDOf(ref).toString());
}

static dbref
refFromJson(const json &v)
{
    if (v.is_number_integer())
        return (dbref) v.get<int>();
    if (v.is_string()) {
        UUID u = UUID::parse(v.get<std::string>().c_str());
        return MUCK::database().refOf(u);
    }
    return NOTHING;
}

/* ------------------------------------------------------------------ */
/* Properties: mirrors db_putprop/db_get_single_prop in property.cpp. */
/* Values that the flat format never persisted (zero ints, empty      */
/* strings, NOTHING refs, TRUE_BOOLEXP locks) are skipped here too.   */
/* Prop refs remain dbref integers until the step 2 memory rebuild.   */
/* ------------------------------------------------------------------ */

static void
propsToJson(json &arr, const char *dir, PropDirPtr d)
{
    /* HEAP, not stack: this function recurses once per propdir level,
     * and two 64KB stack arrays per frame meant an ordinary player
     * with a propdir nested ~60 deep (a/a/a/...:1, trivially short in
     * bytes) blew the seal worker's stack and took the whole server
     * down at the next dump. Heap frames make the same depth cost a
     * few hundred stack bytes each. */
    std::vector<char> bufv(BUFFER_LEN), ubufv(BUFFER_LEN);
    char *buf = bufv.data();
    char *ubuf = ubufv.data();

    if (!d)
        return;
    for (PropPtr p = d->first(); p; p = d->nextAfter(p->name())) {
        int type = PropType(p);
        int flags = PropFlagsRaw(p) & ~(PROP_TOUCHED | PROP_ISUNLOADED | PROP_NOASCIICHK | PROP_COMPRESSED);
        bool keep = true;
        json v;

        switch (type) {
            case PROP_INTTYP:
                if (!PropDataVal(p))
                    keep = false;
                else
                    v = PropDataVal(p);
                break;
            case PROP_FLTTYP:
                if (!PropDataFVal(p))
                    keep = false;
                else
                    v = floatToJson(PropDataFVal(p));
                break;
            case PROP_REFTYP:
                if (PropDataRef(p) == NOTHING)
                    keep = false;
                else
                    v = (int) PropDataRef(p);
                break;
            case PROP_STRTYP:
                if (!PropDataStr(p) || !*PropDataStr(p))
                    keep = false;
                else
                    v = jstr(PropDataUNCStr(p));
                break;
            case PROP_LOKTYP:
                if ((PropFlagsRaw(p) & PROP_ISUNLOADED) || PropDataLok(p) == TRUE_BOOLEXP)
                    keep = false;
                else
                    v = jstr(unparse_boolexp(ubuf, (dbref) 1, PropDataLok(p), 0));
                break;
            default:           /* PROP_DIRTYP carries no value */
                keep = false;
                break;
        }

        if (keep) {
            json entry;
            entry["name"] = jstr((std::string(dir + 1) + PropName(p)).c_str());
            entry["flags"] = flags;
            entry["value"] = v;
            arr.push_back(entry);
        }

        if (PropDir(p)) {
            snprintf(buf, BUFFER_LEN, "%s%s%c", dir, PropName(p), PROPDIR_DELIMITER);
            propsToJson(arr, buf, PropDir(p));
        }
    }
}

static void
propsFromJson(dbref obj, const json &arr)
{
    for (const auto &entry : arr) {
        std::string name = junstr(entry.value("name", ""));
        int flags = entry.value("flags", 0);
        PData pdat;

        if (name.empty() || !entry.contains("value"))
            continue;
        /* set_property_nofetch strcpys the name into a BUFFER_LEN
         * stack array; an oversized name off disk is damage to skip,
         * not a stack overflow to suffer */
        if (name.size() >= BUFFER_LEN) {
            fprintf(stderr, "STORE: #%d property name of %zu bytes "
                    "exceeds the limit; skipped\n", obj, name.size());
            continue;
        }
        pdat.flags = (unsigned short) (flags & ~PROP_COMPRESSED);
        const json &v = entry["value"];
        std::string sval;

        /* Every access below is TYPE-GUARDED. This data comes off
         * disk, an unchecked .get<T>() throws on the first mismatched
         * field, and this runs inside a parallel load worker where an
         * escaping exception would terminate the process instead of
         * being reported as damage. A wrong-typed value loses that one
         * property, not the server. */
        switch (pdat.flags & PROP_TYPMASK) {
            case PROP_STRTYP:
                if (!v.is_string())
                    continue;
                pdat.flags &= ~PROP_ISUNLOADED;
                sval = junstr(v.get<std::string>());
                pdat.data.str = (char *) sval.c_str();
                set_property_nofetch(obj, name.c_str(), &pdat, 1);
                continue;
            case PROP_LOKTYP:
                if (!v.is_string())
                    continue;
                pdat.flags &= ~PROP_ISUNLOADED;
                sval = junstr(v.get<std::string>());
                pdat.data.lok = parse_boolexp(-1, (dbref) 1, sval.c_str(), 32767);
                break;
            case PROP_INTTYP:
                if (!v.is_number_integer())
                    continue;
                pdat.data.val = v.get<int64_t>();
                break;
            case PROP_FLTTYP: {
                bool ok = true;

                pdat.data.fval = jsonToFloat(v, &ok);
                if (!ok)
                    fprintf(stderr, "STORE: #%d property \"%s\" had an "
                            "unrepresentable float; loaded as 0\n",
                            obj, name.c_str());
                break;
            }
            case PROP_REFTYP:
                if (!v.is_number_integer())
                    continue;
                pdat.data.ref = (dbref) v.get<int64_t>();
                break;
            default:
                continue;
        }
        set_property_nofetch(obj, name.c_str(), &pdat, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Containment chains serialize as ordered UUID lists.                */
/* ------------------------------------------------------------------ */

/* Serialize a contents/exits chain. Legacy databases can carry corrupt
 * chains: a member's next escaping into another container's list, lists
 * converging on a shared tail, even self-containment. Two rules keep the
 * store sane regardless of source corruption: a member must claim its
 * container as location, and an object can appear in exactly one chain
 * per save pass (the claimed set). Violations are dropped with a log
 * line; this is the conversion-time equivalent of @sanfix. */
/* The chain sanitizer's claimed set: which dbref already appeared in
 * some contents or exits list this save pass. One atomic flag per
 * dbref rather than a set, because parallel seal workers serialize
 * different objects' lists concurrently; first claim wins, exactly
 * as the serial set did. */
static std::unique_ptr<std::atomic<unsigned char>[]> chainClaimed;
static size_t chainClaimedSize = 0;

static void
chainClaimReset(void)
{
    size_t need = (size_t) MUCK::database().top();

    if (need > chainClaimedSize) {
        chainClaimed.reset(new std::atomic<unsigned char>[need]);
        chainClaimedSize = need;
    }
    for (size_t i = 0; i < chainClaimedSize; i++)
        chainClaimed[i].store(0, std::memory_order_relaxed);
}

/* true = newly claimed by this caller */
static bool
chainClaim(dbref i)
{
    if (i < 0 || (size_t) i >= chainClaimedSize)
        return true;            /* outside the table: never cut */
    return chainClaimed[i].exchange(1, std::memory_order_relaxed) == 0;
}
static bool bulkSaveActive = false;

static json
listToJson(dbref container, const std::vector<MUCK::DbObject *> &list,
           bool claim = true)
{
    json arr = json::array();

    for (MUCK::DbObject *o : list) {
        dbref i = o->ref();

        if (!MUCK::database().valid(i) || i == container
            || MUCK::getLocation(i) != container) {
            fprintf(stderr, "STORE: dropping corrupt list member #%d from #%d\n",
                    i, container);
            continue;
        }
        /* The one-membership check is a whole-pass repair for legacy
         * chain corruption: it can only mean something while every
         * list is being written together. Materializing a single
         * entry has no pass to be part of, so it keeps the per-object
         * location check and skips the cross-object claim. */
        if (claim && !chainClaim(i)) {
            fprintf(stderr, "STORE: #%d already serialized in another list; dropping from #%d\n",
                    i, container);
            continue;
        }
        arr.push_back(refToJson(i));
    }
    return arr;
}

/* Fill a containment vector from a stored list, order preserved. */
static void
listFromJson(std::vector<MUCK::DbObject *> &out, const json &arr)
{
    out.clear();
    for (const auto &v : arr) {
        dbref i = refFromJson(v);

        if (i == NOTHING || !MUCK::database().valid(i))
            continue;

        MUCK::DbObject *o = MUCK::database().get(i);

        if (o)
            out.push_back(o);
    }
}

/* ------------------------------------------------------------------ */
/* One object to and from JSON.                                       */
/* ------------------------------------------------------------------ */

static const char *
typeNameOf(dbref i)
{
    return MUCK::typeName(MUCK::typeOf(i));
}

/* ------------------------------------------------------------------ */
/* The chunk pool: content-addressed storage for bulk values (program */
/* source lines). A chunk file's name is the sha1 of its content;     */
/* writing is idempotent, identical lines are stored once, and prune  */
/* garbage-collects unreferenced chunks. Chunk files hold raw bytes,  */
/* so no JSON escaping applies. docs/DATABASE.txt section 3.          */
/* ------------------------------------------------------------------ */

static bool
ensureDirsFor(const std::string &path)
{
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] != '/')
            continue;
        std::string dir = path.substr(0, i);

        if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}


/* ------------------------------------------------------------------ */
/* The value model. An object's persistent state is a set of          */
/* namespaced entries, each a typed value stamped with the rev at     */
/* which it took effect. The closed type set (docs section 3):        */
/*   string, int, float, bool, ref, list                              */
/* Prop entries carry the legacy prop flag word as "flags". All keys  */
/* are spelled out: the store sits on compressed filesystems, so      */
/* abbreviations would save nothing and cost readability.             */
/* ------------------------------------------------------------------ */

static json
entry(const char *t, json v)
{
    json e;

    e["type"] = t;
    e["value"] = std::move(v);
    return e;
}

/* ------------------------------------------------------------------ */
/* One entry, by key.                                                 */
/*                                                                    */
/* Sealing a journal layer materializes only the entries that actually */
/* changed, so an object carrying a hundred thousand properties costs  */
/* one serialization when one of them ticks, not a hundred thousand.   */
/* Returns a null json when the entry does not exist, which is how a   */
/* removal is expressed. The shapes here MUST match objectToJson.      */
/* ------------------------------------------------------------------ */

static json
entryValueOf(dbref i, const std::string &key)
{
    struct object *o = DBFETCH(i);

    if (!o)
        return json();

    /* --- $core --- */
    if (key == "$core/name")
        return entry("string", jstr(o->name));
    if (key == "$core/location")
        return entry("ref", refToJson(o->location));
    if (key == "$core/owner")
        return entry("ref", refToJson(o->owner));
    if (key == "$core/flags") {
        int typeBitsOut = (int) MUCK::typeOf(i);

        auto pt = dormantTypeInfo.find(MUCK::database().UUIDOf(i));

        if (pt != dormantTypeInfo.end())
            typeBitsOut = pt->second.value("bits", 0) & TYPE_MASK;
        return entry("list", json::array({
            (int) (((o->flags & ~DUMP_MASK) & ~TYPE_MASK) | typeBitsOut),
            (int) (o->flag2 & ~DUM2_MASK),
            (int) (o->flag3 & ~DUM3_MASK), (int) (o->flag4 & ~DUM4_MASK) }));
    }
    if (key == "$core/powers")
        return entry("list", json::array({
            (int) (o->powers & ~POWERS_DUMP_MASK),
            (int) (o->power2 & ~POWER2_DUMP_MASK) }));
    if (key == "$core/ts")
        return entry("list", json::array({
            (long) o->ts.created, (long) o->ts.modified, (long) o->ts.lastused,
            (long) o->ts.usecount, (long) o->ts.dcreated,
            (long) o->ts.dmodified, (long) o->ts.dlastused }));
    if (key == "$core/deleted") {
        DbObject *d = MUCK::database().get(i);

        /* absent when alive: that absence is exactly what makes a
         * rollback to an earlier revision bring the object back */
        if (!d || !d->isDeleted())
            return json();
        return entry("list", json::array({ (long) d->deletedAt(),
                                           (int) d->deletedBy() }));
    }

    /* --- $type --- */
    if (key == "$type/contents")
        return entry("list", listToJson(i, MUCK::contentsOf(i), false));
    if (key == "$type/exits")
        return entry("list", listToJson(i, MUCK::exitsOf(i), false));
    if (key == "$type/dropto")
        return entry("ref", refToJson(MUCK::roomDropToRef(i)));
    if (key == "$type/home")
        return entry("ref", refToJson(MUCK::typeOf(i) == TYPE_PLAYER
                                      ? MUCK::playerHomeRef(i)
                                      : MUCK::thingHomeRef(i)));
    if (key == "$type/value")
        return entry("int", MUCK::thingValue(i));
    if (key == "$type/pennies")
        return entry("int", MUCK::playerPennies(i));
    if (key == "$type/password")
        return entry("string", jstr(MUCK::playerPasswordSlot(i)));
    if (key == "$type/dests") {
        json dests = json::array();
        int nd = MUCK::exitDestCount(i);

        for (int k = 0; k < nd; k++)
            dests.push_back(refToJson(MUCK::exitDestRef(i, k)));
        return entry("list", dests);
    }
    if (key == "$type/source") {
        json lv = json::array();
        const std::vector<std::string> *lines = MUCK::programs().sourceLines(i);

        if (lines)
            for (const auto &ln : *lines)
                lv.push_back(jstr(ln.c_str()));
        return entry("list", lv);
    }

    /* --- a property, by its slash path --- */
    if (!key.empty() && key[0] == '/') {
        PropPtr p = get_property(i, key.c_str() + 1);

        if (!p)
            return json();          /* removed */

        int flags = PropFlagsRaw(p) & ~PROP_ISUNLOADED;
        json v;
        const char *t = "string";

        switch (flags & PROP_TYPMASK) {
            case PROP_INTTYP:
                if (!PropDataVal(p))
                    return json();
                v = (int64_t) PropDataVal(p);
                t = "int";
                break;
            case PROP_FLTTYP:
                if (!PropDataFVal(p))
                    return json();
                v = floatToJson(PropDataFVal(p));
                t = "float";
                break;
            case PROP_REFTYP:
                if (PropDataRef(p) == NOTHING)
                    return json();
                v = (int) PropDataRef(p);
                t = "int";
                break;
            case PROP_STRTYP:
                if (!PropDataStr(p) || !*PropDataStr(p))
                    return json();
                v = jstr(PropDataUNCStr(p));
                break;
            case PROP_LOKTYP: {
                char ubuf[BUFFER_LEN];

                if ((PropFlagsRaw(p) & PROP_ISUNLOADED)
                    || PropDataLok(p) == TRUE_BOOLEXP)
                    return json();
                v = jstr(unparse_boolexp(ubuf, (dbref) 1, PropDataLok(p), 0));
                break;
            }
            default:
                return json();      /* a pure directory carries no value */
        }

        json ent = entry(t, v);

        ent["flags"] = flags & ~PROP_COMPRESSED;
        return ent;
    }

    return json();
}

static json
objectToJson(dbref i)
{
    struct object *o = DBFETCH(i);
    json j;

    j["uuid"] = MUCK::database().UUIDOf(i).toString();
    j["dbref"] = (int) i;
    j["type"] = typeNameOf(i);
    j["modules"] = json::array();

    /* An UNSUPPORTED placeholder saves as what it would have been:
     * the stored type name and its original type bits, verbatim. */
    int typeBitsOut = (int) (o->flags & TYPE_MASK);

    {
        auto pt = dormantTypeInfo.find(UUID::parse(j["uuid"].get<std::string>()));

        if (pt != dormantTypeInfo.end()) {
            j["type"] = pt->second.value("name", "garbage");
            typeBitsOut = pt->second.value("bits", 0) & TYPE_MASK;
        }
    }

    json e;

    /* --- $core: what every object has --- */
    e["$core/name"] = entry("string", jstr(o->name));
    e["$core/location"] = entry("ref", refToJson(o->location));
    e["$core/owner"] = entry("ref", refToJson(o->owner));
    e["$core/flags"] = entry("list", json::array({
        (int) (((o->flags & ~DUMP_MASK) & ~TYPE_MASK) | typeBitsOut),
        (int) (o->flag2 & ~DUM2_MASK),
        (int) (o->flag3 & ~DUM3_MASK), (int) (o->flag4 & ~DUM4_MASK) }));
    e["$core/powers"] = entry("list", json::array({
        (int) (o->powers & ~POWERS_DUMP_MASK),
        (int) (o->power2 & ~POWER2_DUMP_MASK) }));
    e["$core/ts"] = entry("list", json::array({
        (long) o->ts.created, (long) o->ts.modified, (long) o->ts.lastused,
        (long) o->ts.usecount, (long) o->ts.dcreated,
        (long) o->ts.dmodified, (long) o->ts.dlastused }));
    {
        json del = entryValueOf(i, "$core/deleted");

        if (!del.is_null())
            e["$core/deleted"] = del;
    }

    /* --- $type: the type module's fields --- */
    switch (MUCK::typeOf(i)) {
        case TYPE_ROOM:
            e["$type/dropto"] = entry("ref", refToJson(MUCK::roomDropToRef(i)));
            e["$type/contents"] = entry("list", listToJson(i, MUCK::contentsOf(i)));
            e["$type/exits"] = entry("list", listToJson(i, MUCK::exitsOf(i)));
            break;
        case TYPE_THING:
            e["$type/home"] = entry("ref", refToJson(MUCK::thingHomeRef(i)));
            e["$type/value"] = entry("int", MUCK::thingValue(i));
            e["$type/contents"] = entry("list", listToJson(i, MUCK::contentsOf(i)));
            e["$type/exits"] = entry("list", listToJson(i, MUCK::exitsOf(i)));
            break;
        case TYPE_PLAYER:
            e["$type/home"] = entry("ref", refToJson(MUCK::playerHomeRef(i)));
            e["$type/pennies"] = entry("int", MUCK::playerPennies(i));
            e["$type/password"] = entry("string", jstr(MUCK::playerPasswordSlot(i)));
            e["$type/contents"] = entry("list", listToJson(i, MUCK::contentsOf(i)));
            e["$type/exits"] = entry("list", listToJson(i, MUCK::exitsOf(i)));
            break;
        case TYPE_EXIT: {
            json dests = json::array();
            int nd = MUCK::exitDestCount(i);

            for (int k = 0; k < nd; k++)
                dests.push_back(refToJson(MUCK::exitDestRef(i, k)));
            e["$type/dests"] = entry("list", dests);
            break;
        }
        case TYPE_PROGRAM: {
            /* source lines live inline in the object file */
            json lv = json::array();
            const std::vector<std::string> *lines = MUCK::programs().sourceLines(i);

            if (lines)
                for (const auto &ln : *lines)
                    lv.push_back(jstr(ln.c_str()));
            e["$type/source"] = entry("list", lv);
            break;
        }
        default:
            break;
    }

    /* --- properties: their traditional slash paths --- */
    {
        json props = json::array();

        propsToJson(props, "/", MUCK::propRoot(i));
        for (const auto &pe : props) {
            std::string key = "/" + pe["name"].get<std::string>();
            const json &v = pe["value"];
            const char *t = v.is_string() ? "string"
                : v.is_number_float() ? "float" : "int";
            json ent = entry(t, v);

            ent["flags"] = pe["flags"];
            e[key] = std::move(ent);
        }
    }

    /* dormant module data rides along untouched */
    {
        UUID du = UUID::parse(j["uuid"].get<std::string>());
        auto d = dormantEntries.find(du);

        if (d != dormantEntries.end())
            for (auto it = d->second.begin(); it != d->second.end(); ++it)
                e[it.key()] = it.value();
        auto dm = dormantModules.find(du);

        if (dm != dormantModules.end())
            j["modules"] = dm->second;
    }

    /* live feature modules persist through the value-model contract
     * and record their attachment; their fresh entries win over any
     * dormant copy */
    {
        DbObject *obj = MUCK::database().get(i);
        json &mods = j["modules"];

        obj->eachModule([&](Module *m) {
            if (!moduleRegistry().knows(m->moduleName()))
                return;
            m->saveEntries(e);
            for (const auto &existing : mods)
                if (existing == m->moduleName())
                    return;
            mods.push_back(m->moduleName());
        });
    }

    j["entries"] = e;
    return j;
}

struct PendingLinks {
    dbref ref;
    json td;
    json core;
    json props;
    json entries;               /* raw v2 entries, for module slices */
    std::vector<std::string> modules;
};

/* Phase one of load: create the object, set scalars, stash refs. */
static void
objectFromJsonPhase1(json &j, std::vector<PendingLinks> &later)
{
    dbref i = (dbref) j.value("dbref", -1);
    std::string type = j.value("type", "garbage");

    if (i < 0)
        return;
    MUCK::database().ensureTop(i + 1);
    struct object *o = DBFETCH(i);

    UUID u = UUID::parse(j.value("uuid", "").c_str());
    MUCK::database().assignUUID(i, u);

    const json &core = j["core"];
    /* the shell's name is heap-allocated (recycle sets it through
     * setName), so overwriting the pointer without freeing leaks it */
    if (o->name)
        delete[](char *) o->name;
    o->name = alloc_string(junstr(core.value("name", "")).c_str());

    const json &fl = core["flags"];
    o->flags = fl[0].get<int>();
    /* The stored flags word carries the type bits; lift them into the
     * type field, which is what everything reads. Without this an
     * object loaded from a store comes back as Garbage no matter what
     * it was, and the failure is quiet: names and properties still
     * look right, so only type-dependent behavior misbehaves. */
    MUCK::setType(i, (MUCK::ObjectType) (o->flags & TYPE_MASK));
    o->flag2 = fl[1].get<int>();
    o->flag3 = fl[2].get<int>();
    o->flag4 = fl[3].get<int>();
    const json &pw = core["powers"];
    o->powers = pw[0].get<int>();
    o->power2 = pw[1].get<int>();

    const json &ts = core["ts"];
    o->ts.created = (time_t) ts.value("created", 0L);
    o->ts.modified = (time_t) ts.value("modified", 0L);
    o->ts.lastused = (time_t) ts.value("lastused", 0L);
    o->ts.usecount = ts.value("usecount", 0);
    o->ts.dcreated = (dbref) ts.value("dcreated", -1);
    o->ts.dmodified = (dbref) ts.value("dmodified", -1);
    o->ts.dlastused = (dbref) ts.value("dlastused", -1);

    /* A recycled object is not a special kind of file: it is an
     * ordinary object whose latest state carries the deletion entry.
     * Roll back past that entry and it is simply alive again. */
    if (j.contains("deleted") && j["deleted"].is_array()
        && j["deleted"].size() >= 2) {
        MUCK::database().get(i)->markDeleted(j["deleted"][0].get<long>(),
                                             (dbref) j["deleted"][1].get<int>());
    } else if (DbObject *dd = MUCK::database().get(i)) {
        dd->markAlive();
    }

    /* props load in phase two: lock props reference other objects by
     * dbref, and the parser rejects targets that have not loaded yet */
    if (PropDirPtr pd_ = MUCK::propRoot(i))
        pd_->clear();

    /* links start empty NOW; phase two only wires. Resetting these any
     * later would tear down chain wiring already done by containers
     * whose phase two ran first. */
    o->location = NOTHING;
    o->owner = NOTHING;
    MUCK::contentsOf(i).clear();
    MUCK::exitsOf(i).clear();

    /* type scalars now, refs later */
    const json &td = j["type_data"];
    switch (MUCK::typeOf(i)) {
        case TYPE_THING:
            MUCK::thingSetValue(i, td.value("value", 0));
            break;
        case TYPE_PLAYER:
            MUCK::playerSetPennies(i, td.value("pennies", 0));
            MUCK::playerPasswordSlot(i) =
                alloc_string(junstr(td.value("password", "")).c_str());
            break;
        case TYPE_PROGRAM:
            if (td.contains("source")) {
                std::vector<std::string> lines;
                for (const auto &ln : td["source"])
                    lines.push_back(junstr(ln.get<std::string>()));
                MUCK::programs().setSourceLines(i, std::move(lines));
            }
            break;
        default:
            break;
    }

    PendingLinks pl;

    pl.ref = i;
    /* MOVE the subtrees rather than copy: at a hundred thousand
     * objects these copies were most of the serial apply cost. The
     * caller's json is dead after this call; both call sites agree. */
    pl.td = std::move(j["type_data"]);
    pl.core = std::move(j["core"]);
    if (j.contains("props"))
        pl.props = std::move(j["props"]);
    later.push_back(std::move(pl));
}

/* Translate an object file's entry model into the flat load shape
 * the phase machinery consumes. */
static json
fileToLoadShape(const json &j, const std::string &root)
{
    json out;
    const json &e = j["entries"];

    out["uuid"] = j.value("uuid", "");
    out["dbref"] = j.value("dbref", -1);
    out["type"] = j.value("type", "garbage");
    out["modules"] = j.value("modules", json::array());

    auto val = [&e](const char *k) -> json {
        auto it = e.find(k);
        return it == e.end() ? json() : (*it)["value"];
    };

    json core;
    core["name"] = val("$core/name").is_string() ? val("$core/name") : json("");
    core["location"] = val("$core/location");
    core["owner"] = val("$core/owner");
    /* flags and powers feed unchecked fl[0..3]/pw[0..1] indexing in
     * phase one; a missing, short, or wrong-typed entry off disk
     * would throw there and abort the WHOLE boot for one bad object.
     * Default a malformed entry to a coherent zero shape here, the
     * same way ts is guarded just below. */
    {
        json fl = val("$core/flags");

        core["flags"] = (fl.is_array() && fl.size() >= 4)
            ? fl : json::array({ 0, 0, 0, 0 });
        json pw = val("$core/powers");

        core["powers"] = (pw.is_array() && pw.size() >= 2)
            ? pw : json::array({ 0, 0 });
    }

    json tsl = val("$core/ts");
    json ts;
    if (tsl.is_array() && tsl.size() >= 7) {
        ts["created"] = tsl[0];
        ts["modified"] = tsl[1];
        ts["lastused"] = tsl[2];
        ts["usecount"] = tsl[3];
        ts["dcreated"] = tsl[4];
        ts["dmodified"] = tsl[5];
        ts["dlastused"] = tsl[6];
    }
    core["ts"] = ts;
    out["core"] = core;

    if (e.contains("$core/deleted"))
        out["deleted"] = e["$core/deleted"]["value"];

    json td;
    for (const char *k : { "dropto", "home", "value", "pennies",
                           "password", "contents", "exits", "dests" }) {
        std::string key = std::string("$type/") + k;
        auto it = e.find(key);
        if (it != e.end())
            td[k] = (*it)["value"];
    }
    if (e.contains("$type/source"))
        td["source"] = e["$type/source"]["value"];
    out["type_data"] = td;

    json props = json::array();
    for (auto it = e.begin(); it != e.end(); ++it) {
        if (it.key().empty() || it.key()[0] != '/')
            continue;
        json pe;
        pe["name"] = it.key().substr(1);
        pe["flags"] = it.value().value("flags", 0);
        pe["value"] = it.value()["value"];
        props.push_back(pe);
    }
    out["props"] = props;

    return out;
}

/* Phase two: every object exists, so UUIDs resolve; wire the refs. */
static void
objectFromJsonPhase2(const PendingLinks &pl)
{
    struct object *o = DBFETCH(pl.ref);

    if (pl.props.is_array())
        propsFromJson(pl.ref, pl.props);

    /* re-attach registered feature modules and hand each its entry
     * slice; unregistered and excluded names stay dormant */
    for (const auto &name : pl.modules) {
        if (!moduleRegistry().knows(name))
            continue;
        if (ObjectStore::moduleExcluded(name))
            continue;

        DbObject *obj = MUCK::database().get(pl.ref);
        Module *m = obj->attach(moduleRegistry().make(name));

        m->loadEntries(pl.entries);
    }

    o->location = refFromJson(pl.core["location"]);
    o->owner = refFromJson(pl.core["owner"]);

    switch (MUCK::typeOf(pl.ref)) {
        case TYPE_ROOM:
            MUCK::roomSetDropToRef(pl.ref, refFromJson(pl.td["dropto"]));
            listFromJson(MUCK::contentsOf(pl.ref), pl.td["contents"]);
            listFromJson(MUCK::exitsOf(pl.ref), pl.td["exits"]);
            break;
        case TYPE_THING:
            MUCK::thingSetHomeRef(pl.ref, refFromJson(pl.td["home"]));
            listFromJson(MUCK::contentsOf(pl.ref), pl.td["contents"]);
            listFromJson(MUCK::exitsOf(pl.ref), pl.td["exits"]);
            break;
        case TYPE_PLAYER:
            MUCK::playerSetHomeRef(pl.ref, refFromJson(pl.td["home"]));
            listFromJson(MUCK::contentsOf(pl.ref), pl.td["contents"]);
            listFromJson(MUCK::exitsOf(pl.ref), pl.td["exits"]);
            /* add_player happens in the loader's serial pass: the
             * name table is shared and phase two runs in parallel */
            break;
        case TYPE_EXIT: {
            const json &dests = pl.td["dests"];
            std::vector<dbref> dl;

            for (const auto &dv : dests)
                dl.push_back(refFromJson(dv));
            if (MUCK::Exit *x = MUCK::database().get(pl.ref)->As<MUCK::Exit>())
                x->setDestRefs(dl.empty() ? NULL : dl.data(),
                               (int) dl.size());
            break;
        }
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Filesystem plumbing.                                               */
/* ------------------------------------------------------------------ */

static bool
ensureDir(const std::string &path)
{
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
        return true;
    return false;
}

/* Shard on the LAST four hex digits: uuidv7 leads with a timestamp,
 * so leading digits are identical for every object minted in the
 * same era and would put the whole database in one directory. The
 * tail is random. */
static std::string
UUIDObjectPath(const std::string &root, const std::string &u)
{
    size_t n = u.size();

    if (n < 4)
        return root + "/objects/xx/xx/" + u + ".json";

    return root + "/objects/" + u.substr(n - 4, 2) + "/" + u.substr(n - 2, 2)
        + "/" + u + ".json";
}

std::string
ObjectStore::objectPath(dbref i) const
{
    return UUIDObjectPath(root_, MUCK::database().UUIDOf(i).toString());
}

/* Durability: torn files after power loss have been a real problem
 * here, so a write is not finished until its bytes are on the platter
 * and the rename that publishes them is too. docs/DATABASE.txt 7.1. */
static void
syncFile(const std::string &path)
{
    int fd = open(path.c_str(), O_RDONLY);

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static void
syncDirOf(const std::string &path)
{
    size_t slash = path.rfind('/');
    std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static bool
atomicWrite(const std::string &path, const std::string &content,
            bool syncIt = true)
{
    /* The temp name is unique per writer and per call. The game thread
     * and the dump thread can both have a legitimate reason to write
     * the same file, and sharing one ".tmp" path lets their writes
     * interleave into a corrupt file that then gets renamed into
     * place. A filesystem race is invisible to a thread sanitizer, so
     * this is structural rather than something testing would catch. */
    static std::atomic<unsigned long> seq(0);
    std::string tmp = path + ".tmp." + std::to_string((unsigned long) getpid())
        + "." + std::to_string(seq.fetch_add(1));
    std::ofstream f(tmp, std::ios::trunc);

    if (!f)
        return false;
    f << content;
    f.close();
    if (!f) {
        /* leave no orphan tmp behind: on sustained ENOSPC these
         * accumulate and worsen the very condition that failed */
        unlink(tmp.c_str());
        return false;
    }
    /* fsync the data, then publish, then fsync the directory so the
     * rename itself survives. syncIt=false is for batched writers
     * (the dump's layer phase), which pay ONE filesystem barrier for
     * the whole batch before their commit point instead of two
     * fsyncs per file; the rename is still atomic either way. */
    if (syncIt)
        syncFile(tmp);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (syncIt)
        syncDirOf(path);
    return true;
}

/* The clock the retention ladder ages markers against, guarded
 * against a jump. An NTP correction, a hypervisor resume, or an admin
 * typo can move the wall clock forward by months; fed to the ladder
 * raw, that retires nearly every unlocked marker in a single fire and
 * compaction then merges the history away for good. A jump beyond a
 * day is treated as a clock event, not elapsed time: the ladder
 * advances by an hour instead, so at worst one bucket's worth of
 * thinning happens per dump until the clock and the ladder agree.
 * Backward jumps need no clamp (ages shrink, markers survive). */
long
ObjectStore::ladderNow()
{
    long now = (long) current_systime;

    if (lastLadderNow_ == 0) {
        lastLadderNow_ = now;
        return now;
    }
    if (now - lastLadderNow_ > 86400L) {
        fprintf(stderr, "STORE: clock jumped forward %ld seconds; "
                "aging snapshots by one hour instead of the full jump\n",
                now - lastLadderNow_);
        lastLadderNow_ += 3600L;
        return lastLadderNow_;
    }
    if (now > lastLadderNow_)
        lastLadderNow_ = now;
    return now;
}

/* How many committed journal segments may sit unfolded before the
 * dump thread starts distributing the oldest into the per-object
 * files. Larger = fewer, better-coalesced background writes; the
 * cost is only boot-replay work after a crash. */
static const long kJournalLag = 8;

/* A single capture is split into segments of at most this many
 * layers. Folding yields to pending commits between segments, so
 * this is the quantum bounding how long any commit can wait behind
 * housekeeping, even right after a mass import wrote a hundred
 * thousand layers in one dump: measured, a fold of 5000 full-object
 * lines held a waiting commit for seconds, so the quantum is 1000. */
static const long kSegmentLayers = 1000;

bool
ObjectStore::isStore(const char *path)
{
    struct stat st;
    std::string manifest = std::string(path) + "/manifest.json";

    return stat(manifest.c_str(), &st) == 0;
}

std::string
ObjectStore::buildManifest()
{
    json m;

    ensureDir(root_);

    m["format"] = STORE_FORMAT;
    m["next_dbref"] = (int) MUCK::database().top();

    /* marker aging happens in fire(), through the retention ladder;
     * this only serializes whatever survived */
    m["rev"] = rev_;
    m["markers"] = markersToJson(markers_);
    /* the journal watermarks: segments at or below distributed are
     * folded into the per-object files; segments above committed are
     * uncommitted crash leftovers; the span between them replays at
     * boot (docs/DATABASE.txt 7.1) */
    m["journal_committed"] = journalSeq_;
    m["journal_distributed"] = journalDistributed_.load();
    m["global_modules"] = { "properties" };
    m["hash_passwords"] = (bool) MUCK::PasswordHash::enabled;
    m["hash_version"] = MUCK::PasswordHash::version;

    /* Tombstones age out once their objects' data is long gone.
     * Reclaiming the data itself, and creating the tombstone that
     * records it, belong to compaction: this only ages the record. */
    {
        long tombCutoff = tp_tombstone_retention >= 0
            ? (long) current_systime - (long) tp_tombstone_retention * 86400L
            : -1;

        if (tombCutoff >= 0) {
            std::vector<Database::Tombstone> kept;
            int pruned = 0;

            for (const auto &t : MUCK::database().tombstones()) {
                if (t.deletedAt < tombCutoff) {
                    pruned++;
                    continue;
                }
                kept.push_back(t);
            }
            if (pruned)
                log_status("STORE: pruned %d aged tombstone%s\n",
                           pruned, pruned == 1 ? "" : "s");
            if (pruned)
                MUCK::database().setTombstones(std::move(kept));
        }
    }
    json ts = json::array();
    for (const auto &t : MUCK::database().tombstones()) {
        json e;
        e["uuid"] = t.uuid.toString();
        e["dbref"] = (int) t.ref;
        e["deleted_at"] = t.deletedAt;
        e["deleted_by"] = t.deletedBy.toString();
        e["deleted_rev"] = t.deletedRev;
        ts.push_back(e);
    }
    m["tombstones"] = ts;

    /* @tune parameters live in the manifest (docs section 6); the
     * legacy parmfile.cfg is still written as an export for ops
     * visibility, but the manifest is the authority on store boots. */
    {
        char *pbuf = NULL;
        size_t plen = 0;
        FILE *pf = open_memstream(&pbuf, &plen);

        if (pf) {
            tune_save_parms_to_file(pf);
            fclose(pf);

            json parms = json::array();
            std::istringstream ps(std::string(pbuf, plen));
            std::string line;

            while (std::getline(ps, line))
                if (!line.empty())
                    parms.push_back(jstr(line.c_str()));
            m["parms"] = parms;
            free(pbuf);
        }
    }

    /* The committed object index: which uuid owns each dbref, for
     * every object that has (or is about to have, in the set this
     * manifest rides with) a base file on disk. The manifest is the
     * commit point, so this index is what makes a crash-torn dump
     * unambiguous at the next boot: a file the index does not name is
     * an uncommitted leftover to discard, a file the index names but
     * that is missing is damage, and when two files claim one dbref
     * the index says which one is real.
     *
     * Serializing a 100k-entry index costs the game thread most of a
     * second, so the serialized blob is CACHED and spliced into the
     * manifest text; membership only changes on a baseWritten
     * transition (creation's first dump, reclamation), which
     * invalidates it (storeIndexInvalidate). */
    if (indexBlobDirty_) {
        json idx = json::object();

        for (dbref i = 0; i < MUCK::database().top(); i++) {
            DbObject *o = MUCK::database().get(i);

            if (o && o->baseWritten() && !o->uuid().isNil())
                idx[std::to_string(i)] = o->uuid().toString();
        }
        indexBlob_ = idx.dump();
        indexBlobDirty_ = false;
    }

    std::string out = m.dump(1);
    size_t brace = out.rfind('}');

    if (brace != std::string::npos)
        out.insert(brace, std::string(",\n \"index\": ") + indexBlob_ + "\n");
    return out;
}

/* the manifest's cached index blob is stale (a baseWritten flip) */
void
storeIndexInvalidate()
{
    g_objectStore.invalidateIndexBlob();
}

/* Build on the game thread, write on the dump thread: the manifest
 * reads live state (the tune parms, the tombstone table, the object
 * count) and the dump thread may not. */
bool
ObjectStore::writeManifest()
{
    std::string text = buildManifest();

    if (text.empty())
        return false;
    return atomicWrite(root_ + "/manifest.json", text);
}

std::string
ObjectStore::histPath(dbref i) const
{
    std::string p = objectPath(i);

    return p.substr(0, p.size() - 5) + ".hist";
}

/* Does any retained marker fall inside [from, to)? Retained markers
 * are the union of the global list and this object's own. */
static bool
markerInWindow(long from, long to,
               const std::vector<ObjectStore::Marker> &globals,
               const std::vector<ObjectStore::Marker> &own)
{
    for (const auto &m : globals)
        if (m.rev >= from && m.rev < to)
            return true;
    for (const auto &m : own)
        if (m.rev >= from && m.rev < to)
            return true;
    return false;
}

static std::vector<ObjectStore::Marker>
markersFromJson(const json &arr)
{
    std::vector<ObjectStore::Marker> out;

    if (!arr.is_array())
        return out;
    for (const auto &e : arr) {
        ObjectStore::Marker m;

        m.rev = e.value("rev", 0L);
        m.when = e.value("when", 0L);
        m.label = e.value("label", "");
        m.locked = e.value("locked", false);
        out.push_back(m);
    }
    return out;
}

static json
markersToJson(const std::vector<ObjectStore::Marker> &list)
{
    json arr = json::array();

    for (const auto &m : list) {
        json e;

        e["rev"] = m.rev;
        e["when"] = m.when;
        e["label"] = m.label;
        e["locked"] = m.locked;
        arr.push_back(e);
    }
    return arr;
}

/* The retention ladder (docs/DATABASE.txt 7.2): which markers survive,
 * given the keep_* tunes. Each unlocked marker is classified by age
 * into the finest enabled tier whose window still covers it; within a
 * tier, the newest marker per bucket of the tier's grain survives.
 * Markers younger than one hour are all kept, so a snapshot taken a
 * moment ago cannot be thinned away; locked markers always survive;
 * a marker no enabled tier covers is gone. Pure over its inputs, so
 * fire(), offline gc, and every test agree on the same answer. */
static std::vector<ObjectStore::Marker>
ladderSurvivors(const std::vector<ObjectStore::Marker> &markers, long now)
{
    struct Tier {
        long grain;
        long window;
    };
    const Tier tiers[] = {
        {3600L, (long) tp_keep_hourly_snapshots * 3600L},
        {86400L, (long) tp_keep_daily_snapshots * 86400L},
        {604800L, (long) tp_keep_weekly_snapshots * 604800L},
        {2592000L, (long) tp_keep_monthly_snapshots * 2592000L},
    };
    std::map<std::pair<int, long>, const ObjectStore::Marker *> best;
    std::vector<ObjectStore::Marker> out;

    for (const auto &m : markers) {
        long age = now - m.when;

        if (m.locked || age < 3600L) {
            out.push_back(m);
            continue;
        }
        for (int t = 0; t < 4; t++) {
            if (tiers[t].window <= 0 || age >= tiers[t].window)
                continue;

            auto key = std::make_pair(t, m.when / tiers[t].grain);
            auto it = best.find(key);

            if (it == best.end() || m.when > it->second->when)
                best[key] = &m;
            break;
        }
    }
    for (const auto &kv : best)
        out.push_back(*kv.second);
    std::sort(out.begin(), out.end(),
              [](const ObjectStore::Marker &a, const ObjectStore::Marker &b) {
                  return a.rev < b.rev;
              });
    return out;
}

/* The same ladder over an object's own scoped markers. */
static std::vector<ScopedMarker>
scopedLadderSurvivors(const std::vector<ScopedMarker> &markers, long now)
{
    std::vector<ObjectStore::Marker> conv;

    for (const auto &m : markers)
        conv.push_back({m.rev, m.when, m.label, m.locked});
    conv = ladderSurvivors(conv, now);

    std::vector<ScopedMarker> out;

    for (const auto &m : conv)
        out.push_back({m.rev, m.when, m.label, m.locked});
    return out;
}

bool
ObjectStore::saveObject(dbref i)
{
    std::string path = objectPath(i);

    /* the chain sanitizer's claimed set is per save PASS; a standalone
     * save is its own pass. Without this, the second single-object
     * save ever made would drop the object's entire contents chain. */
    if (!bulkSaveActive)
        chainClaimReset();

    ensureDirsFor(path);

    json cur = objectToJson(i);

    /* Stamp the revision this base represents. */
    cur["rev"] = rev_;

    /* A full base supersedes every layer that sat on it, so the
     * history goes with it. This is the compacting write, used by
     * conversion and by panic; ordinary saves append layers instead
     * (persist), which is what preserves rollback history.
     *
     * Dropping the layers also drops what the object's own markers
     * pointed at, so those go too rather than being left dangling at
     * revisions nothing can reconstruct. */
    cur.erase("markers");

    if (DbObject *o = MUCK::database().get(i)) {
        o->journal().discardTop();
        o->setBaseWritten(true);
        /* the in-memory mirror tracks the file */
        o->scopedMarkers().clear();
    }
    if (!atomicWrite(path, cur.dump(1)))
        return false;
    /* The history goes only AFTER the new base is safely on disk: it
     * is superseded by the base, and unlinking it first would strand
     * the OLD base with no layers if a crash lands between the two,
     * silently losing everything the layers held. */
    unlink(histPath(i).c_str());
    return true;
}

bool
ObjectStore::removeObject(dbref i)
{
    unlink(histPath(i).c_str());
    return unlink(objectPath(i).c_str()) == 0;
}

long
ObjectStore::snapshotGlobal(const char *label, bool locked)
{
    Marker m;

    /* A marker only covers what is on disk, so seal first: the layers
     * keep the era they were written in. */
    CaptureSet set = fire();

    /* the marker captures the CURRENT era; writes after the snapshot
     * stamp the next one, so a read at the marker excludes them */
    m.rev = rev_++;
    m.when = (long) current_systime;
    m.label = label ? label : "";
    m.locked = locked;
    markers_.push_back(m);

    /* Re-capture the manifest now that the marker is in it, and let
     * the dump thread write it. The game thread must not write
     * manifest.json itself while the dump thread might be writing the
     * same file. */
    set.manifest = buildManifest();
    hold(std::move(set));
    log_status("SNAPSHOT: global rev %ld (%s)%s\n", m.rev,
               m.label.empty() ? "unlabeled" : m.label.c_str(),
               locked ? " LOCKED" : "");
    return m.rev;
}

long
ObjectStore::snapshotObject(dbref i, const char *label, bool locked)
{
    std::string path = objectPath(i);

    /* flush the object first: the marker must cover its current
     * state, not whatever the last dump happened to capture */
    /* The era is global, so seal EVERY object before it advances: a
     * scoped snapshot must not strand another object's pending
     * layer in the era that just ended. The marker goes into the
     * object's file below, which needs what was sealed to be on disk,
     * so this one case does write. */
    syncNow();

    std::ifstream pf(path);

    if (!pf)
        return -1;
    json j = json::parse(pf, nullptr, false);

    if (j.is_discarded())
        return -1;

    Marker m;

    m.rev = rev_++;             /* same era rule as snapshotGlobal */
    m.when = (long) current_systime;
    m.label = label ? label : "";
    m.locked = locked;

    json arr = j.value("markers", json::array());
    json e;

    e["rev"] = m.rev;
    e["when"] = m.when;
    e["label"] = m.label;
    e["locked"] = m.locked;
    arr.push_back(e);
    j["markers"] = arr;

    if (!atomicWrite(path, j.dump(1)))
        return -1;
    /* keep the in-memory mirror in step with the file */
    if (DbObject *o = MUCK::database().get(i))
        o->scopedMarkers().push_back({m.rev, m.when, m.label, m.locked});
    writeManifest();            /* persist the advanced rev counter */
    log_status("SNAPSHOT: #%d rev %ld (%s)%s\n", i, m.rev,
               m.label.empty() ? "unlabeled" : m.label.c_str(),
               locked ? " LOCKED" : "");
    return m.rev;
}

std::vector<ObjectStore::Marker>
ObjectStore::objectMarkers(dbref i) const
{
    /* Served from the DbObject's in-memory mirror, never from the
     * file: this feeds examine's snapshot count, which is an ordinary
     * command and must not pay a parse of the object's whole base per
     * look. The mirror is loaded at boot, appended by snapshotObject,
     * and cleared when a full base write drops the history. A deleted
     * object still has its DbObject until reclamation, so its markers
     * list normally; after reclamation the files are gone and there is
     * nothing to list. */
    std::vector<Marker> out;
    DbObject *o = MUCK::database().get(i);

    if (!o)
        return out;
    for (const auto &m : o->scopedMarkers())
        out.push_back({m.rev, m.when, m.label, m.locked});
    return out;
}

/* Value of every entry as of a revision: current entries whose rev is
 * at or below the target, patched by history entries whose lifetime
 * covers the target. */
static json
entriesAtRev(const json &file, const std::string &hist, long rev)
{
    json out = json::object();

    /* The base is the state as of the revision stamped on it, and
     * layers only move forward. A target below that stamp cannot be
     * reconstructed: compaction may have folded the layers that would
     * have taken us back there. Returning the base anyway would hand
     * back a later state while claiming it is the earlier one. */
    if (file.contains("rev") && rev < file.value("rev", 0L))
        return out;

    /* Start from the base, which is the object's state as of the
     * revision stamped on the file. A base written after the target
     * revision cannot be walked backwards, so its entries are still
     * the starting point; the layers below only move forward. */
    if (file.contains("entries"))
        out = file["entries"];

    /* Apply every layer up to and including the target era, in the
     * order they were written. A null value is a removal. */
    std::istringstream hs(hist);
    std::string line;

    while (std::getline(hs, line)) {
        json layer = json::parse(line, nullptr, false);

        if (layer.is_discarded() || !layer.contains("entries"))
            continue;
        if (layer.value("era", 0L) > rev)
            break;              /* layers are appended in era order */

        const json &e = layer["entries"];

        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.value().is_null())
                out.erase(it.key());
            else
                out[it.key()] = it.value();
        }
    }
    return out;
}

bool
ObjectStore::rollbackObject(dbref i, long rev, std::string *err)
{
    /* rolling back to a marker whose set has not landed yet would
     * find no stored state */
    syncNow();

    std::ifstream pf(objectPath(i));

    if (!pf) {
        if (err)
            *err = "no stored state for that object.";
        return false;
    }
    json j = json::parse(pf, nullptr, false);

    if (j.is_discarded() || !j.contains("entries")) {
        if (err)
            *err = "the object's stored state is unreadable.";
        return false;
    }

    std::string hist;
    std::ifstream hf(histPath(i));

    if (hf)
        hist.assign(std::istreambuf_iterator<char>(hf),
                    std::istreambuf_iterator<char>());

    /* Refuse a target compaction has taken away rather than silently
     * handing back older state. Below the base rev nothing is
     * reconstructable; inside a merged layer's covered range
     * [covers_from, era) the intermediate states were folded into the
     * layer and only its era survives as a target. */
    long baseRev = j.value("rev", 0L);

    if (rev < baseRev) {
        if (err)
            *err = "revision " + std::to_string(rev)
                + " predates the oldest reconstructable state (rev "
                + std::to_string(baseRev)
                + "); older history has been compacted away.";
        return false;
    }
    {
        std::istringstream hs(hist);
        std::string line;

        while (std::getline(hs, line)) {
            json layer = json::parse(line, nullptr, false);

            if (layer.is_discarded() || !layer.contains("covers_from"))
                continue;

            long era = layer.value("era", 0L);
            long from = layer.value("covers_from", 0L);

            if (rev >= from && rev < era) {
                if (err)
                    *err = "revision " + std::to_string(rev)
                        + " was coalesced away; the nearest available "
                        "revisions are " + std::to_string(from - 1)
                        + " and " + std::to_string(era) + ".";
                return false;
            }
        }
    }

    json e = entriesAtRev(j, hist, rev);

    if (e.empty()) {
        if (err)
            *err = "nothing is reconstructable at revision "
                + std::to_string(rev) + ".";
        return false;
    }

    /* Everything a rollback restores goes through the setters, so the
     * restored state is journalled like any other change. Writing the
     * fields directly would leave the rollback unrecorded, and the
     * next restart would replay the pre-rollback state over it. */

    /* A rollback that changes a player's name or turns something
     * into or out of a player must keep the player-name lookup table
     * in step, exactly as @name and @frob do; a restored player whose
     * name is not in the table cannot log in until the next restart. */
    bool wasPlayer = MUCK::typeOf(i) == TYPE_PLAYER;

    if (wasPlayer)
        delete_player(i);

    /* Life and death are just another entry. If the target revision
     * has no deletion entry then the object was alive then, so it is
     * alive now: that is the whole of resurrection, and it needs no
     * path of its own. Both directions are CHANGES and must journal:
     * an unrecorded revival re-deletes itself at the next restart,
     * because the disk's latest word on $core/deleted still stands. */
    if (e.contains("$core/deleted")) {
        const json &d = e["$core/deleted"]["value"];

        if (DbObject *o = MUCK::database().get(i)) {
            o->markDeleted(d.is_array() && d.size() > 0 ? d[0].get<long>() : 0,
                           d.is_array() && d.size() > 1
                           ? (dbref) d[1].get<int>() : NOTHING);
            /* markDeleted leaves deletedRev_ unset, unlike
             * Database::deleteObject; without this a rollback into a
             * dead window after a later real deletion builds a
             * tombstone whose three fields come from three eras */
            o->setDeletedRev(store().rev());
        }
        MUCK::database().retireUUID(i);
    } else {
        MUCK::database().reviveHole(i);
    }
    journalRecord(i, "$core/deleted");

    /* Flags carry the type, and rolling back past a recycle has to put
     * the type back or the object stays a garbage shell wearing the
     * right name and properties. */
    if (e.contains("$core/flags")) {
        const json &fl = e["$core/flags"]["value"];

        if (fl.is_array() && fl.size() >= 4) {
            MUCK::setType(i, (MUCK::ObjectType) (fl[0].get<int>() & TYPE_MASK));
            MUCK::setFlags(i, fl[0].get<int>());
            MUCK::setFlags2(i, fl[1].get<int>());
            MUCK::setFlags3(i, fl[2].get<int>());
            MUCK::setFlags4(i, fl[3].get<int>());
        }
    }

    if (e.contains("$core/name") && e["$core/name"]["value"].is_string())
        MUCK::setName(i, junstr(e["$core/name"]["value"].get<std::string>()).c_str());

    /* Owner and location come back too. Recycling clears both, so an
     * object revived without them is owned by nobody and nowhere, and
     * anything that prints its owner's name walks off a null. */
    /* powers and timestamps are ordinary versioned entries and roll
     * back like everything else; leaving them out silently preserved
     * a power grant (or revoked one) across the rollback */
    if (e.contains("$core/powers")) {
        const json &pw = e["$core/powers"]["value"];

        if (pw.is_array() && pw.size() >= 2) {
            MUCK::setOwnPowers(i, (object_power_type) pw[0].get<int>());
            MUCK::setOwnPowers2(i, (object_power_type) pw[1].get<int>());
        }
    }
    if (e.contains("$core/ts")) {
        const json &ts = e["$core/ts"]["value"];

        if (ts.is_array() && ts.size() >= 7) {
            MUCK::setCreated(i, (time_t) ts[0].get<long>(),
                             (dbref) ts[4].get<long>());
            MUCK::setModified(i, (time_t) ts[1].get<long>(),
                              (dbref) ts[5].get<long>());
            MUCK::setLastUsed(i, (time_t) ts[2].get<long>(),
                              (dbref) ts[6].get<long>());
            MUCK::setUseCount(i, (int) ts[3].get<long>());
        }
    }

    if (e.contains("$core/owner")) {
        dbref own = refFromJson(e["$core/owner"]["value"]);

        /* the snapshot's owner may itself be recycled by now; an
         * object owned by a dead shell walks callers into nulls
         * (getPowers reads the OWNER), so fall back to GOD, the same
         * answer chown-on-toad gives */
        if (own == NOTHING || !MUCK::database().valid(own)
            || MUCK::typeOf(own) != TYPE_PLAYER)
            own = (dbref) 1;
        MUCK::setOwner(i, own);
    }
    if (e.contains("$core/location")) {
        dbref loc = refFromJson(e["$core/location"]["value"]);
        dbref cur = MUCK::getLocation(i);

        /* DETACH from wherever the object is now before attaching it
         * anywhere else, exactly as moveto does: attach alone leaves
         * the object listed in two containers at once */
        if (cur != NOTHING && MUCK::database().valid(cur)) {
            if (MUCK::typeOf(i) == TYPE_EXIT)
                MUCK::detachExit(cur, i);
            else
                MUCK::detachContent(cur, i);
        }
        if (loc != NOTHING && MUCK::database().valid(loc)
            && MUCK::typeOf(loc) != ObjectType::Garbage) {
            MUCK::setLocation(i, loc);
            if (MUCK::typeOf(i) == TYPE_EXIT)
                MUCK::attachExit(loc, i);
            else
                MUCK::attachContent(loc, i);
        } else {
            /* the snapshot's location no longer exists: nowhere is
             * the honest answer, and it must not stay listed in its
             * pre-rollback container */
            MUCK::setLocation(i, NOTHING);
        }
    }

    /* properties: wipe and rebuild from the snapshot. The wipe has to
     * be journalled too, or the properties it removed come back. */
    if (PropDirPtr pd_ = MUCK::propRoot(i)) {
        MUCK::journalRecordPropTree(i, "");
        pd_->clear();
    }
    {
        json props = json::array();

        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.key().empty() || it.key()[0] != '/')
                continue;
            json pe;

            pe["name"] = it.key().substr(1);
            pe["flags"] = it.value().value("flags", 0);
            pe["value"] = it.value()["value"];
            props.push_back(pe);
        }
        propsFromJson(i, props);
    }

    /* program source */
    if (Typeof(i) == TYPE_PROGRAM && e.contains("$type/source")) {
        std::vector<std::string> lines;

        for (const auto &ln : e["$type/source"]["value"])
            lines.push_back(junstr(ln.get<std::string>()));
        MUCK::programs().setSourceLines(i, std::move(lines));
        uncompile_program(i);
    }

    /* the other half of the name-table bracket above */
    if (MUCK::typeOf(i) == TYPE_PLAYER)
        add_player(i);

    DBDIRTY(i);
    log_status("ROLLBACK: #%d to rev %ld\n", i, rev);
    return true;
}

int
ObjectStore::snapshotSummary(dbref i, long *oldest) const
{
    int n = 0;
    long old = 0;
    auto acc = [&n, &old](const Marker &m) {
        n++;
        if (!old || m.when < old)
            old = m.when;
    };

    for (const auto &m : markers_)
        acc(m);
    for (const auto &m : objectMarkers(i))
        acc(m);
    if (oldest)
        *oldest = old;
    return n;
}

int
ObjectStore::saveAll(bool dirtyOnly)
{
    int written = 0;

    if (root_.empty())
        return -1;

    /* first save into a fresh store writes everything: freshly imported
     * objects carry no dirty flags */
    if (dirtyOnly && !isStore(root_.c_str()))
        dirtyOnly = false;

    /* chain sanitizer state is per save pass */
    chainClaimReset();
    bulkSaveActive = true;

    ensureDir(root_);
    ensureDir(root_ + "/objects");
    for (dbref i = 0; i < MUCK::database().top(); i++) {
        struct object *o = DBFETCH(i);

        if (MUCK::typeOf(i) == ObjectType::Garbage)
            continue;
        if (dirtyOnly && !(o->flags & OBJECT_CHANGED))
            continue;
        if (!saveObject(i)) {
            bulkSaveActive = false;
            return -1;
        }
        o->flags &= ~OBJECT_CHANGED;
        written++;
    }
    bulkSaveActive = false;
    /* a full save supersedes the journal: every file now carries its
     * object's current state, and replaying older segments over that
     * at boot would regress it. Mark everything distributed, commit,
     * then drop the segments. */
    journalDistributed_.store(journalSeq_);
    journalLandedCommitted_.store(journalSeq_);
    if (!writeManifest())
        return -1;
    unlinkDistributedSegments(journalSeq_);
    return written;
}

/* One store file's load-time preparation: read, parse, verify, then
 * apply its history sidecar and any committed journal lines. Pure
 * over its inputs (the committed index and the journal map are only
 * READ here), which is what lets the loader run many of these in
 * parallel; every mutation of shared state belongs to the serial
 * apply pass that follows. File-level failures set skip after
 * recording the problem; history line-level failures record and
 * carry on, exactly as the serial loader did. */
struct PreparedFile {
    std::string path;
    json j;
    json shape;                 /* fileToLoadShape output */
    json fileEntries;           /* the (pruned) entry map, for phase 2 */
    json fileMods;
    json fileMarkers;
    json dormantUnknown;        /* unknown-namespace slice */
    json dormantTypeInfoJ;      /* unsupported-type record */
    std::string tname;
    UUID fileUUID;
    dbref claimed = -1;
    long delEra = -1;
    std::string consumedUUID;   /* journalReplay key applied, if any */
    std::vector<std::string> problems;
    bool uncommitted = false;
    bool skip = false;
    bool noEntries = false;
    bool unsupported = false;
};

static void
prepareStoreFile(const std::string &full, PreparedFile &out,
                 const json *index, const std::set<std::string> &indexUUIDs,
                 int top, const std::string &root,
                 const std::map<std::string,
                                std::vector<json> > &journalReplay)
{
    out.path = full;

    std::ifstream f(full);

    if (!f) {
        out.problems.push_back(full + ": cannot open for reading");
        out.skip = true;
        return;
    }
    out.j = json::parse(f, nullptr, false);
    if (out.j.is_discarded()) {
        out.problems.push_back(full
                               + ": not valid JSON (truncated or corrupt)");
        out.skip = true;
        return;
    }

    json &j = out.j;

    out.fileUUID = UUID::parse(j.value("uuid", ""));
    out.claimed = (dbref) j.value("dbref", -1);
    if (out.fileUUID.isNil()) {
        out.problems.push_back(full + ": missing or unparsable uuid");
        out.skip = true;
        return;
    }
    {
        /* case-insensitively: UUID::parse accepts upper hex, so a
         * case-normalizing restore step must not read as damage */
        std::string fname = full.substr(full.rfind('/') + 1);

        for (auto &c : fname)
            c = (char) tolower(c);
        if (out.fileUUID.toString() + ".json" != fname) {
            out.problems.push_back(full + ": file name does not match "
                                   "the uuid inside it ("
                                   + out.fileUUID.toString() + ")");
            out.skip = true;
            return;
        }
    }
    if (out.claimed < 0) {
        out.problems.push_back(full + ": missing or unparsable dbref");
        out.skip = true;
        return;
    }
    if (index) {
        /* the committed index says which uuid owns this dbref; a file
         * it does not name is an uncommitted leftover from an
         * interrupted dump */
        auto it = index->find(std::to_string(out.claimed));

        if (it == index->end()
            || it->get<std::string>() != out.fileUUID.toString()) {
            /* the file's own dbref field does not match the index at
             * that dbref. Only DISCARD it as an uncommitted leftover
             * if its uuid appears nowhere in the index at all. If the
             * uuid IS committed under some dbref (the file's dbref
             * field is corrupt, but the object is real), discarding
             * would destroy recoverable data under --force-load, so
             * report it as damage and keep the file for repair. */
            if (indexUUIDs.count(out.fileUUID.toString())) {
                out.problems.push_back(full + ": committed uuid but its "
                        "internal dbref (#" + std::to_string(out.claimed)
                        + ") disagrees with the index; kept for repair");
                out.skip = true;
                return;
            }
            out.uncommitted = true;
            return;
        }
    } else if (out.claimed >= top) {
        /* no index (store written before it existed): a dbref past
         * the committed top is still provably uncommitted */
        out.uncommitted = true;
        return;
    }
    if (!j.contains("entries")) {
        /* the applier reports this one */
        out.noEntries = true;
        return;
    }

    /* The stored object is its base plus the layers its history
     * holds, applied in era order (docs/DATABASE.txt 6). delEra is
     * the era the deletion entry was recorded in, for reclamation:
     * from the base's rev when the base already carries it, else
     * from the layer that introduces it (a later null revives). */
    if (j["entries"].contains("$core/deleted"))
        out.delEra = j.value("rev", 0L);
    {
        std::string histFile = full.substr(0, full.size() - 5) + ".hist";
        std::ifstream hf(histFile);

        if (hf) {
            std::string line;
            long lineNo = 0;

            while (std::getline(hf, line)) {
                json layer = json::parse(line, nullptr, false);

                lineNo++;
                if (layer.is_discarded()) {
                    out.problems.push_back(histFile + " line "
                                           + std::to_string(lineNo)
                                           + ": not valid JSON (truncated "
                                           "or corrupt); the changes it "
                                           "carried are lost");
                    continue;
                }
                /* a history line written by the retired copy-on-write
                 * path describes an OLD value and carries "key";
                 * nothing writes those any more */
                if (layer.contains("key")) {
                    out.problems.push_back(histFile + " line "
                                           + std::to_string(lineNo)
                                           + ": legacy copy-on-write "
                                           "record; this store predates "
                                           "the current format");
                    continue;
                }
                if (!layer.contains("entries")) {
                    out.problems.push_back(histFile + " line "
                                           + std::to_string(lineNo)
                                           + ": no entries map");
                    continue;
                }
                if (layer.contains("type"))
                    j["type"] = layer["type"];
                {
                    auto dit = layer["entries"].find("$core/deleted");

                    if (dit != layer["entries"].end())
                        out.delEra = dit.value().is_null()
                            ? -1 : layer.value("era", 0L);
                }
                for (auto lit = layer["entries"].begin();
                     lit != layer["entries"].end(); ++lit) {
                    if (lit.value().is_null())
                        j["entries"].erase(lit.key());
                    else
                        j["entries"][lit.key()] = lit.value();
                }
            }
        }
    }

    /* committed journal lines not yet folded into this object's
     * files replay over it now, newest last; the map is shared and
     * read-only here, the erase happens in the serial pass */
    {
        auto jr = journalReplay.find(j.value("uuid", ""));

        if (jr != journalReplay.end()) {
            for (const json &l : jr->second) {
                if (l.value("full", false)) {
                    /* a full line is the whole object file, verbatim */
                    j = l["entries"];
                    j["rev"] = l.value("era", 0L);
                    out.delEra = j.contains("entries")
                        && j["entries"].contains("$core/deleted")
                        ? l.value("era", 0L) : -1;
                    continue;
                }
                if (l.contains("type"))
                    j["type"] = l["type"];
                {
                    auto dit = l["entries"].find("$core/deleted");

                    if (dit != l["entries"].end())
                        out.delEra = dit.value().is_null()
                            ? -1 : l.value("era", 0L);
                }
                for (auto lit = l["entries"].begin();
                     lit != l["entries"].end(); ++lit) {
                    if (lit.value().is_null())
                        j["entries"].erase(lit.key());
                    else
                        j["entries"][lit.key()] = lit.value();
                }
            }
            out.consumedUUID = jr->first;
        }
    }
    if (!j.contains("entries")) {
        /* a journal full-line replacement without an entry model */
        out.noEntries = true;
        return;
    }

    /* Dormant detection and the load shape, off the serial path; the
     * shared dormant maps themselves are written by the applier. */
    out.tname = j.value("type", "garbage");
    out.unsupported = !storedTypeSupported(out.tname);
    {
        json unknown = json::object();

        for (auto eit = j["entries"].begin();
             eit != j["entries"].end(); ++eit)
            if (!knownNamespace(eit.key())
                || (out.unsupported
                    && eit.key().rfind("$type/", 0) == 0))
                unknown[eit.key()] = eit.value();
        out.dormantUnknown = std::move(unknown);
    }
    out.fileMods = j.value("modules", json::array());
    out.fileMarkers = j.value("markers", json::array());
    if (out.unsupported) {
        /* The type module is absent: keep its slice dormant, remember
         * what the object would have been, and load only the core. */
        json info;
        int bits = 0;
        const json &ents = j["entries"];
        auto fl = ents.find("$core/flags");

        if (fl != ents.end() && (*fl)["value"].is_array())
            bits = (*fl)["value"][0].get<int>() & TYPE_MASK;
        info["name"] = out.tname;
        info["bits"] = bits;
        out.dormantTypeInfoJ = std::move(info);

        for (auto eit = j["entries"].begin();
             eit != j["entries"].end();)
            if (eit.key().rfind("$type/", 0) == 0)
                eit = j["entries"].erase(eit);
            else
                ++eit;
    }
    out.shape = fileToLoadShape(j, root);
    out.fileEntries = std::move(j["entries"]);
    out.j = json();             /* the raw parse is dead; free it */
}

dbref
ObjectStore::loadAll()
{
    std::ifstream mf(root_ + "/manifest.json");
    std::vector<PendingLinks> later;

    if (!mf) {
        fprintf(stderr, "STORE: cannot open %s/manifest.json; without "
                "the manifest there is no committed state to boot "
                "from.\n", root_.c_str());
        log_status("DIE: store manifest missing\n");
        return -1;
    }
    json manifest = json::parse(mf, nullptr, false);
    if (manifest.is_discarded()) {
        fprintf(stderr, "STORE: %s/manifest.json is not valid JSON "
                "(truncated or corrupt); restore it from backup.\n",
                root_.c_str());
        log_status("DIE: store manifest unparsable\n");
        return -1;
    }

    if (manifest.value("format", 0) != STORE_FORMAT) {
        fprintf(stderr,
                "STORE: manifest format %d, this build wants %d. "
                "Rebuild the store by re-importing the legacy flat "
                "database.\n",
                manifest.value("format", 0), STORE_FORMAT);
        log_status("DIE: store format mismatch (%d, want %d)\n",
                   manifest.value("format", 0), STORE_FORMAT);
        return -1;
    }

    int top = manifest.value("next_dbref", 0);

    /* password hashing state travels in the manifest; without it a
     * store boot would compare plaintext against stored hashes and
     * reject every login */
    MUCK::PasswordHash::enabled = manifest.value("hash_passwords", false);
    MUCK::PasswordHash::version = manifest.value("hash_version", 0);

    rev_ = manifest.value("rev", 0L);
    markers_ = markersFromJson(manifest.value("markers", json::array()));
    /* nothing read from disk is a change: setters record nothing for
     * the whole load, which is also what lets phase two run its
     * setters in parallel without racing on the dirty index */
    journalSuppress(true);
    journalSeq_ = manifest.value("journal_committed", 0L);
    journalDistributed_.store(manifest.value("journal_distributed", 0L));
    journalLandedCommitted_.store(journalSeq_);
    journalUnlinked_ = journalDistributed_.load();

    if (manifest.contains("parms")) {
        std::string text;

        for (const auto &l : manifest["parms"])
            text += junstr(l.get<std::string>()) + "\n";

        FILE *pf = fmemopen((void *) text.data(), text.size(), "r");

        if (pf) {
            tune_load_parms_from_file(pf, NOTHING, -1);
            fclose(pf);
            log_status("STORE: applied %d tune parms from the manifest\n",
                       (int) manifest["parms"].size());
        }
    }

    if (manifest.contains("tombstones")) {
        std::vector<Database::Tombstone> list;

        for (const auto &e : manifest["tombstones"]) {
            Database::Tombstone t;

            t.uuid = UUID::parse(e.value("uuid", "").c_str());
            t.ref = (dbref) e.value("dbref", -1);
            t.deletedAt = e.value("deleted_at", 0L);
            t.deletedBy = UUID::parse(e.value("deleted_by", "").c_str());
            t.deletedRev = e.value("deleted_rev", 0L);
            list.push_back(t);
        }
        MUCK::database().setTombstones(std::move(list));
    }

    /* A recycled object still has a file for as long as a snapshot
     * can reach it, and it loads like any other object: its deletion
     * entry is what makes it dead. Only a tombstone means the data is
     * actually gone, and a tombstoned object has no file left to
     * load. */
    MUCK::database().ensureTop(top);

    /* ensureTop pre-initialized every slot as a garbage shell */

    /* --- load-time integrity state (docs/DATABASE.txt 6) ---
     *
     * problems: damage. Anything here refuses the boot unless
     * --force-load was given; booting past damaged or missing files
     * silently regresses objects to older state.
     *
     * uncommitted: normal crash recovery, not damage. The manifest is
     * the commit point; a file its index does not name was written by
     * a dump that never committed, and is discarded (after the boot
     * is known to proceed) exactly as any other unsaved change is
     * lost at a crash. Without the discard, the dbref such a file
     * claims can be reassigned by the recovered world, and the next
     * boot would find two files claiming one slot. */
    std::vector<std::string> problems;
    std::vector<std::string> uncommitted;
    auto damaged = [&problems](const std::string &msg) {
        /* both channels: a serving boot has detached and closed
         * stderr by the time the store loads, so the log file is the
         * only place an operator can read the failure */
        fprintf(stderr, "STORE: %s\n", msg.c_str());
        log_status("STORE: %s\n", msg.c_str());
        problems.push_back(msg);
    };
    const json *index = manifest.contains("index")
        && manifest["index"].is_object() ? &manifest["index"] : nullptr;
    /* reverse of the index: every committed uuid, to tell a file with
     * a corrupt dbref field (uuid still committed) from a genuinely
     * uncommitted leftover */
    std::set<std::string> indexUUIDs;

    if (index)
        for (auto it = index->begin(); it != index->end(); ++it)
            if (it->is_string())
                indexUUIDs.insert(it->get<std::string>());
    std::map<int, std::string> refClaimed;  /* dbref -> file, dup check */
    std::set<std::string> jsonSeen, histSeen;

    /* Journal segments in (distributed, committed] carry committed
     * changes not yet folded into the per-object files; they replay
     * over the files during the walk below. Beyond committed is an
     * uncommitted crash leftover; at or below distributed is an
     * already-folded leftover; both are discarded once the boot is
     * known to proceed. Lines are grouped per uuid in segment order. */
    std::map<std::string, std::vector<json> > journalReplay;
    std::vector<std::string> staleSegments;
    {
        std::vector<std::pair<long, std::string> > committedSegs;
        DIR *jd = opendir((root_ + "/journal").c_str());

        if (jd) {
            struct dirent *je;

            while ((je = readdir(jd)) != NULL) {
                std::string n = je->d_name;

                if (n.size() < 7 || n.compare(n.size() - 6, 6, ".jsonl"))
                    continue;

                long seq = atol(n.c_str());
                std::string p = root_ + "/journal/" + n;

                if (seq <= 0)
                    continue;
                if (seq <= journalDistributed_.load()
                    || seq > journalSeq_)
                    staleSegments.push_back(p);
                else
                    committedSegs.push_back({seq, p});
            }
            closedir(jd);
        }
        std::sort(committedSegs.begin(), committedSegs.end());
        for (const auto &cs : committedSegs) {
            std::ifstream sf(cs.second);
            std::string line;
            long lineNo = 0;

            while (sf && std::getline(sf, line)) {
                if (line.empty())
                    continue;
                lineNo++;

                json l = json::parse(line, nullptr, false);

                if (l.is_discarded() || !l.contains("uuid")
                    || !l.contains("entries")) {
                    damaged(cs.second + " line " + std::to_string(lineNo)
                            + ": committed journal record is unreadable");
                    continue;
                }
                journalReplay[l["uuid"].get<std::string>()]
                    .push_back(std::move(l));
            }
        }
    }

    /* --- pass A: enumerate (serial, cheap) --- */
    std::string objroot = root_ + "/objects";
    DIR *d0 = opendir(objroot.c_str());

    if (!d0)
        return -1;

    std::vector<std::string> jsonFiles;
    struct dirent *e0;

    /* readdir returns NULL for BOTH end-of-directory and an I/O error
     * (EIO, ESTALE): a mid-listing disk fault would silently truncate
     * enumeration, and files never seen are invisible to the damage
     * checker, which only judges files it saw. Reset errno before each
     * loop and treat a nonzero errno at the end as damage. */
    errno = 0;
    while ((e0 = readdir(d0)) != NULL) {
        if (e0->d_name[0] == '.')
            continue;
        std::string l1 = objroot + "/" + e0->d_name;
        DIR *d1 = opendir(l1.c_str());
        if (!d1)
            continue;
        struct dirent *e1;
        errno = 0;
        while ((e1 = readdir(d1)) != NULL) {
            if (e1->d_name[0] == '.')
                continue;
            std::string l2 = l1 + "/" + e1->d_name;
            DIR *d2 = opendir(l2.c_str());
            if (!d2)
                continue;
            struct dirent *e2;
            errno = 0;
            while ((e2 = readdir(d2)) != NULL) {
                size_t n = strlen(e2->d_name);
                std::string full = l2 + "/" + e2->d_name;

                if (n > 5 && !strcmp(e2->d_name + n - 5, ".hist")) {
                    histSeen.insert(full);
                    continue;
                }
                if (n < 6 || strcmp(e2->d_name + n - 5, ".json"))
                    continue;   /* stray .tmp leftovers are not damage */
                jsonSeen.insert(full);
                jsonFiles.push_back(full);
            }
            if (errno != 0)
                damaged(l2 + ": directory read failed mid-listing ("
                        + std::string(strerror(errno)) + "); some "
                        "objects may not have been seen");
            closedir(d2);
            errno = 0;
        }
        if (errno != 0)
            damaged(l1 + ": directory read failed mid-listing ("
                    + std::string(strerror(errno)) + ")");
        closedir(d1);
        errno = 0;
    }
    if (errno != 0)
        damaged(objroot + ": directory read failed mid-listing ("
                + std::string(strerror(errno)) + ")");
    closedir(d0);

    /* Deterministic order: with two files claiming one dbref, the
     * duplicate check keeps whichever the applier sees FIRST, and
     * readdir order is filesystem-dependent. Sorting makes the
     * survivor the lexicographically first path, so the same damaged
     * store loads identically on every boot and every machine. */
    std::sort(jsonFiles.begin(), jsonFiles.end());

    /* --- pass B/C: a bounded parse pipeline. Workers read, parse,
     * verify, and merge history and journal for at most a window of
     * files ahead of the applier; the main thread applies each result
     * in file order (shared tables, the duplicate check, dormant
     * slices, phase one). Workers only READ shared inputs (the
     * committed index, the journal replay map), so preparation needs
     * no locks. The window bound is not optional: a parsed shape is
     * an order of magnitude bigger than its file, and holding a
     * hundred thousand of them at once was an OOM kill, not a
     * speedup. --- */
    /* journal keys the applier consumed; erased once the parse pool
     * has joined (the workers read the map concurrently) */
    std::vector<std::string> consumedJournalUUIDs;
    auto applyPrepared = [&](PreparedFile &prep) {
        for (const auto &msg : prep.problems)
            damaged(msg);
        if (prep.uncommitted) {
            uncommitted.push_back(prep.path);
            return;
        }
        if (prep.skip)
            return;

        const std::string &full = prep.path;
        json &j = prep.j;
        UUID fileUUID = prep.fileUUID;
        dbref claimed = prep.claimed;
        long delEra = prep.delEra;

        {
            auto dup = refClaimed.find(claimed);

            if (dup != refClaimed.end()) {
                damaged(full + " and " + dup->second
                        + " both claim dbref #"
                        + std::to_string(claimed));
                return;
            }
            refClaimed[claimed] = full;
        }
        /* NOT erased here: the parse workers are still running and
         * are calling journalReplay.find() concurrently, so mutating
         * the map now is undefined behavior. Consumed keys are
         * collected and erased after the pool joins; the only later
         * reader is the journal-founded-object pass, which sees the
         * same result either way. */
        if (!prep.consumedUUID.empty())
            consumedJournalUUIDs.push_back(prep.consumedUUID);

        if (prep.noEntries) {
            damaged(full + ": no entry model; this store predates "
                    "the current format. Rebuild it by re-importing "
                    "the legacy flat database.");
            return;
        }

        /* the heavy json work (dormant detection, the load shape) was
         * done by the worker; what remains is shared-table inserts and
         * phase one's field pokes */
        if (!prep.dormantUnknown.empty())
            dormantEntries[fileUUID] = std::move(prep.dormantUnknown);
        if (!prep.fileMods.empty())
            dormantModules[fileUUID] = prep.fileMods;
        if (prep.unsupported)
            dormantTypeInfo[fileUUID] = std::move(prep.dormantTypeInfoJ);

        objectFromJsonPhase1(prep.shape, later);
        /* it came off disk, so it has a base there. This includes
         * recycled shells: their retained files must stay in the
         * manifest index, or the next boot would discard them as
         * uncommitted. */
        if (DbObject *bo = MUCK::database().get(claimed)) {
            bo->setBaseWritten(true);
            if (bo->isDeleted() && delEra >= 0)
                bo->setDeletedRev(delEra);
            for (const auto &me : prep.fileMarkers)
                bo->scopedMarkers().push_back(
                    {me.value("rev", 0L),
                     me.value("when", 0L),
                     me.value("label", std::string()),
                     me.value("locked", false)});
        }
        if (prep.unsupported)
            MUCK::setType(claimed, ObjectType::Unsupported);
        if (!later.empty() && later.back().ref == claimed) {
            later.back().entries = std::move(prep.fileEntries);
            for (const auto &mn : prep.fileMods)
                later.back().modules.push_back(mn.get<std::string>());
        }
    };

    {
        const size_t nfiles = jsonFiles.size();
        const size_t window = 256;
        unsigned nthreads = std::thread::hardware_concurrency();

        if (nthreads < 1)
            nthreads = 1;
        if (nthreads > 16)
            nthreads = 16;

        if (nfiles < 64 || nthreads <= 1) {
            for (size_t i = 0; i < nfiles; i++) {
                PreparedFile prep;

                prepareStoreFile(jsonFiles[i], prep, index, indexUUIDs, top, root_,
                                 journalReplay);
                applyPrepared(prep);
            }
        } else {
            std::vector<PreparedFile> ring(window);
            std::vector<char> ready(window, 0);
            std::mutex pm;
            std::condition_variable canProduce, canConsume;
            size_t nextClaim = 0, applied = 0;

            auto workerFn = [&]() {
                for (;;) {
                    size_t i;

                    {
                        std::unique_lock<std::mutex> lk(pm);

                        canProduce.wait(lk, [&] {
                            return nextClaim >= nfiles
                                || nextClaim < applied + window;
                        });
                        if (nextClaim >= nfiles)
                            return;
                        i = nextClaim++;
                    }

                    PreparedFile tmp;

                    /* an escaping exception here would terminate the
                     * process; a bad file is damage, not a crash */
                    try {
                        prepareStoreFile(jsonFiles[i], tmp, index, indexUUIDs, top,
                                         root_, journalReplay);
                    } catch (const std::exception &e) {
                        tmp = PreparedFile();
                        tmp.path = jsonFiles[i];
                        tmp.skip = true;
                        tmp.problems.push_back(jsonFiles[i]
                                               + ": unreadable ("
                                               + e.what() + ")");
                    }
                    {
                        std::unique_lock<std::mutex> lk(pm);

                        ring[i % window] = std::move(tmp);
                        ready[i % window] = 1;
                    }
                    canConsume.notify_all();
                }
            };
            std::vector<std::thread> pool;

            for (unsigned t = 0; t < nthreads; t++)
                pool.emplace_back(workerFn);

            for (size_t i = 0; i < nfiles; i++) {
                PreparedFile prep;

                {
                    std::unique_lock<std::mutex> lk(pm);

                    canConsume.wait(lk, [&] {
                        return ready[i % window] != 0;
                    });
                    prep = std::move(ring[i % window]);
                    ring[i % window] = PreparedFile();
                    ready[i % window] = 0;
                    applied = i + 1;
                }
                canProduce.notify_all();
                applyPrepared(prep);
            }
            for (auto &th : pool)
                th.join();
        }
    }
    /* safe now: no worker is reading the map any more */
    for (const auto &u : consumedJournalUUIDs)
        journalReplay.erase(u);

    /* Objects that exist only in the journal: created and committed
     * after the last distribution, so no per-object file exists yet.
     * The full journal record IS the object file, verbatim; rebuild
     * exactly as a file would have loaded. A delta-only run with no
     * file underneath is damage. */
    for (auto &kv : journalReplay) {
        json j;
        bool founded = false;
        long delEra = -1;

        for (json &l : kv.second) {
            if (l.value("full", false)) {
                j = l["entries"];
                j["rev"] = l.value("era", 0L);
                founded = true;
                delEra = j.contains("entries")
                    && j["entries"].contains("$core/deleted")
                    ? l.value("era", 0L) : -1;
                continue;
            }
            if (!founded)
                continue;
            if (l.contains("type"))
                j["type"] = l["type"];
            {
                auto dit = l["entries"].find("$core/deleted");

                if (dit != l["entries"].end())
                    delEra = dit.value().is_null()
                        ? -1 : l.value("era", 0L);
            }
            for (auto lit = l["entries"].begin();
                 lit != l["entries"].end(); ++lit) {
                if (lit.value().is_null())
                    j["entries"].erase(lit.key());
                else
                    j["entries"][lit.key()] = lit.value();
            }
        }
        if (!founded) {
            damaged("journal carries layers for uuid " + kv.first
                    + " but the store has no file and no full record "
                    "for it");
            continue;
        }

        dbref claimed = (dbref) j.value("dbref", -1);

        if (claimed < 0 || claimed >= top) {
            damaged("journal object uuid " + kv.first + " claims dbref #"
                    + std::to_string(claimed)
                    + " outside the manifest's range");
            continue;
        }
        /* The manifest is newer truth than the journal: an object a
         * LATER manifest dropped from the committed index (reclaimed
         * after its creation was journaled but before distribution)
         * must stay gone, not be resurrected from its segment. */
        if (index) {
            auto iit = index->find(std::to_string(claimed));

            if (iit == index->end()
                || iit->get<std::string>() != kv.first) {
                log_status("STORE: journal object uuid %s (#%d) is no "
                           "longer in the committed index; discarded\n",
                           kv.first.c_str(), claimed);
                continue;
            }
        }
        {
            auto dup = refClaimed.find(claimed);

            if (dup != refClaimed.end()) {
                damaged("journal object uuid " + kv.first + " and "
                        + dup->second + " both claim dbref #"
                        + std::to_string(claimed));
                continue;
            }
        }
        if (!storedTypeSupported(j.value("type", "garbage"))) {
            damaged("journal object uuid " + kv.first
                    + " has type \"" + j.value("type", "garbage")
                    + "\" with no loaded type module");
            continue;
        }

        json fileEntries = j["entries"];
        json fileMods = j.value("modules", json::array());

        j = fileToLoadShape(j, root_);
        objectFromJsonPhase1(j, later);
        if (DbObject *bo = MUCK::database().get(claimed)) {
            bo->setBaseWritten(true);
            if (bo->isDeleted() && delEra >= 0)
                bo->setDeletedRev(delEra);
        }
        if (!later.empty() && later.back().ref == claimed) {
            later.back().entries = fileEntries;
            for (const auto &mn : fileMods)
                later.back().modules.push_back(mn.get<std::string>());
        }
        refClaimed[claimed] = "journal:" + kv.first;
    }

    /* orphaned history: layers with no base beside them cannot be
     * applied to anything, so the state they carried is unreachable */
    for (const auto &h : histSeen) {
        std::string beside = h.substr(0, h.size() - 5) + ".json";

        if (!jsonSeen.count(beside))
            damaged(h + ": history with no object file beside it");
    }

    /* every object the committed index names must have loaded */
    if (index)
        for (auto it = index->begin(); it != index->end(); ++it) {
            int r = atoi(it.key().c_str());

            if (!refClaimed.count(r))
                damaged("object #" + it.key() + " (uuid "
                        + it.value().get<std::string>()
                        + "): file missing from the store");
        }

    if (!problems.empty()) {
        if (!forceLoadFlag) {
            fprintf(stderr,
                    "STORE: %d integrity problem(s); refusing to boot. "
                    "Fix the store or restore it from backup, or start "
                    "with --force-load to boot anyway with the damaged "
                    "data skipped.\n", (int) problems.size());
            log_status("DIE: store integrity: %d problem(s) at load\n",
                       (int) problems.size());
            return -1;
        }
        fprintf(stderr,
                "STORE: --force-load: booting despite %d integrity "
                "problem(s); the damaged data was skipped.\n",
                (int) problems.size());
        log_status("STORE: --force-load boot with %d problem(s)\n",
                   (int) problems.size());
    }

    /* Now that the boot is going ahead, discard uncommitted leftovers
     * from an interrupted dump: the dbref such a file claims can be
     * reassigned by the recovered world, and the change it carried is
     * exactly as lost as any other change the crash threw away.
     * Deferred to here so a refused boot never modifies the store it
     * is refusing to load. */
    for (const auto &u : uncommitted) {
        log_status("STORE: discarding uncommitted %s from an "
                   "interrupted dump\n", u.c_str());
        unlink(u.c_str());
        unlink((u.substr(0, u.size() - 5) + ".hist").c_str());
    }
    /* journal segments past the committed watermark (an interrupted
     * dump's tail) or at or below the distributed one (already folded,
     * not yet removed) go the same way */
    for (const auto &s : staleSegments) {
        log_status("STORE: discarding stale journal segment %s\n",
                   s.c_str());
        unlink(s.c_str());
    }

    /* Phase two in PARALLEL: building thirteen million properties was
     * the actual boot cost, and it is object-local work. Every write
     * a phase-two entry makes lands on its own object (its property
     * tree, its module slices, its own contents and exits vectors,
     * its fields); other objects are only READ (resolving refs to
     * pointers), and phase one already built every slot and the whole
     * uuid map. The two shared touchpoints are handled: journaling is
     * suppressed for the load, and add_player moved to a serial pass
     * below. Lock properties parse on the dbload path, which never
     * touches the matcher; the parse buffers are stack locals. */
    {
        unsigned nthreads = std::thread::hardware_concurrency();

        if (nthreads < 1)
            nthreads = 1;
        if (nthreads > 16)
            nthreads = 16;
        if (later.size() < 64)
            nthreads = 1;

        /* the ART node pools are shared allocators; they lock
         * themselves while phase two builds trees from many threads */
        setPropPoolsThreadSafe(nthreads > 1);

        std::atomic<size_t> nextPl(0);
        auto phase2Worker = [&]() {
            for (;;) {
                size_t k = nextPl.fetch_add(1);

                if (k >= later.size())
                    break;
                /* same rule as every other worker pool: a throw would
                 * terminate the process mid-boot */
                try {
                    objectFromJsonPhase2(later[k]);
                } catch (const std::exception &e) {
                    fprintf(stderr, "STORE: #%d failed to load its "
                            "properties or links: %s\n",
                            later[k].ref, e.what());
                }
            }
        };

        if (nthreads <= 1) {
            phase2Worker();
        } else {
            std::vector<std::thread> pool;

            for (unsigned t = 0; t < nthreads; t++)
                pool.emplace_back(phase2Worker);
            for (auto &th : pool)
                th.join();
        }
        setPropPoolsThreadSafe(false);
    }

    /* A stale or hand-edited manifest can name a next_dbref below what
     * the index and disk actually hold; ensureTop grew the world past
     * it silently during the load. The post-load passes below MUST
     * cover every real slot, or a high-numbered player never enters
     * the name table and cannot log in. Use the live top from here. */
    top = (int) MUCK::database().top();

    /* the player name lookup table is runtime state the flat importer
     * built as it read; without it every login fails before the
     * password is even checked. Serial: the table is shared. */
    for (dbref i = 0; i < top; i++)
        if (MUCK::typeOf(i) == TYPE_PLAYER)
            add_player(i);

    /* A recycled object keeps its file and its state, so it loaded
     * like anything else; it is dead because its entry says so, and
     * its uuid stops resolving exactly as it did when it was
     * recycled. */
    for (dbref i = 0; i < top; i++)
        if (DbObject *o = MUCK::database().get(i))
            if (o->isDeleted())
                MUCK::database().retireUUID(i);

    /* holes (never-written slots) stay dead shells; mark them so
     * modern code sees isDeleted() */
    for (dbref i = 0; i < top; i++)
        if (MUCK::typeOf(i) == ObjectType::Garbage)
            MUCK::database().noteHole(i);

    /* A fresh load is clean by definition: the setters used during
     * wiring record journal entries and raise dirty flags that would
     * otherwise force a full rewrite, and revision churn, on the next
     * dump. What was just read is not a change. */
    for (dbref i = 0; i < top; i++) {
        DBFETCH(i)->flags &= ~OBJECT_CHANGED;
        if (DbObject *o = MUCK::database().get(i)) {
            o->journal().discardTop();
            forgetDirty(i);
            /* it came off disk, so it has a base there already */
            if (MUCK::typeOf(i) != ObjectType::Garbage)
                o->setBaseWritten(true);
        }
    }

    journalSuppress(false);
    return (dbref) top;
}


/* ------------------------------------------------------------------ */
/* Fire and persist (docs/DATABASE.txt 7.1)                           */
/* ------------------------------------------------------------------ */

/* Decide one object's compaction work (game thread, fire time).
 * Returns false when the object has no base on disk and so needs no
 * order. Mutates live state for its decisions: prunes the object's
 * scoped marker mirror through the ladder, and on reclaim records
 * the tombstone and drops the object from the committed index
 * (baseWritten false), so the manifest built after this excludes it
 * and the dump thread can unlink the files once that manifest lands. */
bool
ObjectStore::buildCompactOrder(dbref i, const std::vector<long> &globalRevs,
                               long now, CompactOrder *out)
{
    DbObject *o = MUCK::database().get(i);

    if (!o || !o->baseWritten() || o->uuid().isNil())
        return false;

    out->ref = i;
    out->uuid = o->uuid().toString();
    out->reclaim = false;

    /* the object's own markers age on the same ladder */
    std::vector<ScopedMarker> keep =
        scopedLadderSurvivors(o->scopedMarkers(), now);

    o->scopedMarkers() = keep;

    out->survivors = globalRevs;
    for (const auto &m : keep) {
        out->survivors.push_back(m.rev);
        out->scopedKeep.push_back({m.rev, m.when, m.label, m.locked});
    }
    std::sort(out->survivors.begin(), out->survivors.end());
    out->survivors.erase(
        std::unique(out->survivors.begin(), out->survivors.end()),
        out->survivors.end());

    if (o->isDeleted()) {
        /* a recycled shell stays for as long as any retained snapshot
         * predates the deletion and so could still revive it; when
         * none can, the data is finally reclaimed and the tombstone
         * becomes the record that the dbref was used */
        long drev = o->deletedRev();
        bool revivable = drev < 0;      /* unknown errs toward keeping */

        for (long s : out->survivors)
            if (s < drev) {
                revivable = true;
                break;
            }
        if (!revivable) {
            Database::Tombstone t;

            t.uuid = o->uuid();
            t.ref = i;
            t.deletedAt = o->deletedAt();
            t.deletedBy = MUCK::database().UUIDOf(o->deletedBy());
            t.deletedRev = drev;
            MUCK::database().addTombstone(t);
            o->setBaseWritten(false);   /* leaves the committed index */
            o->scopedMarkers().clear();
            /* the dormant slices die with the object, or they leak
             * for the rest of the uptime */
            dormantEntries.erase(o->uuid());
            dormantModules.erase(o->uuid());
            dormantTypeInfo.erase(o->uuid());
            o->journal().discardTop();
            forgetDirty(i);
            out->reclaim = true;
            out->scopedKeep.clear();
            out->survivors.clear();
        }
    }
    return true;
}

ObjectStore::CaptureSet
ObjectStore::fire(bool compact)
{
    CaptureSet set;

    set.rev = rev_;

    /* chain sanitation is per pass, and a fire is one pass */
    chainClaimReset();
    bulkSaveActive = true;

    /* Walk what changed, not what exists, and seal it in PARALLEL.
     *
     * Why this is safe: the game thread is right here, blocked, so
     * the world is frozen for the duration. Each dirty object is
     * sealed by exactly one worker, and everything a seal MUTATES is
     * that object's own state (its journal top, its baseWritten flag,
     * a possible lazy module attach); everything it reads of OTHER
     * objects (a uuid, a ref in a contents list) is a plain field no
     * worker writes. The two pieces of shared state a seal touches
     * are lock-free: the chain sanitizer's claim table (atomic
     * per-dbref flags, first claim wins) and the manifest index dirty
     * flag (atomic bool). The serializers themselves were audited
     * static-free: jstr builds locally, the residency hook is a
     * no-op, and lock unparsing at fullname 0 never reaches the
     * static unparse buffer. Sealing thirteen million properties was
     * three quarters of the all-dirty dump's stall; it splits across
     * cores cleanly. */
    std::vector<dbref> dirty(dirtyObjects().begin(), dirtyObjects().end());
    std::vector<SealedLayer> slots(dirty.size());
    std::vector<char> hasLayer(dirty.size(), 0);

    {
        unsigned nthreads = std::thread::hardware_concurrency();

        if (nthreads < 1)
            nthreads = 1;
        if (nthreads > 16)
            nthreads = 16;
        if (dirty.size() < 256)
            nthreads = 1;

        std::atomic<size_t> next(0);
        auto sealWorker = [&]() {
          /* The seal runs on every dump, on the live game thread's
           * behalf. An escaping exception here terminates the whole
           * server; losing one object's layer costs that object its
           * unsaved changes, which the next change re-dirties. */
          try {
            for (;;) {
                size_t k = next.fetch_add(1);

                if (k >= dirty.size())
                    break;

                dbref i = dirty[k];
                DbObject *o = MUCK::database().get(i);

                if (!o)
                    continue;

                JournalLayer *top = o->journal().peek();

                if (!top || top->empty())
                    continue;

                SealedLayer &sealed = slots[k];

                sealed.era = top->era();
                sealed.ref = i;
                sealed.uuid = MUCK::database().UUIDOf(i).toString();
                sealed.typeName = typeNameOf(i);
                sealed.entries = json::object();

                if (!o->baseWritten()) {
                    /* No base yet: this object has never been
                     * written, so the whole object is the change. A
                     * layer over nothing would restore nothing. */
                    sealed.entries = objectToJson(i);
                    sealed.full = true;
                    o->setBaseWritten(true);
                } else {
                    for (const std::string &key : top->keys()) {
                        json v = entryValueOf(i, key);

                        /* a null value is a removal, recorded as one */
                        sealed.entries[key] = v.is_null() ? json() : v;
                    }
                }
                o->journal().discardTop();

                /* pre-serialize the segment line here, in parallel;
                 * the json tree is not needed after this and its
                 * memory goes back immediately */
                {
                    json l;

                    l["uuid"] = sealed.uuid;
                    l["dbref"] = sealed.ref;
                    l["era"] = sealed.era;
                    l["entries"] = std::move(sealed.entries);
                    if (!sealed.typeName.empty())
                        l["type"] = sealed.typeName;
                    if (sealed.full)
                        l["full"] = true;
                    sealed.segmentLine = l.dump();
                    sealed.segmentLine += '\n';
                    sealed.entries = json::object();
                }
                hasLayer[k] = 1;
            }
          } catch (const std::exception &e) {
            healthWorkerException();
            fprintf(stderr, "STORE: seal worker failed: %s\n", e.what());
          }
        };

        if (nthreads <= 1) {
            sealWorker();
        } else {
            std::vector<std::thread> pool;

            for (unsigned t = 0; t < nthreads; t++)
                pool.emplace_back(sealWorker);
            for (auto &th : pool)
                th.join();
        }
    }
    for (size_t k = 0; k < dirty.size(); k++)
        if (hasLayer[k])
            set.layers.push_back(std::move(slots[k]));
    bulkSaveActive = false;
    clearDirtyObjects();

    if (compact) {
        long now = ladderNow();

        /* the retention ladder decides which snapshots remain */
        markers_ = ladderSurvivors(markers_, now);

        std::vector<long> globalRevs;

        for (const auto &m : markers_)
            globalRevs.push_back(m.rev);

        /* sweep the next batch of objects; the cursor wraps, so the
         * whole store is revisited every few dozen dumps and no
         * object's history outlives the ladder by more than that */
        dbref dtop = MUCK::database().top();
        long batch = dtop / 64 > 512 ? dtop / 64 : 512;

        if (batch > dtop)
            batch = dtop;
        for (long k = 0; k < batch; k++) {
            if (sweepCursor_ >= dtop)
                sweepCursor_ = 0;

            CompactOrder ord;

            if (buildCompactOrder(sweepCursor_++, globalRevs, now, &ord))
                set.compactions.push_back(std::move(ord));
        }
    }
    /* assign this set's journal segments BEFORE the manifest is
     * serialized, so journal_committed covers them; a large capture
     * spans several bounded segments so folding can yield between
     * them (kSegmentLayers) */
    if (!set.layers.empty()) {
        set.journalSeq = journalSeq_ + 1;
        journalSeq_ += ((long) set.layers.size() + kSegmentLayers - 1)
            / kSegmentLayers;
    }
    set.committedAtBuild = journalSeq_;
    set.distributedAtBuild = journalDistributed_.load();
    set.manifest = buildManifest();
    set.firedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    return set;
}

/* Compact one object's files against its frozen work order: pure file
 * work, no live state, so it runs on the dump thread (or offline).
 *
 * Layers at or below the oldest survivor merge into the base, whose
 * rev advances to the newest era it absorbed. Every other layer
 * buckets to the smallest survivor at or above its era; past the
 * newest survivor it buckets to its own era, which folds the many
 * same-era lines a frequent dump interval writes into one. A bucket
 * with one layer is kept verbatim, so its era stays a valid rollback
 * target; a merged bucket records covers_from, the oldest era it
 * swallowed, so a rollback into the swallowed range can refuse
 * instead of silently handing back older state.
 *
 * The base is written before the history: replaying an absorbed layer
 * over a base that already contains it is idempotent (values are
 * absolute), so a crash between the two writes loses nothing.
 *
 * Returns the number of layers eliminated. */
static long
compactObjectFile(const std::string &root, const ObjectStore::CompactOrder &ord)
{
    std::string path = UUIDObjectPath(root, ord.uuid);
    std::string histFile = path.substr(0, path.size() - 5) + ".hist";

    if (ord.reclaim) {
        /* the manifest that no longer names this object has already
         * landed, so removing the files cannot orphan the index; a
         * crash between these unlinks is cleaned as an uncommitted
         * leftover at the next boot */
        unlink(histFile.c_str());
        unlink(path.c_str());
        return 0;
    }

    std::ifstream bf(path);

    if (!bf)
        return 0;
    json base = json::parse(bf, nullptr, false);
    bf.close();
    if (base.is_discarded() || !base.contains("entries"))
        return 0;               /* damage; the loader reports it */

    json keepMarkers = json::array();

    for (const auto &m : ord.scopedKeep) {
        json e;

        e["rev"] = m.rev;
        e["when"] = m.when;
        e["label"] = m.label;
        e["locked"] = m.locked;
        keepMarkers.push_back(e);
    }
    bool markersChanged =
        base.value("markers", json::array()) != keepMarkers;

    std::vector<json> layers;
    {
        std::ifstream hf(histFile);
        std::string line;

        while (hf && std::getline(hf, line)) {
            if (line.empty())
                continue;
            json l = json::parse(line, nullptr, false);

            /* full type validation, not just presence: a wrong-typed
             * field would otherwise throw out of the merge below and
             * misreport an already-landed set as failed */
            if (l.is_discarded() || !l.contains("entries")
                || !l["entries"].is_object()
                || (l.contains("era") && !l["era"].is_number_integer())
                || (l.contains("covers_from")
                    && !l["covers_from"].is_number_integer())
                || (l.contains("type") && !l["type"].is_string()))
                return 0;       /* damage; leave it for the loader */
            layers.push_back(std::move(l));
        }
    }
    if (layers.empty() && !markersChanged)
        return 0;

    long s0 = ord.survivors.empty() ? -1 : ord.survivors.front();
    auto bucketOf = [&ord, s0](long era) -> long {
        if (ord.survivors.empty())
            return -1;          /* no rollback targets: all into base */
        if (era <= s0)
            return -1;
        for (long s : ord.survivors)
            if (s >= era)
                return s;
        return era;             /* past the newest survivor */
    };

    json newEntries = base["entries"];
    std::string newType = base.value("type", "");
    long newRev = base.value("rev", 0L);
    bool baseChanged = false;
    long merged = 0;
    /* bucket -> layers, in era order (eras ascend, buckets follow) */
    std::map<long, std::vector<json> > buckets;

    for (auto &l : layers) {
        long era = l.value("era", 0L);
        long b = bucketOf(era);

        if (b < 0) {
            for (auto it = l["entries"].begin();
                 it != l["entries"].end(); ++it) {
                if (it.value().is_null())
                    newEntries.erase(it.key());
                else
                    newEntries[it.key()] = it.value();
            }
            if (l.contains("type"))
                newType = l["type"].get<std::string>();
            if (era > newRev)
                newRev = era;
            baseChanged = true;
            merged++;
            continue;
        }
        buckets[b].push_back(std::move(l));
    }

    std::string newHist;

    for (auto &kv : buckets) {
        std::vector<json> &run = kv.second;

        if (run.size() == 1) {
            newHist += run[0].dump() + "\n";
            continue;
        }

        json out;
        long coversFrom = -1, maxEra = -1;

        out["entries"] = json::object();
        for (auto &l : run) {
            long era = l.value("era", 0L);
            long from = l.value("covers_from", era);

            if (coversFrom < 0 || from < coversFrom)
                coversFrom = from;
            if (era > maxEra)
                maxEra = era;
            for (auto it = l["entries"].begin();
                 it != l["entries"].end(); ++it)
                out["entries"][it.key()] = it.value();
            if (l.contains("type"))
                out["type"] = l["type"];
        }
        out["era"] = maxEra;
        out["covers_from"] = coversFrom;
        newHist += out.dump() + "\n";
        merged += (long) run.size() - 1;
    }

    if (!merged && !markersChanged)
        return 0;

    if (baseChanged || markersChanged) {
        base["entries"] = newEntries;
        if (!newType.empty())
            base["type"] = newType;
        if (baseChanged)
            base["rev"] = newRev;
        if (keepMarkers.empty())
            base.erase("markers");
        else
            base["markers"] = keepMarkers;
        if (!atomicWrite(path, base.dump(1)))
            return 0;           /* keep the history if the base failed */
    }
    if (merged) {
        /* unsynced: the base write above was fsynced, so it is
         * durable before this trim can possibly land; losing the
         * trim in a crash merely replays layers the base already
         * carries, which is idempotent */
        if (newHist.empty())
            unlink(histFile.c_str());
        else if (!atomicWrite(histFile, newHist, false))
            /* the base already carries the merged layers, so a stale
             * hist is safe; the next sweep simply finds the same
             * work, but the operator should know */
            fprintf(stderr, "STORE: could not rewrite %s; stale "
                    "layers kept\n", histFile.c_str());
    }
    return merged;
}

std::string
ObjectStore::journalSegmentPath(long seq) const
{
    char name[64];

    sprintf(name, "/journal/%010ld.jsonl", seq);
    return root_ + name;
}

/* Fold one committed journal segment into the per-object files:
 * bases for full lines, history appends for delta lines, coalescing
 * repeated same-era lines per object into one. None of it is
 * fsynced: the segment already guarantees durability, and if a crash
 * loses these writes the segment is simply replayed (idempotent:
 * values are absolute). Returns the number of lines folded, -1 when
 * the segment is unreadable. */
long
ObjectStore::distributeOneSegment(long seq)
{
    std::ifstream f(journalSegmentPath(seq));

    if (!f)
        return 0;               /* already gone: nothing to fold */

    /* per-object runs, in arrival order per object */
    std::map<std::string, std::vector<json> > runs;
    std::string line;
    long lines = 0;

    long badLines = 0;

    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        json l = json::parse(line, nullptr, false);

        if (l.is_discarded() || !l.contains("uuid")
            || !l["uuid"].is_string() || !l.contains("entries")
            || !l["entries"].is_object()) {
            /* one corrupt line must not discard the whole committed
             * segment: everything readable still folds, the file is
             * kept aside as evidence, and only the unreadable line
             * itself is lost. The is_string/is_object checks matter:
             * l["uuid"].get<string>() below would THROW on a
             * non-string, wedging the folder on this segment forever. */
            fprintf(stderr, "STORE: journal segment %ld has an "
                    "unreadable line; folding the rest\n", seq);
            badLines++;
            continue;
        }
        lines++;

        std::string u = l["uuid"].get<std::string>();
        std::vector<json> &run = runs[u];

        if (l.value("full", false)) {
            /* a full line supersedes everything before it */
            run.clear();
            run.push_back(std::move(l));
        } else if (!run.empty() && !run.back().value("full", false)
                   && run.back().value("era", 0L) == l.value("era", 0L)) {
            /* same era: coalesce into one layer, later values win */
            json &prev = run.back();

            for (auto it = l["entries"].begin();
                 it != l["entries"].end(); ++it)
                prev["entries"][it.key()] = it.value();
            if (l.contains("type"))
                prev["type"] = l["type"];
        } else {
            run.push_back(std::move(l));
        }
    }
    f.close();

    bool writeFailed = false;

    for (auto &kv : runs) {
        std::string path = UUIDObjectPath(root_, kv.first);

        ensureDirsFor(path);
        for (json &l : kv.second) {
            if (l.value("full", false)) {
                /* the object's whole state: supersedes the old base
                 * and every layer that sat on it, so the stale hist
                 * goes too (a resurrected object would otherwise
                 * revert at load, its pre-deletion layers replayed
                 * over the state it was restored to) */
                json j = l["entries"];

                j["rev"] = l.value("era", 0L);
                {
                    std::ifstream pf(path);
                    json prev = pf ? json::parse(pf, nullptr, false)
                        : json();

                    /* a full write must not silently drop the scoped
                     * snapshots taken against this object */
                    if (prev.is_object()
                        && !prev.value("markers", json::array()).empty())
                        j["markers"] = prev["markers"];
                }
                /* the hist goes only if the base actually landed: a
                 * failed write (disk full, remount) with the unlink
                 * anyway silently destroyed the object's history */
                if (atomicWrite(path, j.dump(1), false))
                    unlink((path.substr(0, path.size() - 5)
                            + ".hist").c_str());
                else
                    writeFailed = true;
                continue;
            }

            /* a delta for an object with NO base is a line for an
             * object reclaimed after this segment committed: the
             * manifest is newer truth, and appending would create an
             * orphaned hist the next boot refuses over */
            {
                struct stat st;

                if (stat(path.c_str(), &st) != 0) {
                    /* ONLY a genuinely absent base means "reclaimed,
                     * skip". Any other stat error (EIO, ESTALE,
                     * EACCES) on a live object is a transient fault:
                     * dropping the delta would silently lose it, so
                     * signal a write failure and let the -2 retry
                     * contract refold this segment later. */
                    if (errno == ENOENT)
                        continue;
                    writeFailed = true;
                    continue;
                }
            }

            json rec;

            rec["era"] = l.value("era", 0L);
            rec["entries"] = l["entries"];
            /* the stored type name lives at the top level of the
             * object file, so a layer carries it explicitly */
            if (l.contains("type"))
                rec["type"] = l["type"];

            std::string hist = path.substr(0, path.size() - 5) + ".hist";
            std::ofstream hf(hist, std::ios::app);

            if (hf) {
                hf << rec.dump() << "\n";
                hf.close();
                if (!hf)
                    writeFailed = true;
            } else {
                writeFailed = true;
            }
        }
    }
    /* -2: transient write failure. The segment stays exactly where it
     * is and the watermark must NOT advance; the next wake retries
     * (refolding what did land is idempotent). -1: parse damage; the
     * caller keeps the file aside, and everything readable was folded
     * above. */
    if (writeFailed)
        return -2;
    if (badLines)
        return -1;
    return lines;
}

/* Fold committed segments until at most keepAtMost remain pending.
 * Never runs past the last LANDED manifest's committed watermark:
 * folding an uncommitted segment would let changes a crash is
 * supposed to discard leak into the per-object files. Used inline by
 * the offline paths (gc, clean shutdown after the threads join); the
 * live folder thread folds one segment per wake instead. */
void
ObjectStore::distributeSegments(long keepAtMost)
{
    if (keepAtMost < 0)
        keepAtMost = 0;
    while (journalLandedCommitted_.load() - journalDistributed_.load()
           > keepAtMost) {
        long s = journalDistributed_.load() + 1;
        long r = distributeOneSegment(s);

        if (r == -2) {
            /* the disk would not take the fold; the segment is intact
             * and the watermark stays put, so a later pass retries */
            healthFailedFold();
            fprintf(stderr, "STORE: folding segment %ld failed; will "
                    "retry\n", s);
            return;
        }
        if (r == -1) {
            /* damaged: preserve the evidence rather than delete data */
            healthDamagedSegment();
            fprintf(stderr, "STORE: segment %ld damaged; folded what "
                    "was readable, quarantined the rest as %s.damaged\n",
                    s, journalSegmentPath(s).c_str());
            rename(journalSegmentPath(s).c_str(),
                   (journalSegmentPath(s) + ".damaged").c_str());
        }
        journalDistributed_.store(s);
    }
}

void
ObjectStore::unlinkDistributedSegments(long upTo)
{
    if (journalUnlinked_ >= upTo)
        return;

    /* A segment is the ONLY durable copy of its mutations until the
     * per-object files that folded it are themselves on the platter.
     * Folding writes those files UNSYNCED, so before retiring a
     * segment we force one filesystem barrier: without it a crash
     * after this unlink but before the OS wrote the folded bytes back
     * loses a mutation that was already fsynced into the journal, the
     * documented durability boundary, with the safety net just
     * deleted. One syncfs covers the whole batch and lands on the
     * dump/folder thread where latency is free. */
    {
        int fd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);

        if (fd >= 0) {
#ifdef __linux__
            syncfs(fd);
#else
            sync();
#endif
            close(fd);
        }
    }
    while (journalUnlinked_ < upTo) {
        journalUnlinked_++;
        unlink(journalSegmentPath(journalUnlinked_).c_str());
    }
}

bool
ObjectStore::persist(const CaptureSet &set)
{
    if (root_.empty())
        return false;

    ensureDir(root_);
    ensureDir(root_ + "/objects");

    /* THE DUMP IS ONE FILE. Every sealed layer becomes one line in
     * this set's journal segment: one sequential write and one fsync
     * commit a dump of any size. The per-object scatter (bases,
     * history sidecars) happens in distributeSegments, behind the
     * commit, unsynced and coalesced; it never sits on the dump's
     * critical path. Scattering 7k dirty objects into 7k files with
     * two fsyncs each made a dump take a minute; this takes as long
     * as writing the bytes. */
    if (!set.layers.empty() && set.journalSeq > 0) {
        ensureDir(root_ + "/journal");

        long seq = set.journalSeq;
        size_t i = 0;

        while (i < set.layers.size()) {
            std::string seg;
            size_t chunkEnd = i + (size_t) kSegmentLayers;

            if (chunkEnd > set.layers.size())
                chunkEnd = set.layers.size();
            for (; i < chunkEnd; i++)
                seg += set.layers[i].segmentLine;
            /* full sync: these writes ARE the durability point */
            if (!atomicWrite(journalSegmentPath(seq), seg))
                return false;
            seq++;
        }
        layersSinceCommit_.fetch_add((long) set.layers.size());
    }

    /* The manifest commits the set. It was serialized on the game
     * thread, because building it reads live state (tune parms, the
     * tombstone table, the object count) and this thread may not. A
     * set with no manifest is a HELD set: its segment is durable, and
     * the manifest of the set behind it commits it. The manifest is
     * still the commit point, just shared. */
    if (set.manifest.empty())
        return true;

    if (!atomicWrite(root_ + "/manifest.json", set.manifest))
        return false;
    journalLandedCommitted_.store(set.committedAtBuild);
    /* the game loop polls this and posts the dump-done message; the
     * stats feed the completion notice to whoever ran @dump */
    lastDumpLayers_.store(layersSinceCommit_.exchange(0));
    if (set.firedAtMs > 0) {
        long long nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

        lastDumpMillis_.store((long) (nowMs - set.firedAtMs));
    }
    dumpLanded_.store(true);

    /* Housekeeping belongs to the folder thread, never to this one:
     * a commit must not wait behind folding or compaction. Hand it
     * the sweep orders, wake it (the landed watermark above is what
     * lets it advance), and move on. Segments an earlier landed
     * manifest recorded as distributed can leave the disk now; the
     * folder only reads above that watermark. Compaction ordering
     * stays safe because a reclaimed object left the committed index
     * in THIS manifest, and its files go whenever the folder gets
     * there; a crash in between reads as uncommitted leftovers. */
    unlinkDistributedSegments(set.distributedAtBuild);
    {
        std::unique_lock<std::mutex> lk(folderMutex_);

        for (const CompactOrder &ord : set.compactions)
            folderOrders_.push_back(ord);
    }
    folderCv_.notify_one();

    if (set.distributeAll) {
        /* a sync barrier: file readers behind syncNow need every
         * per-object file current before drain() returns, so wait
         * for the folder to fold everything and go idle */
        folderLagTarget_.store(0);
        folderCv_.notify_one();
        {
            std::unique_lock<std::mutex> lk(folderMutex_);

            /* bounded: on a dead disk the folder can never finish,
             * and a barrier that waits forever hangs the game thread
             * behind it. Better a loud, slightly-stale read path than
             * a silent hang. */
            if (!folderIdleCv_.wait_for(lk, std::chrono::seconds(60),
                                        [this] {
                    return !folderBusy_ && folderOrders_.empty()
                        && journalDistributed_.load()
                           >= journalLandedCommitted_.load();
                }))
            {
                healthBarrierTimeout();
                fprintf(stderr, "STORE: sync barrier timed out waiting "
                        "for the folder; per-object files may lag the "
                        "journal\n");
            }
        }
        folderLagTarget_.store(kJournalLag);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* The dump thread (docs/DATABASE.txt 7.1)                            */
/*                                                                    */
/* One long-lived worker, the only writer to the store during normal  */
/* operation. It consumes frozen capture sets and never touches a     */
/* live object, which is why it takes no object locks and cannot race */
/* a mutation.                                                        */
/* ------------------------------------------------------------------ */

void
ObjectStore::ensureDumpThread()
{
    ensureFolderThread();
    if (dumpThreadRunning_)
        return;
    dumpThreadRunning_ = true;
    dumpThreadStop_ = false;
    dumpThread_ = std::thread(&ObjectStore::dumpThreadMain, this);
}

void
ObjectStore::ensureFolderThread()
{
    if (folderRunning_)
        return;
    folderRunning_ = true;
    folderStop_ = false;
    folderThread_ = std::thread(&ObjectStore::folderThreadMain, this);
}

/* The folder: folds committed journal segments into the per-object
 * files and executes sweep compaction orders, one unit of work per
 * wake, so a stop or a barrier is honored promptly. This is the only
 * thread that touches per-object files during normal operation; the
 * dump thread touches only segments and the manifest, which is why a
 * commit can never wait behind a hundred-megabyte whale being
 * folded. */
void
ObjectStore::folderThreadMain()
{
    for (;;) {
        CompactOrder ord;
        bool haveOrder = false;

        {
            std::unique_lock<std::mutex> lk(folderMutex_);

            folderCv_.wait(lk, [this] {
                return folderStop_ || !folderOrders_.empty()
                    || journalLandedCommitted_.load()
                       - journalDistributed_.load()
                       > folderLagTarget_.load();
            });
            if (folderStop_)
                break;
            folderBusy_ = true;
            if (!folderOrders_.empty()) {
                ord = std::move(folderOrders_.front());
                folderOrders_.pop_front();
                haveOrder = true;
            }
        }

        try {
            if (haveOrder) {
                compactObjectFile(root_, ord);
            } else {
                long s = journalDistributed_.load() + 1;
                long r = distributeOneSegment(s);

                if (r == -2) {
                    /* the disk refused the fold (full, remounted);
                     * keep the watermark, back off so a full disk is
                     * not a busy loop, and retry on a later wake */
                    fprintf(stderr, "STORE: folding segment %ld "
                            "failed; will retry\n", s);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                } else {
                    if (r == -1) {
                        healthDamagedSegment();
                        fprintf(stderr, "STORE: segment %ld damaged; "
                                "quarantined as %s.damaged\n", s,
                                journalSegmentPath(s).c_str());
                        rename(journalSegmentPath(s).c_str(),
                               (journalSegmentPath(s) + ".damaged").c_str());
                    }
                    journalDistributed_.store(s);
                }
            }
        } catch (const std::exception &e) {
            healthWorkerException();
            fprintf(stderr, "STORE: folder work failed: %s\n", e.what());
        }

        {
            std::unique_lock<std::mutex> lk(folderMutex_);

            folderBusy_ = false;
        }
        folderIdleCv_.notify_all();
    }

    {
        std::unique_lock<std::mutex> lk(folderMutex_);

        folderBusy_ = false;
    }
    folderIdleCv_.notify_all();
}

void
ObjectStore::dumpThreadMain()
{
    for (;;) {
        CaptureSet set;

        {
            std::unique_lock<std::mutex> hold(queueMutex_);

            queueCv_.wait(hold, [this] {
                return dumpThreadStop_ || !queue_.empty();
            });
            if (queue_.empty()) {
                if (dumpThreadStop_)
                    return;
                continue;
            }
            /* FIFO: sets must land in fire order or the eras they
             * describe stop meaning anything */
            set = std::move(queue_.front());
            queue_.pop_front();
            persisting_ = true;
        }

        /* The layers were discarded from their objects at fire time,
         * so this set is the only copy of those changes: dropping it
         * on a failed write loses them with nothing left to re-dirty
         * from. Retry a few times before giving up, and say so loudly
         * if we do. */
        bool ok = false;

        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            if (attempt)
                fprintf(stderr, "DUMP: retrying rev %ld (attempt %d)\n",
                        set.rev, attempt + 1);
            /* A throw here would unwind out of the thread function
             * and take the whole process down through std::terminate.
             * Treat it as a failed write like any other. */
            try {
                ok = persist(set);
            } catch (const std::exception &ex) {
                fprintf(stderr, "DUMP: persist threw: %s\n", ex.what());
                ok = false;
            } catch (...) {
                fprintf(stderr, "DUMP: persist threw\n");
                ok = false;
            }
            /* persist marks each layer as landed, so a retry appends
             * only what did not make it: retrying wholesale would
             * duplicate .hist records and replay them twice at load */
        }

        {
            std::unique_lock<std::mutex> hold(queueMutex_);

            persisting_ = false;
            if (!ok) {
                /* stderr only: this is the dump thread, and
                 * log_status walls wizards through the descriptor
                 * list */
                healthFailedPersist();
                fprintf(stderr, "DUMP: PERSIST FAILED for rev %ld after "
                        "3 attempts; those changes are lost\n", set.rev);
            }
            idleCv_.notify_all();
        }
    }
}

void
ObjectStore::hold(CaptureSet set)
{
    if (set.layers.empty())
        return;                 /* nothing sealed, nothing to hold */
    held_.push_back(std::move(set));
}

void
ObjectStore::flushHeld(CaptureSet set)
{
    /* Held sets were sealed earlier, so they go out first: the eras
     * they describe only make sense in the order they were sealed. */
    for (CaptureSet &h : held_) {
        /* the manifest each held set carried is stale by now; the one
         * this dump builds supersedes it, and the manifest is the
         * commit point for everything ahead of it */
        h.manifest.clear();
        enqueue(std::move(h));
    }
    held_.clear();
    enqueue(std::move(set));
}

void
ObjectStore::enqueue(CaptureSet set)
{
    /* A set with nothing to write and no manifest would fail persist
     * three times over for no reason. */
    if (set.layers.empty() && set.manifest.empty())
        return;
    ensureDumpThread();
    {
        std::unique_lock<std::mutex> hold(queueMutex_);

        queue_.push_back(std::move(set));
    }
    queueCv_.notify_one();
}

void
ObjectStore::syncNow()
{
    /* Anything sealed but held is not on disk yet, and reading stored
     * state without it would read a world missing its most recent
     * changes. Push it out with a distribute-everything barrier (file
     * readers like rollback need the per-object files current, not
     * just the journal durable), then wait. */
    CaptureSet set = fire();

    set.distributeAll = true;
    flushHeld(std::move(set));
    drain();
}

void
ObjectStore::drain()
{
    if (!dumpThreadRunning_)
        return;

    std::unique_lock<std::mutex> hold(queueMutex_);

    idleCv_.wait(hold, [this] { return queue_.empty() && !persisting_; });
}

bool
ObjectStore::persistPending()
{
    std::unique_lock<std::mutex> hold(queueMutex_);

    return !queue_.empty() || persisting_;
}

void
ObjectStore::requestDumpStop()
{
    if (folderRunning_) {
        {
            std::unique_lock<std::mutex> lk(folderMutex_);

            folderStop_ = true;
        }
        folderCv_.notify_all();
    }
    if (!dumpThreadRunning_)
        return;
    {
        std::unique_lock<std::mutex> hold(queueMutex_);

        dumpThreadStop_ = true;
        queue_.clear();
    }
    queueCv_.notify_all();

    /* This is the PANIC path: the caller is about to run saveAll on
     * its own thread, writing the same manifest and object files an
     * in-flight persist or fold may still be renaming onto. If the
     * worker's stale manifest rename lands AFTER panic's, the store
     * reverts to the older committed index and the next boot discards
     * panic's files as uncommitted, destroying exactly the data panic
     * exists to save. So wait, bounded, for both workers to go quiet.
     *
     * CRITICAL: panic() runs SYNCHRONOUSLY from a signal handler
     * (bailout on SIGSEGV/SIGBUS/etc), and the faulting thread may
     * already hold queueMutex_ or folderMutex_ from the very code the
     * signal interrupted. A blocking lock here would self-deadlock
     * that thread against itself and hang the crash forever. So this
     * uses try_lock in a bounded poll: it never blocks on a mutex it
     * cannot get, it just spins briefly and then proceeds. A held
     * mutex, or a worker that is itself the casualty, simply means
     * panic goes ahead without the barrier, which is exactly the
     * pre-existing behavior and no worse. */
    auto spinUntil = [](std::mutex &m, auto quiet) {
        for (int i = 0; i < 200; i++) {   /* ~2s at 10ms */
            if (m.try_lock()) {
                bool ok = quiet();

                m.unlock();
                if (ok)
                    return;
            }
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
        }
    };

    spinUntil(queueMutex_, [this] { return !persisting_; });
    spinUntil(folderMutex_, [this] { return !folderBusy_; });
}

void
ObjectStore::stopDumpThread()
{
    if (!dumpThreadRunning_)
        return;
    drain();
    {
        std::unique_lock<std::mutex> hold(queueMutex_);

        dumpThreadStop_ = true;
    }
    queueCv_.notify_all();
    if (dumpThread_.joinable())
        dumpThread_.join();
    dumpThreadRunning_ = false;

    if (folderRunning_) {
        {
            std::unique_lock<std::mutex> lk(folderMutex_);

            folderStop_ = true;
        }
        folderCv_.notify_all();
        if (folderThread_.joinable())
            folderThread_.join();
        folderRunning_ = false;
    }

    /* The workers are gone, so their machinery is safe to run inline:
     * a clean shutdown leaves the store fully at rest (bases and
     * sidecars current, journal empty, watermarks committed), and the
     * next boot replays nothing. A crash skips all of this and the
     * loader replays the journal instead. */
    if (!root_.empty()) {
        {
            std::unique_lock<std::mutex> lk(folderMutex_);

            while (!folderOrders_.empty()) {
                try {
                    compactObjectFile(root_, folderOrders_.front());
                } catch (const std::exception &e) {
                    fprintf(stderr, "STORE: compaction of %s failed: "
                            "%s\n", folderOrders_.front().uuid.c_str(),
                            e.what());
                }
                folderOrders_.pop_front();
            }
        }
        /* the same protection the folder thread has: a malformed
         * segment line must not turn a graceful shutdown into an
         * uncaught-exception abort that never reaches the re-exec */
        try {
            distributeSegments(0);
        } catch (const std::exception &e) {
            healthWorkerException();
            fprintf(stderr, "STORE: shutdown fold failed: %s\n", e.what());
        }
        writeManifest();
        unlinkDistributedSegments(journalDistributed_.load());
    }
}

long
ObjectStore::verifyEntrySerialization()
{
    long mismatches = 0;
    long checked = 0;

    for (dbref i = 0; i < MUCK::database().top(); i++) {
        if (MUCK::typeOf(i) == ObjectType::Garbage)
            continue;

        json full = objectToJson(i);

        if (!full.contains("entries"))
            continue;

        const json &e = full["entries"];

        for (auto it = e.begin(); it != e.end(); ++it) {
            json one = entryValueOf(i, it.key());

            /* the base carries a rev stamp the per-key form does not */
            json expect = it.value();

            expect.erase("rev");
            checked++;
            if (one != expect) {
                if (mismatches < 20)
                    fprintf(stderr,
                            "ENTRY MISMATCH #%d %s\n  full: %s\n  key : %s\n",
                            i, it.key().c_str(), expect.dump().c_str(),
                            one.dump().c_str());
                mismatches++;
            }
        }
    }
    fprintf(stderr, "VERIFY: %ld entries checked, %ld mismatches\n",
            checked, mismatches);
    log_status("VERIFY: %ld entries checked, %ld mismatches\n",
               checked, mismatches);
    return mismatches;
}

long
ObjectStore::gcStore()
{
    if (root_.empty())
        return -1;

    long now = ladderNow();

    /* fold any pending journal segments first: compaction works on
     * the per-object files and must see everything (offline mode,
     * single-threaded, so calling the dump thread's machinery
     * directly is safe) */
    journalLandedCommitted_.store(journalSeq_);
    try {
        distributeSegments(-1);
    } catch (const std::exception &e) {
        healthWorkerException();
        fprintf(stderr, "STOREGC: fold failed: %s\n", e.what());
    }

    /* the retention ladder decides which snapshots remain */
    markers_ = ladderSurvivors(markers_, now);

    std::vector<long> globalRevs;

    for (const auto &m : markers_)
        globalRevs.push_back(m.rev);

    /* the offline pass is simply the dump-time sweep run over every
     * object at once, on the same engine */
    std::vector<CompactOrder> orders;
    long reclaimed = 0;

    for (dbref i = 0; i < MUCK::database().top(); i++) {
        CompactOrder ord;

        if (buildCompactOrder(i, globalRevs, now, &ord)) {
            if (ord.reclaim)
                reclaimed++;
            orders.push_back(std::move(ord));
        }
    }

    /* commit the reclaim decisions, the pruned markers, and the
     * distribution watermark BEFORE any file is removed: a crash
     * mid-gc then reads as leftovers at the next boot, never as
     * damage */
    writeManifest();
    unlinkDistributedSegments(journalDistributed_.load());

    long removed = 0;

    for (const auto &ord : orders) {
        try {
            removed += compactObjectFile(root_, ord);
        } catch (const std::exception &e) {
            fprintf(stderr, "STOREGC: compaction of %s failed: %s\n",
                    ord.uuid.c_str(), e.what());
        }
    }
    log_status("STOREGC: merged %ld layer(s), reclaimed %ld object(s)\n",
               removed, reclaimed);
    return removed;
}

} /* namespace MUCK */
