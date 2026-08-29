#!/usr/bin/env python3
# The retention ladder, tested at accelerated time.
#
# Real time cannot be waited out, so the test backdates marker
# timestamps in the manifest while the server is down, then boots and
# triggers a dump: fire() runs the ladder and the sweep, and the
# manifest and history files afterwards show what survived. The
# expected survivor set comes from a python reimplementation of the
# ladder, which doubles as an oracle: if the two ever drift, a test
# here goes red.
#
# Also covered: the journal-size invariant (after a full sweep no
# layer sits below the oldest surviving marker), rollback refusal for
# coalesced revisions, locked markers pinning, and reclamation of a
# recycled object once nothing predates its deletion (files gone,
# tombstone present, store still boots clean, which also proves the
# manifest-first reclaim ordering).
#
# Usage: run_ladder_tests.py <binary> <gamedir> <port>
import sys, os, shutil, json, glob, re, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
HOUR, DAY, WEEK, MONTH = 3600, 86400, 604800, 2592000


def ladder_oracle(markers, now, hourly=72, daily=90, weekly=52, monthly=24):
    """Python mirror of ladderSurvivors in ObjectStore.cpp."""
    tiers = [(HOUR, hourly * HOUR), (DAY, daily * DAY),
             (WEEK, weekly * WEEK), (MONTH, monthly * MONTH)]
    out = []
    best = {}
    for m in markers:
        age = now - m['when']
        if m.get('locked') or age < HOUR:
            out.append(m)
            continue
        for t, (grain, window) in enumerate(tiers):
            if window <= 0 or age >= window:
                continue
            key = (t, m['when'] // grain)
            if key not in best or m['when'] > best[key]['when']:
                best[key] = m
            break
    out.extend(best.values())
    return sorted(out, key=lambda m: m['rev'])


def manifest():
    return json.load(open(STORE + '/manifest.json'))


def write_manifest(m):
    json.dump(m, open(STORE + '/manifest.json', 'w'))


def hist_of(ref):
    """History lines of the object with the given dbref."""
    idx = manifest()['index']
    u = idx[str(ref)]
    path = STORE + '/objects/%s/%s/%s.hist' % (u[-4:-2], u[-2:], u)
    if not os.path.exists(path):
        return []
    return [json.loads(l) for l in open(path) if l.strip()]


def base_of(ref):
    idx = manifest()['index']
    u = idx[str(ref)]
    return json.load(open(STORE + '/objects/%s/%s/%s.json' % (u[-4:-2], u[-2:], u)))


# ---- build a store with real history ----
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)      # no surprise auto markers
out = sess.cmd('@create Subject', 0.5)
ref = int(re.search(r'#(\d+)', out).group(1))

# 12 snapshots with a property change before each: every era gets a
# layer for the subject, so thinning has something real to merge
revs = []
for k in range(12):
    sess.cmd('@set #%d=/step:%d' % (ref, k), 0.3)
    out = sess.cmd('@snapshot', 1.0)
    m = re.search(r'rev (\d+)', out)
    revs.append(int(m.group(1)) if m else -1)
sess.cmd('@set #%d=/step:final' % ref, 0.3)
sess.cmd('@dump', 3.0)
stop(sess)

check('12 snapshots took', all(r >= 0 for r in revs), repr(revs))
m = manifest()
check('manifest carries the markers', len(m['markers']) >= 12,
      len(m['markers']))

# ---- backdate: markers become one per hour reaching 200 hours back ----
now = int(time.time())
marks = sorted(m['markers'], key=lambda x: x['rev'])
for i, mk in enumerate(marks):
    # oldest marker 200h ago ... newest 200-11*17h; spread 17h apart so
    # they cross the hourly->daily boundary (72h) mid-list
    mk['when'] = now - (200 - 17 * i) * HOUR
m['markers'] = marks
write_manifest(m)

expected = ladder_oracle(marks, now)

sess = start(binpath, STORE, STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)
sess.cmd('@dump', 4.0)          # fire(true): ladder + sweep
time.sleep(2)                   # let the dump thread land the set
sess.cmd('@dump', 4.0)          # second dump commits post-sweep manifest
time.sleep(2)
got = sorted(manifest()['markers'], key=lambda x: x['rev'])
# oracle 'now' and server 'now' differ by seconds; compare rev sets
check('ladder thinned the markers to the oracle set',
      [x['rev'] for x in got] == [x['rev'] for x in expected],
      'got %r expected %r' % ([x['rev'] for x in got],
                              [x['rev'] for x in expected]))

