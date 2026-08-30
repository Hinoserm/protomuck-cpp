#!/usr/bin/env python3
# An object whose type module is absent must keep its real identity.
#
# Such an object loads as an UNSUPPORTED placeholder that remembers
# what it really is in dormantTypeInfo, and EVERY writer has to agree
# on that. objectToJson consulted the map, so a full base write was
# correct; the seal path did not, so the first edit to a dormant-type
# object sealed a delta line stamped with the generic placeholder
# name, and the next compaction folded that placeholder over the base
# file's true original type name. Permanently: nothing anywhere
# remembers the original after the fold, so a later boot WITH the type
# module present no longer recognizes the object.
#
# One ordinary property edit on an object of an excluded type is all
# it takes, which makes this the common case for any deployment
# running with a type module excluded, not an edge case.
#
# Usage: run_dormanttype_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, json, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')


def ref_of(out):
    m = re.search(r'#(\d+)', out)
    return int(m.group(1)) if m else None


def stored_type(ref):
    """The type name recorded in the object's base file, which is what
    a later boot reads to decide what the object is."""
    man = json.load(open(STORE + '/manifest.json'))
    uuid = man.get('index', {}).get(str(ref))
    if not uuid:
        return None
    hits = glob.glob(STORE + '/objects/*/*/%s.json' % uuid)
    if not hits:
        return None
    return json.load(open(hits[0])).get('type')


# --- an exit, written normally ---
# exits are excludable; rooms, things and players are not (their
# contents chains would break), so an exit is the one easy fixture.
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@open dormexit', 0.8)
exit_ref = ref_of(sess.cmd('ex dormexit', 0.8))
check('exit fixture created', exit_ref is not None, repr(exit_ref))
sess.cmd('@set #%d=/before:one' % exit_ref, 0.5)
sess.cmd('@dump', 5.0)
stop(sess)
check('exit stores as an exit normally', stored_type(exit_ref) == 'exit',
      repr(stored_type(exit_ref)))

# --- boot without the exit module and edit the object ---
# The edit dirties it, so the next dump seals a DELTA line for it
# (its base already exists). That delta carries a type name, and the
# fold applies the delta's type to the base file.
sess = start(binpath, STORE, STORE, port,
             extra=['--db-exclude-type', 'exit'])
sess.cmd('@set #%d=/after:two' % exit_ref, 0.5)

# fold the delta into the base: distribution deliberately lags the
# commit by kJournalLag segments, so a single dump leaves it in the
# journal where the corruption is not yet visible. A scoped snapshot
# runs a full distribute-everything barrier.
sess.cmd('@snapshot #%d=fold' % exit_ref, 4.0)
sess.cmd('@dump', 5.0)
stop(sess)

check('an edit under an excluded type does not rewrite its identity',
      stored_type(exit_ref) == 'exit', repr(stored_type(exit_ref)))

# --- and the object must come back as a real exit ---
sess = start(binpath, STORE, STORE, port)
out = sess.cmd('ex #%d' % exit_ref, 1.0)
check('the object is still an EXIT with the module present again',
      'EXIT' in out.upper(), out.strip()[-200:])

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
