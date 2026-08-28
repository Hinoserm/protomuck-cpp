#!/usr/bin/env python3
# End-to-end test: deleted-object retention and @rollback resurrection.
# Usage: restest.py <binary> <gamedir> <port>
import socket, sys, time, select, os, subprocess, shutil, re

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)

def start(infile):
    srv = subprocess.Popen([binpath, infile, 'data/store', str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    return srv

class Conn:
    def __init__(self):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.s.setblocking(False)
        self.buf = b''
    def pump(self, d=0.4):
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
    def send(self, l):
        self.s.sendall(l.encode('latin-1') + b'\r\n')
    def cmd(self, l, d=0.6):
        mark = len(self.buf)
        self.send(l)
        self.pump(d)
        raw = self.buf[mark:].decode('latin-1', 'replace')
        return re.sub(r'\x1b\[[0-9;]*m', '', raw)

fails = []
def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)
        if detail:
            print('  detail: ' + detail.strip()[:400])

shutil.copy('minimal.db', 'live.db')
srv = start('live.db')
c = Conn()
c.pump(1.0)
c.send('connect One potrzebie')
c.pump(1.0)

# --- live resurrect -------------------------------------------------
out = c.cmd('@create Relic')
out = c.cmd('ex Relic')
m = re.search(r'#(\d+)', out)
check('created relic', m is not None, out)
ref = m.group(1)
c.cmd('@set Relic=/testprop:hello')
snap = c.cmd('@snapshot =t1')
m = re.search(r'rev (\d+)', snap)
check('snapshot t1', m is not None, snap)
rev = m.group(1)
c.cmd('@recycle #' + ref, 1.0)
out = c.cmd('ex #' + ref)
check('shows deleted', 'garbage' in out.lower(), out)
out = c.cmd('@rollback #%s=%s' % (ref, rev), 1.0)
check('resurrected', 'Resurrected' in out, out)
out = c.cmd('ex #' + ref)
check('name back', 'Relic' in out, out)
check('snapshot line shown', 'Snapshots:' in out, out)
out = c.cmd('ex #%s=/' % ref)
check('prop back', 'testprop:hello' in out, out)

# --- reboot survival ------------------------------------------------
out = c.cmd('@create Relictwo')
out = c.cmd('ex Relictwo')
m = re.search(r'#(\d+)', out)
ref2 = m.group(1)
c.cmd('@set Relictwo=/twoprop:world')
snap = c.cmd('@snapshot =t2')
rev2 = re.search(r'rev (\d+)', snap).group(1)
c.cmd('@recycle #' + ref2, 1.0)
c.cmd('@dump', 2.0)
c.send('@shutdown muck=muck')
c.pump(2.0)
srv.wait(timeout=20)

srv = start('data/store')
c = Conn()
c.pump(1.0)
c.send('connect One potrzebie')
c.pump(1.0)
out = c.cmd('ex #' + ref2)
check('still deleted after reboot', 'garbage' in out.lower(), out)
out = c.cmd('ex #' + ref)
check('first relic alive after reboot', 'Relic' in out, out)
out = c.cmd('@rollback #%s=%s' % (ref2, rev2), 1.0)
check('resurrected after reboot', 'Resurrected' in out, out)
out = c.cmd('ex #%s=/' % ref2)
check('prop back after reboot', 'twoprop:world' in out, out)

c.send('@shutdown muck=muck')
c.pump(2.0)
try:
    srv.wait(timeout=20)
except subprocess.TimeoutExpired:
    srv.kill()

print('%s: %d failures' % ('RESULT', len(fails)))
sys.exit(1 if fails else 0)
