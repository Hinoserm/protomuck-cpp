#!/usr/bin/env python3
# The websocket sideband, end to end, with a real websocket client.
#
# Covers the protocol and every rule agreed for it: registration and
# its checks, pre-login dispatch as #-1, the data dictionary and
# command @, _error replies that do not reveal registrations, output
# ordering with text, READ bypass, DESCR_SIDEBAND raising errors that
# TRY/CATCH can catch, its own-connection permission rule, the JSON
# prims and their strict mapping, 7-bit input, the frame-size cap, and
# X-Forwarded-For from a trusted proxy only. Also the fixes that came
# up along the way: DESCR_SETUSER refusing a wrong password, the
# welcome screen at upgrade, websocket lines cut to MAX_COMMAND_LEN,
# and the 64-bit frame length.
#
# Usage: run_sideband_tests.py <binary> <gamedir> <port>
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


# ------------------------------------------------------------------
# a minimal websocket client
# ------------------------------------------------------------------

def frame(payload, opcode=1):
    """One masked client frame, using the 16- and 64-bit length forms
    when the payload needs them."""
    p = payload.encode('utf-8') if isinstance(payload, str) else payload
    m = os.urandom(4)
    body = bytes(c ^ m[i % 4] for i, c in enumerate(p))
    n = len(p)
    hdr = bytes([0x80 | opcode])
    if n < 126:
        hdr += bytes([0x80 | n])
    elif n <= 0xFFFF:
        hdr += bytes([0x80 | 126]) + struct.pack('>H', n)
    else:
        hdr += bytes([0x80 | 127]) + struct.pack('>Q', n)
    return hdr + m + body


class WS:
    def __init__(self, extra_headers=''):
        self.s = socket.create_connection(('127.0.0.1', wwwport), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /ws HTTP/1.1\r\nHost: h\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n%s\r\n"
                        % (key, extra_headers)).encode())
        self.buf = b''
        self.closed = False
        deadline = time.time() + 5
        while b'\r\n\r\n' not in self.buf and time.time() < deadline:
            self._read(0.5)
        head, _, self.buf = self.buf.partition(b'\r\n\r\n')
        self.status = head.split(b'\r\n', 1)[0]
        self.pending = []

    def _read(self, t):
        self.s.settimeout(t)
        try:
            chunk = self.s.recv(1 << 20)
            if not chunk:
                self.closed = True
            self.buf += chunk
        except socket.timeout:
            pass
        except OSError:
            self.closed = True

    def _parse(self):
        while len(self.buf) >= 2:
            op = self.buf[0] & 0x0F
            n = self.buf[1] & 0x7F
            i = 2
            if n == 126:
                if len(self.buf) < 4:
                    return
                n = struct.unpack('>H', self.buf[2:4])[0]
                i = 4
            elif n == 127:
                if len(self.buf) < 10:
                    return
                n = struct.unpack('>Q', self.buf[2:10])[0]
                i = 10
            if len(self.buf) < i + n:
                return
            self.pending.append((op, self.buf[i:i + n]))
            self.buf = self.buf[i + n:]

    def collect(self, seconds):
        """Every frame that arrives within the given time, in order."""
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            self._read(0.2)
            self._parse()
        self._parse()
        out, self.pending = self.pending, []
        return out

    def wait(self, pred, seconds=5):
        """Frames up to and including the first that satisfies pred."""
        got = []
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            self._read(0.2)
            self._parse()
            while self.pending:
                f = self.pending.pop(0)
                got.append(f)
                if pred(f):
                    return got, f
        return got, None

    def cmd(self, text):
        self.s.sendall(frame(json.dumps({"muck": {"command": text}})))

    def sb(self, cmd, data=None, raw=None):
        body = {"cmd": cmd}
        if data is not None:
            body["data"] = data
        self.s.sendall(frame(raw if raw is not None
                             else json.dumps({"muck": {"sideband": body}})))

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def sideband_of(f):
    op, payload = f
    if op != 1:
        return None
    try:
        return json.loads(payload)["muck"]["sideband"]
    except (ValueError, KeyError, TypeError):
        return None


def text_of(f):
    op, payload = f
    if op != 1:
        return None
    try:
        return json.loads(payload)["muck"]["text"]
    except (ValueError, KeyError, TypeError):
        return None


def is_sb(name):
    return lambda f: (sideband_of(f) or {}).get("cmd") == name


# ------------------------------------------------------------------
# setup: programs and registrations
# ------------------------------------------------------------------

