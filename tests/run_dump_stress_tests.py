#!/usr/bin/env python3
# Dumps under abuse: 100 back to back, layered over conflicting
# property overlays, with snapshots mixed in.
#
# Every dump queues a frozen set for the one dump thread, so overlap
# means queue depth, never two writers in one file. What this proves
# from the outside: the server stays responsive while the queue
# drains, the final state after a real restart is the last value
# written (later layers beat earlier ones for the same keys), a
# rollback still reconstructs an intermediate overlay correctly, and
# the store passes --verify-entries afterwards.
#
# Usage: run_dump_stress_tests.py <binary> <gamedir> <port>
import sys, os, shutil, subprocess, re, time, random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()
random.seed(11)

STORE = 'data/store'
NOBJ = 40
DUMPS = 100

shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)

refs = []
for i in range(NOBJ):
    out = sess.cmd('@create Obj%d' % i, 0.2)
    refs.append(int(re.search(r'#(\d+)', out).group(1)))

# conflicting overlays: the same few keys on the same objects are
# rewritten before every dump, so each object's history piles layer on
# layer for identical keys
final = {}
snap_at = {}
t0 = time.time()
for d in range(DUMPS):
    for _ in range(10):
        r = random.choice(refs)
        key = 'k%d' % random.randrange(5)
        val = 'd%d.%d' % (d, random.randrange(1000))
        sess.cmd('@set #%d=/%s:%s' % (r, key, val), 0.05)
        final[(r, key)] = val
    sess.cmd('@dump', 0.05)     # do NOT wait; pile them up
    if d in (25, 50, 75):
        out = sess.cmd('@snapshot =overlay%d' % d, 1.0)
        mrev = re.search(r'rev (\d+)', out)
        snap_at[d] = (int(mrev.group(1)) if mrev else -1, dict(final))
elapsed = time.time() - t0
print('INFO %d dumps + churn issued in %.1fs' % (DUMPS, elapsed))

out = sess.cmd('say still alive', 3.0)
check('server is responsive after %d back-to-back dumps' % DUMPS,
      'still alive' in out, out)

# a clean shutdown drains the queue; then reboot from the store
stop(sess)
sess = start(binpath, STORE, STORE, port)

ok = 0
sample = random.sample(sorted(final), 60)
for (r, key) in sample:
    out = sess.cmd('ex #%d=/%s' % (r, key), 0.3)
    if final[(r, key)] in out:
        ok += 1
check('the last overlay won for every sampled key (%d/60)' % ok, ok == 60)

# rollback to a mid-run snapshot must reproduce the overlay of that
# moment, not the final one
d = 50
mrev, state = snap_at[d]
check('mid-run snapshot took', mrev >= 0)
victim = random.choice([r for (r, k) in state if (r, 'k1') in state])
out = sess.cmd('@rollback #%d=%d' % (victim, mrev), 3.0)
check('rollback to the mid-run snapshot works', 'Rolled' in out, out)
out = sess.cmd('ex #%d=/k1' % victim, 0.5)
check('rolled-back value is the overlay as of that snapshot',
      state[(victim, 'k1')] in out,
      'wanted %s got %s' % (state[(victim, 'k1')], out[-200:]))

stop(sess)

r = subprocess.run([binpath, '--verify-entries', STORE, STORE],
                   capture_output=True, text=True, timeout=300)
check('store loads clean after the stress run', r.returncode == 0,
      r.stderr[-300:])
ver = open('logs/status').read()
m = re.findall(r'VERIFY: (\d+) entries checked, (\d+) mismatches', ver)
check('entry serialization audit is clean',
      bool(m) and m[-1][1] == '0', repr(m[-3:]))

shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
