#!/usr/bin/env python3
# Snapshot/rollback property semantics plus history dedup check.
# Usage: proptest2.py <binary> <gamedir> <port>
import socket, sys, time, select, os, subprocess, shutil, re, json, glob

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)

srv = subprocess.Popen([binpath, 'live.db', 'data/store', str(port)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)

s = socket.create_connection(('127.0.0.1', port), timeout=10)
s.setblocking(False)
buf = b''

def pump(d=0.5):
    global buf
    end = time.time() + d
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            try:
                x = s.recv(1 << 20)
                if x:
                    buf += x
            except (BlockingIOError, ConnectionResetError):
                return

def cmd(l, d=0.7):
    global buf
    mark = len(buf)
    s.sendall(l.encode('latin-1') + b'\r\n')
    pump(d)
    return re.sub(r'\x1b\[[0-9;]*m', '', buf[mark:].decode('latin-1', 'replace'))

fails = []
def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)
        if detail:
            print('  detail: ' + detail.strip()[:500])

def props(ref):
    out = cmd('ex #%s=/' % ref)
    got = {}
    for m in re.finditer(r'^(?:str|int) /(\w+):(\S+)', out, re.M):
        got[m.group(1)] = m.group(2)
    return got

pump(1.0)
cmd('connect One potrzebie', 1.2)

cmd('@create Verobj')
out = cmd('ex Verobj')
ref = re.search(r'#(\d+)', out).group(1)
uuid = re.search(r'UUID:\s*([0-9a-f-]{36})', out).group(1)
print('# ref', ref, 'uuid', uuid)

cmd('@set Verobj=/a:1')
cmd('@set Verobj=/b:two')
cmd('@set Verobj=/c:three')
snap = cmd('@snapshot =s1', 2.0)
r1 = re.search(r'rev (\d+)', snap).group(1)

cmd('@set Verobj=/d:four')          # add
cmd('@set Verobj=/b:')              # delete
cmd('@set Verobj=/c:trois')         # change
cmd('@set Verobj=/a:1')             # same value: must not churn history
snap = cmd('@snapshot =s2', 2.0)
r2 = re.search(r'rev (\d+)', snap).group(1)
cmd('@set Verobj=/a:1')             # same again, then force another save
cmd('@dump', 2.0)
print('# revs', r1, r2)

p = props(ref)
check('live state pre-rollback', p == {'a': '1', 'c': 'trois', 'd': 'four'}, str(p))

# History layers: the sidecar holds forward journal layers, one JSON
# object per line as {"era": N, "entries": {key: value-or-null}}.
# The intent checked here is unchanged from the old undo-record format:
# a no-op write must not churn history, a deletion must be recorded as
# a removal, and a change must be recorded with its new value.
histfile = glob.glob('data/store/objects/*/*/%s.hist' % uuid)
layers = []
if histfile:
    layers = [json.loads(l) for l in open(histfile[0]) if l.strip()]


def recorded(key):
    """Every value this key was given across all layers."""
    return [l['entries'][key] for l in layers if key in l.get('entries', {})]


check('no history for unchanged /a', recorded('/a') == [], str(recorded('/a')))
b = recorded('/b')
check('one deletion record for /b', len(b) == 1 and b[0] is None, str(b))
c = recorded('/c')
check('one change record for /c',
      len(c) == 1 and c[0] and c[0].get('value') == 'trois', str(c))
d = recorded('/d')
check('the added /d is recorded', len(d) == 1 and d[0].get('value') == 'four',
      str(d))

# the base still holds the pre-snapshot values, which is what makes a
# rollback to s1 possible at all
objfile = glob.glob('data/store/objects/*/*/%s.json' % uuid)[0]
oj = json.load(open(objfile))
base = {k: v.get('value') for k, v in oj['entries'].items() if k.startswith('/')}
check('base holds the s1 state', base == {'/a': '1', '/b': 'two', '/c': 'three'},
      str(base))

out = cmd('ex #' + ref)
check('examine counts 2 snapshots', re.search(r'Snapshots: 2 available', out) is not None, out)

cmd('@rollback #%s=%s' % (ref, r1), 1.5)
p = props(ref)
check('rollback to s1', p == {'a': '1', 'b': 'two', 'c': 'three'}, str(p))

cmd('@rollback #%s=%s' % (ref, r2), 1.5)
p = props(ref)
check('rollback to s2', p == {'a': '1', 'c': 'trois', 'd': 'four'}, str(p))

s.sendall(b'@shutdown muck=muck\r\n')
pump(2.0)
try:
    srv.wait(timeout=20)
except subprocess.TimeoutExpired:
    srv.kill()

print('RESULT: %d failures' % len(fails))
sys.exit(1 if fails else 0)
