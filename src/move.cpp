#include <algorithm>
#include <vector>

#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "match.h"
#include "externs.h"
#include "ObjectAccess.h"
#include "Modules.h"

#define anotify_nolisten2(x, y) anotify_nolisten(x, y, 1);

void
moveto(dbref what, dbref where)
{
    dbref loc;

    /* do NOT move garbage */
    if (what != NOTHING && Typeof(what) == TYPE_GARBAGE) {
        return;
    }

    /* remove what from old loc */
    if ((loc = MUCK::getLocation(what)) != NOTHING) {
        MUCK::detachContent(loc, what);
        DBDIRTY(loc);
    }
    /* test for special cases */
    switch (where) {
        case NOTHING:
            MUCK::setLocation(what, NOTHING);
            return;             /* NOTHING doesn't have contents */
        case HOME:
            switch (Typeof(what)) {
                case TYPE_PLAYER:
                    where = MUCK::playerHomeRef(what);
                    break;
                case TYPE_THING:
                    where = [&]{ MUCK::Thing *t = MUCK::database().get(what)->As<MUCK::Thing>(); return (t && t->home()) ? t->home()->ref() : NOTHING; }();
                    if (parent_loop_check(what, where))
                        where = MUCK::playerHomeRef(MUCK::getOwner(what));
                    break;
                case TYPE_ROOM:
                    where = GLOBAL_ENVIRONMENT;
                    break;
                case TYPE_PROGRAM:
                case TYPE_UNSUPPORTED:
                    where = MUCK::getOwner(what);
                    break;
            }
            break;
        case NIL:
            switch (Typeof(what)) {
                case TYPE_PLAYER:
                    where = tp_player_start;
                    break;
                case TYPE_THING:
                    where = MUCK::getOwner(what);
                    break;
                case TYPE_ROOM:
                    where = tp_default_parent;
                    break;
                case TYPE_PROGRAM:
                case TYPE_UNSUPPORTED:
                    where = MUCK::getOwner(what);
                    break;
            }
            break;
    }

    /* now put what in where */
    MUCK::attachContent(where, what);
    DBDIRTY(where);
    MUCK::setLocation(what, where);
}

void
send_contents(int descr, dbref loc, dbref dest)
{
    /* snapshot: moveto mutates the list we are draining */
    std::vector<MUCK::DbObject *> snapshot = MUCK::contentsOf(loc);

    MUCK::contentsOf(loc).clear();

    /* blast locations of everything in the old list */
    for (MUCK::DbObject *o : snapshot)
        MUCK::setLocation(o->ref(), NOTHING);

    for (MUCK::DbObject *o : snapshot) {
        dbref first = o->ref();

        if ((Typeof(first) != TYPE_THING)
            && (Typeof(first) != TYPE_PROGRAM)) {
            moveto(first, loc);
        } else {
            moveto(first, FLAGS(first) & STICKY ? HOME : dest);
        }
    }

    /* arrivals prepended in walk order; the reverse restores the
     * original relative order for whatever stayed here, exactly as
     * the old chain reverse did */
    std::reverse(MUCK::contentsOf(loc).begin(), MUCK::contentsOf(loc).end());
    DBDIRTY(loc);
}

void
maybe_dropto(int descr, dbref loc, dbref dropto)
{
    if (loc == dropto)
        return;                 /* bizarre special case */

    /* check for players */
    for (MUCK::DbObject *o : MUCK::contentsOf(loc)) {
        if (Typeof(o->ref()) == TYPE_PLAYER)
            return;
    }

    /* no players, send everything to the dropto */
    send_contents(descr, loc, dropto);
}

int
parent_loop_check(dbref source, dbref dest)
{
    if (source == dest)
        return 1;               /* That's an easy one! */
    if (dest == NOTHING)
        return 0;
    if (dest == HOME)
        return 0;
    if (dest == NIL)
        return 0;
    if (Typeof(dest) == TYPE_THING && parent_loop_check(source, [&]{ MUCK::Thing *t = MUCK::database().get(dest)->As<MUCK::Thing>(); return (t && t->home()) ? t->home()->ref() : NOTHING; }()))
        return 1;
    return parent_loop_check(source, MUCK::getLocation(dest));
}

