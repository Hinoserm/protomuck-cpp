#!/usr/bin/env python3
# A crash mid-dump must leave a loadable store.
#
# Writes go temp-file, fsync, rename, fsync-the-directory, and the
# manifest commits the set, so a kill at any moment should leave every
# individual file whole and the previously committed state intact.
# Changes made after the last completed dump are expected to be gone;
# that is ordinary database behavior, not a defect.
#
# Usage: run_crash_tests.py <binary> <gamedir> <port>
import socket, sys, time, select, os, signal, subprocess, shutil, re, glob

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
fails = []


def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)
        if detail:
            print('  detail: ' + detail.strip()[:300])


class S:
    def __init__(self):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.s.setblocking(False)
        self.buf = b''

    def pump(self, d=0.5):
        end = time.time() + d
        while time.time() < end:
            r, _, _ = select.select([self.s], [], [], 0.1)
            if r:
                try:
                    x = self.s.recv(1 << 20)
                    if x:
                        self.buf += x
                except (BlockingIOError, ConnectionResetError):
                    return

    def cmd(self, line, d=0.6):
        m = len(self.buf)
        self.s.sendall(line.encode('latin-1') + b'\r\n')
        self.pump(d)
        return re.sub(r'\x1b\[[0-9;]*m', '', self.buf[m:].decode('latin-1', 'replace'))


def start(infile):
    srv = subprocess.Popen([binpath, infile, 'data/store', str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    s = S()
    s.pump(1.0)
    s.cmd('connect One potrzebie', 1.5)
    return srv, s


N = 30
shutil.copy('minimal.db', 'live.db')
srv, sess = start('live.db')
for i in range(N):
    sess.cmd('@create Crash%d' % i, 0.15)
sess.cmd('@dump', 8.0)          # this state must survive

# dirty more, then die mid-write without a clean shutdown
for i in range(N):
    sess.cmd('@set #%d=/x:%d' % (5 + i, i), 0.1)
sess.s.sendall(b'@dump\r\n')
time.sleep(0.02)
os.kill(int(open('protomuck.pid').read().strip()), signal.SIGKILL)
try:
    srv.wait(timeout=10)
except subprocess.TimeoutExpired:
    srv.kill()
time.sleep(1)

srv, sess = start('data/store')
survived = sum(1 for i in range(N)
               if 'Crash%d' % i in sess.cmd('ex #%d' % (5 + i), 0.25))
check('every committed object survived the kill', survived == N,
      'survived %d/%d' % (survived, N))
check('database is intact', 'total objects' in sess.cmd('@stats', 3.0))
check('no half-written temp files left behind',
      len(glob.glob('data/store/objects/*/*/*.tmp')) == 0)

sess.s.sendall(b'@shutdown muck=muck\r\n')
sess.pump(2.0)
try:
    srv.wait(timeout=25)
except subprocess.TimeoutExpired:
    srv.kill()

print('RESULT: %d failures' % len(fails))
sys.exit(1 if fails else 0)
