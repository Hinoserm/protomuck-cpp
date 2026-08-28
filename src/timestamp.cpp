#include "config.h"
#include "db.h"
#include "tune.h"
#include "ObjectAccess.h"


/* creds: all stuff related to refstamps was done by the foxy fox, Alynna */
void
ts_newobject(dbref player, struct object *thing)
{
    time_t now = current_systime;

    thing->ts.created = now;
    thing->ts.modified = now;
    thing->ts.lastused = now;
    thing->ts.usecount = 0;
    thing->ts.dcreated = player;
    thing->ts.dmodified = player;
    thing->ts.dlastused = player;
}

void
ts_useobject(dbref player, dbref thing)
{
    if (thing == NOTHING)
        return;
    MUCK::setLastUsed(thing, current_systime, player);
    DBFETCH(thing)->ts.usecount++;
    DBDIRTY(thing);
    if (Typeof(thing) == TYPE_ROOM)
        ts_useobject(player, MUCK::getLocation(thing));
}

void
ts_lastuseobject(dbref player, dbref thing)
{
    if (thing == NOTHING)
        return;
    MUCK::setLastUsed(thing, current_systime, player);
    if (Typeof(thing) == TYPE_ROOM)
        ts_lastuseobject(player, MUCK::getLocation(thing));
}

void
ts_modifyobject(dbref player, dbref thing)
{
    MUCK::setModified(thing, current_systime, player);
}
