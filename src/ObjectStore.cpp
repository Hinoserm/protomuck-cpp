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
#include "ProgramStore.h"
#include "PasswordHash.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fstream>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace MUCK {

ObjectStore g_objectStore;

static const int STORE_FORMAT = 2;

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

static json
chainToJson(dbref container, dbref first)
{
    json arr = json::array();
    dbref guard = MUCK::database().top();

    for (dbref i = first; i != NOTHING && guard-- > 0; i = DBFETCH(i)->next) {
        if (!MUCK::database().valid(i) || i == container
            || DBFETCH(i)->location != container) {
            fprintf(stderr, "STORE: dropping corrupt chain member #%d from #%d\n",
                    i, container);
            break;
        }
        if (!chainClaimed.insert((int) i).second) {
            fprintf(stderr, "STORE: #%d already serialized in another chain; dropping from #%d\n",
                    i, container);
            break;
        }
        arr.push_back(refToJson(i));
    }
    return arr;
}

/* Rebuild a next-chain from a list; returns the chain head and sets
 * each member's location-ish linkage through the next pointers. */
static dbref
chainFromJson(const json &arr)
{
    dbref head = NOTHING, tail = NOTHING;

    for (const auto &v : arr) {
        dbref i = refFromJson(v);

        if (i == NOTHING || !MUCK::database().valid(i))
            continue;
        if (head == NOTHING)
            head = i;
        else
            DBFETCH(tail)->next = i;
        tail = i;
        DBFETCH(i)->next = NOTHING;
    }
    return head;
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

    json e;

    /* --- $core: what every object has --- */
    e["$core/name"] = entry("s", jstr(o->name));
    e["$core/location"] = entry("r", refToJson(o->location));
    e["$core/owner"] = entry("r", refToJson(o->owner));
    e["$core/flags"] = entry("l", json::array({
        (int) (o->flags & ~DUMP_MASK), (int) (o->flag2 & ~DUM2_MASK),
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
            e["$type/dropto"] = entry("r", refToJson(o->sp.room.dropto));
            e["$type/contents"] = entry("l", chainToJson(i, o->contents));
            e["$type/exits"] = entry("l", chainToJson(i, o->exits));
            break;
        case TYPE_THING:
            e["$type/home"] = entry("r", refToJson(o->sp.thing.home));
            e["$type/value"] = entry("i", o->sp.thing.value);
            e["$type/contents"] = entry("l", chainToJson(i, o->contents));
            e["$type/exits"] = entry("l", chainToJson(i, o->exits));
            break;
        case TYPE_PLAYER:
            e["$type/home"] = entry("r", refToJson(o->sp.player.home));
            e["$type/pennies"] = entry("i", o->sp.player.pennies);
            e["$type/password"] = entry("s", jstr(o->sp.player.password));
            e["$type/contents"] = entry("l", chainToJson(i, o->contents));
            e["$type/exits"] = entry("l", chainToJson(i, o->exits));
            break;
        case TYPE_EXIT: {
            json dests = json::array();

            for (int k = 0; k < o->sp.exit.ndest; k++)
                dests.push_back(refToJson((o->sp.exit.dest)[k]));
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

    j["entries"] = e;
    return j;
}

struct PendingLinks {
    dbref ref;
    json td;
    json core;
    json props;
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
    o->contents = NOTHING;
    o->exits = NOTHING;
    o->next = NOTHING;

    /* type scalars now, refs later */
    const json &td = j["type_data"];
    switch (o->flags & TYPE_MASK) {
        case TYPE_THING:
            o->sp.thing.value = td.value("value", 0);
            break;
        case TYPE_PLAYER:
            o->sp.player.pennies = td.value("pennies", 0);
            o->sp.player.password = alloc_string(junstr(td.value("password", "")).c_str());
            o->sp.player.curr_prog = NOTHING;
            o->sp.player.insert_mode = 0;
            o->sp.player.descrs = NULL;
            o->sp.player.descr_count = 0;
            break;
        case TYPE_PROGRAM:
            memset(&o->sp.program, 0, sizeof(o->sp.program));
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

    o->location = refFromJson(pl.core["location"]);
    o->owner = refFromJson(pl.core["owner"]);

    switch (o->flags & TYPE_MASK) {
        case TYPE_ROOM:
            o->sp.room.dropto = refFromJson(pl.td["dropto"]);
            o->contents = chainFromJson(pl.td["contents"]);
            o->exits = chainFromJson(pl.td["exits"]);
            break;
        case TYPE_THING:
            o->sp.thing.home = refFromJson(pl.td["home"]);
            o->contents = chainFromJson(pl.td["contents"]);
            o->exits = chainFromJson(pl.td["exits"]);
            break;
        case TYPE_PLAYER:
            o->sp.player.home = refFromJson(pl.td["home"]);
            o->contents = chainFromJson(pl.td["contents"]);
            o->exits = chainFromJson(pl.td["exits"]);
            /* the player name lookup table is runtime state the flat
             * importer built as it read; without it every login fails
             * before the password is even checked */
            add_player(pl.ref);
            break;
        case TYPE_EXIT: {
            const json &dests = pl.td["dests"];
            o->sp.exit.ndest = (int) dests.size();
            if (o->sp.exit.ndest > 0) {
                o->sp.exit.dest = new dbref[o->sp.exit.ndest];
                for (int k = 0; k < o->sp.exit.ndest; k++)
                    o->sp.exit.dest[k] = refFromJson(dests[k]);
            } else {
                o->sp.exit.dest = NULL;
            }
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
    m["rev"] = 0;               /* versioning arrives in step 3 */
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
    return atomicWrite(root_ + "/manifest.json", m.dump(1));
}

bool
ObjectStore::saveObject(dbref i)
{
    std::string path = objectPath(i);
    std::string dir1 = path.substr(0, path.rfind('/'));
    std::string dir0 = dir1.substr(0, dir1.rfind('/'));

    ensureDir(root_);
    ensureDir(root_ + "/objects");
    ensureDir(dir0);
    ensureDir(dir1);
    return atomicWrite(path, objectToJson(i).dump(1));
}

bool
ObjectStore::removeObject(dbref i)
{
    return unlink(objectPath(i).c_str()) == 0;
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

    ensureDir(root_);
    ensureDir(root_ + "/objects");
    for (dbref i = 0; i < MUCK::database().top(); i++) {
        struct object *o = DBFETCH(i);

        if ((o->flags & TYPE_MASK) == TYPE_GARBAGE)
            continue;
        if (dirtyOnly && !(o->flags & OBJECT_CHANGED))
            continue;
        if (!saveObject(i))
            return -1;
        o->flags &= ~OBJECT_CHANGED;
        written++;
    }
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
                if (j.contains("entries"))
                    j = v2ToV1(j, root_);
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

    return (dbref) top;
}

} /* namespace MUCK */