static int donelook = 0;
void
enter_room(int descr, dbref player, dbref loc, dbref exit)
{
    dbref old;
    dbref dropto;
    char buf[BUFFER_LEN];

    /* check for room == HOME */
    if (loc == HOME)
        loc = MUCK::playerHomeRef(player); /* home */
    /* check for room == NIL */
    if (loc == NIL)
        loc = Typeof(player) == TYPE_PLAYER ? tp_player_start : MUCK::getOwner(player);

    /* get old location */
    old = MUCK::getLocation(player);

    /* check for self-loop */
    /* self-loops don't do move or other player notification */
    /* but you still get autolook and penny check */
    if (loc != old) {

        /* go there */
        moveto(player, loc);

        if (old != NOTHING) {
            propqueue(descr, player, old, exit, player, NOTHING, "@depart", "Depart", 1, 1);
            envpropqueue(descr, player, old, exit, old, NOTHING, "@depart", "Depart", 1, 1);

            propqueue(descr, player, old, exit, player, NOTHING, "@odepart", "Odepart", 1, 0);
            envpropqueue(descr, player, old, exit, old, NOTHING, "@odepart", "Odepart", 1, 0);

            propqueue(descr, player, old, exit, player, NOTHING, "~depart", "Depart", 1, 1);
            envpropqueue(descr, player, old, exit, old, NOTHING, "~depart", "Depart", 1, 1);

            propqueue(descr, player, old, exit, player, NOTHING, "~odepart", "Odepart", 1, 0);
            envpropqueue(descr, player, old, exit, old, NOTHING, "~odepart", "Odepart", 1, 0);

            if (tp_allow_old_trigs) {
                propqueue(descr, player, old, exit, player, NOTHING, "_depart", "Depart", 1, 1);
                envpropqueue(descr, player, old, exit, old, NOTHING, "_depart", "Depart", 1, 1);

                propqueue(descr, player, old, exit, player, NOTHING, "_odepart", "Odepart", 1, 0);
                envpropqueue(descr, player, old, exit, old, NOTHING, "_odepart", "Odepart", 1, 0);
            }

            /* notify others unless DARK */
            if (!Hidden(player) && !Dark(old) && !Dark(player)
                /* && (Typeof(exit) != TYPE_EXIT || !Dark(exit) ) */
                && !tp_quiet_moves) {
#if !defined(QUIET_MOVES)
                sprintf(buf, CMOVE "%s has left.", MUCK::getName(player));
                anotify_except(CONTENTS(old), player, buf, player);
#endif
            }
        }

        /* if old location has STICKY dropto, send stuff through it;
         * the raw ref read keeps a HOME dropto working */
        if (old != NOTHING && (FLAGS(old) & STICKY)
            && (dropto = MUCK::roomDropToRef(old)) != NOTHING) {
            maybe_dropto(descr, old, dropto);
        }

        /* tell other folks in new location if not DARK */
        if (!Hidden(player) && !Dark(loc) && !Dark(player)
            /* && (Typeof(exit) != TYPE_EXIT || !Dark(exit) ) */
            && !tp_quiet_moves) {
#if !defined(QUIET_MOVES)
            sprintf(buf, CMOVE "%s has arrived.", MUCK::getName(player));
            anotify_except(CONTENTS(loc), player, buf, player);
#endif
        }
    }
    /* autolook */
    if (donelook < 8) {
        donelook++;
        if (!((Typeof(exit) == TYPE_EXIT) && (FLAGS(exit) & HAVEN))) {
            /* These 'look's were changed to 'loo' per Grisson's bugfix */
            if (can_move(descr, player, "loo", 1)) {
                do_move(descr, player, "loo", 1);
            } else {
                do_look_around(descr, player);
            }
        }
        donelook--;
    } else
        anotify_nolisten2(player, CINFO "Look aborted because of look action loop.");

    if (tp_penny_rate != 0) {
        /* check for pennies */
        if (!controls(player, loc)
            && MUCK::playerPennies(player) <= tp_max_pennies && RANDOM() % tp_penny_rate == 0) {
            anotify_fmt(player, CINFO "You found a %s!", tp_penny);
            MUCK::playerAddPennies(MUCK::getOwner(player), 1);
            DBDIRTY(MUCK::getOwner(player));
        }
    }

    if (loc != old) {
        envpropqueue(descr, player, loc, exit, player, NOTHING, "@arrive", "Arrive", 1, 1);
        envpropqueue(descr, player, loc, exit, player, NOTHING, "@oarrive", "Oarrive", 1, 0);
        envpropqueue(descr, player, loc, exit, player, NOTHING, "~arrive", "Arrive", 1, 1);
        envpropqueue(descr, player, loc, exit, player, NOTHING, "~oarrive", "Oarrive", 1, 0);
        if (tp_allow_old_trigs) {
            envpropqueue(descr, player, loc, exit, player, NOTHING, "_arrive", "Arrive", 1, 1);
            envpropqueue(descr, player, loc, exit, player, NOTHING, "_oarrive", "Oarrive", 1, 0);
        }
    }
}

void
send_home(int descr, dbref thing, int puppethome)
{
    switch (Typeof(thing)) {
        case TYPE_PLAYER:
            /* send his possessions home first! */
            /* that way he sees them when he arrives */
            send_contents(descr, thing, HOME);
            enter_room(descr, thing, MUCK::playerHomeRef(thing), MUCK::getLocation(thing));
            break;
        case TYPE_THING:
            if (puppethome)
                send_contents(descr, thing, HOME);
            if (FLAGS(thing) & (ZOMBIE | LISTENER)) {
                enter_room(descr, thing, MUCK::playerHomeRef(thing), MUCK::getLocation(thing));
                break;
            }
            moveto(thing, HOME); /* home */
            break;
        case TYPE_PROGRAM:
            moveto(thing, MUCK::getOwner(thing));
            break;
        default:
            /* no effect */
            break;
    }
    return;
}

int
can_move(int descr, dbref player, const char *direction, int lev)
{

    struct match_data md;
    dbref matched;

    if (!string_compare(direction, "home") && (tp_enable_home == 1))
        return 1;

    /* otherwise match on exits */
    init_match(descr, player, direction, TYPE_EXIT, &md);
    md.match_level = lev;
    match_all_exits(&md);

    matched = last_match_result(&md);

    return (matched != NOTHING);
}

