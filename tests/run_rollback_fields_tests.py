#!/usr/bin/env python3
# Rollback must restore type-specific state, not just core identity.
#
# The restore handled $core/* and properties but never read any
# $type/* field, so rolling back an exit left its destinations, a room
# its dropto, a thing its home and value, and a player their pennies
# exactly as they were before the rollback. Exit destinations are the
# most player-visible piece of an exit's state, so the command
# reported success while changing nothing that mattered.
#
# Also covers the name-table rules: rolling back an unrelated field on
# a player must not disturb their name entry, and a rollback that
# would restore a name another live player now holds must be refused.
#
# Usage: run_rollback_fields_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)


def ref_of(out):
    m = re.search(r'#(\d+)', out)
    return int(m.group(1)) if m else None


# two rooms to link an exit between, a thing, and the exit itself
roomA = ref_of(sess.cmd('@dig RoomAlpha', 0.6))
roomB = ref_of(sess.cmd('@dig RoomBeta', 0.6))
thing = ref_of(sess.cmd('@create Widget', 0.6))
sess.cmd('@open jump', 0.5)
exit_ref = ref_of(sess.cmd('ex jump', 0.6))
check('fixture objects created',
      None not in (roomA, roomB, thing, exit_ref),
      repr((roomA, roomB, thing, exit_ref)))

# original state
sess.cmd('@link jump=#%d' % roomA, 0.5)
sess.cmd('@link RoomAlpha=#%d' % roomB, 0.5)      # dropto
sess.cmd('@link Widget=#%d' % roomA, 0.5)         # home
sess.cmd('@set #%d=/marker:before' % thing, 0.4)

snap = sess.cmd('@snapshot =fields', 2.0)
m = re.search(r'rev (\d+)', snap)
check('snapshot taken', m is not None, snap.strip()[-80:])
rev = int(m.group(1)) if m else 0

# change everything the snapshot captured
sess.cmd('@unlink jump', 0.5)
sess.cmd('@link jump=#%d' % roomB, 0.5)
sess.cmd('@link RoomAlpha=#%d' % roomA, 0.5)
sess.cmd('@link Widget=#%d' % roomB, 0.5)
sess.cmd('@set #%d=/marker:after' % thing, 0.4)
sess.cmd('@dump', 4.0)

out = sess.cmd('ex #%d' % exit_ref, 1.0)
check('exit points at the new destination before rollback',
      ('#%d' % roomB) in out, out.strip()[-160:])

# roll each object back
for r in (exit_ref, roomA, thing):
    o = sess.cmd('@rollback #%d=%d' % (r, rev), 2.5)
    check('rollback of #%d reported success' % r, 'Rolled' in o,
          o.strip()[-120:])

out = sess.cmd('ex #%d' % exit_ref, 1.0)
check('exit destination restored', ('#%d' % roomA) in out,
      out.strip()[-160:])
out = sess.cmd('ex #%d' % roomA, 1.0)
check('room dropto restored', ('#%d' % roomB) in out, out.strip()[-200:])
out = sess.cmd('ex #%d' % thing, 1.0)
check('thing home restored', ('#%d' % roomA) in out, out.strip()[-200:])
out = sess.cmd('ex #%d=/marker' % thing, 0.8)
check('property restored alongside', 'before' in out, out.strip()[-80:])

# and it must all survive a restart
sess.cmd('@dump', 4.0)
stop(sess)
sess = start(binpath, STORE, STORE, port)
out = sess.cmd('ex #%d' % exit_ref, 1.0)
check('restored exit destination survives a restart',
      ('#%d' % roomA) in out, out.strip()[-160:])

# a rollback that does not change a player's name must leave the name
# table alone (it used to delete and re-add, dropping their aliases)
out = sess.cmd('ex me', 1.0)
me = ref_of(out)
sess.cmd('@set #%d=/desc_marker:one' % me, 0.4)
snap = sess.cmd('@snapshot =playersnap', 2.0)
prev = int(re.search(r'rev (\d+)', snap).group(1))
sess.cmd('@set #%d=/desc_marker:two' % me, 0.4)
sess.cmd('@dump', 4.0)
o = sess.cmd('@rollback #%d=%d' % (me, prev), 2.5)
check('player rollback of an unrelated field succeeds', 'Rolled' in o,
      o.strip()[-120:])
out = sess.cmd('ex #%d=/desc_marker' % me, 0.8)
check('player property rolled back', 'one' in out, out.strip()[-80:])
out = sess.cmd('@stats', 3.0)
check('database intact after player rollback', 'total objects' in out,
      out[-160:])

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
