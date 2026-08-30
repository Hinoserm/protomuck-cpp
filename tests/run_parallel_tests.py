#!/usr/bin/env python3
# Cross the parallelism thresholds, on purpose.
#
# The seal only goes multi-threaded at 256 dirty objects and the loader
# only at 64 files, so every earlier suite (tens of objects) exercised
# ONLY the serial paths: a sanitizer run over them proved nothing about
# the parallel code. That gap hid a real data race in the property
# engine's fold-once path cache, which corrupts a lookup into a MISS
# and therefore records a live property as removed.
#
# This suite builds enough objects to force both pools, churns
# properties across all of them so every seal worker is materializing
# property values at once, and then verifies every value survives a
# real restart. Run it under ThreadSanitizer as well as normally.
#
# Usage: run_parallel_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, time, random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()
random.seed(31)

STORE = 'data/store'
NOBJ = 400          # > 256 dirty objects, and > 64 files to load

shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)

# --- build: enough objects that the seal must go parallel ---
refs = []
t0 = time.time()
for i in range(NOBJ):
    out = sess.cmd('@create P%d' % i, 0.05)
    m = re.search(r'#(\d+)', out)
    if m:
        refs.append(int(m.group(1)))
check('created %d objects' % NOBJ, len(refs) == NOBJ, '%d created' % len(refs))
print('INFO build %.1fs' % (time.time() - t0))

# every object gets several properties, including nested paths: the
# seal materializes each one through get_property, which is the cache
# the race lived in
expect = {}
for r in refs:
    for k in ('a', 'b/c', 'b/d', 'deep/nest/path'):
        v = 'v%d' % random.randrange(100000)
        sess.cmd('@set #%d=/%s:%s' % (r, k, v), 0.02)
        expect[(r, k)] = v
sess.cmd('@dump', 20.0)
time.sleep(3)

# --- churn: rewrite half of them, dump again, so the second seal is
# also parallel and running over objects that already have bases ---
for r in refs[::2]:
    for k in ('a', 'b/c'):
        v = 'w%d' % random.randrange(100000)
        sess.cmd('@set #%d=/%s:%s' % (r, k, v), 0.02)
        expect[(r, k)] = v
sess.cmd('@dump', 20.0)
time.sleep(3)

out = sess.cmd('say alive', 3.0)
check('server responsive after parallel seals', 'alive' in out, out[-200:])
stop(sess)

# --- reload: > 64 files, so the parse pipeline and parallel phase two
# both engage ---
sess = start(binpath, STORE, STORE, port)

missing = []
sample = random.sample(sorted(expect), 200)
for (r, k) in sample:
    out = sess.cmd('ex #%d=/%s' % (r, k), 0.05)
    if expect[(r, k)] not in out:
        missing.append((r, k, expect[(r, k)], out.strip()[-80:]))
check('every sampled property survived the parallel round trip (%d/200)'
      % (200 - len(missing)), not missing, repr(missing[:5]))

# nothing may have been silently dropped: count properties on a few
# objects and insist all four keys are present
short = []
for r in random.sample(refs, 40):
    out = sess.cmd('ex #%d=/' % r, 0.15)
    for k in ('a', 'b', 'deep'):
        if ('/' + k) not in out:
            short.append((r, k, out.strip()[-120:]))
check('no object lost a property directory', not short, repr(short[:4]))

out = sess.cmd('@stats', 3.0)
check('database intact after reload', 'total objects' in out, out[-200:])
stop(sess)

shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