int
can_move2(int descr, dbref player, const char *direction, int lev)
{

    struct match_data md;
    dbref matched;

    if (!string_compare(direction, "home") && (tp_enable_home == 1))
        return 1;

    /* otherwise match on exits */
    init_match(descr, player, direction, TYPE_EXIT, &md);
    md.match_level = lev;
    match_all_exits(&md);

    matched = last_match_result(&md);

    if (OkObj(matched)) {
        dbref dest = MUCK::exitDestCount(matched) ? MUCK::exitDestRef(matched, 0) : NOTHING;

        if ((FLAG2(player) & F2IMMOBILE) && !(FLAG2(matched) & F2IMMOBILE) && (!OkObj(dest) || Typeof(dest) != TYPE_PROGRAM)) {
            envpropqueue(descr, player, OkObj(player) ? MUCK::getLocation(player) : -1, matched, player, NOTHING, "@immobile", "Immobile", 1, 1);
            return 2;
        }
    }
    return (matched != NOTHING);
}

/*
 * trigger()
 *
 * This procedure triggers a series of actions, or meta-actions
 * which are contained in the 'dest' field of the exit.
 * Locks other than the first one are over-ridden.
 *
 * `player' is the player who triggered the exit
 * `exit' is the exit triggered
 * `pflag' is a flag which indicates whether player and room exits
 * are to be used (non-zero) or ignored (zero).  Note that
 * player/room destinations triggered via a meta-link are
 * ignored.
 *
 */

