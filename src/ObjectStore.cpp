/* Standard headers FIRST: db.h defines function-like macros (getloc
 * among them) that clobber identically named members inside libstdc++
 * headers included after it. */
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
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
#include "Modules.h"
#include "ProgramStore.h"
#include "PasswordHash.h"
#include "ModuleRegistry.h"

using json = nlohmann::json;

extern void tune_save_parms_to_file(FILE *);
extern void tune_load_parms_from_file(FILE *, dbref, int);

namespace MUCK {

ObjectStore g_objectStore;

static const int STORE_FORMAT = 2;

/* Dormant module data (docs section 4): entries in namespaces no
 * loaded module claims, plus unrecognized attached-module names, are
 * carried verbatim across load and save so unloading a module can
 * never destroy its data. Keyed by uuid string. */
static std::unordered_map<std::string, json> dormantEntries;
static std::unordered_map<std::string, json> dormantModules;

/* UNSUPPORTED placeholders: uuid -> {"name": stored type name,
 * "bits": original TYPE_MASK bits}. Populated at load for objects
 * whose type module is excluded or unknown; consulted at save so the
 * file keeps saying what the object would have been. */
static std::unordered_map<std::string, json> dormantTypeInfo;
static std::set<std::string> excludedTypes;

/* Chunk payloads referenced by dormant entries, captured at load so a
 * save into a fresh root can materialize them there. In-place saves
 * find the chunks already on disk either way. */
static std::unordered_map<std::string, std::string> dormantChunks;

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
    auto it = dormantTypeInfo.find(MUCK::database().uuidOf(ref).toString());

    if (it == dormantTypeInfo.end())
        return "";
    return it->second.value("name", "");
}

static bool
knownNamespace(const std::string &k)
{
    if (k.empty())
        return false;
    if (k[0] == '/')
        return true;            /* properties */
    if (k.rfind("$core/", 0) == 0 || k.rfind("$type/", 0) == 0)
        return true;
    return false;
}

/* forward declarations: marker helpers are defined with the versioned
 * save path below but used by the manifest writer above it */
static std::vector<ObjectStore::Marker> markersFromJson(const json &arr);
static json markersToJson(const std::vector<ObjectStore::Marker> &list);

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
    return json(MUCK::database().uuidOf(ref).toString());
}

