#include "copyright.h"
#include "config.h"
#include "db.h"
#include "props.h"
#include "params.h"
#include "tune.h"
#include "interface.h"
#include "match.h"
#include "externs.h"
#include "Modules.h"
#include "ObjectAccess.h"

void
do_give(int descr, dbref player, const char *recipient, int amount)
{
    dbref who;
    char buf[BUFFER_LEN];
    struct match_data md;

    if (Guest(player)) {
        anotify_fmt(player, CFAIL "%s", tp_noguest_mesg);
        return;
    }

    /* do amount consistency check */
    if (amount < 0 && !Mage(MUCK::getOwner(player))) {
        anotify_nolisten2(player, CINFO "You can not steal money. Welcome to a true utopia.");
        return;
    } else if (amount == 0) {
        anotify_fmt(player, CFAIL "You must specify a positive number of %s.", tp_pennies);
        return;
    }
    /* check recipient */
    init_match(descr, player, recipient, TYPE_PLAYER, &md);
    match_neighbor(&md);
    match_me(&md);
    if (Mage(MUCK::getOwner(player))) {
        match_player(&md);
        match_absolute(&md);
    }
    switch (who = match_result(&md)) {
        case NOTHING:
            anotify_nolisten2(player, CINFO "Give to whom?");
            return;
        case AMBIGUOUS:
            anotify_nolisten2(player, CINFO "I don't know who you mean!");
            return;
        default:
            if (!Mage(MUCK::getOwner(player)) && !(MUCK::getPowers(player) & POW_NO_PAY)) {
                if (Typeof(who) != TYPE_PLAYER) {
                    anotify_nolisten2(player, CFAIL "You can only give to other players.");
                    return;
                } else if (MUCK::playerPennies(who) + amount > tp_max_pennies) {
                    anotify_fmt(player, SYSRED "That player doesn't need that many %s!", tp_pennies);
                    return;
                }
            }
            break;
    }

    /* try to do the give */
    if (!payfor(player, amount)) {
        anotify_fmt(player, CFAIL "You don't have that many %s to give!", tp_pennies);
    } else {
        /* he can do it */
        switch (Typeof(who)) {
            case TYPE_PLAYER:
                MUCK::playerAddPennies(who, amount);
                sprintf(buf, CSUCC "You give %d %s to %s.", amount, amount == 1 ? tp_penny : tp_pennies, MUCK::getName(who));
                anotify_nolisten2(player, buf);
                sprintf(buf, CNOTE "%s gives you %d %s.", MUCK::getName(player), amount, amount == 1 ? tp_penny : tp_pennies);
                anotify_nolisten2(who, buf);
                break;
            case TYPE_THING: {
                MUCK::Thing *t = MUCK::database().get(who)->As<MUCK::Thing>();

                t->setValue(t->value() + amount);
                sprintf(buf, CSUCC "You change the value of %s to %d %s.", MUCK::getName(who), t->value(), t->value() == 1 ? tp_penny : tp_pennies);
                anotify_nolisten2(player, buf);
                break;
            }
            default:
                anotify_fmt(player, CFAIL "You can't give %s to that!", tp_pennies);
                break;
        }
        DBDIRTY(who);
    }
}