void
trigger(int descr, dbref player, dbref exit, int pflag)
{
    int i;
    dbref dest;
    dbref loc;
    int sobjact;                /* sticky object action flag, sends home
                                 * source obj */
    struct frame *tmpfr;

    loc = MUCK::getLocation(player);

    sobjact = 0;

    for (i = 0; i < MUCK::exitDestCount(exit); i++) {
        dest = MUCK::exitDestRef(exit, i);
        if (dest == HOME) {
            dest = MUCK::playerHomeRef(player);
        }
        if (dest == NIL) {      /* null destination, do nothing but the succ statements. */
            if (GETSUCC(exit)) {
                exec_or_notify(descr, player, exit, GETSUCC(exit), "(@Succ)");
            }
            if (GETOSUCC(exit) && !Dark(player)) {
                parse_omessage(descr, player, loc, exit, GETOSUCC(exit), MUCK::getName(player), "(@Osucc)");
            }
        } else {
            switch (Typeof(dest)) {
                case TYPE_ROOM:
                    if (pflag) {
                        if (parent_loop_check(player, dest)) {
                            anotify_nolisten2(player, CINFO "That would cause a paradox.");
                            break;
                        }
                        if (!Mage(MUCK::getOwner(player))
                            && Typeof(player) == TYPE_THING && ((FLAGS(dest) | FLAGS(exit)) & ZOMBIE)) {
                            anotify_nolisten2(player, CFAIL "Puppets can't go that way.");
                            break;
                        }
                        if ((FLAGS(player) & VEHICLE)
                            && Typeof(player) == TYPE_THING && ((FLAGS(dest) | FLAGS(exit)) & VEHICLE)) {
                            anotify_nolisten2(player, CFAIL "Vehicles can't go that way.");
                            break;
                        }
                        if (Guest(player) && (tp_guest_needflag ? !(FLAG2(dest) & F2GUEST)
                                              : (FLAG2(dest) & F2GUEST))) {
                            anotify_nolisten2(player, CFAIL "Guests can't go in there.");
                            break;
                        }
                        if (Guest(player) && (0 ? !(FLAG2(exit) & F2GUEST)
                                              : (FLAG2(exit) & F2GUEST))) {
                            anotify_nolisten2(player, CFAIL "Guests can't do that.");
                            break;
                        }
                        if (GETSUCC(exit)) {
                            exec_or_notify(descr, player, exit, GETSUCC(exit), "(@Succ)");
                        }
                        if (GETOSUCC(exit) && !Dark(player)) {
                            parse_omessage(descr, player, loc, exit, GETOSUCC(exit), MUCK::getName(player), "(@Osucc)");
                        }
                        if (GETDROP(exit))
                            exec_or_notify(descr, player, exit, GETDROP(exit), "(@Drop)");
                        if (GETODROP(exit) && !Dark(player)) {
                            parse_omessage(descr, player, dest, exit, GETODROP(exit), MUCK::getName(player), "(@Odrop)");
                        }
#ifdef DYNAMIC_LINKS
                        dest = MUCK::exitDestRef(exit, i);
                        if (Typeof(dest) != TYPE_ROOM)
                            break;
#endif
                        enter_room(descr, player, dest, exit);
                    }
                    break;
                case TYPE_THING:
                    if (dest == MUCK::getLocation(exit) && (FLAGS(dest) & VEHICLE)) {
                        if (pflag) {
                            if (parent_loop_check(player, dest)) {
                                anotify_nolisten2(player, CINFO "That would cause a paradox.");
                                break;
                            }
                            if (GETSUCC(exit)) {
                                exec_or_notify(descr, player, exit, GETSUCC(exit), "(@Succ)");
                            }
                            if (GETOSUCC(exit) && !Dark(player)) {
                                parse_omessage(descr, player, loc, exit, GETOSUCC(exit), MUCK::getName(player), "(@Osucc)");
                            }
                            if (GETDROP(exit))
                                exec_or_notify(descr, player, exit, GETDROP(exit), "(@Drop)");
                            if (GETODROP(exit) && !Dark(player)) {
                                parse_omessage(descr, player, dest, exit, GETODROP(exit), MUCK::getName(player), "(@Odrop)");
                            }
#ifdef DYNAMIC_LINKS
                            dest = MUCK::exitDestRef(exit, i);
                            if (Typeof(dest) != TYPE_THING)
                                break;
#endif
                            enter_room(descr, player, dest, exit);
                        }
                    } else {
                        if (Typeof(MUCK::getLocation(exit)) == TYPE_THING) {
                            if (parent_loop_check(dest, MUCK::getLocation(MUCK::getLocation(exit)))) {
                                anotify_nolisten2(player, CINFO "That would cause a paradox.");
                                break;
                            }
                            moveto(dest, MUCK::getLocation(MUCK::getLocation(exit)));
                            if (!(FLAGS(exit) & STICKY)) {
                                /* send home source object */
                                sobjact = 1;
                            }
                        } else {
                            if (parent_loop_check(dest, MUCK::getLocation(exit))) {
                                anotify_nolisten2(player, CINFO "That would cause a paradox.");
                                break;
                            }
                            moveto(dest, MUCK::getLocation(exit));
                        }
                    }
                    break;
                case TYPE_EXIT: /* It's a meta-link(tm)! */
                    ts_useobject(player, dest);
                    trigger(descr, player, MUCK::exitDestRef(exit, i), 0);
                    break;
                case TYPE_PLAYER:
                    if (pflag && MUCK::getLocation(dest) != NOTHING) {
                        if (parent_loop_check(player, dest)) {
                            anotify_nolisten2(player, CINFO "That would cause a paradox.");
                            break;
                        }
                        if (FLAGS(dest) & JUMP_OK) {
                            if (GETDROP(exit)) {
                                exec_or_notify(descr, player, exit, GETDROP(exit), "(@Drop)");
                            }
                            if (GETODROP(exit) && !Dark(player)) {
                                parse_omessage(descr, player, MUCK::getLocation(dest), exit, GETODROP(exit), MUCK::getName(player), "(@Odrop)");
                            }
#ifdef DYNAMIC_LINKS
                            dest = MUCK::exitDestRef(exit, i);
                            if (Typeof(dest) != TYPE_PLAYER)
                                break;
#endif
                            enter_room(descr, player, MUCK::getLocation(dest), exit);
                        } else {
                            anotify_nolisten2(player, CINFO "That player does not wish to be disturbed.");
                        }
                    }
                    break;
                case TYPE_PROGRAM:
                    if (Guest(player) && (0 ? !(FLAG2(dest) & F2GUEST)
                                          : (FLAG2(dest) & F2GUEST))) {
                        anotify_nolisten2(player, CFAIL "Guests can't use that program.");
                        break;
                    }
                    if (Guest(player) && (0 ? !(FLAG2(exit) & F2GUEST)
                                          : (FLAG2(exit) & F2GUEST))) {
                        anotify_nolisten2(player, CFAIL "Guests can't do that.")
                            break;
                    }
                    if (!Mage(MUCK::getOwner(player)) && Typeof(player) == TYPE_THING && (FLAGS(exit) & ZOMBIE)) {
                        anotify_nolisten2(player, CFAIL "Puppets can't go that way.");
                        break;
                    }
                    if (GETSUCC(exit)) {
                        exec_or_notify(descr, player, exit, GETSUCC(exit), "(@Succ)");
                    }
                    if (GETOSUCC(exit) && !Dark(player)) {
                        parse_omessage(descr, player, loc, exit, GETOSUCC(exit), MUCK::getName(player), "(@Osucc)");
                    }
                    tmpfr = interp(descr, player, MUCK::getLocation(player), dest, exit, FOREGROUND, STD_REGUID, 0);
                    if (tmpfr) {
                        interp_loop(player, dest, tmpfr, 0);
                    }
                    return;
                default:
                    /* an UNSUPPORTED placeholder destination */
                    anotify_nolisten2(player, CINFO "Nothing happens.");
                    break;
            }
        }
    }
    if (sobjact)
        send_home(descr, MUCK::getLocation(exit), 0);
}

