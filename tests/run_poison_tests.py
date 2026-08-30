#!/usr/bin/env python3
# Values that ordinary MUF can produce but JSON cannot represent.
#
# A float property holding infinity or NaN dumps as the JSON literal
# null (nlohmann has no other option), and reading that back threw
# inside a parallel load worker, which terminates the process. One
# unprivileged player storing 1e308 1e308 F+ therefore bricked the
# server: every subsequent boot aborted, with the poison baked into
# the store. Non-finite floats now travel as strings and round-trip
# exactly, and every typed read off disk is guarded.
#
# Usage: run_poison_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, json, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'

POISON = '''
: main
  me @ "inf" 1.0e308 1.0e308 + setprop
  me @ "ninf" 1.0e308 1.0e308 + -1.0 * setprop
  me @ "nan" 1.0e308 1.0e308 + 1.0e308 1.0e308 + - setprop
  me @ "ok" 2.5 setprop
  me @ "POISON-DONE" notify
;
'''

shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)

sess.cmd('@prog poison.muf', 0.5)
sess.cmd('i', 0.3)
for line in POISON.strip('\n').split('\n'):
    sess.s.sendall(line.encode('latin-1') + b'\r\n')
out = sess.cmd('.', 1.0) + sess.cmd('c', 2.0)
sess.cmd('q', 0.5)
check('poison program compiled', 'Compiler done' in out, out[-300:])
sess.cmd('@set poison.muf=W', 0.3)
sess.cmd('@act poison=me', 0.3)
sess.cmd('@link poison=poison.muf', 0.3)
out = sess.cmd('poison', 3.0)
check('poison program ran', 'POISON-DONE' in out, out[-200:])

out = sess.cmd('ex me=/inf', 1.0)
print('INFO live value reads back as: %s' % out.strip()[-120:])

sess.cmd('@dump', 8.0)
stop(sess)

# the store must not contain a bare null for those properties
nulls = []
for f in glob.glob(STORE + '/objects/*/*/*.json'):
    txt = open(f).read()
    if '"value":null' in txt.replace(' ', ''):
        nulls.append(f)
check('no property serialized as a bare null', not nulls, repr(nulls[:3]))

# THE POINT: the server must still boot from that store
sess = start(binpath, STORE, STORE, port)
out = sess.cmd('@stats', 5.0)
check('server boots from a store containing non-finite floats',
      'total objects' in out, out[-200:])
out = sess.cmd('ex me=/ok', 1.0)
check('an ordinary float beside the poison survived', '2.5' in out,
      out.strip()[-120:])
out = sess.cmd('ex me=/inf', 1.0)
check('the non-finite value round-tripped rather than vanishing',
      'inf' in out.lower(), out.strip()[-120:])
stop(sess)

shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
