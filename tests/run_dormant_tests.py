#!/usr/bin/env python3
# Dormant data and parm round-tripping must not lose anything.
#
# Two defects this covers:
#
#   - "--db-exclude-module properties" loaded every property into the
#     live tree anyway. The save path is right (an excluded module's
#     entries are re-emitted verbatim from the dormant copy, which
#     wins over the live one), so the game showed a full, apparently
#     editable property tree and then threw every edit away at save.
#     An excluded module has to be dormant, not merely unsaved.
#
#   - A string tune parm holding a newline was written into the
#     manifest's parm block as a raw newline, and the block is parsed
#     one parm per line, so on the next boot the parm came back
#     truncated at the newline and the remainder was read as a bogus
#     parm name.
#
# Usage: run_dormant_tests.py <binary> <gamedir> <port>
import sys, os, shutil, subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')

# --- set up a world with a property worth losing ---
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@set me=/dormant_marker:original', 0.5)
out = sess.cmd('ex me=/dormant_marker', 0.8)
check('property set normally', 'original' in out, out.strip()[-80:])

# a string parm carrying a newline, to be read back after a restart
NL_PARM = 'first line|second line'
sess.cmd('@tune muckname=%s' % NL_PARM.replace('|', '\r'), 0.5)
sess.cmd('@dump', 5.0)
stop(sess)

# --- an excluded-properties boot must not show the property ---
sess = start(binpath, STORE, STORE, port,
             extra=['--db-exclude-module', 'properties'])
out = sess.cmd('ex me=/dormant_marker', 0.8)
check('excluded properties are not visible in game',
      'original' not in out, out.strip()[-120:])

# whatever the game does this boot, the stored copy must survive
sess.cmd('@set me=/dormant_marker:clobbered', 0.5)
sess.cmd('@dump', 5.0)
stop(sess)

# --- and a normal boot must find it exactly as it was ---
sess = start(binpath, STORE, STORE, port)
out = sess.cmd('ex me=/dormant_marker', 0.8)
check('dormant property survives an excluded boot unchanged',
      'original' in out and 'clobbered' not in out, out.strip()[-120:])

out = sess.cmd('@tune muckname', 0.8)
check('multi-line string parm round-trips through the manifest',
      'second line' in out, out.strip()[-160:])

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
