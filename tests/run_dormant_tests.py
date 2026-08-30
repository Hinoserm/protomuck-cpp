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
import sys, os, shutil, json

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
stop(sess)

# --- the escaped multi-line string parm ---
# A newline cannot be typed at the @tune prompt (the input layer ends
# the command there), and the manifest's parm block supersedes
# parmfile.cfg once a store exists, so the escape is exercised where
# it actually lives: the block is an array of one-parm-per-line
# strings, and an escaped value in it has to come back as a real
# newline. The writer's escaping is the exact inverse, applied in the
# same change; before the fix a real newline here split the value in
# two and the tail was read as a bogus parm name.
man = json.load(open(STORE + '/manifest.json'))
man['parms'] = [l for l in man.get('parms', [])
                if not l.startswith('huh_mesg=')]
man['parms'].append('huh_mesg=first part\\nsecond part')
json.dump(man, open(STORE + '/manifest.json', 'w'))

sess = start(binpath, STORE, STORE, port)
out = sess.cmd('@tune huh_mesg', 1.0)
# the literal two-character backslash-n must be GONE: leaving it in
# is exactly what an unescaping loader fails to do, and merely
# finding both halves in the output would pass either way
check('escaped newline in a string parm loads as a real newline',
      'first part' in out and 'second part' in out
      and '\\n' not in out, out.strip()[-200:])

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
