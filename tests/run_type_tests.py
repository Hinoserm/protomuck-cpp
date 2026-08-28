#!/usr/bin/env python3
# Object type must survive a store round trip.
#
# This exists because the type once lived in the flags word and moved
# to a field on the object; the store loader kept restoring the word
# and not the field, so every object came back as garbage. That failure
# was quiet -- names and properties still read correctly -- and it even
# made the resurrection test pass for the wrong reason, because
# resurrecting requires a garbage slot. So: create one of each type,
# reboot from the store, and insist each is still what it was.
#
# Usage: run_type_tests.py <binary> <gamedir> <port>
import socket, sys, time, select, os, subprocess, shutil, re

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)

fails = []


def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)
        if detail:
            print('  detail: ' + detail.strip()[:300])


class Session:
    def __init__(self, srv):
        self.srv = srv
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

    def cmd(self, line, d=0.7):
        mark = len(self.buf)
        self.s.sendall(line.encode('latin-1') + b'\r\n')
        self.pump(d)
        raw = self.buf[mark:].decode('latin-1', 'replace')
        return re.sub(r'\x1b\[[0-9;]*m', '', raw)


def start(infile):
    srv = subprocess.Popen([binpath, infile, 'data/store', str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    s = Session(srv)
    s.pump(1.0)
    s.cmd('connect One potrzebie', 1.2)
    return s


def type_of(sess, ref):
    out = sess.cmd('ex #%s' % ref)
    m = re.search(r'^Type:\s*(\w+)', out, re.M)
    return m.group(1).upper() if m else ('?? ' + out.strip()[:120])


shutil.copy('minimal.db', 'live.db')
sess = start('live.db')

# one of every type we can make from the command line
made = {}
for cmd, name, want in (
        ('@dig Typeroom', 'Typeroom', 'ROOM'),
        ('@create Typething', 'Typething', 'THING'),
        ('@open Typeexit', 'Typeexit', 'EXIT'),
        ('@program Typeprog', 'Typeprog', 'PROGRAM'),
):
    sess.cmd(cmd, 1.0)
    if cmd.startswith('@program'):
        sess.cmd('q', 0.6)          # leave the editor
    out = sess.cmd('ex %s' % name)
    m = re.search(r'#(\d+)', out)
    if not m:
        check('created %s' % name, False, out)
        continue
    made[name] = (m.group(1), want)
    check('created %s' % name, True)

# the player we are connected as
out = sess.cmd('ex me')
m = re.search(r'#(\d+)', out)
if m:
    made['me'] = (m.group(1), 'PLAYER')

for name, (ref, want) in made.items():
    got = type_of(sess, ref)
    check('%s is %s before dump' % (name, want), got == want, 'got %s' % got)

sess.cmd('@dump', 3.0)
sess.s.sendall(b'@shutdown muck=muck\r\n')
sess.pump(2.0)
try:
    sess.srv.wait(timeout=25)
except subprocess.TimeoutExpired:
    sess.srv.kill()

# --- the part that matters: boot from the STORE, not the flat file ---
sess = start('data/store')

for name, (ref, want) in made.items():
    got = type_of(sess, ref)
    check('%s is still %s after store boot' % (name, want), got == want,
          'got %s' % got)

# a program must still be runnable, which needs its type module
out = sess.cmd('ex #%s' % made['Typeprog'][0]) if 'Typeprog' in made else ''
check('program keeps its program-ness', 'PROGRAM' in out.upper(), out)

sess.s.sendall(b'@shutdown muck=muck\r\n')
sess.pump(2.0)
try:
    sess.srv.wait(timeout=25)
except subprocess.TimeoutExpired:
    sess.srv.kill()

print('RESULT: %d failures' % len(fails))
sys.exit(1 if fails else 0)
