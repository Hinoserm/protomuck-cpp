#include "copyright.h"
#include "config.h"

#include "db.h"
#include "params.h"
#include "props.h"
#include "tune.h"
#include "interface.h"
#include "externs.h"
#include "Modules.h"
#include "ObjectAccess.h"

static hash_tab player_list[PLAYER_HASH_SIZE];

/* alias buffer - always has enough space to store @alias related prop paths */
static char abuf[BUFFER_LEN + (sizeof(ALIASDIR_CUR) > sizeof(ALIASDIR_LAST) ? sizeof(ALIASDIR_CUR) : sizeof(ALIASDIR_LAST))];

bool
check_password(dbref player, const char *check_pw)
{
    const char *password = NULL;

    if (player == NOTHING)
        return 0;

    password = MUCK::playerPasswordSlot(player);

    /* We now do this smartly based on the DB_NEWPASSES */
    /*  database flag. -Hinoserm                        */

    if (!password || !*password)
        return 1;

    if (!MUCK::PasswordHash::enabled)
        return !strcmp(check_pw, password);

    if (MUCK::PasswordHash::compare(password, check_pw)) {
        switch (MUCK::PasswordHash::tagToVal(password)) {
            case HTYPE_CURRENT:
                break;          /* Our current best, preserve */
            case HTYPE_NONE:
                break;          /* If there's no password, preserve */
            case HTYPE_DISABLED:
                break;          /* If there's no password, preserve */
            case HTYPE_INVALID:
                break;          /* Don't recognize the type, preserve */
            default:           /* Not our current best, upgrade */
                set_password(player, check_pw);
                break;
        }
        return 1;
    }
    return 0;
}

bool
set_password(dbref player, const char *password)
{
    int res = 0;
    char hashbuf[BUFFER_LEN];

    if (player == NOTHING)
        return 0;

    if (!password || !*password) {
        delete[] MUCK::playerPasswordSlot(player);

        if (!MUCK::PasswordHash::enabled) {
            MUCK::playerPasswordSlot(player) = NULL;
            return 1;
        }

        res = MUCK::PasswordHash::hash(HTYPE_NONE, hashbuf, NULL, NULL);

        if (!res)
            return 0;
        MUCK::playerPasswordSlot(player) = alloc_string(hashbuf);
        DBDIRTY(player);
        return 1;
    }

    if (!ok_password(password))
        return 0;

    delete[] MUCK::playerPasswordSlot(player);

    if (MUCK::PasswordHash::enabled) {
        char hashbuf[BUFFER_LEN];

        res = MUCK::PasswordHash::hash(HTYPE_CURRENT, hashbuf, password, NULL);

        if (!res)
            return 0;
        MUCK::playerPasswordSlot(player) = alloc_string(hashbuf);
        DBDIRTY(player);
        return 1;
    }

    MUCK::playerPasswordSlot(player) = alloc_string(password);
    DBDIRTY(player);
    return 1;
}

dbref
lookup_alias(const char *name, int checkname)
{
    dbref alias;
    PropPtr pptr;

    /* It's important to make sure that an alias lookup based on the name of an
     * existing target never succeeds. This should *only* be possible if an
     * admin manually set the prop on #0 (via @propset or a program), but things
     * can get pretty weird fast if cases of this behavior slip through.
     *
     * Most callers to this function will have already attempted to match the
     * exact target name, and this can be skipped. 'checkname' is used to signal
     * whether or not it's safe to skip this check.
     */
    if (checkname && find_hash(name, player_list, PLAYER_HASH_SIZE) != NULL) {
        return NOTHING;
    }

    sprintf(abuf, ALIASDIR_CUR "%s", name);
    if (*name != '\0' && (pptr = get_property(0, abuf)) && PropType(pptr) == PROP_REFTYP) {
        alias = PropDataRef(pptr);
        if (Typeof(alias) == TYPE_PLAYER) {
            return alias;
        } else {
            /* bogus prop, kill it */
            remove_property(0, abuf);
        }
    }

    return NOTHING;
}

