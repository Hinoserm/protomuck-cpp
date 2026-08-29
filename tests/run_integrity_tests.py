#!/usr/bin/env python3
# A damaged store must refuse to boot; --force-load must boot anyway.
#
# Every scenario probes with --verify-entries, which loads the store
# and exits without serving: exit 2 plus a "refusing to boot" line on
# stderr is a refusal, exit 0 is a successful boot. The pristine store
# is rebuilt by copy for each scenario, so scenarios cannot leak
# damage into each other.
#
# One scenario is recovery, not damage: a file the committed manifest
# index does not name is an uncommitted leftover from an interrupted
# dump, and must be discarded silently rather than refused.
#
# Usage: run_integrity_tests.py <binary> <gamedir> <port>
import sys, os, shutil, subprocess, glob, json, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
PRISTINE = 'data/store.pristine'


def probe(force=False):
    """Load the store and exit. Returns (returncode, stderr)."""
    cmd = [binpath, '--verify-entries']
    if force:
        cmd.append('--force-load')
    cmd += [STORE, STORE]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    return r.returncode, r.stderr


def fresh():
    shutil.rmtree(STORE, ignore_errors=True)
    shutil.copytree(PRISTINE, STORE)


def objfiles():
    return sorted(glob.glob(STORE + '/objects/*/*/*.json'))


def histfiles():
    return sorted(glob.glob(STORE + '/objects/*/*/*.hist'))


# ---- build the pristine store: a few objects, some history ----
shutil.rmtree(STORE, ignore_errors=True)
shutil.rmtree(PRISTINE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
for i in range(5):
    sess.cmd('@create Thing%d' % i, 0.3)
sess.cmd('@dump', 5.0)
for i in range(5):
    sess.cmd('@set Thing%d=/color:blue' % i, 0.2)
sess.cmd('@dump', 5.0)
stop(sess)
shutil.copytree(STORE, PRISTINE)

rc, err = probe()
check('pristine store loads cleanly', rc == 0, err)

# ---- truncated object file ----
fresh()
victim = objfiles()[2]
open(victim, 'w').write(open(victim).read()[:40])
rc, err = probe()
check('truncated object file refuses the boot',
      rc == 2 and 'refusing to boot' in err, err)
check('truncated object file is named', os.path.basename(victim) in err, err)
rc, err = probe(force=True)
check('--force-load boots past the truncated file', rc == 0, err)

# ---- garbage history line ----
fresh()
hists = histfiles()
check('pristine store has history sidecars', len(hists) > 0)
with open(hists[0], 'a') as f:
    f.write('{"era": 1, "entr\n')
rc, err = probe()
check('corrupt history line refuses the boot',
      rc == 2 and 'refusing to boot' in err and 'line' in err, err)
rc, err = probe(force=True)
check('--force-load boots past the corrupt history', rc == 0, err)

# ---- missing object file ----
fresh()
victim = objfiles()[3]
os.unlink(victim)
h = victim[:-5] + '.hist'
if os.path.exists(h):
    os.unlink(h)
rc, err = probe()
check('missing object file refuses the boot',
      rc == 2 and 'file missing from the store' in err, err)
rc, err = probe(force=True)
check('--force-load boots past the missing file', rc == 0, err)

# ---- orphaned history ----
fresh()
victim = objfiles()[1]
os.unlink(victim)
orphan = victim[:-5] + '.hist'
if not os.path.exists(orphan):
    open(orphan, 'w').write('{"era": 1, "entries": {}}\n')
rc, err = probe()
check('orphaned history is reported',
      rc == 2 and 'history with no object file' in err, err)

# ---- missing manifest ----
fresh()
os.unlink(STORE + '/manifest.json')
rc, err = probe()
check('missing manifest refuses the boot', rc != 0, err)

# ---- corrupt manifest ----
fresh()
open(STORE + '/manifest.json', 'w').write('{"format": 1, "next')
rc, err = probe()
check('corrupt manifest refuses the boot',
      rc == 2 and 'manifest' in err, err)

# ---- uncommitted leftover: recovery, not damage ----
fresh()
src = objfiles()[0]
j = json.load(open(src))
rogue_uuid = re.sub(r'[0-9a-f]{4}$', 'dead', j['uuid'])
j['uuid'] = rogue_uuid
j['dbref'] = 4999
rogue_dir = STORE + '/objects/de/ad'
os.makedirs(rogue_dir, exist_ok=True)
rogue = rogue_dir + '/' + rogue_uuid + '.json'
json.dump(j, open(rogue, 'w'))
rc, err = probe()
check('uncommitted leftover does not refuse the boot', rc == 0, err)
check('uncommitted leftover was discarded', not os.path.exists(rogue))

# ---- file name / uuid mismatch ----
fresh()
victim = objfiles()[2]
j = json.load(open(victim))
j['uuid'] = re.sub(r'[0-9a-f]{4}$', 'beef', j['uuid'])
json.dump(j, open(victim, 'w'))
rc, err = probe()
check('uuid mismatch refuses the boot',
      rc == 2 and 'does not match the uuid' in err, err)

# ---- a damaged store must also refuse a real (serving) boot ----
# The serving boot detaches and closes stderr before the store loads,
# so the log file is where the refusal must be readable.
fresh()
victim = objfiles()[2]
open(victim, 'w').write('not json at all')
logfile = 'logs/status'
mark = os.path.getsize(logfile) if os.path.exists(logfile) else 0
p = subprocess.Popen([binpath, '-port', str(port), STORE, STORE],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p.communicate(timeout=60)
import time
deadline = time.time() + 30
logged = ''
while time.time() < deadline:
    logged = open(logfile).read()[mark:] if os.path.exists(logfile) else ''
    if 'DIE: store integrity' in logged:
        break
    time.sleep(0.5)
check('serving boot refuses the damaged store (logged)',
      'DIE: store integrity' in logged, logged[-400:])
check('the problem itself is in the log',
      'not valid JSON' in logged, logged[-400:])

fresh()
shutil.rmtree(PRISTINE, ignore_errors=True)
sys.exit(check.result())
