#!/usr/bin/env python3
# Every kind of change must survive a dump and a restart.
#
# Under the journal a change that is not recorded is never written and
# is lost at restart, with no dirty-flag sweep to catch it. That makes
# "did this actually persist" the central question for every field, not
# just for properties. This walks one object through a change of each
# kind, dumps, reboots from the STORE, and insists everything held.
#
# Usage: run_persist_tests.py <binary> <gamedir> <port>
import socket, sys, time, select, os, subprocess, shutil, re

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)

fails = []


def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)
        if detail:
            print('  detail: ' + detail.strip()[:400])


class Session:
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

    def cmd(self, line, d=0.8):
        mark = len(self.buf)
        self.s.sendall(line.encode('latin-1') + b'\r\n')
        self.pump(d)
        return re.sub(r'\x1b\[[0-9;]*m', '', self.buf[mark:].decode('latin-1', 'replace'))


def start(infile):
    srv = subprocess.Popen([binpath, infile, 'data/store', str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    s = Session()
    s.pump(1.0)
    s.cmd('connect One potrzebie', 1.5)
    return srv, s


def stop(srv, sess):
    sess.s.sendall(b'@shutdown muck=muck\r\n')
    sess.pump(2.0)
    try:
        srv.wait(timeout=30)
    except subprocess.TimeoutExpired:
        srv.kill()


shutil.copy('minimal.db', 'live.db')
srv, sess = start('live.db')

# a room to move things into, and the object under test
sess.cmd('@dig Elsewhere', 1.0)
out = sess.cmd('ex Elsewhere')
room = re.search(r'#(\d+)', out).group(1)

sess.cmd('@create Subject', 1.0)
out = sess.cmd('ex Subject')
ref = re.search(r'#(\d+)', out).group(1)

# one change of every kind that has to reach the journal
# address by dbref throughout: once the object is teleported away the
# player can no longer match it by name, and a command that fails to
# match is a silently passing test
sess.cmd('@set #%s=/deep/nested/prop:kept' % ref)
sess.cmd('@set #%s=/toremove:doomed' % ref)
sess.cmd('@set #%s=D' % ref)                    # a flag: DARK
sess.cmd('@name #%s=Renamed' % ref)
sess.cmd('@chown #%s=One' % ref, 1.0)
sess.cmd('@tel #%s=#%s' % (ref, room), 1.0)
out = sess.cmd('ex #%s=/' % ref)
check('removal target exists before removing', 'toremove' in out, out)
sess.cmd('@set #%s=/toremove:' % ref)           # a removal
out = sess.cmd('ex #%s=/' % ref)
check('removed while running', 'toremove' not in out, out)

sess.cmd('@dump', 8.0)
stop(srv, sess)

srv, sess = start('data/store')

out = sess.cmd('ex #%s' % ref)
check('name survived', 'Renamed' in out, out)
check('flag survived', re.search(r'^Flags:.*DARK', out, re.M) is not None
      or ' DARK' in out, out)
check('owner survived', 'One' in out, out)

loc = re.search(r'Location:.*#(\d+)', out)
check('location survived', loc is not None and loc.group(1) == room,
      out if not loc else 'got #%s want #%s' % (loc.group(1), room))

props = sess.cmd('ex #%s=/deep/nested/' % ref)
check('nested property survived', 'prop:kept' in props, props)

root = sess.cmd('ex #%s=/' % ref)
check('removed property stayed removed', 'toremove' not in root, root)

# and the object is still the right type after all of that
check('type survived', re.search(r'Type:\s*THING', out) is not None, out)

stop(srv, sess)
print('RESULT: %d failures' % len(fails))
sys.exit(1 if fails else 0)
