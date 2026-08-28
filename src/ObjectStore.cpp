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

static const int STORE_FORMAT = 1;

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

static json
objectToJson(dbref i)
{
    struct object *o = DBFETCH(i);
    json j;

    j["uuid"] = MUCK::database().uuidOf(i).toString();
    j["dbref"] = (int) i;
    j["type"] = typeName(o);
    j["modules"] = json::array();

    json core;
    core["name"] = jstr(o->name);
    core["location"] = refToJson(o->location);
    core["owner"] = refToJson(o->owner);
    core["flags"] = { (int) (o->flags & ~DUMP_MASK), (int) (o->flag2 & ~DUM2_MASK),
                      (int) (o->flag3 & ~DUM3_MASK), (int) (o->flag4 & ~DUM4_MASK) };
    core["powers"] = { (int) (o->powers & ~POWERS_DUMP_MASK),
                       (int) (o->power2 & ~POWER2_DUMP_MASK) };
    json ts;
    ts["created"] = (long) o->ts.created;
    ts["modified"] = (long) o->ts.modified;
    ts["lastused"] = (long) o->ts.lastused;
    ts["usecount"] = o->ts.usecount;
    ts["dcreated"] = (int) o->ts.dcreated;
    ts["dmodified"] = (int) o->ts.dmodified;
    ts["dlastused"] = (int) o->ts.dlastused;
    core["ts"] = ts;
    j["core"] = core;

    json td;
    switch (o->flags & TYPE_MASK) {
        case TYPE_ROOM:
            td["dropto"] = refToJson(o->sp.room.dropto);
            td["contents"] = chainToJson(i, o->contents);
            td["exits"] = chainToJson(i, o->exits);
            break;
        case TYPE_THING:
            td["home"] = refToJson(o->sp.thing.home);
            td["value"] = o->sp.thing.value;
            td["contents"] = chainToJson(i, o->contents);
            td["exits"] = chainToJson(i, o->exits);
            break;
        case TYPE_PLAYER:
            td["home"] = refToJson(o->sp.player.home);
            td["pennies"] = o->sp.player.pennies;
            td["password"] = jstr(o->sp.player.password);
            td["contents"] = chainToJson(i, o->contents);
            td["exits"] = chainToJson(i, o->exits);
            break;
        case TYPE_EXIT: {
            json dests = json::array();
            for (int k = 0; k < o->sp.exit.ndest; k++)
                dests.push_back(refToJson((o->sp.exit.dest)[k]));
            td["dests"] = dests;
            break;
        }
        case TYPE_PROGRAM: {
            json src = json::array();
            const std::vector<std::string> *lines = MUCK::programs().sourceLines(i);
            if (lines)
                for (const auto &ln : *lines)
                    src.push_back(jstr(ln.c_str()));
            td["source"] = src;
            break;
        }
        default:
            break;
    }
    j["type_data"] = td;

    json props = json::array();
    propsToJson(o, props, "/", o->properties);
    j["props"] = props;

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
    MUCK::database().growTo(i + 1);
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

    MUCK::database().growTo(top);

    /* every slot starts as garbage; files fill in the live objects */
    for (dbref i = 0; i < top; i++) {
        struct object *o = DBFETCH(i);
        memset(o, 0, sizeof(*o));
        o->name = NULL;
        o->flags = TYPE_GARBAGE;
        o->location = o->owner = o->contents = o->exits = o->next = NOTHING;
        o->properties = NULL;
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
                objectFromJsonPhase1(j, later);
            }
            closedir(d2);
        }
        closedir(d1);
    }
    closedir(d0);

    for (const auto &pl : later)
        objectFromJsonPhase2(pl);

    /* rebuild the recycle chain from the garbage holes so the legacy
     * allocator still works until step 2 removes it */
    MUCK::database().setRecycleHead(NOTHING);
    for (dbref i = top - 1; i >= 0; i--) {
        struct object *o = DBFETCH(i);
        if ((o->flags & TYPE_MASK) == TYPE_GARBAGE) {
            o->name = alloc_string("<garbage>");
            o->next = MUCK::database().recycleHead();
            MUCK::database().setRecycleHead(i);
        }
    }

    return (dbref) top;
}

} /* namespace MUCK */