void
do_move(int descr, dbref player, const char *direction, int lev)
{
    dbref exit;
    dbref loc;
    char buf[BUFFER_LEN];
    struct match_data md;

    if (!string_compare(direction, "home") && tp_enable_home) {
        if (FLAG2(player) & F2IMMOBILE) {
            anotify_nolisten2(player, CFAIL "Movement restricted, cannot return home.");
        } else {
            /* send him home */
            if ((loc = MUCK::getLocation(player)) != NOTHING) {
                /* tell everybody else */
                sprintf(buf, CMOVE "%s goes home.", MUCK::getName(player));
                anotify_except(CONTENTS(loc), player, buf, player);
            }
            /* give the player the messages */
            anotify_nolisten2(player, SYSRED "There's no place like home...");
            anotify_nolisten2(player, SYSWHITE "There's no place like home...");
            anotify_nolisten2(player, SYSBLUE "There's no place like home...");
            send_home(descr, player, 1);
        }
    } else {
        /* find the exit */
        init_match_check_keys(descr, player, direction, TYPE_EXIT, &md);
        md.match_level = lev;
        match_all_exits(&md);
        switch (exit = match_result(&md)) {
            case NOTHING:
                anotify_nolisten2(player, CFAIL "You can't go that way.");
                break;
            case AMBIGUOUS:
                anotify_nolisten2(player, CINFO "I don't know which way you mean!");
                break;
            default:
                /* we got one */
                /* check to see if we got through */
                ts_useobject(player, exit);
                loc = MUCK::getLocation(player);
                if (can_doit(descr, player, exit, "You can't go that way.")) {
                    trigger(descr, player, exit, 1);
                }
                break;
        }
    }
}


void
do_leave(int descr, dbref player)
{
    dbref loc, dest;

    loc = MUCK::getLocation(player);
    if (loc == NOTHING || Typeof(loc) == TYPE_ROOM) {
        anotify_nolisten2(player, CFAIL "You can't go that way.");
        return;
    }

    if (!(FLAGS(loc) & VEHICLE) && !(Typeof(loc) == TYPE_PLAYER)) {
        anotify_nolisten2(player, CFAIL "You can only exit vehicles.");
        return;
    }

    dest = MUCK::getLocation(loc);
    if (dest < 0 || dest >= MUCK::database().top())
        return;

    if (Typeof(dest) != TYPE_ROOM && Typeof(dest) != TYPE_THING) {
        anotify_nolisten2(player, CFAIL "You can't exit a vehicle inside of a player.");
        return;
    }

/*
 *  if (Typeof(dest) == TYPE_ROOM && !controls(player, dest)
 *          && !(FLAGS(dest) | JUMP_OK)) {
 *      anotify_nolisten2(player, CFAIL "You can't go that way.");
 *      return;
 *  }
 */

    if (parent_loop_check(player, dest)) {
        anotify_nolisten2(player, CFAIL "You can't go that way.");
        return;
    }

    anotify_nolisten2(player, CSUCC "You exit the vehicle.");
    enter_room(descr, player, dest, loc);
}


void
do_get(int descr, dbref player, const char *what, const char *obj)
{
    dbref thing, cont;
    int cando;
    struct match_data md;

    if (tp_db_readonly) {
        anotify_nolisten2(player, CFAIL DBRO_MESG);
        return;
    }

    init_match_check_keys(descr, player, what, TYPE_THING, &md);
    match_neighbor(&md);
    match_possession(&md);
    if (Mage(MUCK::getOwner(player)) || (MUCK::getPowers(player) & POW_LONG_FINGERS))
        match_absolute(&md);    /* the wizard has long fingers */

    if ((thing = noisy_match_result(&md)) != NOTHING) {
        cont = thing;
        if (obj && *obj) {
            init_match_check_keys(descr, player, obj, TYPE_THING, &md);
            match_rmatch(cont, &md);
            if (Mage(MUCK::getOwner(player)) || (MUCK::getPowers(player) & POW_LONG_FINGERS))
                match_absolute(&md); /* the wizard has long fingers */
            if ((thing = noisy_match_result(&md)) == NOTHING) {
                return;
            }
            if (Typeof(cont) == TYPE_PLAYER) {
                anotify_nolisten2(player, CFAIL "You can't steal things from players.");
                return;
            }
            if (!test_lock_false_default(descr, player, cont, "_/clk")) {
                anotify_nolisten2(player, CFAIL "You can't open that container.");
                return;
            }
        }
        if (Typeof(player) != TYPE_PLAYER) {
            if (Typeof(MUCK::getLocation(thing)) != TYPE_ROOM) {
                if (MUCK::getOwner(player) != MUCK::getOwner(thing)) {
                    anotify(player, CFAIL "Puppets aren't allowed to be thieves!");
                    return;
                }
            }
        }
        if (MUCK::getLocation(thing) == player) {
            anotify_nolisten2(player, CINFO "You already have that.");
            return;
        }
        if (Typeof(cont) == TYPE_PLAYER) {
            anotify_nolisten2(player, CFAIL "You can't steal stuff from players.");
            return;
        }
        if (parent_loop_check(thing, player)) {
            anotify_nolisten2(player, CFAIL "You can't pick yourself up by your bootstraps!");
            return;
        }
        switch (Typeof(thing)) {
            case TYPE_THING:
                ts_useobject(player, thing);
            case TYPE_PROGRAM:
                if (obj && *obj) {
                    cando = could_doit(descr, player, thing);
                    if (!cando)
                        anotify_nolisten2(player, CFAIL "You can't get that.");
                } else {
                    cando = can_doit(descr, player, thing, "You can't pick that up.");
                }
                if (cando) {
                    if (GETSUCC(thing)) {
                        exec_or_notify(descr, player, thing, GETSUCC(thing), "(@Succ)");
                    }
                    if (GETOSUCC(thing)) {
                        parse_omessage(descr, player, MUCK::getLocation(thing), thing, GETOSUCC(thing), MUCK::getName(player), "(@Osucc)");
                    }
                    moveto(thing, player);
                    anotify_nolisten2(player, CSUCC "Taken.");
                }
                break;
            default:
                anotify_nolisten2(player, CFAIL "You can't take that!");
                break;
        }
    }
}