sess = start(binpath, 'live.db', STORE, port)
sess.cmd('@tune www_root=#0', 0.6)


def make_prog(name, source, level):
    sess.cmd('@prog %s' % name, 0.6)
    sess.cmd('i', 0.2)
    for line in source.strip().split('\n'):
        sess.cmd(line, 0.05)
    sess.cmd('.', 0.2)
    out = sess.cmd('c', 1.0)
    sess.cmd('q', 0.4)
    sess.cmd('@set %s=%s' % (name, level), 0.3)
    sess.cmd('@set %s=L' % name, 0.3)
    m = re.search(r'#(\d+)', sess.cmd('ex %s' % name, 0.6))
    ref = int(m.group(1)) if m else None
    check('program %s compiled' % name, ref is not None and 'rror' not in out,
          out.strip()[-200:])
    return ref


progs = {
    'Echo': ('''
lvar data
: main data !
  descr "Pong" { "cmd" command @ "me" me @ int "data" data @ }dict
  descr_sideband
;''', 'W2'),
    'Order': ('''
: main pop
  descr "BEFORE" descrnotify
  descr "Mid" { "n" 1 }dict descr_sideband
  descr "AFTER" descrnotify
;''', 'W2'),
    'Caught': ('''
lvar msg
: main pop
  0 try
    999999 "X" { }dict descr_sideband
    "no error"
  catch
  endcatch
  msg !
  descr "Caught" { "msg" msg @ }dict descr_sideband
;''', 'W2'),
    'JsonT': ('''
lvar r
: main pop
  { }dict r !
  "{\\"a\\":[1,2.5,\\"x\\"],\\"b\\":true}" json_to_array array_to_json
  r @ "rt" array_setitem r !
  0 try "[null]" json_to_array pop "none" catch endcatch
  r @ "null" array_setitem r !
  0 try "123456789012345678901234567890" json_to_array pop "none"
  catch endcatch
  r @ "bigint" array_setitem r !
  0 try #0 array_to_json "none" catch endcatch
  r @ "dbref" array_setitem r !
  { 1 "one" }dict array_to_json r @ "intkey" array_setitem r !
  0 try { 1 "a" "1" "b" }dict array_to_json "none" catch endcatch
  r @ "collide" array_setitem r !
  descr "JsonResult" r @ descr_sideband
;''', 'W2'),
    'Login': ('''
lvar data
lvar who
: main data !
  data @ "name" array_getitem pmatch who !
  who @ ok? not if
    descr "PlayerLogin.Result" { "ok" 0 "error" "no such player" }dict
    descr_sideband exit
  then
  descr who @ data @ "password" array_getitem descr_setuser if
    descr "PlayerLogin.Result" { "ok" 1 }dict descr_sideband
  else
    descr "PlayerLogin.Result" { "ok" 0 "error" "bad password" }dict
    descr_sideband
  then
;''', 'W2'),
    'Low': (': main pop ;', 'M3'),
    'ReadProg': ('''
: main pop
  me @ "READING" notify
  read
  me @ "GOT:" rot strcat notify
;''', 'M3'),
    'SbSelf': (': main pop descr "Self" { "ok" 1 }dict descr_sideband ;',
               'M3'),
    'SbOther': ('''
: main pop
  0 try
    "WsUser" pmatch descr_array 0 array_getitem "X" { }dict
    descr_sideband "sent"
  catch
  endcatch
  me @ "_perm" rot setprop
;''', 'M3'),
}
refs = {}
for name, (src, lvl) in progs.items():
    refs[name] = make_prog(name, src, lvl)

for cmd, prog in (('Echo', 'Echo'), ('Order', 'Order'), ('Caught', 'Caught'),
                  ('JsonT', 'JsonT'), ('PlayerLogin', 'Login'),
                  ('Low', 'Low')):
    sess.cmd('@set #0=_ws/%s:#%d' % (cmd, refs[prog]), 0.3)
# The permission rule is about M3, and every program the MAN character
# owns runs at the top level whatever its flags, so the two M3 programs
# that exercise it belong to a genuine M3 mortal instead.
sess.cmd('@pcreate Mort=mortpw', 1.0)
sess.cmd('@set *Mort=M3', 0.4)
for prog in ('SbSelf', 'SbOther'):
    sess.cmd('@chown %s=*Mort' % prog, 0.4)
    sess.cmd('@set %s=M3' % prog, 0.3)
    sess.cmd('@set %s=L' % prog, 0.3)

for act, prog in (('waitread', 'ReadProg'), ('sbself', 'SbSelf')):
    sess.cmd('@act %s=#0' % act, 0.4)
    sess.cmd('@link %s=#%d' % (act, refs[prog]), 0.4)
