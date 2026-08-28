#ifndef MUCK_OBJECTTYPE_H
#define MUCK_OBJECTTYPE_H

/* What an object IS, as a field rather than bits borrowed from the
 * flags word. docs/DATABASE.txt sections 2 and 4.
 *
 * Values 0 through 6 are ON-DISK ABI. They are bits 0-2 of the
 * $core/flags entry in every stored object file and bits 0-2 of the
 * flags word in a legacy flat dump. DO NOT RENUMBER.
 *
 * The flags word keeps mirroring these bits, and that is deliberate
 * rather than transitional laziness. Type rides inside the versioned
 * $core/flags entry, which is what gives @rollback and resurrection
 * their type fidelity; a separate unversioned field would restore
 * today's type instead of the historical one. Several places also
 * treat the word as a unit: "FLAGS(x) & ~TYPE_MASK" means "any flag
 * other than the type" and gates the visible Flags: header in
 * examine, and the MUF frame save/restore round-trips the whole word.
 * MUCK::setType is the only writer, and it keeps the two in step.
 */

namespace MUCK {

enum class ObjectType : int {
    Room = 0x0,
    Thing = 0x1,
    Exit = 0x2,
    Player = 0x3,
    Program = 0x4,
    Unsupported = 0x5,          /* runtime only: type module absent */
    Garbage = 0x6,

    /* Never held by an object: the match wildcard the legacy code
     * spelled NOTYPE, meaning "no preference". */
    NoType = 0x7,

    /* Not an object at all: an out-of-range or sentinel dbref
     * (NOTHING, AMBIGUOUS, NIL). Negative on purpose so it can never
     * collide with a stored value or with a zeroed field. */
    Invalid = -1,
};

/* True for the six types an object can actually hold. */
inline bool isStorableType(ObjectType t)
{
    return t >= ObjectType::Room && t <= ObjectType::Garbage;
}

/* True for the types that carry contents and exits. */
inline bool isContainerType(ObjectType t)
{
    return t == ObjectType::Room || t == ObjectType::Thing
        || t == ObjectType::Player;
}

} /* namespace MUCK */

#endif /* MUCK_OBJECTTYPE_H */
