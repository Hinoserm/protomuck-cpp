#!/usr/bin/env python3
# Object type must survive a store round trip.
#
# This exists because the type once lived in the flags word and moved
# to a field on the object; the store loader kept restoring the word
# and not the field, so every object came back as garbage. That failure
# was quiet -- names and properties still read correctly -- and it even
# made the resurrection test pass for the wrong reason, because
# resurrecting requires a garbage slot. So: create one of each type,
# reboot from the store, and insist each is still what it was.
#
# Usage: run_type_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()


def type_of(sess, ref):
    out = sess.cmd('ex #%s' % ref)
    m = re.search(r'^Type:\s*(\w+)', out, re.M)
    return m.group(1).upper() if m else ('?? ' + out.strip()[:120])


shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', 'data/store', port)

made = {}
for cmd, name, want in (
        ('@dig Typeroom', 'Typeroom', 'ROOM'),
        ('@create Typething', 'Typething', 'THING'),
        ('@open Typeexit', 'Typeexit', 'EXIT'),
        ('@program Typeprog', 'Typeprog', 'PROGRAM'),
):
    sess.cmd(cmd, 1.0)
    if cmd.startswith('@program'):
        sess.cmd('q', 0.6)          # leave the editor
    out = sess.cmd('ex %s' % name)
    m = re.search(r'#(\d+)', out)
    if not m:
        check('created %s' % name, False, out)
        continue
    made[name] = (m.group(1), want)
    check('created %s' % name, True)

out = sess.cmd('ex me')
m = re.search(r'#(\d+)', out)
if m:
    made['me'] = (m.group(1), 'PLAYER')

for name, (ref, want) in made.items():
    got = type_of(sess, ref)
    check('%s is %s before dump' % (name, want), got == want, 'got %s' % got)

sess.cmd('@dump', 8.0)
stop(sess)

# the part that matters: boot from the STORE, in a new process
sess = start(binpath, 'data/store', 'data/store', port)

for name, (ref, want) in made.items():
    got = type_of(sess, ref)
    check('%s is still %s after store boot' % (name, want), got == want,
          'got %s' % got)

out = sess.cmd('ex #%s' % made['Typeprog'][0]) if 'Typeprog' in made else ''
check('program keeps its program-ness', 'PROGRAM' in out.upper(), out)

stop(sess)
sys.exit(check.result())
