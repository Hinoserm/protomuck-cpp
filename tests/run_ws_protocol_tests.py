#!/usr/bin/env python3
# The websocket layer against RFC 6455, and the rule that no protocol
# traffic ever counts as the player doing something.
#
#   Ping (0x9)   the server answers with a Pong carrying the same payload
#   Pong (0xA)   accepted as a reply or as a one-way heartbeat
#   Close (0x8)  the closing handshake: the peer's status code is echoed,
#                and the server sends its own Close, with a code, when
#                it drops a connection (1000 QUIT, 1001 keepalive timeout,
#                1002 protocol error, 1003 binary data, 1009 too large)
#
# Also: client frames must be masked, fragmented messages are
# reassembled (with control frames allowed between the fragments), the
# server pings on web_ws_ping_interval and drops a connection silent for
# three intervals, and none of the ping traffic unidles the player.
#
# Usage: run_ws_protocol_tests.py <binary> <gamedir> <port>
import sys, os, shutil, re, struct, json, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from muckharness import start, stop, Checker
import wsclient
from wsclient import frame, text_of, close_code

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


def WS(auto_pong=True):
    w = wsclient.WS(wwwport, auto_pong=auto_pong)
    w.collect(1.0)              # the welcome screen
    return w


def is_close(f):
    return f[0] == 8


def idle_of(name):
    out = sess.cmd('WHO', 1.0).replace('\r', '')
    m = re.search(r'^%s\s+\S+\s+(\d+)s' % re.escape(name), out, re.M)
    return int(m.group(1)) if m else None


sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune www_root=#0', 0.6)
sess.cmd('@pcreate WsUser=wspw', 1.0)

ws = WS()
check('websocket upgrade accepted', b'101' in ws.status, repr(ws.status))
ws.cmd('connect WsUser wspw')
_, f = ws.wait(lambda f: 'WsUser' in (text_of(f) or ''), 5)
time.sleep(1.0)

# --- Ping and Pong -------------------------------------------------------

ws.collect(0.5)
ws.send(frame(b'hb42', opcode=9))
_, f = ws.wait(lambda f: f[0] == 0xA)
check('a Ping is answered with a Pong carrying the same payload',
      f is not None and f[1] == b'hb42', repr(f))

ws.send(frame(b'', opcode=0xA))            # unsolicited: a heartbeat
ws.cmd('say still here')
time.sleep(1.0)
out = sess.cmd('look', 1.0)
check('an unsolicited Pong is accepted and the connection carries on',
      'still here' in out and not ws.closed, out.strip()[-160:])

# --- pings never unidle --------------------------------------------------

time.sleep(3)
before = idle_of('WsUser')
for _ in range(6):
    ws.send(frame(b'x', opcode=9))
    ws.send(frame(b'', opcode=0xA))
    time.sleep(1.0)
ws.collect(0.3)
after = idle_of('WsUser')
check('pings and pongs do not refresh idle',
      before is not None and after is not None and after >= before + 5,
      'idle %r -> %r' % (before, after))
ws.cmd('say typing')
time.sleep(1.5)
check('a typed line does', (idle_of('WsUser') or 99) <= 2,
      repr(idle_of('WsUser')))

# --- fragmented messages -------------------------------------------------

msg = json.dumps({"muck": {"command": "say fragmented hello"}}).encode()
ws.send(frame(msg[:10], opcode=1, fin=False))
ws.send(frame(msg[10:20], opcode=0, fin=False))
ws.send(frame(b'mid', opcode=9))           # a control frame in between
ws.send(frame(msg[20:], opcode=0, fin=True))
_, f = ws.wait(lambda f: f[0] == 0xA)
check('a Ping between fragments is answered', f is not None and f[1] == b'mid',
      repr(f))
time.sleep(1.0)
out = sess.cmd('look', 1.0)
check('a fragmented message is reassembled and runs',
      'fragmented hello' in out, out.strip()[-160:])

# --- the closing handshake ----------------------------------------------

c = WS()
c.send(frame(struct.pack('>H', 4000) + b'bye', opcode=8))
_, f = c.wait(is_close)
check("the peer's Close is answered with its status code echoed",
      f is not None and close_code(f) == 4000, repr(f))
c.collect(1.0)
check('and the connection is closed', c.closed, 'still open')
c.close()