void
do_drop(int descr, dbref player, const char *name, const char *obj)
{
    dbref loc, cont;
    dbref thing;
    char buf[BUFFER_LEN];
    struct match_data md;

    if (tp_db_readonly) {
        anotify_nolisten2(player, CFAIL DBRO_MESG);
        return;
    }

    if ((loc = MUCK::getLocation(player)) == NOTHING)
        return;

    init_match(descr, player, name, NOTYPE, &md);
    match_possession(&md);
    if ((thing = noisy_match_result(&md)) == NOTHING || thing == AMBIGUOUS)
        return;

    cont = loc;
    if (obj && *obj) {
        init_match(descr, player, obj, NOTYPE, &md);
        match_possession(&md);
        match_neighbor(&md);
        if (Mage(MUCK::getOwner(player)) || (MUCK::getPowers(player) & POW_LONG_FINGERS))
            match_absolute(&md); /* the wizard has long fingers */
        if ((cont = noisy_match_result(&md)) == NOTHING || thing == AMBIGUOUS) {
            return;
        }
    }
    switch (Typeof(thing)) {
        case TYPE_THING:
            ts_useobject(player, thing);
        case TYPE_PROGRAM:
        case TYPE_UNSUPPORTED:
            if (MUCK::getLocation(thing) != player) {
                /* Shouldn't ever happen. */
                anotify_nolisten2(player, CFAIL "You can't drop that.");
                break;
            }
            if (Typeof(cont) != TYPE_ROOM && Typeof(cont) != TYPE_PLAYER && Typeof(cont) != TYPE_THING) {
                anotify_nolisten2(player, CFAIL "You can't put anything in that.");
                break;
            }
            if (Typeof(cont) != TYPE_ROOM && !test_lock_false_default(descr, player, cont, "_/clk")) {
                anotify_nolisten2(player, CFAIL "You don't have permission to put something in that.");
                break;
            }
            if (parent_loop_check(thing, cont)) {
                anotify_nolisten2(player, CFAIL "You can't put something inside of itself.");
                break;
            }
            if (Typeof(cont) == TYPE_ROOM && (FLAGS(thing) & STICKY) && Typeof(thing) == TYPE_THING) {
                send_home(descr, thing, 0);
            } else {
                dbref cdrop = MUCK::roomDropToRef(cont);
                bool immediate_dropto = (Typeof(cont) == TYPE_ROOM
                                         && cdrop != NOTHING
                                         && !(FLAGS(cont) & STICKY));

                moveto(thing, immediate_dropto ? cdrop : cont);
            }
            if (Typeof(cont) == TYPE_THING) {
                anotify_nolisten2(player, CSUCC "Put away.");
                return;
            } else if (Typeof(cont) == TYPE_PLAYER) {
                anotify_fmt(cont, CNOTE "%s hands you %s.", MUCK::getName(player), MUCK::getName(thing));
                anotify_fmt(player, CSUCC "You hand %s to %s.", MUCK::getName(thing), MUCK::getName(cont));
                return;
            }

            if (GETDROP(thing))
                exec_or_notify(descr, player, thing, GETDROP(thing), "(@Drop)");
            else
                anotify_nolisten2(player, CSUCC "Dropped.");

            if (GETDROP(loc))
                exec_or_notify(descr, player, loc, GETDROP(loc), "(@Drop)");

            if (GETODROP(thing)) {
                parse_omessage(descr, player, loc, thing, GETODROP(thing), MUCK::getName(player), "(@Odrop)");
            } else {
                sprintf(buf, SYSBLUE "%s drops %s.", MUCK::getName(player), MUCK::getName(thing));
                anotify_except(CONTENTS(loc), player, buf, player);
            }

            if (GETODROP(loc)) {
                parse_omessage(descr, player, loc, loc, GETODROP(loc), MUCK::getName(thing), "(@Odrop)");
            }
            break;
        default:
            anotify_nolisten2(player, CFAIL "You can't drop that.");
            break;
    }
}