sess.cmd('@act sbother=*Mort', 0.4)
sess.cmd('@link sbother=#%d' % refs['SbOther'], 0.4)
sess.cmd('@pcreate WsUser=wspw', 1.0)

# ------------------------------------------------------------------
# before login
# ------------------------------------------------------------------

ws = WS()
check('websocket upgrade accepted', b'101' in ws.status, repr(ws.status))
first = ws.collect(2.0)
check('the welcome screen arrives at upgrade, before anything is sent',
      any(text_of(f) for f in first), repr(first[:2]))

ws.sb('Echo', {"x": 1, "s": "hi"})
_, f = ws.wait(is_sb('Pong'))
pong = (sideband_of(f) or {}).get('data', {}) if f else {}
check('a registered packet runs before login',
      f is not None, 'no Pong')
check('command @ is the name the client sent', pong.get('cmd') == 'Echo',
      repr(pong))
check('me @ is #-1 before login', pong.get('me') == -1, repr(pong))
check('the data object arrives as a dictionary',
      pong.get('data') == {"x": 1, "s": "hi"}, repr(pong))

ws.sb('echo', {})
_, f = ws.wait(is_sb('Pong'))
check('names match case-insensitively; command @ keeps the client spelling',
      f is not None and sideband_of(f)['data']['cmd'] == 'echo', repr(f))


def expect_error(label, send, want):
    send()
    _, f = ws.wait(is_sb('_error'))
    err = (sideband_of(f) or {}).get('data', {}) if f else {}
    check(label, want in err.get('error', ''), repr(err))


expect_error('an unregistered name is "unknown command"',
             lambda: ws.sb('Nope', {}), 'unknown command')
expect_error('a registered program below W1 is refused the same way',
             lambda: ws.sb('Low', {}), 'unknown command')
expect_error('data that is not an object is refused',
             lambda: ws.sb('Echo', [1, 2]), 'data must be a JSON object')
expect_error('null in the data is refused with its path',
             lambda: ws.sb('Echo', {"v": None}),
             '$.v: null has no MUF equivalent')
expect_error('a reserved name is refused',
             lambda: ws.sb('_secret', {}), 'invalid command name')

# ------------------------------------------------------------------
# PlayerLogin, and DESCR_SETUSER refusing a wrong password
# ------------------------------------------------------------------

ws.sb('PlayerLogin', {"name": "WsUser", "password": "wrong"})
_, f = ws.wait(is_sb('PlayerLogin.Result'))
res = (sideband_of(f) or {}).get('data', {}) if f else {}
check('a wrong password is refused', res.get('ok') == 0, repr(res))
who = sess.cmd('WHO', 1.0)
check('and the descriptor is NOT logged in', 'WsUser' not in who,
      who[-300:])

ws.sb('PlayerLogin', {"name": "WsUser", "password": "wspw"})
_, f = ws.wait(is_sb('PlayerLogin.Result'))
res = (sideband_of(f) or {}).get('data', {}) if f else {}
check('the right password logs in', res.get('ok') == 1, repr(res))
who = sess.cmd('WHO', 1.0)
check('and WHO shows the player', 'WsUser' in who, who[-300:])

m = re.search(r'#(\d+)', sess.cmd('ex *WsUser', 0.8))
wsuser = int(m.group(1)) if m else None
ws.sb('Echo', {})
_, f = ws.wait(is_sb('Pong'))
check('after login me @ is the player',
      f is not None and sideband_of(f)['data']['me'] == wsuser,
      repr(f))

# ------------------------------------------------------------------
# output ordering, errors, JSON prims
# ------------------------------------------------------------------

ws.collect(0.5)
ws.sb('Order', {})
got, _ = ws.wait(lambda f: 'AFTER' in (text_of(f) or ''))
seq = []
for f in got:
    t = text_of(f)
    if t and 'BEFORE' in t:
        seq.append('BEFORE')
    if sideband_of(f) and sideband_of(f)['cmd'] == 'Mid':
        seq.append('Mid')
    if t and 'AFTER' in t:
        seq.append('AFTER')
check('text and sideband go out in the order they were produced',
      seq == ['BEFORE', 'Mid', 'AFTER'], repr(seq))

ws.sb('Caught', {})
_, f = ws.wait(is_sb('Caught'))
msg = (sideband_of(f) or {}).get('data', {}).get('msg', '') if f else ''
check('DESCR_SIDEBAND raises an error that TRY/CATCH catches',
      'not a valid descriptor' in msg, repr(msg))

