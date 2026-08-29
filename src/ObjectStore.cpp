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
    if (!f)
        return false;
    /* fsync the data, then publish, then fsync the directory so the
     * rename itself survives. syncIt=false is for batched writers
     * (the dump's layer phase), which pay ONE filesystem barrier for
     * the whole batch before their commit point instead of two
     * fsyncs per file; the rename is still atomic either way. */
    if (syncIt)
        syncFile(tmp);
    if (rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    if (syncIt)
        syncDirOf(path);
    return true;
}

/* How many committed journal segments may sit unfolded before the
 * dump thread starts distributing the oldest into the per-object
 * files. Larger = fewer, better-coalesced background writes; the
 * cost is only boot-replay work after a crash. */
static const long kJournalLag = 8;

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
        chainClaimed.clear();

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

    /* Life and death are just another entry. If the target revision
     * has no deletion entry then the object was alive then, so it is
     * alive now: that is the whole of resurrection, and it needs no
     * path of its own. */
    if (e.contains("$core/deleted")) {
        const json &d = e["$core/deleted"]["value"];

        if (DbObject *o = MUCK::database().get(i))
            o->markDeleted(d.is_array() && d.size() > 0 ? d[0].get<long>() : 0,
                           d.is_array() && d.size() > 1
                           ? (dbref) d[1].get<int>() : NOTHING);
        MUCK::database().retireUUID(i);
    } else {
        MUCK::database().reviveHole(i);
    }

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

    if (e.contains("$core/name"))
        MUCK::setName(i, junstr(e["$core/name"]["value"].get<std::string>()).c_str());

    /* Owner and location come back too. Recycling clears both, so an
     * object revived without them is owned by nobody and nowhere, and
     * anything that prints its owner's name walks off a null. */
    if (e.contains("$core/owner"))
        MUCK::setOwner(i, refFromJson(e["$core/owner"]["value"]));
    if (e.contains("$core/location")) {
        dbref loc = refFromJson(e["$core/location"]["value"]);

        if (loc != NOTHING && MUCK::database().valid(loc)
            && MUCK::typeOf(loc) != ObjectType::Garbage) {
            MUCK::setLocation(i, loc);
            if (MUCK::typeOf(i) == TYPE_EXIT)
                MUCK::attachExit(loc, i);
            else
                MUCK::attachContent(loc, i);
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
    /* a full save supersedes the journal: every file now carries its
     * object's current state, and replaying older segments over that
     * at boot would regress it. Mark everything distributed, commit,
     * then drop the segments. */
    journalDistributed_.store(journalSeq_);
    journalLandedCommitted_ = journalSeq_;
    if (!writeManifest())
        return -1;
    unlinkDistributedSegments(journalSeq_);
    return written;
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
    journalSeq_ = manifest.value("journal_committed", 0L);
    journalDistributed_.store(manifest.value("journal_distributed", 0L));
    journalLandedCommitted_ = journalSeq_;
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
                std::string full = l2 + "/" + e2->d_name;

                if (n > 5 && !strcmp(e2->d_name + n - 5, ".hist")) {
                    histSeen.insert(full);
                    continue;
                }
                if (n < 6 || strcmp(e2->d_name + n - 5, ".json"))
                    continue;   /* stray .tmp leftovers are not damage */
                jsonSeen.insert(full);
                std::ifstream f(full);
                if (!f) {
                    damaged(full + ": cannot open for reading");
                    continue;
                }
                json j = json::parse(f, nullptr, false);
                if (j.is_discarded()) {
                    damaged(full + ": not valid JSON (truncated or corrupt)");
                    continue;
                }
                UUID fileUUID = UUID::parse(j.value("uuid", ""));
                dbref claimed = (dbref) j.value("dbref", -1);

                if (fileUUID.isNil()) {
                    damaged(full + ": missing or unparsable uuid");
                    continue;
                }
                {
                    /* case-insensitively: UUID::parse accepts upper
                     * hex, so a case-normalizing restore step must
                     * not read as damage */
                    std::string fname(e2->d_name);

                    for (auto &c : fname)
                        c = (char) tolower(c);
                    if (fileUUID.toString() + ".json" != fname) {
                        damaged(full + ": file name does not match the "
                                "uuid inside it ("
                                + fileUUID.toString() + ")");
                        continue;
                    }
                }
                if (claimed < 0) {
                    damaged(full + ": missing or unparsable dbref");
                    continue;
                }
                if (index) {
                    /* the committed index says which uuid owns this
                     * dbref; a file it does not name is an uncommitted
                     * leftover from an interrupted dump */
                    auto it = index->find(std::to_string(claimed));

                    if (it == index->end()
                        || it->get<std::string>() != fileUUID.toString()) {
                        uncommitted.push_back(full);
                        continue;
                    }
                } else if (claimed >= top) {
                    /* no index (store written before it existed): a
                     * dbref past the committed top is still provably
                     * uncommitted */
                    uncommitted.push_back(full);
                    continue;
                }
                {
                    auto dup = refClaimed.find(claimed);

                    if (dup != refClaimed.end()) {
                        damaged(full + " and " + dup->second
                                + " both claim dbref #"
                                + std::to_string(claimed));
                        continue;
                    }
                    refClaimed[claimed] = full;
                }

                /* The stored object is its base plus the layers its
                 * history holds, applied in era order: that is what
                 * makes a compacted and an uncompacted object
                 * indistinguishable here (docs/DATABASE.txt 6). */
                /* the era the deletion entry was recorded in, for
                 * reclamation: from the base's rev when the base
                 * already carries it, else from the layer that
                 * introduces it (a later null revives) */
                long delEra = -1;

                if (j["entries"].contains("$core/deleted"))
                    delEra = j.value("rev", 0L);
                {
                    std::string base = full;
                    std::string histFile =
                        base.substr(0, base.size() - 5) + ".hist";
                    std::ifstream hf(histFile);

                    if (hf && j.contains("entries")) {
                        std::string line;
                        long lineNo = 0;

                        while (std::getline(hf, line)) {
                            json layer = json::parse(line, nullptr, false);

                            lineNo++;
                            if (layer.is_discarded()) {
                                damaged(histFile + " line "
                                        + std::to_string(lineNo)
                                        + ": not valid JSON (truncated "
                                        "or corrupt); the changes it "
                                        "carried are lost");
                                continue;
                            }
                            /* a history line written by the retired
                             * copy-on-write path describes an OLD
                             * value and carries "key"; nothing writes
                             * those any more */
                            if (layer.contains("key")) {
                                damaged(histFile + " line "
                                        + std::to_string(lineNo)
                                        + ": legacy copy-on-write "
                                        "record; this store predates "
                                        "the current format");
                                continue;
                            }
                            if (!layer.contains("entries")) {
                                damaged(histFile + " line "
                                        + std::to_string(lineNo)
                                        + ": no entries map");
                                continue;
                            }
                            if (layer.contains("type"))
                                j["type"] = layer["type"];
                            {
                                auto dit = layer["entries"].find("$core/deleted");

                                if (dit != layer["entries"].end())
                                    delEra = dit.value().is_null()
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

                /* committed journal lines not yet folded into this
                 * object's files replay over it now, newest last */
                {
                    auto jr = journalReplay.find(j.value("uuid", ""));

                    if (jr != journalReplay.end() && j.contains("entries")) {
                        for (json &l : jr->second) {
                            if (l.value("full", false)) {
                                /* a full line is the whole object
                                 * file, verbatim */
                                j = l["entries"];
                                j["rev"] = l.value("era", 0L);
                                delEra = j.contains("entries")
                                    && j["entries"].contains("$core/deleted")
                                    ? l.value("era", 0L) : -1;
                                continue;
                            }
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
                        journalReplay.erase(jr);
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
                    json fileMarkers = j.value("markers", json::array());

                    j = fileToLoadShape(j, root_);
                    objectFromJsonPhase1(j, later);
                    /* it came off disk, so it has a base there. This
                     * includes recycled shells: their retained files
                     * must stay in the manifest index, or the next
                     * boot would discard them as uncommitted. */
                    if (DbObject *bo = MUCK::database().get(claimed)) {
                        bo->setBaseWritten(true);
                        if (bo->isDeleted() && delEra >= 0)
                            bo->setDeletedRev(delEra);
                    }
                    if (!fileMarkers.empty()) {
                        dbref mref = (dbref) j.value("dbref", -1);
                        DbObject *mo = mref >= 0
                            ? MUCK::database().get(mref) : nullptr;

                        if (mo)
                            for (const auto &me : fileMarkers)
                                mo->scopedMarkers().push_back(
                                    {me.value("rev", 0L),
                                     me.value("when", 0L),
                                     me.value("label", std::string()),
                                     me.value("locked", false)});
                    }
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
                damaged(full + ": no entry model; this store predates "
                        "the current format. Rebuild it by re-importing "
                        "the legacy flat database.");
            }
            closedir(d2);
        }
        closedir(d1);
    }
    closedir(d0);

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

    for (const auto &pl : later)
        objectFromJsonPhase2(pl);

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
        sealed.typeName = typeNameOf(i);
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

    if (compact) {
        long now = (long) current_systime;

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
    /* assign this set's journal segment BEFORE the manifest is
     * serialized, so journal_committed covers it */
    if (!set.layers.empty())
        set.journalSeq = ++journalSeq_;
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
        if (newHist.empty())
            unlink(histFile.c_str());
        else if (!atomicWrite(histFile, newHist))
            /* the base already carries the merged layers, so a stale
             * hist is safe (idempotent replay); the next sweep simply
             * finds the same work, but the operator should know */
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

    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        json l = json::parse(line, nullptr, false);

        if (l.is_discarded() || !l.contains("uuid")
            || !l.contains("entries")) {
            fprintf(stderr, "STORE: journal segment %ld is damaged; "
                    "keeping it aside\n", seq);
            return -1;
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
                atomicWrite(path, j.dump(1), false);
                unlink((path.substr(0, path.size() - 5)
                        + ".hist").c_str());
                continue;
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
            }
        }
    }
    return lines;
}

/* Fold committed segments until at most keepAtMost remain pending.
 * Never runs past the last LANDED manifest's committed watermark:
 * folding an uncommitted segment would let changes a crash is
 * supposed to discard leak into the per-object files. */
void
ObjectStore::distributeSegments(long keepAtMost)
{
    while (journalLandedCommitted_ - journalDistributed_.load()
           > keepAtMost) {
        long s = journalDistributed_.load() + 1;

        if (distributeOneSegment(s) < 0) {
            /* damaged: preserve the evidence rather than delete data */
            rename(journalSegmentPath(s).c_str(),
                   (journalSegmentPath(s) + ".damaged").c_str());
        }
        journalDistributed_.store(s);
    }
}

void
ObjectStore::unlinkDistributedSegments(long upTo)
{
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
        std::string seg;

        ensureDir(root_ + "/journal");
        for (const SealedLayer &layer : set.layers) {
            json l;

            l["uuid"] = layer.uuid;
            l["dbref"] = layer.ref;
            l["era"] = layer.era;
            l["entries"] = layer.entries;
            if (!layer.typeName.empty())
                l["type"] = layer.typeName;
            if (layer.full)
                l["full"] = true;
            seg += l.dump() + "\n";
        }
        /* full sync: this write IS the durability point of the dump */
        if (!atomicWrite(journalSegmentPath(set.journalSeq), seg))
            return false;
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
    journalLandedCommitted_ = set.committedAtBuild;
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

    /* Post-commit housekeeping, in order: segments an earlier landed
     * manifest already recorded as distributed can leave the disk;
     * then fold pending segments down to the lag bound (all of them
     * for a sync barrier), so the per-object files trail the journal
     * by a bounded, replayable amount. Folding repeats are coalesced
     * per object and era, which is what turns a hot object's line per
     * dump into one history append per era. */
    unlinkDistributedSegments(set.distributedAtBuild);
    distributeSegments(set.distributeAll ? 0 : kJournalLag);

    /* Compaction rides BEHIND the commit: a reclaimed object must
     * leave the committed index before its files leave the disk, and
     * layer merging only rewrites content the manifest never names.
     * Failures here are not failures of the set; the data is intact
     * and the next sweep simply finds the same work again. */
    if (!set.compactions.empty()) {
        long merged = 0, reclaimed = 0;

        for (const CompactOrder &ord : set.compactions) {
            if (ord.reclaim)
                reclaimed++;
            /* one poisoned object must not fail the set: the layers
             * and manifest above are already durably committed, and
             * failing here would retry (and falsely report losing)
             * a dump that in fact landed */
            try {
                merged += compactObjectFile(root_, ord);
            } catch (const std::exception &e) {
                fprintf(stderr, "STORE: compaction of %s failed: %s\n",
                        ord.uuid.c_str(), e.what());
            }
        }
        /* stderr, not log_status: this thread must not walk the
         * descriptor list */
        if (merged || reclaimed)
            fprintf(stderr, "STORE: sweep merged %ld layer(s), "
                    "reclaimed %ld object(s)\n", merged, reclaimed);
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
    if (!dumpThreadRunning_)
        return;
    {
        std::unique_lock<std::mutex> hold(queueMutex_);

        dumpThreadStop_ = true;
        queue_.clear();
    }
    queueCv_.notify_all();
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

    /* The worker is gone, so its machinery is safe to run inline: a
     * clean shutdown leaves the store fully at rest (bases and
     * sidecars current, journal empty, watermarks committed), and the
     * next boot replays nothing. A crash skips all of this and the
     * loader replays the journal instead. */
    if (!root_.empty()) {
        distributeSegments(0);
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

    long now = (long) current_systime;

    /* fold any pending journal segments first: compaction works on
     * the per-object files and must see everything (offline mode,
     * single-threaded, so calling the dump thread's machinery
     * directly is safe) */
    journalLandedCommitted_ = journalSeq_;
    distributeSegments(0);

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
