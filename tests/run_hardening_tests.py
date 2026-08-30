#!/usr/bin/env python3
# Corrupt input is damage to report, not a crash to suffer, and a
# setting has to survive its own round trip.
#
# Two defects this covers:
#
#   - timestr_full did not survive the parser that reads it back. A
#     negative time parm printed its sign inside a field ("0d 0:00:-30"
#     for -30), and the reader's %2d stops after "-3", so the value
#     came back as -3. The manifest stores parms in this format, so a
#     negative dump_interval quietly became a different number on
#     every boot, compounding each time.
#
#   - rollbackObject read the core numeric arrays with a bare
#     .get<int>(), guarded only on array size. A damaged element type
#     threw json::type_error out of a command handler that catches
#     nothing: the whole server went down, and it went down part way
#     through the restore with fields already applied and journalled.
#     A rollback over a damaged field must refuse as a whole and leave
#     the server up.
#
# Usage: run_hardening_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, json, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')


def ref_of(out):
    m = re.search(r'#(\d+)', out)
    return int(m.group(1)) if m else None


def object_files(ref):
    man = json.load(open(STORE + '/manifest.json'))
    uuid = man.get('index', {}).get(str(ref))
    if not uuid:
        return None, None
    hits = glob.glob(STORE + '/objects/*/*/%s.json' % uuid)
    if not hits:
        return None, None
    return hits[0], hits[0][:-5] + '.hist'


# --- a negative time parm must round-trip exactly ---
# The check is string equality of the DISPLAY across a restart, which
# is the actual invariant: the manifest stores this format and reads
# it back, so whatever it shows now it must still show later.
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune dump_warntime=-30s', 0.8)


def warntime(session):
    out = session.cmd('@tune dump_warntime', 0.8)
    m = re.search(r'dump_warntime\s*=\s*(\S.*?)\r', out)
    return m.group(1).strip() if m else out.strip()[-80:]


before = warntime(sess)
check('a negative time parm keeps its sign when displayed',
      before.startswith('-') and ':30' in before, repr(before))

sess.cmd('@dump', 5.0)
stop(sess)
sess = start(binpath, STORE, STORE, port)
after = warntime(sess)
check('and survives the manifest round trip unchanged',
      after == before, '%r -> %r' % (before, after))

# --- a rollback over a damaged core field must refuse, not crash ---
thing = ref_of(sess.cmd('@create Fragile', 0.8))
sess.cmd('@set #%d=/marker:before' % thing, 0.5)
snap = sess.cmd('@snapshot #%d=hard' % thing, 3.0)
m = re.search(r'rev (\d+)', snap)
check('snapshot for the rollback taken', m is not None, snap.strip()[-120:])
rev = int(m.group(1)) if m else 0

# Change the flags AFTER the snapshot, so a newer $core/flags layer
# exists beside the base. That matters: the corruption goes in the
# BASE only, so the merged current state the loader sees stays valid
# and the server still boots, while the older revision the rollback
# reconstructs is the damaged one. Corrupting the current state
# instead would (correctly) refuse the boot outright and prove
# nothing about rollback.
sess.cmd('@set #%d=D' % thing, 0.5)
sess.cmd('@set #%d=/marker:after' % thing, 0.5)
sess.cmd('@dump', 5.0)
stop(sess)

base, hist = object_files(thing)
check('stored object file located', base is not None, repr(base))
poisoned = False
if base:
    j = json.load(open(base))
    fl = j.get('entries', {}).get('$core/flags')
    if fl and isinstance(fl.get('value'), list) and len(fl['value']) > 1:
        # only the ELEMENT TYPE is wrong; the array keeps its length,
        # which is exactly what a size-only guard lets through
        fl['value'][1] = "not an integer"
        json.dump(j, open(base, 'w'))
        poisoned = True
    layers = 0
    if os.path.exists(hist):
        for line in open(hist):
            if line.strip() and '$core/flags' in line:
                layers += 1
    check('a newer flags layer exists to keep the live state valid',
          layers > 0, 'layers with $core/flags: %d' % layers)
check('base flags poisoned', poisoned, repr(base))

sess = start(binpath, STORE, STORE, port)
out = sess.cmd('@rollback #%d=%d' % (thing, rev), 3.0)
check('a rollback over a damaged core field does not report success',
      'Rolled' not in out, out.strip()[-200:])

# the point of refusing: the server is still there afterwards
out = sess.cmd('@stats', 3.0)
check('the server survives the damaged rollback',
      'total objects' in out, out.strip()[-160:])

out = sess.cmd('ex #%d=/marker' % thing, 0.8)
check('and the object was not left half-restored',
      'after' in out, out.strip()[-120:])

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
