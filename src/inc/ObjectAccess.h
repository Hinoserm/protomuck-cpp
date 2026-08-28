#ifndef MUCK_OBJECTACCESS_H
#define MUCK_OBJECTACCESS_H

/* Core field access for database objects.
 *
 * THE RULE (docs/DATABASE.txt section 2): nothing reads or writes
 * persistent object state directly. Every access goes through one of
 * these functions, and every setter records a journal entry, which is
 * how a change becomes persistent at all. A raw field poke records no
 * entry, so it is invisible to the dump and lost at restart.
 *
 * All of these take a dbref and are blanket safe: an invalid ref
 * reads a harmless default and writes nothing, so guarded legacy
 * expressions convert one for one.
 *
 * Naming: get/set pairs in camelCase. Flag words have add/clear forms
 * because the legacy |= and &= ~ idioms are everywhere and doing them
 * through read-modify-write at the call site would be noise.
 */

#include <vector>

#include "db.h"
#include "ObjectType.h"

namespace MUCK {

/* --- type ------------------------------------------------------- */
/* HOME reports as Room, exactly as the legacy Typeof did, because MUF
 * asks "#-3 room?" and has always been told yes. Every other
 * non-object ref reads Invalid. */
ObjectType typeOf(dbref ref);

/* Same, without the HOME special case. */
ObjectType rawTypeOf(dbref ref);

/* The one writer. Sets the field and mirrors the bits into the flags
 * word, so the two can never drift; the modules rebuild themselves on
 * their next access. */
void setType(dbref ref, ObjectType type);

/* Stored name of a type ("room", "thing", "exit", "player",
 * "muf_program", "unsupported", "garbage") and its inverse; this is
 * the on-disk representation. typeFromName yields Invalid on an
 * unknown name so the loader can tell a corrupt file from a type this
 * build does not implement. */
const char *typeName(ObjectType type);
ObjectType typeFromName(const char *name);

/* The single-letter code examine and unparse show: R - E P F U G.
 * A function rather than an array index because the enum must not be
 * silently convertible back to an integer. */
char typeCode(ObjectType type);

/* --- name ------------------------------------------------------- */
/* getName CAN return null, deliberately: an unnamed object and an
 * invalid ref both read null, so the legacy "if (NAME(x))" guards
 * convert one for one and keep meaning what they meant. Anything
 * printing a name wants DoNull() around it, as it always did.
 * setName COPIES; the caller keeps ownership of what it passes and
 * must not hand over an alloc_string result. */
const char *getName(dbref ref);
void setName(dbref ref, const char *name);

/* Drop the name WITHOUT freeing it, for the one case where the
 * pointer is not this object's to free: prim_copyobj struct-copies a
 * source object over a fresh one, so for an instant both hold the same
 * name buffer. Calling setName there would free the shared buffer,
 * read it back to copy it, and leave the source dangling. */
void disownName(dbref ref);

/* --- links ------------------------------------------------------ */
dbref getLocation(dbref ref);
void setLocation(dbref ref, dbref loc);
dbref getOwner(dbref ref);
void setOwner(dbref ref, dbref owner);

/* --- flag words ------------------------------------------------- */
/* Word 1 carries the type bits; typeOf reads them with the legacy
 * HOME special case, rawTypeOf without it. */
object_flag_type getFlags(dbref ref);
void setFlags(dbref ref, object_flag_type v);
void addFlags(dbref ref, object_flag_type bits);
void clearFlags(dbref ref, object_flag_type bits);

object_flag_type getFlags2(dbref ref);
void setFlags2(dbref ref, object_flag_type v);
void addFlags2(dbref ref, object_flag_type bits);
void clearFlags2(dbref ref, object_flag_type bits);

object_flag_type getFlags3(dbref ref);
void setFlags3(dbref ref, object_flag_type v);
void addFlags3(dbref ref, object_flag_type bits);
void clearFlags3(dbref ref, object_flag_type bits);

object_flag_type getFlags4(dbref ref);
void setFlags4(dbref ref, object_flag_type v);
void addFlags4(dbref ref, object_flag_type bits);
void clearFlags4(dbref ref, object_flag_type bits);

/* --- powers ----------------------------------------------------- */
/* The legacy POWERS(x) reads the powers of x's OWNER, not of x, and
 * game logic depends on that; getPowers keeps it. getOwnPowers is the
 * object's own word (the legacy POWERSDB). */
object_power_type getPowers(dbref ref);
object_power_type getPowers2(dbref ref);
object_power_type getOwnPowers(dbref ref);
object_power_type getOwnPowers2(dbref ref);
void setOwnPowers(dbref ref, object_power_type v);
void setOwnPowers2(dbref ref, object_power_type v);
void addOwnPowers(dbref ref, object_power_type bits);
void clearOwnPowers(dbref ref, object_power_type bits);
void addOwnPowers2(dbref ref, object_power_type bits);
void clearOwnPowers2(dbref ref, object_power_type bits);

/* --- containment ------------------------------------------------ */
/* Snapshots in MUF-visible order: front is what the old chain called
 * the head. Safe to iterate while the underlying object moves, which
 * the classic walk idioms rely on. */
std::vector<dbref> getContents(dbref ref);
std::vector<dbref> getExits(dbref ref);

/* --- timestamps ------------------------------------------------- */
time_t getCreated(dbref ref);
time_t getModified(dbref ref);
time_t getLastUsed(dbref ref);
int getUseCount(dbref ref);
dbref getCreatedBy(dbref ref);
dbref getModifiedBy(dbref ref);
dbref getLastUsedBy(dbref ref);

void setCreated(dbref ref, time_t when, dbref who);
void setModified(dbref ref, time_t when, dbref who);
void setLastUsed(dbref ref, time_t when, dbref who);
void setUseCount(dbref ref, int n);

/* --- descriptions and the other message properties -------------- */
/* These are properties, so they already journal through the property
 * funnel; they exist here so call sites have one vocabulary. */
const char *getDesc(dbref ref);
void setDesc(dbref ref, const char *text);

} /* namespace MUCK */

#endif /* MUCK_OBJECTACCESS_H */