/* killold probably isn't necessary, but there's no sense in clearing the old
 * alias hint if it's going to get overwritten by the caller anyway. */
int
rotate_alias(dbref target, int killold)
{
    const char *oldalias = NULL;
    PropPtr pptr;
    int valid;

    sprintf(abuf, ALIASDIR_LAST "%d", (int) target);
    pptr = get_property(0, abuf);
    if (pptr) {
        if ((valid = PropType(pptr) == PROP_STRTYP)) {
            oldalias = PropDataUNCStr(pptr);
        }

        if (killold || !valid) {
            /* kill the old 'last' hint if we were asked to, or if it's bogus */
            remove_property(0, abuf);
        }
        if (oldalias) {
            /* kill the old alias. */
            sprintf(abuf, ALIASDIR_CUR "%s", oldalias);
            remove_property(0, abuf);
            return 1;
        }
    }

    return 0;
}

void
clear_alias(dbref target, const char *alias)
{
    if (target) {
        rotate_alias(target, 1);
        return;
    }
    if (alias) {
        sprintf(abuf, ALIASDIR_CUR "%s", alias);
        remove_property(0, abuf);
        return;
    }

    /* should not be reached */
    return;
}

int
set_alias(dbref target, const char *alias, int rotate)
{
    PData pdat;
    const char *p = alias;

    /* is the new alias legal? */
    while (*p && *p != ':' && *p != PROPDIR_DELIMITER)
        p++;
    if (*p || !ok_player_name(alias)) {
        return NOTHING;
    }

    /* is the new alias actually available? */
    if (lookup_alias(alias, 0) != NOTHING) {
        return AMBIGUOUS;
    }

    /* set the new alias */
    sprintf(abuf, ALIASDIR_CUR "%s", alias);
    pdat.flags = PROP_REFTYP;
    pdat.data.ref = target;
    set_property(0, abuf, &pdat);

    if (rotate) {
        rotate_alias(target, 0);
        /* set the "last alias" hint */
        sprintf(abuf, ALIASDIR_LAST "%d", (int) target);
        add_property(0, abuf, alias, 0);
    }

    return 0;
}

dbref
lookup_player_noalias(const char *name)
{
    hash_data *hd;

    if (*name == '#') {
        name++;
        if (!OkObj(atoi(name)) || Typeof(atoi(name)) != TYPE_PLAYER)
            return NOTHING;
        else
            return atoi(name);
    } else {
        if (*name == '*')
            name++;

        if ((hd = find_hash(name, player_list, PLAYER_HASH_SIZE)) == NULL) {
            return NOTHING;
        } else {
            return (hd->dbval);
        }
    }
}

dbref
lookup_player(const char *name)
{
    hash_data *hd;

    if (*name == '#') {
        name++;
        if (!OkObj(atoi(name)) || Typeof(atoi(name)) != TYPE_PLAYER)
            return NOTHING;
        else
            return atoi(name);
    } else {
        if (*name == '*')
            name++;

        if ((hd = find_hash(name, player_list, PLAYER_HASH_SIZE)) == NULL) {
            /* secondary check - is there an exact @alias? this will return
             * NOTHING if no alias was found. */
            return lookup_alias(name, 0);
        } else {
            return (hd->dbval);
        }
    }
}

dbref
connect_player(const char *name, const char *password)
{
    dbref player;

    if (*name == NUMBER_TOKEN && number(name + 1) && atoi(name + 1)) {
        player = (dbref) atoi(name + 1);
        if ((player < 0) || (player >= MUCK::database().top())
            || (Typeof(player) != TYPE_PLAYER))
            player = NOTHING;
    } else {
        player = lookup_player(name);
    }
    if (player == NOTHING)
        return NOTHING;
    if (!check_password(player, password))
        return NOTHING;

    return player;
}

