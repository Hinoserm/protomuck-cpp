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

# --- the pre-login propqueues fire for websockets ---
# A telnet login screen fires _login when it connects and _disclogin
# when it leaves without logging in. A websocket did neither: _login
# was never called at upgrade, and _disclogin was skipped for all of
# CT_HTTP. _login must also run AFTER the upgrade, or its output lands
# as raw text inside the handshake.


def make_prog(name, source):
    sess.cmd('@prog %s' % name, 0.6)
    sess.cmd('i', 0.2)
    for line in source.split('\n'):
        sess.cmd(line, 0.1)
    sess.cmd('.', 0.2)
    sess.cmd('c', 0.8)
    sess.cmd('q', 0.4)
    sess.cmd('@set %s=W' % name, 0.3)
    sess.cmd('@set %s=L' % name, 0.3)
    m = re.search(r'#(\d+)', sess.cmd('ex %s' % name, 0.6))
    return int(m.group(1)) if m else None


login_prog = make_prog('WsLoginHook',
                       ': main pop descr "LOGIN-HOOK-FIRED" descrnotify ;')
disc_prog = make_prog('WsDiscHook',
                      ': main pop #0 "_test/disclogin" over over getpropval'
                      ' 1 + setprop ;')
check('pre-login hook programs created',
      login_prog is not None and disc_prog is not None,
      repr((login_prog, disc_prog)))
sess.cmd('@set #0=_login:#%d' % login_prog, 0.4)
sess.cmd('@set #0=_disclogin:#%d' % disc_prog, 0.4)

pre = socket.create_connection(('127.0.0.1', wwwport), timeout=10)
pre.sendall(("GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n"
             % base64.b64encode(os.urandom(16)).decode()).encode())
got = b''
pre.settimeout(1.5)
deadline = time.time() + 6
while time.time() < deadline and b'LOGIN-HOOK-FIRED' not in got:
    try:
        chunk = pre.recv(65536)
        if not chunk:
            break
        got += chunk
    except socket.timeout:
        pass
head, _, frames = got.partition(b'\r\n\r\n')
check('websocket upgrade completes before any _login output',
      head.startswith(b'HTTP/1.1 101') and b'LOGIN-HOOK-FIRED' not in head,
      repr(head[:120]))
check('_login fires for a websocket and reaches it as a frame',
      b'LOGIN-HOOK-FIRED' in frames and frames[:1] == b'\x81',
      repr(got[:200]))

pre.close()
time.sleep(4)
m = re.search(r'_test/disclogin:(\d+)', sess.cmd('ex #0=_test/', 1.0))
fired = int(m.group(1)) if m else 0
check('_disclogin fires exactly once when a login-screen websocket leaves',
      fired == 1, 'fired %d times' % fired)

sess.cmd('@set #0=_login:', 0.4)
sess.cmd('@set #0=_disclogin:', 0.4)

# --- a websocket left at the login screen follows connidle ---
# Telnet login screens are dropped after connidle; websockets were
# exempted along with the rest of CT_HTTP and lived forever. 30s is
# the smallest value the reaper honors.
sess.cmd('@tune connidle=30s', 0.8)
lurker = socket.create_connection(('127.0.0.1', wwwport), timeout=10)
lurker.sendall(("GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
                % base64.b64encode(os.urandom(16)).decode()).encode())
time.sleep(1.5)
check('login-screen websocket upgraded', b'101' in lurker.recv(65536), '')

time.sleep(45)
closed = False
lurker.settimeout(5)
try:
    while True:
        chunk = lurker.recv(65536)
        if not chunk:
            closed = True
            break
except socket.timeout:
    pass                        # still open: the failure being tested for
except OSError:
    closed = True               # reset counts as dropped too
check('an idle login-screen websocket is dropped after connidle',
      closed, 'socket still open after 45s')
lurker.close()

stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
