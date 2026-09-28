#!/usr/bin/env python3
# @frob must leave a sane object, and a websocket connection must be
# a first-class player connection.
#
# @frob: setType swaps the Player module for a FRESH Thing, whose home
# defaulted to -3 (HOME) under a comment claiming NOTHING. Legacy MUCK
# carried the player's home across in a shared union slot; nothing
# does that now, so every frob left an object @sanity rejected. The
# frobbed object is now homed on the wizard who frobbed it.
#
# Websockets: process_input returned early for them before the only
# line that refreshes d->last_time, so a wsclient player read as idle
# since login however much they typed; shutdownsock sent them down a
# branch that never called announce_disconnect, so no disconnect
# message or propqueue; WHO had no case for their descriptor type and
# printed "[Unknown]"; the frame parser decoded one frame per read and
# stranded the rest; and ping frames went unanswered.
#
# Usage: run_frob_ws_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, socket, base64, struct, json, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker

binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.chdir(gamedir)
check = Checker()

STORE = 'data/store'
shutil.rmtree(STORE, ignore_errors=True)
shutil.copy('minimal.db', 'live.db')

wwwport = None
for line in open('data/parmfile.cfg'):
    if line.startswith('wwwport='):
        wwwport = int(line.split('=', 1)[1])

sess = start(binpath, 'live.db', STORE, port)

# --- @frob ---
sess.cmd('@pcreate Victim=victimpw', 1.0)
out = sess.cmd('@sanity', 8.0)
check('sanity is clean before the frob', 'Object "' not in out,
      out.strip()[-200:])
sess.cmd('@frob Victim', 2.0)
out = sess.cmd('@sanity', 8.0)
check('sanity is still clean after the frob', 'Object "' not in out,
      out.strip()[-300:])

# --- websocket ---
sess.cmd('@tune www_root=#0', 0.8)
sess.cmd('@pcreate WsUser=wspw', 1.0)


def frame(text, opcode=1):
    p = text.encode()
    m = os.urandom(4)
    body = bytes(c ^ m[i % 4] for i, c in enumerate(p))
    n = len(p)
    hdr = bytes([0x80 | opcode])
    hdr += bytes([0x80 | n]) if n < 126 else \
        bytes([0x80 | 126]) + struct.pack('>H', n)
    return hdr + m + body


def cmd(text):
    return frame(json.dumps({"muck": {"command": text}}))


def idle_of(who, name):
    m = re.search(r'^%s\s+\S+\s+(\d+)s' % re.escape(name), who, re.M)
    return int(m.group(1)) if m else None


ws = socket.create_connection(('127.0.0.1', wwwport), timeout=10)
ws.sendall(("GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
            % base64.b64encode(os.urandom(16)).decode()).encode())
time.sleep(1.5)
check('websocket upgrade accepted', b'101' in ws.recv(65536), '')

ws.sendall(cmd('connect WsUser wspw'))
time.sleep(2.0)
who = sess.cmd('WHO', 1.5).replace('\r', '')
check('a websocket player is named in WHO, not "[Unknown]"',
      'WsUser' in who and '[Unknown]' not in who, who[-300:])

time.sleep(8)
before = idle_of(sess.cmd('WHO', 1.5).replace('\r', ''), 'WsUser')
ws.sendall(cmd('say ping-one'))
time.sleep(1.5)
after = idle_of(sess.cmd('WHO', 1.5).replace('\r', ''), 'WsUser')
check('a websocket command resets idle',
      before is not None and after is not None and after < before,
      'before=%r after=%r' % (before, after))

# two frames in one write: both must run, not just the first
ws.sendall(cmd('say batch-A') + cmd('say batch-B'))
time.sleep(2.0)
out = sess.cmd('look', 1.0)
check('both frames of one write are executed',
      'batch-A' in out and 'batch-B' in out, out[-300:])

# drain, then ping: the reply must be a pong carrying the same payload
ws.settimeout(1.5)
try:
    while ws.recv(65536):
        pass
except socket.timeout:
    pass
ws.settimeout(5)
ws.sendall(frame('hb', 9))
r = ws.recv(64)
check('a ping is answered with a pong',
      r[:1] == b'\x8a' and r[2:4] == b'hb', repr(r[:16]))

ws.close()
time.sleep(3)
out = sess.cmd('WHO', 1.5)
check('a websocket disconnect is announced',
      'WsUser has disconnected' in out, out[-200:])

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