c = WS()
c.send(frame(b'', opcode=8))
_, f = c.wait(is_close)
check('an empty Close is answered with an empty Close',
      f is not None and f[1] == b'', repr(f))
c.close()

# --- protocol violations: Close with the RFC status code ----------------

c = WS()
c.send(frame(json.dumps({"muck": {"command": "look"}}), mask=False))
_, f = c.wait(is_close)
check('an unmasked client frame is refused with 1002',
      f is not None and close_code(f) == 1002, repr(f))
c.close()

c = WS()
c.send(frame(b'x' * 200, opcode=9))
_, f = c.wait(is_close)
check('a control frame over 125 bytes is refused with 1002',
      f is not None and close_code(f) == 1002, repr(f))
c.close()

c = WS()
c.send(frame(b'\x00\x01', opcode=2))
_, f = c.wait(is_close)
check('a binary message is refused with 1003',
      f is not None and close_code(f) == 1003, repr(f))
c.close()

c = WS()
c.send(frame(b'x', opcode=0))
_, f = c.wait(is_close)
check('a continuation with nothing to continue is refused with 1002',
      f is not None and close_code(f) == 1002, repr(f))
c.close()

c = WS()
c.send(bytes([0x81, 0x80 | 127]) + struct.pack('>Q', 2 * 1024 * 1024)
       + os.urandom(4) + b'x' * 64)
_, f = c.wait(is_close)
check('a frame over json_max_len is refused with 1009 at its header',
      f is not None and close_code(f) == 1009, repr(f))
c.close()

# --- the server's own Close on QUIT -------------------------------------

sess.cmd('@pcreate Quitter=qpw', 1.0)
q = WS()
q.cmd('connect Quitter qpw')
q.collect(1.5)
q.cmd('QUIT')
got, f = q.wait(is_close)
check('QUIT ends with a Close 1000 from the server',
      f is not None and close_code(f) == 1000, repr(got[-3:]))
check('after the goodbye text, not before it',
      f is not None and any(text_of(g) for g in got[:-1]), repr(got[-3:]))
q.close()

# --- keepalive ------------------------------------------------------------

ws.close()

out = sess.cmd('@tune web_ws_ping_interval', 0.6)
check('web_ws_ping_interval is a time @tune defaulting to 10s',
      '0:00:10' in out, out.strip()[-160:])
out = sess.cmd('@tune web_ws_ping_interval=4s', 0.6)
check('it refuses anything under 5s', 'Parameter set' not in out,
      out.strip()[-160:])
out = sess.cmd('@tune web_ws_ping_interval', 0.6)
check('and keeps its previous value when refused', '0:00:10' in out,
      out.strip()[-160:])
out = sess.cmd('@tune web_ws_ping_interval=5s', 0.6)
check('5s is accepted', 'Parameter set' in out, out.strip()[-160:])

k = WS(auto_pong=False)
_, f = k.wait(lambda f: f[0] == 9, 8)
check('the server pings on web_ws_ping_interval', f is not None, 'no Ping')
# on schedule, even on a quiet server: select used to sleep up to 10s
# regardless, so a 5s interval really pinged about every 10s
t1 = time.time()
_, f = k.wait(lambda f: f[0] == 9, 9)
gap = time.time() - t1
check('pings keep the interval on a quiet server (about 5s apart)',
      f is not None and 3.5 <= gap <= 6.5, 'gap %.1fs' % gap)
# play dead: never answer, never send
got, f = k.wait(is_close, 25)
check('a connection silent for three intervals is closed with 1001',
      f is not None and close_code(f) == 1001, repr(got[-3:]))
k.close()

k = WS()                        # answers its pings, as a browser does
got = k.collect(20.0)           # well past three intervals
check('a connection that answers its pings is kept',
      not k.closed and not any(g[0] == 8 for g in got),
      repr([g[0] for g in got]))
k.close()
sess.cmd('@tune web_ws_ping_interval=10s', 0.5)

# --- server shutdown -------------------------------------------------------
# close_sockets used to write the shutdown message raw into every
# socket, which on a websocket lands outside any frame
sd = WS()
sd.cmd('connect WsUser wspw')
sd.collect(1.5)
stop(sess)
got = sd.collect(2.0)
check('the shutdown message reaches a websocket as a text frame',
      any(text_of(g) for g in got), repr(got[-3:]))
check('followed by Close 1001 (going away)',
      any(close_code(g) == 1001 for g in got), repr(got[-3:]))
sd.close()
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
