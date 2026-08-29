#!/usr/bin/env python3
# The A+B performance database and the snapshot-count measurements.
#
# Shape A: 100,000 objects with 100 properties each. Shape B: twenty
# whale objects, most at 100k properties, one at 500k, one at 1M. All
# of it is generated through MUF on a live server, so every property
# goes through the real engine and the real journal.
#
# Then snapshot cycles: every cycle churns properties (random objects
# plus one whale, conflicting keys) and takes a global snapshot; every
# tenth cycle also dumps. At each target count (default 100, 1000,
# 10000) the harness measures: @dump round trip (game-thread stall),
# @snapshot round trip, whale examine, and a full restart (boot
# replay), and reports store size. PERF: lines carry the numbers.
#
# Usage: run_perf_ab.py <binary> <gamedir> <port> [target ...]
#        run_perf_ab.py ... --small   (1/50 scale smoke run)
import sys, os, shutil, subprocess, re, time, random, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, server_pid, Checker, Session

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
rest = sys.argv[4:]
SMALL = '--small' in rest
targets = [int(x) for x in rest if x.isdigit()] or \
    ([20, 100] if SMALL else [100, 1000, 10000])

os.chdir(gamedir)
random.seed(7)
STORE = 'data/store'

if SMALL:
    A_TRIGGERS, A_PER, A_PROPS = 4, 500, 100
    WHALES = [2000] * 3 + [10000]
else:
    A_TRIGGERS, A_PER, A_PROPS = 100, 1000, 100
    WHALES = [100000] * 18 + [500000, 1000000]
WCHUNK = 20000 if not SMALL else 2000


def perf(name, val):
    print('PERF %s %s' % (name, val), flush=True)


def install(sess, name, source):
    sess.cmd('@prog %s.muf' % name, 0.4)
    sess.cmd('i', 0.2)
    for line in source.strip('\n').split('\n'):
        sess.s.sendall(line.encode('latin-1') + b'\r\n')
    out = sess.cmd('.', 1.0)
    out += sess.cmd('c', 2.0)
    sess.cmd('q', 0.5)
    if 'Compiler done' not in out:
        print('FATAL %s did not compile: %s' % (name, out[-500:]))
        sys.exit(1)
    sess.cmd('@set %s.muf=W' % name, 0.3)
    sess.cmd('@set %s.muf=L' % name, 0.3)
    sess.cmd('@act %s=me' % name, 0.3)
    sess.cmd('@link %s=%s.muf' % (name, name), 0.3)


def trigger(sess, name, marker, timeout=300):
    mark = len(sess.buf)
    sess.s.sendall((name + '\r\n').encode())
    end = time.time() + timeout
    while time.time() < end:
        sess.pump(0.2)
        if marker.encode() in sess.buf[mark:]:
            return sess.buf[mark:].decode('latin-1', 'replace')
    print('FATAL trigger %s never finished' % name)
    sys.exit(1)


def timed_cmd(sess, cmd, timeout=600):
    """Round-trip: how long until the game thread answers again."""
    token = 'MARK%d' % random.randrange(1 << 30)
    mark = len(sess.buf)
    t0 = time.time()
    sess.s.sendall((cmd + '\r\nsay ' + token + '\r\n').encode())
    end = time.time() + timeout
    while time.time() < end:
        sess.pump(0.1)
        if token.encode() in sess.buf[mark:]:
            return time.time() - t0
    return -1.0


def store_stats():
    n = subprocess.run(['du', '-sm', STORE], capture_output=True, text=True)
    mb = n.stdout.split()[0] if n.returncode == 0 else '?'
    hists = glob.glob(STORE + '/objects/*/*/*.hist')
    lines = 0
    for h in random.sample(hists, min(len(hists), 500)):
        with open(h, 'rb') as f:
            lines += f.read().count(b'\n')
    return mb, len(hists), lines


GEN_A = '''
var n
var p
var o
var k
: main
  preempt
  %(per)d n !
  begin n @ 0 > while
    #0 "gA" newobject o !
    1 k !
    begin k @ %(props)d <= while
      o @ "p" k @ intostr strcat "v" k @ intostr strcat setprop
      k @ 1 + k !
    repeat
    n @ 1 - n !
  repeat
  me @ "GENA-DONE" notify
;
''' % {'per': A_PER, 'props': A_PROPS}

WFILL = '''
var k
var e
: main
  preempt
  %(ref)s "fillpos" getpropval k !
  k @ %(chunk)d + e !
  begin k @ e @ < while
    %(ref)s "w/p" k @ intostr strcat "v" k @ intostr strcat setprop
    k @ 1 + k !
  repeat
  %(ref)s "fillpos" e @ setprop
  me @ "WFILL-DONE" notify
;
'''

