/* The property leaf: value storage, legacy type encoding, residency.
 * See docs/PROPERTIES.txt and inc/PropNode.h. Self-contained except
 * for the boolexp free routine.
 */

#include "inc/PropNode.h"

/* legacy type constants; kept in lockstep with inc/props.h */
#define PN_DIRTYP   0x0
#define PN_STRTYP   0x2
#define PN_INTTYP   0x3
#define PN_LOKTYP   0x4
#define PN_REFTYP   0x5
#define PN_FLTTYP   0x6
#define PN_TYPMASK  0x7

extern void free_boolexp(struct boolexp *b);

namespace MUCK {

PropNode::~PropNode()
{
    clearValue();
    /* children_ clears itself, deleting the subtree's leaves */
}

int
PropNode::type() const
{
    return flags & PN_TYPMASK;
}

void
PropNode::setType(int t)
{
    flags = (unsigned short) ((flags & ~PN_TYPMASK) | (t & PN_TYPMASK));
}

void
PropNode::ensureResident()
{
    /* Residency hook: today every value is RESIDENT. When the
     * optional lazy-value mode lands, this fetches the value from
     * this property's object store entry. docs/PROPERTIES.txt 4. */
}

const char *
PropNode::strValue()
{
    ensureResident();
    if (type() != PN_STRTYP)
        return nullptr;
    return sval_.c_str();
}

int
PropNode::intValue()
{
    ensureResident();
    return type() == PN_INTTYP ? ival_ : 0;
}

double
PropNode::fltValue()
{
    ensureResident();
    return type() == PN_FLTTYP ? fval_ : 0.0;
}

int
PropNode::refValue()
{
    ensureResident();
    return type() == PN_REFTYP ? ref_ : -1;
}

struct boolexp *
PropNode::lokValue()
{
    ensureResident();
    return type() == PN_LOKTYP ? lok_ : nullptr;
}

void
PropNode::clearValue()
{
    if (type() == PN_LOKTYP && lok_)
        free_boolexp(lok_);
    lok_ = nullptr;
    sval_.clear();
    setType(PN_DIRTYP);
    residency = RESIDENT;
}

void
PropNode::setStr(const char *s)
{
    clearValue();
    sval_ = s ? s : "";
    setType(PN_STRTYP);
}

void
PropNode::setInt(int v)
{
    clearValue();
    ival_ = v;
    setType(PN_INTTYP);
}

void
PropNode::setFlt(double v)
{
    clearValue();
    fval_ = v;
    setType(PN_FLTTYP);
}

void
PropNode::setRef(int r)
{
    clearValue();
    ref_ = r;
    setType(PN_REFTYP);
}

void
PropNode::setLok(struct boolexp *l)
{
    clearValue();
    lok_ = l;
    setType(PN_LOKTYP);
}

} /* namespace MUCK */
