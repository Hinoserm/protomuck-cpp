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
    char buf[BUFFER_LEN];
    char ubuf[BUFFER_LEN];

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
                    v = PropDataFVal(p);
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
            snprintf(buf, sizeof(buf), "%s%s%c", dir, PropName(p), PROPDIR_DELIMITER);
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
        pdat.flags = (unsigned short) (flags & ~PROP_COMPRESSED);
        const json &v = entry["value"];
        std::string sval;

        switch (pdat.flags & PROP_TYPMASK) {
            case PROP_STRTYP:
                pdat.flags &= ~PROP_ISUNLOADED;
                sval = junstr(v.get<std::string>());
                pdat.data.str = (char *) sval.c_str();
                set_property_nofetch(obj, name.c_str(), &pdat, 1);
                continue;
            case PROP_LOKTYP:
                pdat.flags &= ~PROP_ISUNLOADED;
                sval = junstr(v.get<std::string>());
                pdat.data.lok = parse_boolexp(-1, (dbref) 1, sval.c_str(), 32767);
                break;
            case PROP_INTTYP:
                pdat.data.val = v.get<int64_t>();
                break;
            case PROP_FLTTYP:
                pdat.data.fval = v.get<double>();
                break;
            case PROP_REFTYP:
                pdat.data.ref = (dbref) v.get<int>();
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
static std::unordered_set<int> chainClaimed;
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
        if (claim && !chainClaimed.insert((int) i).second) {
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
                v = PropDataFVal(p);
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
objectFromJsonPhase1(const json &j, std::vector<PendingLinks> &later)
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
    pl.td = td;
    pl.core = core;
    if (j.contains("props"))
        pl.props = j["props"];
    later.push_back(pl);
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
    core["flags"] = val("$core/flags");
    core["powers"] = val("$core/powers");

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
            /* the player name lookup table is runtime state the flat
             * importer built as it read; without it every login fails
             * before the password is even checked */
            add_player(pl.ref);
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
atomicWrite(const std::string &path, const std::string &content)
{
    std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);

    if (!f)
        return false;
    f << content;
    f.close();
    if (!f)
        return false;
    /* fsync the data, then publish, then fsync the directory so the
     * rename itself survives */
    syncFile(tmp);
    if (rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    syncDirOf(path);
    return true;
}

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

    /* age out unlocked markers past the snapshot retention window */
    if (tp_snapshot_retention >= 0) {
        long cutoff = (long) current_systime
            - (long) tp_snapshot_retention * 86400L;

        markers_.erase(
            std::remove_if(markers_.begin(), markers_.end(),
                           [cutoff](const Marker &m) {
                               return !m.locked && m.when < cutoff;
                           }),
            markers_.end());
    }
    m["rev"] = rev_;
    m["markers"] = markersToJson(markers_);
    m["global_modules"] = { "properties" };
    m["hash_passwords"] = (bool) MUCK::PasswordHash::enabled;
    m["hash_version"] = MUCK::PasswordHash::version;

    /* Deleted-object retention: a tombstoned object's file and hist
     * survive while any retained snapshot marker, global or the
     * object's own, predates the deletion; @rollback resurrects from
     * them. Once no covering marker remains, the files are reclaimed,
     * and the tombstone itself ages out per tp_tombstone_retention
     * only after its files are gone. */
    {
        long snapCutoff = tp_snapshot_retention >= 0
            ? (long) current_systime - (long) tp_snapshot_retention * 86400L
            : -1;
        long tombCutoff = tp_tombstone_retention >= 0
            ? (long) current_systime - (long) tp_tombstone_retention * 86400L
            : -1;
        std::vector<Database::Tombstone> kept;
        int reclaimed = 0, pruned = 0;

        for (const auto &t : MUCK::database().tombstones()) {
            std::string path = UUIDObjectPath(root_, t.uuid.toString());
            struct stat st;
            bool haveFile = stat(path.c_str(), &st) == 0;

            if (haveFile) {
                std::vector<Marker> own;
                std::ifstream pf(path);
                json pj = pf ? json::parse(pf, nullptr, false) : json();

                if (pj.is_object())
                    own = markersFromJson(pj.value("markers", json::array()));
                if (snapCutoff >= 0)
                    own.erase(std::remove_if(own.begin(), own.end(),
                                             [snapCutoff](const Marker &om) {
                                                 return !om.locked
                                                     && om.when < snapCutoff;
                                             }),
                              own.end());
                if (!markerInWindow(0, t.deletedRev, markers_, own)) {
                    unlink(path.c_str());
                    unlink((path.substr(0, path.size() - 5) + ".hist").c_str());
                    haveFile = false;
                    reclaimed++;
                }
            }
            if (!haveFile && tombCutoff >= 0 && t.deletedAt < tombCutoff) {
                pruned++;
                continue;
            }
            kept.push_back(t);
        }
        if (reclaimed || pruned)
            log_status("STORE: reclaimed %d deleted object file%s, "
                       "pruned %d tombstone%s\n",
                       reclaimed, reclaimed == 1 ? "" : "s",
                       pruned, pruned == 1 ? "" : "s");
        MUCK::database().setTombstones(std::move(kept));
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
    return m.dump(1);
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

bool
ObjectStore::saveObject(dbref i)
{
    std::string path = objectPath(i);

    /* the chain sanitizer's claimed set is per save PASS; a standalone
     * save is its own pass. Without this, the second single-object
     * save ever made would drop the object's entire contents chain. */
    if (!bulkSaveActive)
        chainClaimed.clear();

    ensureDirsFor(path);

    json cur = objectToJson(i);

    /* Carry the object's own markers across, and stamp the revision
     * this base represents. */
    std::ifstream pf(path);
    json prev = pf ? json::parse(pf, nullptr, false) : json();

    if (prev.is_object() && !prev.value("markers", json::array()).empty())
        cur["markers"] = prev["markers"];
    cur["rev"] = rev_;

    /* A full base supersedes every layer that sat on it, so the
     * history goes with it. This is the compacting write: conversion
     * and maintenance use it. Ordinary saves append layers instead
     * (persist), which is what preserves rollback history. */
    unlink(histPath(i).c_str());

    if (DbObject *o = MUCK::database().get(i)) {
        o->journal().discardTop();
        o->setBaseWritten(true);
    }
    return atomicWrite(path, cur.dump(1));
}

bool
ObjectStore::removeObject(dbref i)
{
    unlink(histPath(i).c_str());
    return unlink(objectPath(i).c_str()) == 0;
}

long
ObjectStore::retireObject(dbref i)
{
    /* Flush the object's final state as a journal layer so its
     * history survives; a full base rewrite would drop the layers a
     * rollback needs. */
    enqueue(fireObject(i));
    return rev_;
}

long
ObjectStore::snapshotGlobal(const char *label, bool locked)
{
    Marker m;

    /* a marker only covers what is on disk, so fire first: the
     * journal seals what changed and persists it */
    enqueue(fire());

    /* the marker captures the CURRENT era; writes after the snapshot
     * stamp the next one, so a read at the marker excludes them */
    m.rev = rev_++;
    m.when = (long) current_systime;
    m.label = label ? label : "";
    m.locked = locked;
    markers_.push_back(m);
    if (!writeManifest())
        return -1;
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
    enqueue(fireObject(i));
    drain();                    /* the marker goes into the file below */

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
    writeManifest();            /* persist the advanced rev counter */
    log_status("SNAPSHOT: #%d rev %ld (%s)%s\n", i, m.rev,
               m.label.empty() ? "unlabeled" : m.label.c_str(),
               locked ? " LOCKED" : "");
    return m.rev;
}

std::vector<ObjectStore::Marker>
ObjectStore::objectMarkers(dbref i) const
{
    const_cast<ObjectStore *>(this)->drain();

    std::string path;

    /* a dead shell has no uuid of its own; its retained file is
     * findable through the tombstone */
    if (MUCK::database().UUIDOf(i).isNil()) {
        Database::Tombstone t;

        if (!MUCK::database().findTombstone(i, &t))
            return {};
        path = UUIDObjectPath(root_, t.uuid.toString());
    } else {
        path = objectPath(i);
    }

    std::ifstream pf(path);

    if (!pf)
        return {};
    json j = json::parse(pf, nullptr, false);

    if (j.is_discarded())
        return {};
    return markersFromJson(j.value("markers", json::array()));
}

/* Value of every entry as of a revision: current entries whose rev is
 * at or below the target, patched by history entries whose lifetime
 * covers the target. */
static json
entriesAtRev(const json &file, const std::string &hist, long rev)
{
    json out = json::object();

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
ObjectStore::rollbackObject(dbref i, long rev)
{
    /* rolling back to a marker whose set has not landed yet would
     * find no stored state */
    drain();

    std::ifstream pf(objectPath(i));

    if (!pf)
        return false;
    json j = json::parse(pf, nullptr, false);

    if (j.is_discarded() || !j.contains("entries"))
        return false;

    std::string hist;
    std::ifstream hf(histPath(i));

    if (hf)
        hist.assign(std::istreambuf_iterator<char>(hf),
                    std::istreambuf_iterator<char>());

    json e = entriesAtRev(j, hist, rev);
    struct object *o = DBFETCH(i);

    /* name */
    if (e.contains("$core/name")) {
        if (o->name && Typeof(i) != TYPE_GARBAGE)
            delete[] o->name;
        o->name = alloc_string(junstr(e["$core/name"]["value"].get<std::string>()).c_str());
    }

    /* properties: wipe and rebuild from the snapshot */
    if (PropDirPtr pd_ = MUCK::propRoot(i))
        pd_->clear();
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

    DBDIRTY(i);
    log_status("ROLLBACK: #%d to rev %ld\n", i, rev);
    return true;
}

bool
ObjectStore::resurrectObject(const MUCK::Database::Tombstone &t, long rev,
                             std::string *err)
{
    drain();

    std::string path = UUIDObjectPath(root_, t.uuid.toString());
    std::ifstream pf(path);

    if (!pf) {
        if (err)
            *err = "no retained file for that object; its snapshots aged out";
        return false;
    }
    json j = json::parse(pf, nullptr, false);

    if (j.is_discarded() || !j.contains("entries")) {
        if (err)
            *err = "the retained file is unreadable";
        return false;
    }

    dbref i = t.ref;

    if (i < 0 || i >= MUCK::database().top()
        || MUCK::typeOf(i) != ObjectType::Garbage) {
        if (err)
            *err = "the object's slot is not a dead shell";
        return false;
    }

    std::string hist;
    std::ifstream hf(path.substr(0, path.size() - 5) + ".hist");

    if (hf)
        hist.assign(std::istreambuf_iterator<char>(hf),
                    std::istreambuf_iterator<char>());

    json e = entriesAtRev(j, hist, rev);

    if (!e.contains("$core/flags") || !e.contains("$core/name")) {
        if (err)
            *err = "the object has no stored state at that revision";
        return false;
    }
    j["entries"] = e;

    /* the shell keeps its literal placeholder name, so phase one's
     * overwrite leaks nothing; the boot machinery does the rest */
    std::vector<PendingLinks> later;
    json shape = fileToLoadShape(j, root_);

    objectFromJsonPhase1(shape, later);
    for (const auto &pl : later)
        objectFromJsonPhase2(pl);

    struct object *o = DBFETCH(i);

    /* Containment is not resurrected: the children of record were
     * evacuated or separately recycled before the deletion, so phase
     * two's list wiring is dropped and the object re-enters the world
     * cleanly at its rev-time location when that still stands. */
    dbref loc = o->location;

    MUCK::contentsOf(i).clear();
    MUCK::exitsOf(i).clear();
    o->location = NOTHING;
    if (o->owner < 0 || !MUCK::database().valid(o->owner)
        || Typeof(o->owner) != TYPE_PLAYER)
        o->owner = GOD;

    bool locValid = loc >= 0 && MUCK::database().valid(loc)
        && Typeof(loc) != TYPE_GARBAGE;

    if (Typeof(i) == TYPE_EXIT) {
        if (!locValid)
            loc = MUCK::getOwner(i);
        MUCK::attachExit(loc, i);
        o->location = loc;
        DBDIRTY(loc);
    } else {
        if (!locValid)
            loc = Typeof(i) == TYPE_ROOM ? GLOBAL_ENVIRONMENT : MUCK::getOwner(i);
        moveto(i, loc);
    }

    MUCK::database().reviveHole(i);
    MUCK::database().removeTombstone(t.uuid);
    DBDIRTY(i);
    log_status("RESURRECT: #%d (%s) at rev %ld\n", i, MUCK::getName(i), rev);
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
    chainClaimed.clear();
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
    if (!writeManifest())
        return -1;
    return written;
}

dbref
ObjectStore::loadAll()
{
    std::ifstream mf(root_ + "/manifest.json");
    std::vector<PendingLinks> later;

    if (!mf)
        return -1;
    json manifest = json::parse(mf, nullptr, false);
    if (manifest.is_discarded())
        return -1;

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

    /* deleted objects keep their files while snapshots cover them;
     * they load as dead shells, not live objects */
    std::unordered_set<UUID> deadUUIDs;

    for (const auto &t : MUCK::database().tombstones())
        deadUUIDs.insert(t.uuid);

    MUCK::database().ensureTop(top);

    /* ensureTop pre-initialized every slot as a garbage shell */

    std::string objroot = root_ + "/objects";
    DIR *d0 = opendir(objroot.c_str());

    if (!d0)
        return -1;
    struct dirent *e0;
    while ((e0 = readdir(d0)) != NULL) {
        if (e0->d_name[0] == '.')
            continue;
        std::string l1 = objroot + "/" + e0->d_name;
        DIR *d1 = opendir(l1.c_str());
        if (!d1)
            continue;
        struct dirent *e1;
        while ((e1 = readdir(d1)) != NULL) {
            if (e1->d_name[0] == '.')
                continue;
            std::string l2 = l1 + "/" + e1->d_name;
            DIR *d2 = opendir(l2.c_str());
            if (!d2)
                continue;
            struct dirent *e2;
            while ((e2 = readdir(d2)) != NULL) {
                size_t n = strlen(e2->d_name);
                if (n < 6 || strcmp(e2->d_name + n - 5, ".json"))
                    continue;
                std::ifstream f(l2 + "/" + e2->d_name);
                if (!f)
                    continue;
                json j = json::parse(f, nullptr, false);
                if (j.is_discarded()) {
                    fprintf(stderr, "STORE: skipping unparsable %s\n", e2->d_name);
                    continue;
                }
                UUID fileUUID = UUID::parse(j.value("uuid", ""));

                if (deadUUIDs.count(fileUUID))
                    continue;

                /* The stored object is its base plus the layers its
                 * history holds, applied in era order: that is what
                 * makes a compacted and an uncompacted object
                 * indistinguishable here (docs/DATABASE.txt 6). */
                {
                    std::string base = l2 + "/" + e2->d_name;
                    std::ifstream hf(base.substr(0, base.size() - 5) + ".hist");

                    if (hf && j.contains("entries")) {
                        std::string line;

                        while (std::getline(hf, line)) {
                            json layer = json::parse(line, nullptr, false);

                            if (layer.is_discarded() || !layer.contains("entries"))
                                continue;
                            /* a history line written by the old
                             * copy-on-write path describes an OLD
                             * value and carries "key"; a journal layer
                             * describes new values and does not */
                            if (layer.contains("key"))
                                continue;
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
                if (j.contains("entries")) {
                    json unknown = json::object();
                    std::string tname = j.value("type", "garbage");
                    bool unsupported = !storedTypeSupported(tname);

                    for (auto eit = j["entries"].begin();
                         eit != j["entries"].end(); ++eit)
                        if (!knownNamespace(eit.key())
                            || (unsupported
                                && eit.key().rfind("$type/", 0) == 0))
                            unknown[eit.key()] = eit.value();
                    if (!unknown.empty())
                        dormantEntries[fileUUID] = unknown;
                    if (!j.value("modules", json::array()).empty())
                        dormantModules[fileUUID] = j["modules"];

                    if (unsupported) {
                        /* The type module is absent: keep its slice
                         * dormant, remember what the object would have
                         * been, and load only the core. */
                        json info;
                        int bits = 0;
                        const json &ents = j["entries"];
                        auto fl = ents.find("$core/flags");

                        if (fl != ents.end() && (*fl)["value"].is_array())
                            bits = (*fl)["value"][0].get<int>() & TYPE_MASK;
                        info["name"] = tname;
                        info["bits"] = bits;
                        dormantTypeInfo[fileUUID] = info;

                        for (auto eit = j["entries"].begin();
                             eit != j["entries"].end();)
                            if (eit.key().rfind("$type/", 0) == 0)
                                eit = j["entries"].erase(eit);
                            else
                                ++eit;
                    }

                    json fileEntries = j["entries"];
                    json fileMods = j.value("modules", json::array());

                    j = fileToLoadShape(j, root_);
                    objectFromJsonPhase1(j, later);
                    if (unsupported) {
                        dbref pref = (dbref) j.value("dbref", -1);

                        if (pref >= 0) {
                            struct object *po = DBFETCH(pref);

                            MUCK::setType(pref, ObjectType::Unsupported);
                            (void) po;
                        }
                    }
                    if (!later.empty() && later.back().ref == j.value("dbref", -1)) {
                        later.back().entries = fileEntries;
                        for (const auto &mn : fileMods)
                            later.back().modules.push_back(mn.get<std::string>());
                    }
                    continue;
                }
                fprintf(stderr,
                        "STORE: %s has no entry model; this store "
                        "predates the current format. Rebuild it by "
                        "re-importing the legacy flat database.\n",
                        e2->d_name);
                closedir(d2);
                closedir(d1);
                closedir(d0);
                return -1;
            }
            closedir(d2);
        }
        closedir(d1);
    }
    closedir(d0);

    for (const auto &pl : later)
        objectFromJsonPhase2(pl);

    /* holes (deleted objects) stay dead shells; mark them so modern
     * code sees isDeleted() */
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

    return (dbref) top;
}


/* ------------------------------------------------------------------ */
/* Fire and persist (docs/DATABASE.txt 7.1)                           */
/* ------------------------------------------------------------------ */

/* Seal one object only: deletion and scoped snapshots need exactly
 * their own object's layer on disk, not everyone else's. */
ObjectStore::CaptureSet
ObjectStore::fireObject(dbref i)
{
    CaptureSet set;

    set.rev = rev_;
    chainClaimed.clear();

    DbObject *o = MUCK::database().get(i);

    if (!o)
        return set;

    JournalLayer *top = o->journal().peek();
    SealedLayer sealed;

    sealed.era = top ? top->era() : rev_;
    sealed.ref = i;
    sealed.uuid = MUCK::database().UUIDOf(i).toString();
    sealed.entries = json::object();

    if (!o->baseWritten()) {
        sealed.entries = objectToJson(i);
        sealed.full = true;
        o->setBaseWritten(true);
    } else if (top && !top->empty()) {
        for (const std::string &key : top->keys()) {
            json v = entryValueOf(i, key);

            sealed.entries[key] = v.is_null() ? json() : v;
        }
    } else {
        return set;             /* nothing to say about this object */
    }
    o->journal().discardTop();
    forgetDirty(i);
    set.layers.push_back(std::move(sealed));
    set.manifest = buildManifest();
    return set;
}

ObjectStore::CaptureSet
ObjectStore::fire()
{
    CaptureSet set;

    set.rev = rev_;

    /* chain sanitation is per pass, and a fire is one pass */
    chainClaimed.clear();
    bulkSaveActive = true;

    /* Walk what changed, not what exists. */
    std::vector<dbref> dirty(dirtyObjects().begin(), dirtyObjects().end());

    for (dbref i : dirty) {
        DbObject *o = MUCK::database().get(i);

        if (!o)
            continue;

        JournalLayer *top = o->journal().peek();

        if (!top || top->empty())
            continue;

        SealedLayer sealed;

        sealed.era = top->era();
        sealed.ref = i;
        sealed.uuid = MUCK::database().UUIDOf(i).toString();
        sealed.entries = json::object();

        if (!o->baseWritten()) {
            /* No base yet: this object has never been written, so the
             * whole object is the change. A layer over nothing would
             * restore nothing. */
            json full = objectToJson(i);

            sealed.entries = full;
            sealed.full = true;
            o->setBaseWritten(true);
        } else {
            for (const std::string &key : top->keys()) {
                json v = entryValueOf(i, key);

                /* a null value is a removal, and is recorded as one */
                sealed.entries[key] = v.is_null() ? json() : v;
            }
        }
        o->journal().discardTop();
        set.layers.push_back(std::move(sealed));
    }
    bulkSaveActive = false;
    clearDirtyObjects();
    set.manifest = buildManifest();

    return set;
}

bool
ObjectStore::persist(const CaptureSet &set)
{
    if (root_.empty())
        return false;

    ensureDir(root_);
    ensureDir(root_ + "/objects");

    for (const SealedLayer &layer : set.layers) {
        std::string path = UUIDObjectPath(root_, layer.uuid);

        ensureDirsFor(path);

        if (layer.full) {
            /* the object's first appearance: write the base itself */
            json j = layer.entries;

            j["rev"] = set.rev;
            if (!atomicWrite(path, j.dump(1)))
                return false;
            continue;
        }

        /* otherwise the layer appends to the history sidecar */
        json rec;

        rec["era"] = layer.era;
        rec["entries"] = layer.entries;

        std::string hist = path.substr(0, path.size() - 5) + ".hist";
        std::ofstream hf(hist, std::ios::app);

        if (!hf)
            return false;
        hf << rec.dump() << "\n";
        hf.flush();
        hf.close();
        syncFile(hist);
    }

    /* the manifest commits the set; it was serialized at fire time so
     * this thread never reads live state */
    if (!set.manifest.empty())
        return atomicWrite(root_ + "/manifest.json", set.manifest);
    return writeManifest();
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
    if (dumpThreadRunning_)
        return;
    dumpThreadRunning_ = true;
    dumpThreadStop_ = false;
    dumpThread_ = std::thread(&ObjectStore::dumpThreadMain, this);
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

        bool ok = persist(set);

        {
            std::unique_lock<std::mutex> hold(queueMutex_);

            persisting_ = false;
            if (!ok)
                log_status("DUMP: persist failed for rev %ld\n", set.rev);
            idleCv_.notify_all();
        }
    }
}

void
ObjectStore::enqueue(CaptureSet set)
{
    ensureDumpThread();
    {
        std::unique_lock<std::mutex> hold(queueMutex_);

        queue_.push_back(std::move(set));
    }
    queueCv_.notify_one();
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
    long removed = 0;
    std::string objroot = root_ + "/objects";
    DIR *d0 = opendir(objroot.c_str());

    if (!d0)
        return -1;

    struct dirent *e0;

    while ((e0 = readdir(d0)) != NULL) {
        if (e0->d_name[0] == '.')
            continue;

        std::string l1 = objroot + "/" + e0->d_name;
        DIR *d1 = opendir(l1.c_str());

        if (!d1)
            continue;

        struct dirent *e1;

        while ((e1 = readdir(d1)) != NULL) {
            if (e1->d_name[0] == '.')
                continue;

            std::string l2 = l1 + "/" + e1->d_name;
            DIR *d2 = opendir(l2.c_str());

            if (!d2)
                continue;

            struct dirent *e2;

            while ((e2 = readdir(d2)) != NULL) {
                std::string fn = e2->d_name;
                std::string full = l2 + "/" + fn;

                if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".hist") == 0) {
                    /* prune history no retained marker can see */
                    std::ifstream f(full);
                    std::string kept, line;
                    std::vector<Marker> own;
                    std::string objfile = full.substr(0, full.size() - 5) + ".json";
                    std::ifstream of(objfile);
                    json oj = of ? json::parse(of, nullptr, false) : json();

                    if (oj.is_object())
                        own = markersFromJson(oj.value("markers", json::array()));
                    while (std::getline(f, line)) {
                        json h = json::parse(line, nullptr, false);

                        if (h.is_discarded())
                            continue;
                        if (markerInWindow(h.value("from", 0L), h.value("to", 0L),
                                           markers_, own)) {
                            kept += line + "\n";
                        } else {
                            removed++;
                        }
                    }
                    f.close();
                    if (kept.empty())
                        unlink(full.c_str());
                    else
                        atomicWrite(full, kept);
                }
            }
            closedir(d2);
        }
        closedir(d1);
    }
    closedir(d0);

    log_status("STOREGC: removed %ld dead history entries\n", removed);
    return removed;
}

} /* namespace MUCK */