CHURN = '''
var k
: main
  preempt
  %(count)d k !
  begin k @ 0 > while
    random %(top)d %% dbref
    "c/x" random 5 %% intostr strcat
    "v" random intostr strcat setprop
    k @ 1 - k !
  repeat
  10 k !
  begin k @ 0 > while
    %(whale)s "w/p" random %(wprops)d %% intostr strcat
    "churned" random intostr strcat setprop
    k @ 1 - k !
  repeat
  me @ "CHURN-DONE" notify
;
'''

# ---------------- build ----------------
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')
sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune snapshot_interval=0s', 0.5)

t0 = time.time()
install(sess, 'gena', GEN_A)
for k in range(A_TRIGGERS):
    trigger(sess, 'gena', 'GENA-DONE')
    if (k + 1) % 10 == 0:
        print('INFO shape A: %d/%d chunks' % (k + 1, A_TRIGGERS), flush=True)
perf('shapeA_build_s', '%.1f' % (time.time() - t0))

t0 = time.time()
whale_refs = []
for w, size in enumerate(WHALES):
    out = sess.cmd('@create Whale%d' % w, 0.5)
    ref = int(re.search(r'#(\d+)', out).group(1))
    whale_refs.append(ref)
    install(sess, 'wf%d' % w, WFILL % {'ref': '#%d' % ref, 'chunk': WCHUNK})
    for k in range(size // WCHUNK):
        trigger(sess, 'wf%d' % w, 'WFILL-DONE')
    print('INFO whale %d (#%d): %d props' % (w, ref, size), flush=True)
perf('whales_build_s', '%.1f' % (time.time() - t0))

out = sess.cmd('@stats', 5.0)
m = re.search(r'(\d+) total objects', out)
top = int(m.group(1)) if m else 0
perf('total_objects', top)

bigwhale = whale_refs[-1]
install(sess, 'churn', CHURN % {'count': 200, 'top': top,
                                'whale': '#%d' % bigwhale,
                                'wprops': WHALES[-1]})

# the monster first dump: everything is dirty
d = timed_cmd(sess, '@dump', timeout=3600)
perf('first_full_dump_stall_s', '%.2f' % d)
mb, nh, _ = store_stats()
perf('store_size_mb_initial', mb)

# ---------------- snapshot cycles ----------------
count = 0
for target in targets:
    t0 = time.time()
    while count < target:
        trigger(sess, 'churn', 'CHURN-DONE', timeout=120)
        sess.s.sendall(b'@snapshot\r\n')
        count += 1
        if count % 10 == 0:
            trigger(sess, 'say SYNC', 'SYNC', timeout=120)
            sess.s.sendall(b'@dump\r\n')
        if count % 200 == 0:
            print('INFO %d/%d cycles (%.1fs)' %
                  (count, target, time.time() - t0), flush=True)
        if len(sess.buf) > (1 << 22):
            sess.buf = sess.buf[-4096:]
    trigger(sess, 'say SYNC', 'SYNC', timeout=300)
    perf('cycles_to_%d_s' % target, '%.1f' % (time.time() - t0))

    d = timed_cmd(sess, '@dump')
    perf('dump_stall_s_at_%d' % target, '%.2f' % d)
    d = timed_cmd(sess, '@snapshot')
    count += 1
    perf('snapshot_stall_s_at_%d' % target, '%.2f' % d)
    d = timed_cmd(sess, 'ex #%d' % bigwhale)
    perf('examine_whale_s_at_%d' % target, '%.2f' % d)
    d = timed_cmd(sess, 'ex #500' if top > 500 else 'ex #2')
    perf('examine_small_s_at_%d' % target, '%.2f' % d)

    mb, nh, sl = store_stats()
    perf('store_size_mb_at_%d' % target, mb)
    perf('hist_files_at_%d' % target, nh)
    perf('hist_lines_sampled500_at_%d' % target, sl)

    # boot replay: real restart, time to port
    t0 = time.time()
    stop(sess)
    perf('shutdown_drain_s_at_%d' % target, '%.1f' % (time.time() - t0))
    t0 = time.time()
    sess = start(binpath, STORE, STORE, port)
    perf('boot_s_at_%d' % target, '%.1f' % (time.time() - t0))
    sess.cmd('@tune snapshot_interval=0s', 0.5)

# ---------------- rollback depth ----------------
mark = len(sess.buf)
out = sess.cmd('@snapshot #list', 5.0)
revs = [int(x) for x in re.findall(r'rev (\d+)', out)]
if revs:
    early = min(revs)
    mark = len(sess.buf)
    d = timed_cmd(sess, '@rollback #%d=%d' % (bigwhale, early), timeout=900)
    perf('rollback_whale_to_rev%d_s' % early, '%.1f' % d)
    print('INFO rollback said: %s' %
          sess.buf[mark:].decode('latin-1', 'replace').strip()[:200],
          flush=True)

stop(sess)
print('DONE', flush=True)
