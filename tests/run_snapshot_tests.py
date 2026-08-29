#!/usr/bin/env python3
# Snapshot/rollback property semantics plus history dedup check.
#
# The dump commits to a journal segment; the per-object files trail
# behind until distribution (docs/DATABASE.txt 7.1). File-shape
# assertions therefore run after a clean shutdown, which distributes
# everything, and the rollback assertions run on a fresh boot from
# the store, which also proves the whole cycle.
#
# Usage: run_snapshot_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, json, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

shutil.rmtree('data/store', ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', 'data/store', port)


def props(ref):
    out = sess.cmd('ex #%s=/' % ref)
    got = {}
    for m in re.finditer(r'^(?:str|int) /(\w+):(\S+)', out, re.M):
        got[m.group(1)] = m.group(2)
    return got


sess.cmd('@create Verobj', 0.7)
out = sess.cmd('ex Verobj')
ref = re.search(r'#(\d+)', out).group(1)
uuid = re.search(r'UUID:\s*([0-9a-f-]{36})', out).group(1)
print('# ref', ref, 'uuid', uuid)

sess.cmd('@set Verobj=/a:1')
sess.cmd('@set Verobj=/b:two')
sess.cmd('@set Verobj=/c:three')
snap = sess.cmd('@snapshot =s1', 2.0)
r1 = re.search(r'rev (\d+)', snap).group(1)

sess.cmd('@set Verobj=/d:four')     # add
sess.cmd('@set Verobj=/b:')         # delete
sess.cmd('@set Verobj=/c:trois')    # change
sess.cmd('@set Verobj=/a:1')        # same value: must not churn history
snap = sess.cmd('@snapshot =s2', 2.0)
r2 = re.search(r'rev (\d+)', snap).group(1)
sess.cmd('@set Verobj=/a:1')        # same again, then force another save
sess.cmd('@dump', 2.0)
print('# revs', r1, r2)

p = props(ref)
check('live state pre-restart', p == {'a': '1', 'c': 'trois', 'd': 'four'},
      str(p))

# a clean shutdown distributes the journal into the per-object files;
# only then do the file shapes below exist to inspect
stop(sess)

# History layers: the sidecar holds forward journal layers, one JSON
# object per line as {"era": N, "entries": {key: value-or-null}}.
# A no-op write must not churn history, a deletion must be recorded
# as a removal, and a change must be recorded with its new value.
histfile = glob.glob('data/store/objects/*/*/%s.hist' % uuid)
check('history sidecar exists after clean shutdown', bool(histfile))
layers = []
if histfile:
    layers = [json.loads(l) for l in open(histfile[0]) if l.strip()]


def recorded(key):
    """Every value this key was given across all layers."""
    return [l['entries'][key] for l in layers if key in l.get('entries', {})]


check('no history for unchanged /a', recorded('/a') == [],
      str(recorded('/a')))
b = recorded('/b')
check('one deletion record for /b', len(b) == 1 and b[0] is None, str(b))
c = recorded('/c')
check('one change record for /c',
      len(c) == 1 and c[0] and c[0].get('value') == 'trois', str(c))
d = recorded('/d')
check('the added /d is recorded',
      len(d) == 1 and d[0].get('value') == 'four', str(d))

# the base still holds the pre-snapshot values, which is what makes a
# rollback to s1 possible at all
objglob = glob.glob('data/store/objects/*/*/%s.json' % uuid)
check('base file exists after clean shutdown', bool(objglob))
if objglob:
    oj = json.load(open(objglob[0]))
    base = {k: v.get('value') for k, v in oj['entries'].items()
            if k.startswith('/')}
    check('base holds the s1 state',
          base == {'/a': '1', '/b': 'two', '/c': 'three'}, str(base))

# journal is empty at rest after a clean shutdown
segs = glob.glob('data/store/journal/*.jsonl')
check('journal is empty after clean shutdown', segs == [], str(segs))

# fresh boot from the store: rollback semantics
sess = start(binpath, 'data/store', 'data/store', port)

out = sess.cmd('ex #' + ref)
check('examine counts 2 snapshots',
      re.search(r'Snapshots: 2 available', out) is not None, out[-300:])

sess.cmd('@rollback #%s=%s' % (ref, r1), 2.0)
p = props(ref)
check('rollback to s1', p == {'a': '1', 'b': 'two', 'c': 'three'}, str(p))

sess.cmd('@rollback #%s=%s' % (ref, r2), 2.0)
p = props(ref)
check('rollback to s2', p == {'a': '1', 'c': 'trois', 'd': 'four'}, str(p))

stop(sess)
sys.exit(check.result())
