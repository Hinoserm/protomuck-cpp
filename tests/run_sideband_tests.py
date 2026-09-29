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


import wsclient
from wsclient import frame, sideband_of, text_of, is_sb


def WS(extra_headers=''):
    return wsclient.WS(wwwport, extra_headers)


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
    "Mort" pmatch descr_array 0 array_getitem "X" { }dict
    descr_sideband "sent"
  catch
  endcatch
  me @ "PERM:" rot strcat notify
;''', 'M3'),
    'Touch': (': main pop descr descr_unidle ;', 'W2'),
    'UnSelf': (': main pop descr descr_unidle me @ "UNSELF:ok" notify ;',
               'M3'),
    'UnOther': ('''
: main pop
  0 try
    "Mort" pmatch descr_array 0 array_getitem descr_unidle "done"
  catch
  endcatch
  me @ "UNI:" rot strcat notify
;''', 'M3'),
    'TouchMe': (': main pop descr descr_unidle me @ "TOUCHED" notify ;',
                'W2'),
}
refs = {}
for name, (src, lvl) in progs.items():
    refs[name] = make_prog(name, src, lvl)

for cmd, prog in (('Echo', 'Echo'), ('Order', 'Order'), ('Caught', 'Caught'),
                  ('JsonT', 'JsonT'), ('PlayerLogin', 'Login'),
                  ('Low', 'Low'), ('Touch', 'Touch')):
    sess.cmd('@set #0=_ws/%s:#%d' % (cmd, refs[prog]), 0.3)
# The permission rule is about M3, and every program the MAN character
# owns runs at the top level whatever its flags, so the two M3 programs
# that exercise it belong to a genuine M3 mortal instead.
sess.cmd('@pcreate Mort=mortpw', 1.0)
sess.cmd('@set *Mort=M3', 0.4)
for prog in ('SbSelf', 'SbOther', 'UnSelf', 'UnOther'):
    sess.cmd('@chown %s=*Mort' % prog, 0.4)
    sess.cmd('@set %s=M3' % prog, 0.3)
    sess.cmd('@set %s=L' % prog, 0.3)

for act, prog in (('waitread', 'ReadProg'), ('sbself', 'SbSelf'),
                  ('sbother', 'SbOther'), ('unself', 'UnSelf'),
                  ('unother', 'UnOther'), ('touchme', 'TouchMe')):
    sess.cmd('@act %s=#0' % act, 0.4)
    sess.cmd('@link %s=#%d' % (act, refs[prog]), 0.4)
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

# --- idle: packets never count; DESCR_UNIDLE does -------------------------


def idle_of(name):
    out = sess.cmd('WHO', 1.0).replace('\r', '')
    m = re.search(r'^%s\s+\S+\s+(\d+)s' % re.escape(name), out, re.M)
    return int(m.group(1)) if m else None


time.sleep(4)
before = idle_of('WsUser')
for _ in range(3):
    ws.sb('Echo', {})
    ws.wait(is_sb('Pong'))
    time.sleep(0.5)
after = idle_of('WsUser')
check('a sideband packet does not refresh idle',
      before is not None and after is not None and after >= before,
      'idle %r -> %r' % (before, after))
ws.sb('Touch', {})
time.sleep(1.5)
touched = idle_of('WsUser')
check('DESCR_UNIDLE in a handler counts as activity',
      touched is not None and after is not None and touched < after
      and touched <= 2, 'idle %r -> %r' % (after, touched))

# ...and on a telnet connection too, not just a websocket (typing the
# command resets telnet idle by itself, so this shows only that the
# prim accepts a telnet descriptor)
out = sess.cmd('touchme', 1.0)
check('DESCR_UNIDLE works on a telnet connection', 'TOUCHED' in out,
      out.strip()[-120:])

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
# Mort logs in on a second websocket; WsUser then runs an M3 program
# (owned by Mort, so genuinely M3) that tries to send to Mort's
# connection. It is neither the triggering descriptor nor one WsUser
# is logged in on, so it must be refused, and the error comes back to
# WsUser as text.
ws2 = WS()
ws2.collect(1.0)
ws2.sb('PlayerLogin', {"name": "Mort", "password": "mortpw"})
_, f = ws2.wait(is_sb('PlayerLogin.Result'))
check('a second player logs in on a second websocket',
      f is not None and sideband_of(f)['data'].get('ok') == 1, repr(f))
ws.cmd('sbother')
_, f = ws.wait(lambda f: 'PERM:' in (text_of(f) or ''))
check("an M3 program may not send to someone else's connection",
      f is not None and 'Permission denied' in text_of(f), repr(f))
got = ws2.collect(1.0)
check('and nothing reached the other connection',
      not any(is_sb('X')(g) for g in got), repr(got))
ws.cmd('unother')
_, f = ws.wait(lambda f: 'UNI:' in (text_of(f) or ''))
check("an M3 program may not DESCR_UNIDLE someone else's connection",
      f is not None and 'Permission denied' in text_of(f), repr(f))
ws.cmd('unself')
_, f = ws.wait(lambda f: 'UNSELF:' in (text_of(f) or ''))
check('an M3 program may DESCR_UNIDLE its own connection',
      f is not None and 'UNSELF:ok' in text_of(f), repr(f))
ws2.close()

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


# "=-" is how a string tune is cleared ("=" alone only displays it). The
# game directory's parmfile keeps tunes between runs, so this has to
# really clear the list a previous run left set.
out = sess.cmd('@tune web_trusted_proxies=-', 0.4)
check('the trusted-proxy list can be cleared', 'Parameter set' in out,
      out.strip()[-120:])
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