ws.sb('JsonT', {})
_, f = ws.wait(is_sb('JsonResult'))
jr = (sideband_of(f) or {}).get('data', {}) if f else {}
check('JSON round trip, with true becoming 1',
      jr.get('rt') == '{"a":[1,2.5,"x"],"b":1}', repr(jr.get('rt')))
check('JSON_TO_ARRAY refuses null',
      'null has no MUF equivalent' in jr.get('null', ''), repr(jr.get('null')))
check('JSON_TO_ARRAY refuses an integer too wide for MUF',
      'does not fit a MUF integer' in jr.get('bigint', ''),
      repr(jr.get('bigint')))
check('ARRAY_TO_JSON refuses a dbref',
      'a dbref has no JSON equivalent' in jr.get('dbref', ''),
      repr(jr.get('dbref')))
check('ARRAY_TO_JSON turns an integer key into a string',
      jr.get('intkey') == '{"1":"one"}', repr(jr.get('intkey')))
check('ARRAY_TO_JSON refuses keys 1 and "1" colliding',
      'would both become' in jr.get('collide', ''), repr(jr.get('collide')))

# ------------------------------------------------------------------
# READ bypass and the permission rule
# ------------------------------------------------------------------

ws.cmd('waitread')
_, f = ws.wait(lambda f: 'READING' in (text_of(f) or ''))
check('a foreground READ is waiting', f is not None, 'no READING')
ws.sb('Echo', {})
_, f = ws.wait(is_sb('Pong'))
check('a sideband packet runs while a READ waits', f is not None, 'no Pong')
ws.cmd('typed answer')
_, f = ws.wait(lambda f: 'GOT:' in (text_of(f) or ''))
check('and the READ still gets the next typed line, not the packet',
      f is not None and 'GOT:typed answer' in text_of(f), repr(f))

ws.cmd('sbself')
_, f = ws.wait(is_sb('Self'))
check('an M3 program may send to its own connection', f is not None,
      'no Self')
# run as Mort (offline, so the result is kept on Mort, not notified)
sess.cmd('@force Mort=sbother', 1.5)
out = sess.cmd('ex *Mort=_perm', 1.0)
check("an M3 program may not send to someone else's connection",
      'Permission denied' in out, out.strip()[-200:])

# ------------------------------------------------------------------
# 7-bit input, long lines, the 64-bit length, the frame cap
# ------------------------------------------------------------------

ws.cmd('say café ✓')
time.sleep(1.0)
out = sess.cmd('look', 1.0)
check('non-ASCII input becomes one ? per character',
      'caf? ?' in out, out.strip()[-200:])

ws.cmd('say ' + 'x' * 70000)       # > 64KB: the 64-bit length form
time.sleep(1.5)
out = sess.cmd('WHO', 1.0)
check('a 70KB line is cut, not a stack overflow; the server is alive',
      'WsUser' in out, out[-200:])

big = WS()
big.collect(1.0)
big.s.sendall(bytes([0x81, 0x80 | 127]) + struct.pack('>Q', 2 * 1024 * 1024)
              + os.urandom(4) + b'x' * 64)
big.collect(3.0)
check('a frame over json_max_len drops the connection at its header',
      big.closed, 'still open')
big.close()

# ------------------------------------------------------------------
# X-Forwarded-For
# ------------------------------------------------------------------



def wizwho():
    """both wizard WHO formats: each ! after WHO cycles to the next,
    and which one carries the host column depends on mortalwho"""
    return sess.cmd('WHO !', 1.0) + sess.cmd('WHO !!', 1.0)


sess.cmd('@tune web_trusted_proxies=', 0.4)
untrusted = WS('X-Forwarded-For: 203.0.113.9\r\n')
untrusted.collect(1.0)
out = wizwho()
check('X-Forwarded-For is ignored when no proxy is trusted',
      '203.0.113.9' not in out, out[-300:])
untrusted.close()
time.sleep(1.0)

sess.cmd('@tune web_trusted_proxies=10.9.9.9, 127.0.0.1', 0.4)
trusted = WS('X-Forwarded-For: 198.51.100.7, 203.0.113.9\r\n')
trusted.collect(1.0)
out = wizwho()
check('from a trusted proxy, the right-most untrusted hop is the client',
      '203.0.113.9' in out and '198.51.100.7' not in out, out[-300:])
trusted.close()

ws.close()
stop(sess)
shutil.rmtree(STORE, ignore_errors=True)
sys.exit(check.result())