void
do_recycle(int descr, dbref player, const char *name)
{
    dbref thing;
    char buf[BUFFER_LEN];
    struct match_data md;

    if (Guest(player)) {
        anotify_nolisten2(player, CFAIL NOGUEST_MESG);
        return;
    }

    if (tp_db_readonly) {
        anotify_nolisten2(player, CFAIL DBRO_MESG);
        return;
    }

    init_match(descr, player, name, TYPE_THING, &md);
    match_all_exits(&md);
    match_neighbor(&md);
    match_possession(&md);
    match_registered(&md);
    match_here(&md);
    if (Mage(MUCK::getOwner(player)) || (MUCK::getPowers(player) & POW_LONG_FINGERS)) {
        match_absolute(&md);
    }
    if ((thing = noisy_match_result(&md)) != NOTHING) {
        if ((!controls(player, thing))
            || (Protect(thing) && !(MLevel(player) > MLevel(MUCK::getOwner(thing))))) {
            anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
        } else {
            switch (Typeof(thing)) {
                case TYPE_UNSUPPORTED:
                    anotify_nolisten2(player, CFAIL "That object's type module is not loaded; it can't be recycled right now.");
                    return;
                case TYPE_ROOM:
                    if (MUCK::getOwner(thing) != MUCK::getOwner(player)) {
                        anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
                        return;
                    }
                    if (thing == tp_player_start || thing == GLOBAL_ENVIRONMENT) {
                        anotify_nolisten2(player, CFAIL "This room may not be recycled.");
                        return;
                    }
                    break;
                case TYPE_THING:
                    if (MUCK::getOwner(thing) != MUCK::getOwner(player)) {
                        anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
                        return;
                    }
                    if (thing == player) {
                        sprintf(buf,
                                SYSBLUE
                                "%.512s's owner commands it to kill itself.  It blinks a few times in shock, and says, \"But.. but.. WHY?\"  It suddenly clutches it's heart, grimacing with pain..  Staggers a few steps before falling to it's knees, then plops down on it's face.  *thud*  It kicks it's legs a few times, with weakening force, as it suffers a seizure.  It's color slowly starts changing to purple, before it explodes with a fatal *POOF*!",
                                MUCK::getName(thing));
                        anotify_except(CONTENTS(MUCK::getLocation(thing)), thing, buf, player);
                        anotify_nolisten2(MUCK::getOwner(player), buf);
                        anotify_nolisten2(MUCK::getOwner(player), CINFO "Now don't you feel guilty?");
                    }
                    break;
                case TYPE_EXIT:
                    if (MUCK::getOwner(thing) != MUCK::getOwner(player)) {
                        anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
                        return;
                    }
                    if (!unset_source(player, MUCK::getLocation(player), thing)) {
                        anotify_nolisten2(player, CFAIL "You can't do that to an exit in another room.");
                        return;
                    }
                    break;
                case TYPE_PLAYER:
                    anotify_nolisten2(player, CFAIL "You can't recycle a player!");
                    return;
                    /* NOTREACHED */
                    break;
                case TYPE_PROGRAM:
                    if (MUCK::getOwner(thing) != MUCK::getOwner(player)) {
                        anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
                        return;
                    }
                    break;
                case TYPE_GARBAGE:
                    anotify_nolisten2(player, CINFO "That's already garbage.");
                    return;
                    /* NOTREACHED */
                    break;
            }
            recycle(descr, player, thing);
            if (player != thing) /* Without this, @rec me for puppets caused a crash. -hino */
                anotify_nolisten2(player, CINFO "Thank you for recycling.");
        }
    }
}