dbref
create_player(dbref creator, const char *name, const char *password)
{
    char buf[BUFFER_LEN];

    struct object *newp;

    dbref player;

    if (!ok_player_name(name) || !ok_password(password) || tp_db_readonly)
        return NOTHING;

    /* remove any existing alias with this name */
    clear_alias(0, name);

    /* else he doesn't already exist, create him */
    player = MUCK::database().Create<MUCK::Player>(name, NOTHING)
        ->object()->ref();
    newp = DBFETCH(player);

    if (OkObj(tp_player_prototype)
        && (Typeof(tp_player_prototype) == TYPE_PLAYER)) {
        FLAGS(player) = FLAGS(tp_player_prototype);
        FLAG2(player) = FLAG2(tp_player_prototype);

        if (tp_pcreate_copy_props) {
            copy_prop(tp_player_prototype, player);
#ifdef DISKBASE
            newp->propsfpos = 0;
            newp->propsmode = PROPS_UNLOADED;
            newp->propstime = 0;
            newp->nextold = NOTHING;
            newp->prevold = NOTHING;
            dirtyprops(player);
#endif
        }
    }

    if (OkObj(tp_player_start)) {
        MUCK::setLocation(player, tp_player_start);
        MUCK::database().get(player)->As<MUCK::Player>()->setHome(MUCK::database().get(tp_player_start));
    } else {
        MUCK::setLocation(player, GLOBAL_ENVIRONMENT);
        MUCK::database().get(player)->As<MUCK::Player>()->setHome(MUCK::database().get(GLOBAL_ENVIRONMENT));
    }

    MUCK::setOwner(player, player);
    MUCK::playerSetPennies(player, tp_start_pennies);

    /* set password */
    set_password(player, password);

    /* link him to tp_player_start */
    MUCK::attachContent(tp_player_start, player);
    add_player(player);
    DBDIRTY(player);
    DBDIRTY(tp_player_start);

    sprintf(buf, CNOTE "%s is born!", MUCK::getName(player));
    anotify_except(CONTENTS(tp_player_start), NOTHING, buf, player);

    return player;
}

void
do_password(dbref player, const char *old, const char *newobj)
{

    if (Guest(player)) {
        anotify_fmt(player, CFAIL "%s", tp_noguest_mesg);
        return;
    }

    if (!check_password(player, old)) {
        anotify_nolisten2(player, CFAIL "Syntax: @password <oldpass>=<newpass>");
    } else if (!ok_password(newobj)) {
        anotify_nolisten2(player, CFAIL "Bad new password.");
    } else {
        set_password(player, newobj);
        anotify_nolisten2(player, CFAIL "Password changed.");
    }
}

void
clear_players(void)
{
    kill_hash(player_list, PLAYER_HASH_SIZE, 0);
    return;
}

void
add_player(dbref who)
{
    hash_data hd;

    hd.dbval = who;
    if (add_hash(MUCK::getName(who), hd, player_list, PLAYER_HASH_SIZE) == NULL)
        panic("Out of memory");
    else
        return;
}


void
delete_player(dbref who)
{
    int result;

    result = free_hash(MUCK::getName(who), player_list, PLAYER_HASH_SIZE);

    /* Nuke the alias managed by this dbref. This will not remove any aliases
     * that were manually set on #0, but they should auto-clean as the lookups
     * are attempted. */
    rotate_alias(who, 1);

    if (result) {
        int i;

        wall_wizards(MARK "WARNING: Playername hashtable is inconsistent.  Rebuilding it.");
        clear_players();
        for (i = MUCK::database().top(); i-- > 0;) {
            if (Typeof(i) == TYPE_PLAYER) {
                add_player(i);
            }
        }
        result = free_hash(MUCK::getName(who), player_list, PLAYER_HASH_SIZE);
        if (result) {
            wall_wizards(MARK "WARNING: Playername hashtable still inconsistent after rebuild.");
        }
    }

    return;
}
