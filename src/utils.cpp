#include "copyright.h"
#include "config.h"

#include "db.h"
#include "tune.h"

/* Deep membership: is thing in the list starting at head, or inside
 * anything in it? Callers pass CONTENTS(loc)-style heads. */
int
member(dbref thing, dbref list)
{
    DOLIST(list, list) {
        if (list == thing)
            return 1;
        if ((CONTENTS(list) != NOTHING)
            && (member(thing, CONTENTS(list)))) {
            return 1;
        }
    }

    return 0;
}