# ---- journal bound: nothing below the oldest survivor ----
if got:
    oldest = min(x['rev'] for x in got)
    layers = hist_of(ref)
    below = [l for l in layers if l['era'] < oldest]
    check('no layer sits below the oldest surviving marker',
          not below, repr(below)[:200])
    check('the base absorbed the merged history',
          base_of(ref).get('rev', 0) <= oldest,
          base_of(ref).get('rev'))

# ---- rollback semantics across the thinned history ----
surviving = [x['rev'] for x in got]
dropped = [r for r in revs if r not in surviving]
if surviving:
    out = sess.cmd('@rollback #%d=%d' % (ref, max(surviving)), 2.0)
    check('rollback to a surviving rev works', 'Rolled' in out, out)
if dropped:
    victim = max(dropped)       # a dropped rev inside merged territory
    out = sess.cmd('@rollback #%d=%d' % (ref, victim), 2.0)
    check('rollback to a thinned rev is refused, not silently older',
          'Rollback failed' in out, out)
    check('the refusal names nearest revisions or the compaction',
          'nearest available' in out or 'compacted away' in out, out)

# ---- locked markers pin ----
out = sess.cmd('@snapshot =pinned!lock', 1.5)
lockrev = re.search(r'rev (\d+)', out)
check('locked snapshot took', lockrev is not None, out)
lockrev = int(lockrev.group(1)) if lockrev else -1
sess.cmd('@dump', 3.0)
time.sleep(1)
stop(sess)

m = manifest()
for mk in m['markers']:
    mk['when'] = now - 800 * DAY        # far beyond every tier
write_manifest(m)
sess = start(binpath, STORE, STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)
sess.cmd('@dump', 4.0)
time.sleep(2)
sess.cmd('@dump', 4.0)
time.sleep(2)
left = manifest()['markers']
check('only the locked marker survived beyond every tier',
      [x['rev'] for x in left] == [lockrev],
      repr(left))

# ---- reclamation: recycle, age everything out, sweep ----
out = sess.cmd('@create Doomed', 0.5)
dref = int(re.search(r'#(\d+)', out).group(1))
sess.cmd('@dump', 3.0)
time.sleep(1)
uuid_doomed = manifest()['index'].get(str(dref))
check('doomed object is in the committed index', uuid_doomed is not None)
sess.cmd('@recycle #%d' % dref, 1.0)
sess.cmd('@dump', 3.0)
time.sleep(1)
# still retained: the locked marker predates the deletion
files = glob.glob(STORE + '/objects/*/*/%s.json' % uuid_doomed)
check('recycled object is retained while a snapshot predates it',
      len(files) == 1, repr(files))
stop(sess)

# unlock and age out the last marker
m = manifest()
for mk in m['markers']:
    mk['locked'] = False
    mk['when'] = now - 800 * DAY
write_manifest(m)
sess = start(binpath, STORE, STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)
sess.cmd('@dump', 4.0)
time.sleep(2)
sess.cmd('@dump', 4.0)
time.sleep(2)
files = glob.glob(STORE + '/objects/*/*/%s.json' % uuid_doomed)
hists = glob.glob(STORE + '/objects/*/*/%s.hist' % uuid_doomed)
check('reclaimed object files are gone', not files and not hists,
      repr(files + hists))
tombs = manifest().get('tombstones', [])
check('tombstone records the reclaimed dbref',
      any(t.get('dbref') == dref for t in tombs), repr(tombs)[:200])
check('reclaimed object left the committed index',
      str(dref) not in manifest()['index'])
out = sess.cmd('@rollback #%d=1' % dref, 2.0)
check('rollback on a reclaimed object says the data is gone',
      'reclaimed' in out or 'Rollback failed' in out, out)
stop(sess)

# the store must still boot clean after all of that surgery
sess = start(binpath, STORE, STORE, port)
out = sess.cmd('@stats', 3.0)
check('store boots clean after thinning and reclamation',
      'total objects' in out, out[-300:])
stop(sess)

sys.exit(check.result())