void
recycle(int descr, dbref player, dbref thing)
{
    static int depth = 0;
    dbref first;
    dbref rest;
    char buf[2048];
    int looplimit;

    depth++;
    switch (Typeof(thing)) {
        case TYPE_ROOM:
            if (!Mage(MUCK::getOwner(thing)))
                MUCK::playerAddPennies(MUCK::getOwner(thing), tp_room_cost);
            DBDIRTY(MUCK::getOwner(thing));
            {
                std::vector<MUCK::DbObject *> snap = MUCK::exitsOf(thing);

                for (MUCK::DbObject *eo : snap) {
                    first = eo->ref();
                    if (MUCK::getLocation(first) == NOTHING || MUCK::getLocation(first) == thing)
                        recycle(descr, player, first);
                }
            }
            anotify_except(CONTENTS(thing), NOTHING, CNOTE "You feel a wrenching sensation...", player);
            break;
        case TYPE_THING:
            if (!Mage(MUCK::getOwner(thing)))
                MUCK::playerAddPennies(MUCK::getOwner(thing), MUCK::database().get(thing)->As<MUCK::Thing>()->value());
            DBDIRTY(MUCK::getOwner(thing));
            {
                std::vector<MUCK::DbObject *> snap = MUCK::exitsOf(thing);

                for (MUCK::DbObject *eo : snap) {
                    first = eo->ref();
                    if (MUCK::getLocation(first) == NOTHING || MUCK::getLocation(first) == thing)
                        recycle(descr, player, first);
                }
            }
            break;
        case TYPE_EXIT:
            if (!Mage(MUCK::getOwner(thing)))
                MUCK::playerAddPennies(MUCK::getOwner(thing), tp_exit_cost);
            if (!Mage(MUCK::getOwner(thing)))
                if (MUCK::exitDestCount(thing) != 0)
                    MUCK::playerAddPennies(MUCK::getOwner(thing), tp_link_cost);
            DBDIRTY(MUCK::getOwner(thing));
            break;
        case TYPE_PROGRAM:
            dequeue_prog(thing, 0);
            sprintf(buf, "muf/%d.m", (int) thing);
            unlink(buf);
            break;
    }

    for (rest = 0; rest < MUCK::database().top(); rest++) {
        switch (Typeof(rest)) {
            case TYPE_ROOM: {
                MUCK::Room *rr = MUCK::database().get(rest)->As<MUCK::Room>();

                if (rr && rr->dropTo() && rr->dropTo()->ref() == thing)
                    rr->setDropTo(nullptr);
            }
                if (MUCK::getOwner(rest) == thing) {
                    MUCK::setOwner(rest, MAN);
                    DBDIRTY(rest);
                }
                break;
            case TYPE_THING:
                if ([&]{ MUCK::Thing *t = MUCK::database().get(rest)->As<MUCK::Thing>(); return (t && t->home()) ? t->home()->ref() : NOTHING; }() == thing) {
                    if (MUCK::playerHomeRef(MUCK::getOwner(rest)) == thing)
                        MUCK::database().get(MUCK::getOwner(rest))->As<MUCK::Player>()
                            ->setHome(MUCK::database().get(tp_player_start));
                    MUCK::database().get(rest)->As<MUCK::Thing>()->setHome(
                        MUCK::database().get(MUCK::playerHomeRef(MUCK::getOwner(rest))));
                    DBDIRTY(rest);
                }
                if (MUCK::getOwner(rest) == thing) {
                    MUCK::setOwner(rest, MAN);
                    DBDIRTY(rest);
                }
                break;
            case TYPE_EXIT:
            {
                MUCK::Exit *e = MUCK::database().get(rest)->As<MUCK::Exit>();
                dbref keep[MAX_LINKS];
                int i, j = 0;

                for (i = 0; i < e->destCount() && j < MAX_LINKS; i++) {
                    if (e->destRef(i) != thing)
                        keep[j++] = e->destRef(i);
                }
                if (j < e->destCount()) {
                    MUCK::playerAddPennies(MUCK::getOwner(rest), tp_link_cost);
                    DBDIRTY(MUCK::getOwner(rest));
                    e->setDestRefs(keep, j);
                }
            }
                if (MUCK::getOwner(rest) == thing) {
                    MUCK::setOwner(rest, MAN);
                    DBDIRTY(rest);
                }
                break;
            case TYPE_PLAYER:
                if (Typeof(thing) == TYPE_PROGRAM && (FLAGS(rest) & INTERACTIVE)
                    && (MUCK::playerSession(rest).currProg == thing)) {
                    if (FLAGS(rest) & READMODE) {
                        anotify_nolisten2(rest, CINFO "The program you were running has been recycled.  Aborting program.");
                    } else {
                        free_prog_text(MUCK::programRuntime(thing).first);
                        MUCK::programRuntime(thing).first = NULL;
                        MUCK::playerSession(rest).insertMode = 0;
                        MUCK::clearFlags(thing, INTERNAL);
                        MUCK::clearFlags(rest, INTERACTIVE);
                        MUCK::playerSession(rest).currProg = NOTHING;
                        anotify_nolisten2(rest, CINFO "The program you were editing has been recycled.  Exiting Editor.");
                    }
                }
                if (MUCK::playerHomeRef(rest) == thing) {
                    MUCK::database().get(rest)->As<MUCK::Player>()
                        ->setHome(MUCK::database().get(tp_player_start));
                }
                if (MUCK::playerSession(rest).currProg == thing)
                    MUCK::playerSession(rest).currProg = 0;
                break;
            case TYPE_PROGRAM:
            case TYPE_UNSUPPORTED:
                if (MUCK::getOwner(rest) == thing) {
                    MUCK::setOwner(rest, MAN);
                    DBDIRTY(rest);
                }
        }
        /*
         *if (DBFETCH(rest)->location == thing)
         *    DBSTORE(rest, location, NOTHING);
         */
        if (MUCK::listContains(MUCK::contentsOf(rest), thing)) {
            MUCK::detachContent(rest, thing);
            DBDIRTY(rest);
        }
        if (MUCK::listContains(MUCK::exitsOf(rest), thing)) {
            MUCK::detachExit(rest, thing);
            DBDIRTY(rest);
        }
    }

    looplimit = MUCK::database().top();
    while ((looplimit-- > 0)
           && ((first = CONTENTS(thing)) != NOTHING)) {
        if (Typeof(first) == TYPE_PLAYER) {
            enter_room(descr, first, HOME, MUCK::getLocation(thing));
            /* If the room is set to drag players back, there'll be no
             * reasoning with it.  DRAG the player out.
             */
            if (MUCK::getLocation(first) == thing) {
                notify_fmt(player, "Escaping teleport loop!");
                moveto(first, HOME);
            }
        } else {
            moveto(first, HOME);
        }
    }


    moveto(thing, NOTHING);

    depth--;

    /* the modern deletion gatekeeper: tombstone, store file removal,
     * uuid retirement, OBJECT_DELETED event. Must run while the uuid
     * still resolves, so before the object is emptied. */
    MUCK::database().deleteObject(thing, player);

    MUCK::database().freeObject(thing);
    MUCK::database().clearObject(player, thing);

    /* the slot stays a dead shell forever; dbrefs are never reused */
    MUCK::setName(thing, "<garbage>");
    MUCK::setDesc(thing, "<recyclable>");
    MUCK::setType(thing, MUCK::ObjectType::Garbage);
    MUCK::setOwner(thing, NOTHING);
}
