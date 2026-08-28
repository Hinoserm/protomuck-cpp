#!/usr/bin/env python3
# Every kind of change must survive a dump and a restart.
#
# Under the journal a change that is not recorded is never written and
# is lost at restart, with no dirty-flag sweep to catch it. That makes
# "did this actually persist" the central question for every field, not
# just for properties. This walks one object through a change of each
# kind, dumps, reboots from the STORE, and insists everything held.
#
# Usage: run_persist_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', 'data/store', port)

# a room to move things into, and the object under test
sess.cmd('@dig Elsewhere', 1.0)
room = re.search(r'#(\d+)', sess.cmd('ex Elsewhere')).group(1)
sess.cmd('@create Subject', 1.0)
ref = re.search(r'#(\d+)', sess.cmd('ex Subject')).group(1)

# one change of every kind that has to reach the journal. Address by
# dbref throughout: once the object is teleported away the player can
# no longer match it by name, and a command that fails to match is a
# silently passing test.
sess.cmd('@set #%s=/deep/nested/prop:kept' % ref)
sess.cmd('@set #%s=/toremove:doomed' % ref)
sess.cmd('@set #%s=D' % ref)                    # a flag: DARK
sess.cmd('@name #%s=Renamed' % ref)
sess.cmd('@chown #%s=One' % ref, 1.0)
sess.cmd('@tel #%s=#%s' % (ref, room), 1.0)
sess.cmd('@link #%s=#%s' % (ref, room), 1.0)    # $type/home
sess.cmd('give #%s=5' % ref, 1.0)               # $type/value
out = sess.cmd('ex #%s=/' % ref)
check('removal target exists before removing', 'toremove' in out, out)
sess.cmd('@set #%s=/toremove:' % ref)           # a removal
out = sess.cmd('ex #%s=/' % ref)
check('removed while running', 'toremove' not in out, out)

live = sess.cmd('ex #%s' % ref)
home_live = re.search(r'Home:.*#(\d+)', live)
value_live = re.search(r'Value:\s*(-?\d+)', live)

sess.cmd('@dump', 8.0)
stop(sess)

# boot from the STORE, in a genuinely new process
sess = start(binpath, 'data/store', 'data/store', port)

out = sess.cmd('ex #%s' % ref)
check('name survived', 'Renamed' in out, out)
check('flag survived', 'DARK' in out, out)
check('owner survived', 'One' in out, out)

loc = re.search(r'Location:.*#(\d+)', out)
check('location survived', loc is not None and loc.group(1) == room,
      out if not loc else 'got #%s want #%s' % (loc.group(1), room))

home = re.search(r'Home:.*#(\d+)', out)
check('home survived', home_live is not None and home is not None
      and home.group(1) == home_live.group(1),
      'live %s vs reloaded %s' % (home_live and home_live.group(1),
                                  home and home.group(1)))

value = re.search(r'Value:\s*(-?\d+)', out)
check('value survived', value_live is not None and value is not None
      and value.group(1) == value_live.group(1),
      'live %s vs reloaded %s' % (value_live and value_live.group(1),
                                  value and value.group(1)))

props = sess.cmd('ex #%s=/deep/nested/' % ref)
check('nested property survived', 'prop:kept' in props, props)

root = sess.cmd('ex #%s=/' % ref)
check('removed property stayed removed', 'toremove' not in root, root)
check('type survived', re.search(r'Type:\s*THING', out) is not None, out)

stop(sess)
sys.exit(check.result())