static dbref
refFromJson(const json &v)
{
    if (v.is_number_integer())
        return (dbref) v.get<int>();
    if (v.is_string()) {
        Uuid u = Uuid::parse(v.get<std::string>().c_str());
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
propsToJson(struct object *o, json &arr, const char *dir, PropPtr p)
{
    char buf[BUFFER_LEN];
    char ubuf[BUFFER_LEN];
    char fbuf[BUFFER_LEN];

    if (!p)
        return;
    propsToJson(o, arr, dir, AVL_LF(p));

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
        default:               /* PROP_DIRTYP carries no value */
            keep = false;
            break;
    }

    if (keep) {
        json entry;
        entry["n"] = jstr((std::string(dir + 1) + PropName(p)).c_str());
        entry["f"] = flags;
        entry["v"] = v;
        arr.push_back(entry);
    }

    if (PropDir(p)) {
        snprintf(buf, sizeof(buf), "%s%s%c", dir, PropName(p), PROPDIR_DELIMITER);
        propsToJson(o, arr, buf, PropDir(p));
    }
    propsToJson(o, arr, dir, AVL_RT(p));
}

static void
propsFromJson(dbref obj, const json &arr)
{
    for (const auto &entry : arr) {
        std::string name = junstr(entry.value("n", ""));
        int flags = entry.value("f", 0);
        PData pdat;

        if (name.empty() || !entry.contains("v"))
            continue;
        pdat.flags = (unsigned short) (flags & ~PROP_COMPRESSED);
        const json &v = entry["v"];
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
                pdat.data.val = v.get<int>();
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
listToJson(dbref container, const std::vector<MUCK::DbObject *> &list)
{
    json arr = json::array();

    for (MUCK::DbObject *o : list) {
        dbref i = o->ref();

        if (!MUCK::database().valid(i) || i == container
            || DBFETCH(i)->location != container) {
            fprintf(stderr, "STORE: dropping corrupt list member #%d from #%d\n",
                    i, container);
            continue;
        }
        if (!chainClaimed.insert((int) i).second) {
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
typeName(struct object *o)
{
    switch (o->flags & TYPE_MASK) {
        case TYPE_ROOM:    return "room";
        case TYPE_THING:   return "thing";
        case TYPE_EXIT:    return "exit";
        case TYPE_PLAYER:  return "player";
        case TYPE_PROGRAM: return "muf_program";
        default:           return "garbage";
    }
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

static std::string
chunkPath(const std::string &root, const std::string &hash)
{
    return root + "/chunks/" + hash.substr(0, 2) + "/" + hash;
}

static std::string
chunkHash(const std::string &content)
{
    char hex[48];

    SHA1hex(hex, content.c_str(), (int) content.size());
    return std::string(hex);
}

static bool
ensureChunk(const std::string &root, const std::string &hash,
            const std::string &content)
{
    std::string path = chunkPath(root, hash);
    struct stat st;

    if (stat(path.c_str(), &st) == 0)
        return true;            /* dedup: already stored */
    ensureDirsFor(path);

    std::ofstream f(path + ".tmp", std::ios::trunc | std::ios::binary);

    if (!f)
        return false;
    f.write(content.data(), (std::streamsize) content.size());
    f.close();
    if (!f)
        return false;
    return rename((path + ".tmp").c_str(), path.c_str()) == 0;
}

static bool
readChunk(const std::string &root, const std::string &hash, std::string &out)
{
    std::ifstream f(chunkPath(root, hash), std::ios::binary);

    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
}

/* ------------------------------------------------------------------ */
/* Format 2: the value model. An object's persistent state is a set   */
/* of namespaced entries, each a typed value stamped with the rev at  */
/* which it took effect. The closed type set (docs section 3):        */
/*   s string, i int, f float, b bool, r ref, l list, c chunk-ref     */
/* Prop entries carry the legacy prop flag word as "m".               */
/* ------------------------------------------------------------------ */

static json
entry(const char *t, json v)
{
    json e;

    e["t"] = t;
    e["v"] = std::move(v);
    return e;
}

static json
objectToJson(dbref i)
{
    struct object *o = DBFETCH(i);
    json j;

    j["uuid"] = MUCK::database().uuidOf(i).toString();
    j["dbref"] = (int) i;
    j["type"] = typeName(o);
    j["modules"] = json::array();

    /* An UNSUPPORTED placeholder saves as what it would have been:
     * the stored type name and its original type bits, verbatim. */
    int typeBitsOut = (int) (o->flags & TYPE_MASK);

    {
        auto pt = dormantTypeInfo.find(j["uuid"].get<std::string>());

        if (pt != dormantTypeInfo.end()) {
            j["type"] = pt->second.value("name", "garbage");
            typeBitsOut = pt->second.value("bits", 0) & TYPE_MASK;
        }
    }

    json e;

    /* --- $core: what every object has --- */
    e["$core/name"] = entry("s", jstr(o->name));
    e["$core/location"] = entry("r", refToJson(o->location));
    e["$core/owner"] = entry("r", refToJson(o->owner));
    e["$core/flags"] = entry("l", json::array({
        (int) (((o->flags & ~DUMP_MASK) & ~TYPE_MASK) | typeBitsOut),
        (int) (o->flag2 & ~DUM2_MASK),
        (int) (o->flag3 & ~DUM3_MASK), (int) (o->flag4 & ~DUM4_MASK) }));
    e["$core/powers"] = entry("l", json::array({
        (int) (o->powers & ~POWERS_DUMP_MASK),
        (int) (o->power2 & ~POWER2_DUMP_MASK) }));
    e["$core/ts"] = entry("l", json::array({
        (long) o->ts.created, (long) o->ts.modified, (long) o->ts.lastused,
        (long) o->ts.usecount, (long) o->ts.dcreated,
        (long) o->ts.dmodified, (long) o->ts.dlastused }));

    /* --- $type: the type module's fields --- */
    switch (o->flags & TYPE_MASK) {
        case TYPE_ROOM:
            e["$type/dropto"] = entry("r", refToJson(MUCK::roomDropToRef(i)));
            e["$type/contents"] = entry("l", listToJson(i, MUCK::contentsOf(i)));
            e["$type/exits"] = entry("l", listToJson(i, MUCK::exitsOf(i)));
            break;
        case TYPE_THING:
            e["$type/home"] = entry("r", refToJson(MUCK::thingHomeRef(i)));
            e["$type/value"] = entry("i", MUCK::thingValue(i));
            e["$type/contents"] = entry("l", listToJson(i, MUCK::contentsOf(i)));
            e["$type/exits"] = entry("l", listToJson(i, MUCK::exitsOf(i)));
            break;
        case TYPE_PLAYER:
            e["$type/home"] = entry("r", refToJson(MUCK::playerHomeRef(i)));
            e["$type/pennies"] = entry("i", MUCK::playerPennies(i));
            e["$type/password"] = entry("s", jstr(MUCK::playerPasswordSlot(i)));
            e["$type/contents"] = entry("l", listToJson(i, MUCK::contentsOf(i)));
            e["$type/exits"] = entry("l", listToJson(i, MUCK::exitsOf(i)));
            break;
        case TYPE_EXIT: {
            json dests = json::array();
            int nd = MUCK::exitDestCount(i);

            for (int k = 0; k < nd; k++)
                dests.push_back(refToJson(MUCK::exitDestRef(i, k)));
            e["$type/dests"] = entry("l", dests);
            break;
        }
        case TYPE_PROGRAM: {
            /* source lines live in the content-addressed chunk pool;
             * the entry is a list of chunk-refs */
            json refs = json::array();
            const std::vector<std::string> *lines = MUCK::programs().sourceLines(i);

            if (lines) {
                for (const auto &ln : *lines) {
                    std::string h = chunkHash(ln);

                    ensureChunk(g_objectStore.root(), h, ln);
                    refs.push_back(h);
                }
            }
            e["$type/source"] = entry("c", refs);
            break;
        }
        default:
            break;
    }

    /* --- properties: their traditional slash paths --- */
    {
        json props = json::array();

        propsToJson(o, props, "/", o->properties);
        for (const auto &pe : props) {
            std::string key = "/" + pe["n"].get<std::string>();
            const json &v = pe["v"];
            const char *t = v.is_string() ? "s"
                : v.is_number_float() ? "f" : "i";
            json ent = entry(t, v);

            ent["m"] = pe["f"];
            e[key] = std::move(ent);
        }
    }

    /* dormant module data rides along untouched */
    {
        auto d = dormantEntries.find(j["uuid"].get<std::string>());

        if (d != dormantEntries.end())
            for (auto it = d->second.begin(); it != d->second.end(); ++it) {
                e[it.key()] = it.value();
                /* materialize referenced chunks in this root */
                if (it.value().value("t", "") == "c"
                    && it.value()["v"].is_array())
                    for (const auto &h : it.value()["v"]) {
                        auto c = dormantChunks.find(h.get<std::string>());

                        if (c != dormantChunks.end())
                            ensureChunk(g_objectStore.root(),
                                        h.get<std::string>(), c->second);
                    }
            }
        auto dm = dormantModules.find(j["uuid"].get<std::string>());

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

    Uuid u = Uuid::parse(j.value("uuid", "").c_str());
    MUCK::database().assignUuid(i, u);

    const json &core = j["core"];
    o->name = alloc_string(junstr(core.value("name", "")).c_str());

    const json &fl = core["flags"];
    o->flags = fl[0].get<int>();
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
    o->properties = NULL;

    /* links start empty NOW; phase two only wires. Resetting these any
     * later would tear down chain wiring already done by containers
     * whose phase two ran first. */
    o->location = NOTHING;
    o->owner = NOTHING;
    MUCK::contentsOf(i).clear();
    MUCK::exitsOf(i).clear();

    /* type scalars now, refs later */
    const json &td = j["type_data"];
    switch (o->flags & TYPE_MASK) {
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

/* Translate a format 2 (entry model) object file into the format 1
 * shape and feed it to the same phase machinery. One wiring
 * implementation, every format readable forever. */
static json
v2ToV1(const json &j, const std::string &root)
{
    json out;
    const json &e = j["entries"];

    out["uuid"] = j.value("uuid", "");
    out["dbref"] = j.value("dbref", -1);
    out["type"] = j.value("type", "garbage");
    out["modules"] = j.value("modules", json::array());

    auto val = [&e](const char *k) -> json {
        auto it = e.find(k);
        return it == e.end() ? json() : (*it)["v"];
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
            td[k] = (*it)["v"];
    }
    if (e.contains("$type/source")) {
        json lines = json::array();
        for (const auto &h : e["$type/source"]["v"]) {
            std::string content;
            if (readChunk(root, h.get<std::string>(), content))
                lines.push_back(jstr(content.c_str()));
            else
                fprintf(stderr, "STORE: missing chunk %s for #%d\n",
                        h.get<std::string>().c_str(), out["dbref"].get<int>());
        }
        td["source"] = lines;
    }
    out["type_data"] = td;

    json props = json::array();
    for (auto it = e.begin(); it != e.end(); ++it) {
        if (it.key().empty() || it.key()[0] != '/')
            continue;
        json pe;
        pe["n"] = it.key().substr(1);
        pe["f"] = it.value().value("m", 0);
        pe["v"] = it.value()["v"];
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
     * slice; unregistered names stay dormant */
    for (const auto &name : pl.modules) {
        if (!moduleRegistry().knows(name))
            continue;

        DbObject *obj = MUCK::database().get(pl.ref);
        Module *m = obj->attach(moduleRegistry().make(name));

        m->loadEntries(pl.entries);
    }

    o->location = refFromJson(pl.core["location"]);
    o->owner = refFromJson(pl.core["owner"]);

    switch (o->flags & TYPE_MASK) {
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

std::string
ObjectStore::objectPath(dbref i) const
{
    std::string u = MUCK::database().uuidOf(i).toString();

    /* Shard on the LAST four hex digits: uuidv7 leads with a timestamp,
     * so leading digits are identical for every object minted in the
     * same era and would put the whole database in one directory. The
     * tail is random. */
    size_t n = u.size();

    return root_ + "/objects/" + u.substr(n - 4, 2) + "/" + u.substr(n - 2, 2)
        + "/" + u + ".json";
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
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool
ObjectStore::isStore(const char *path)
{
    struct stat st;
    std::string manifest = std::string(path) + "/manifest.json";

    return stat(manifest.c_str(), &st) == 0;
}

bool
ObjectStore::writeManifest()
{
    json m;

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

    /* tombstones: prune per @tune retention, then persist */
    if (tp_tombstone_retention >= 0)
        MUCK::database().pruneTombstones(
            (long) current_systime - (long) tp_tombstone_retention * 86400L);
    json ts = json::array();
    for (const auto &t : MUCK::database().tombstones()) {
        json e;
        e["u"] = t.uuid.toString();
        e["r"] = (int) t.ref;
        e["t"] = t.deletedAt;
        e["b"] = t.deletedBy.toString();
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
    return atomicWrite(root_ + "/manifest.json", m.dump(1));
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
    json &ce = cur["entries"];

    /* Copy-on-write per entry: diff against the file being replaced.
     * An unchanged value keeps its rev untouched (a no-op write costs
     * nothing); a changed value whose old rev is visible at a retained
     * marker pushes the old value into the history file first. */
    std::ifstream pf(path);
    json prev = pf ? json::parse(pf, nullptr, false) : json();
    bool havePrev = prev.is_object() && prev.contains("entries");
    std::vector<Marker> own =
        havePrev ? markersFromJson(prev.value("markers", json::array()))
                 : std::vector<Marker>();
    std::string histLines;

    for (auto it = ce.begin(); it != ce.end(); ++it) {
        json *old = nullptr;

        if (havePrev) {
            auto pit = prev["entries"].find(it.key());
            if (pit != prev["entries"].end())
                old = &*pit;
        }
        if (old && (*old)["t"] == it.value()["t"]
            && (*old)["v"] == it.value()["v"]
            && old->value("m", 0) == it.value().value("m", 0)) {
            it.value()["rev"] = old->value("rev", 0L);   /* unchanged */
            continue;
        }
        it.value()["rev"] = rev_;
        if (old) {
            long oldRev = old->value("rev", 0L);

            if (markerInWindow(oldRev, rev_, markers_, own)) {
                json h = *old;

                h["k"] = it.key();
                h["from"] = oldRev;
                h["to"] = rev_;
                histLines += h.dump() + "\n";
            }
        }
    }
    /* keys that disappeared entirely: deletion, historicize if seen */
    if (havePrev) {
        for (auto pit = prev["entries"].begin();
             pit != prev["entries"].end(); ++pit) {
            if (ce.contains(pit.key()))
                continue;
            long oldRev = pit.value().value("rev", 0L);

            if (markerInWindow(oldRev, rev_, markers_, own)) {
                json h = pit.value();

                h["k"] = pit.key();
                h["from"] = oldRev;
                h["to"] = rev_;
                histLines += h.dump() + "\n";
            }
        }
        if (!prev.value("markers", json::array()).empty())
            cur["markers"] = prev["markers"];
    }

    if (!histLines.empty()) {
        std::ofstream hf(histPath(i), std::ios::app);

        if (hf)
            hf << histLines;
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
ObjectStore::snapshotGlobal(const char *label, bool locked)
{
    Marker m;

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
    std::ifstream pf(path);

    if (!pf) {
        /* object never saved yet; write it first so the marker has a
         * file to live in */
        if (!saveObject(i))
            return -1;
        pf.open(path);
        if (!pf)
            return -1;
    }
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
    std::ifstream pf(objectPath(i));

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

    if (file.contains("entries")) {
        const json &e = file["entries"];

        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.value().value("rev", 0L) <= rev)
                out[it.key()] = it.value();
    }

    /* newest covering history entry per key wins */
    std::istringstream hs(hist);
    std::string line;
    std::unordered_map<std::string, long> bestFrom;

    while (std::getline(hs, line)) {
        json h = json::parse(line, nullptr, false);

        if (h.is_discarded())
            continue;
        long from = h.value("from", 0L), to = h.value("to", 0L);

        if (!(from <= rev && rev < to))
            continue;
        std::string k = h.value("k", "");
        auto bit = bestFrom.find(k);

        if (bit != bestFrom.end() && bit->second >= from)
            continue;
        bestFrom[k] = from;
        json v = h;

        v.erase("k");
        v.erase("from");
        v.erase("to");
        out[k] = v;
    }
    return out;
}

bool
ObjectStore::rollbackObject(dbref i, long rev)
{
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
        o->name = alloc_string(junstr(e["$core/name"]["v"].get<std::string>()).c_str());
    }

    /* properties: wipe and rebuild from the snapshot */
    if (o->properties) {
        delete_proplist(o->properties);
        o->properties = NULL;
    }
    {
        json props = json::array();

        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.key().empty() || it.key()[0] != '/')
                continue;
            json pe;

            pe["n"] = it.key().substr(1);
            pe["f"] = it.value().value("m", 0);
            pe["v"] = it.value()["v"];
            props.push_back(pe);
        }
        propsFromJson(i, props);
    }

    /* program source */
    if (Typeof(i) == TYPE_PROGRAM && e.contains("$type/source")) {
        std::vector<std::string> lines;

        for (const auto &h : e["$type/source"]["v"]) {
            std::string content;

            if (readChunk(root_, h.get<std::string>(), content))
                lines.push_back(content);
        }
        MUCK::programs().setSourceLines(i, std::move(lines));
        uncompile_program(i);
    }

    DBDIRTY(i);
    log_status("ROLLBACK: #%d to rev %ld\n", i, rev);
    return true;
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

        if ((o->flags & TYPE_MASK) == TYPE_GARBAGE)
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

            t.uuid = Uuid::parse(e.value("u", "").c_str());
            t.ref = (dbref) e.value("r", -1);
            t.deletedAt = e.value("t", 0L);
            t.deletedBy = Uuid::parse(e.value("b", "").c_str());
            list.push_back(t);
        }
        MUCK::database().setTombstones(std::move(list));
    }

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
                if (j.contains("entries")) {
                    json unknown = json::object();
                    std::string tname = j.value("type", "garbage");
                    bool unsupported = !storedTypeSupported(tname);

                    for (auto eit = j["entries"].begin();
                         eit != j["entries"].end(); ++eit)
                        if (!knownNamespace(eit.key())
                            || (unsupported
                                && eit.key().rfind("$type/", 0) == 0)) {
                            unknown[eit.key()] = eit.value();
                            /* keep chunk payloads reachable for saves
                             * into a fresh root */
                            if (eit.value().value("t", "") == "c"
                                && eit.value()["v"].is_array())
                                for (const auto &h : eit.value()["v"]) {
                                    std::string content;

                                    if (readChunk(root_,
                                                  h.get<std::string>(),
                                                  content))
                                        dormantChunks[h.get<std::string>()] =
                                            content;
                                }
                        }
                    if (!unknown.empty())
                        dormantEntries[j.value("uuid", "")] = unknown;
                    if (!j.value("modules", json::array()).empty())
                        dormantModules[j.value("uuid", "")] = j["modules"];

                    if (unsupported) {
                        /* The type module is absent: keep its slice
                         * dormant, remember what the object would have
                         * been, and load only the core. */
                        json info;
                        int bits = 0;
                        const json &ents = j["entries"];
                        auto fl = ents.find("$core/flags");

                        if (fl != ents.end() && (*fl)["v"].is_array())
                            bits = (*fl)["v"][0].get<int>() & TYPE_MASK;
                        info["name"] = tname;
                        info["bits"] = bits;
                        dormantTypeInfo[j.value("uuid", "")] = info;

                        for (auto eit = j["entries"].begin();
                             eit != j["entries"].end();)
                            if (eit.key().rfind("$type/", 0) == 0)
                                eit = j["entries"].erase(eit);
                            else
                                ++eit;
                    }

                    json v2entries = j["entries"];
                    json v2mods = j.value("modules", json::array());

                    j = v2ToV1(j, root_);
                    objectFromJsonPhase1(j, later);
                    if (unsupported) {
                        dbref pref = (dbref) j.value("dbref", -1);

                        if (pref >= 0) {
                            struct object *po = DBFETCH(pref);

                            po->flags =
                                (po->flags & ~TYPE_MASK) | TYPE_UNSUPPORTED;
                        }
                    }
                    if (!later.empty() && later.back().ref == j.value("dbref", -1)) {
                        later.back().entries = v2entries;
                        for (const auto &mn : v2mods)
                            later.back().modules.push_back(mn.get<std::string>());
                    }
                    continue;
                }
                /* format 1 object file: readable forever, but the
                 * type-dormancy machinery is entry-based, so exclusion
                 * needs the file upgraded first (any full save). */
                if (!storedTypeSupported(j.value("type", "garbage"))) {
                    fprintf(stderr,
                            "STORE: %s is a format 1 object of excluded "
                            "type '%s'; run a full save to upgrade the "
                            "store before using --db-exclude-type.\n",
                            e2->d_name,
                            j.value("type", "garbage").c_str());
                    closedir(d2);
                    closedir(d1);
                    closedir(d0);
                    return -1;
                }
                objectFromJsonPhase1(j, later);
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
        if ((DBFETCH(i)->flags & TYPE_MASK) == TYPE_GARBAGE)
            MUCK::database().noteHole(i);

    /* a fresh load is clean by definition: the module setters used
     * during wiring raise dirty flags that would otherwise force a
     * full rewrite (and revision churn) on the next dump */
    for (dbref i = 0; i < top; i++)
        DBFETCH(i)->flags &= ~OBJECT_CHANGED;

    return (dbref) top;
}


long
ObjectStore::gcStore()
{
    long removed = 0;
    std::unordered_set<std::string> liveChunks;
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

                if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".json") == 0) {
                    /* collect live chunk refs */
                    std::ifstream f(full);
                    json j = f ? json::parse(f, nullptr, false) : json();

                    if (j.is_object() && j.contains("entries")
                        && j["entries"].contains("$type/source"))
                        for (const auto &h : j["entries"]["$type/source"]["v"])
                            liveChunks.insert(h.get<std::string>());
                } else if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".hist") == 0) {
                    /* prune unseen history; keep chunk refs of kept lines */
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
                            if (h.value("k", "") == "$type/source")
                                for (const auto &c : h["v"])
                                    liveChunks.insert(c.get<std::string>());
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

    /* sweep the chunk pool */
    std::string chroot = root_ + "/chunks";
    DIR *c0 = opendir(chroot.c_str());

    if (c0) {
        struct dirent *ce;

        while ((ce = readdir(c0)) != NULL) {
            if (ce->d_name[0] == '.')
                continue;

            std::string cl = chroot + "/" + ce->d_name;
            DIR *c1 = opendir(cl.c_str());

            if (!c1)
                continue;

            struct dirent *cf;

            while ((cf = readdir(c1)) != NULL) {
                if (cf->d_name[0] == '.')
                    continue;
                if (!liveChunks.count(cf->d_name)) {
                    unlink((cl + "/" + cf->d_name).c_str());
                    removed++;
                }
            }
            closedir(c1);
        }
        closedir(c0);
    }

    log_status("STOREGC: removed %ld dead history entries and chunks\n", removed);
    return removed;
}

} /* namespace MUCK */
