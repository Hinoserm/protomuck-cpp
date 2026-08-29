"""Shared test harness for driving a live server.

Two things here are load-bearing, and both were learned the hard way
because getting either wrong makes a test pass while proving nothing:

  1. The server DETACHES. The process Python spawns forks and exits
     immediately, so subprocess.Popen's handle refers to a corpse:
     poll() returns 0, wait() returns at once, and kill() kills
     nothing. The real server's pid is in protomuck.pid, and that is
     the only handle worth holding.

  2. "@shutdown muck=muck" does NOT shut anything down. do_shutdown
     compares its argument against tp_muckname, which is "ProtoMUCK",
     so that command is answered with a usage message and ignored.

Together those meant a test could "restart" the server, fail to bind
the port because the original was still listening, silently connect
back to the ORIGINAL still-running process, and then assert happily
about state that had never been through a save/load cycle at all.
stop() below waits for the pid to actually disappear and start()
refuses to proceed while the port is still held.
"""

import os
import re
import select
import signal
import socket
import subprocess
import time


class Session:
    def __init__(self, port):
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
        try:
            self.s.sendall(line.encode('latin-1') + b'\r\n')
        except (BrokenPipeError, OSError):
            return ''
        self.pump(d)
        return re.sub(r'\x1b\[[0-9;]*m', '', self.buf[mark:].decode('latin-1', 'replace'))

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def _port_busy(port):
    probe = socket.socket()
    try:
        probe.settimeout(0.3)
        probe.connect(('127.0.0.1', port))
        return True
    except OSError:
        return False
    finally:
        probe.close()


def server_pid(gamedir='.'):
    path = os.path.join(gamedir, 'protomuck.pid')
    try:
        return int(open(path).read().strip())
    except (OSError, ValueError):
        return None


def _alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def start(binpath, infile, store, port, login='connect One potrzebie'):
    """Launch a server and connect. Refuses to start over a live one."""
    deadline = time.time() + 20
    while _port_busy(port) and time.time() < deadline:
        time.sleep(0.25)
    if _port_busy(port):
        raise RuntimeError('port %d still held; a previous server did not '
                           'die, and connecting now would silently talk to '
                           'it instead of the new one' % port)

    # -port, not a positional: the legacy positional-port parser eats
    # the FIRST port argument as a bare "enable sockets" toggle and
    # binds the default port instead, so a single positional port is
    # silently ignored.
    subprocess.Popen([binpath, '-port', str(port), infile, store],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # generous: sanitizer builds boot an order of magnitude slower
    deadline = time.time() + 120
    while not _port_busy(port) and time.time() < deadline:
        time.sleep(0.25)
    if not _port_busy(port):
        raise RuntimeError('server never came up on port %d' % port)

    time.sleep(0.5)
    sess = Session(port)
    sess.pump(1.0)
    if login:
        sess.cmd(login, 1.5)
    return sess


def stop(sess, gamedir='.', timeout=40):
    """Shut the server down and WAIT for the process to be gone."""
    pid = server_pid(gamedir)
    if sess:
        # the real name, not the "muck=muck" that only prints usage
        sess.cmd('@shutdown ProtoMUCK=now', 2.0)
        sess.close()

    if not pid:
        return

    deadline = time.time() + timeout
    while _alive(pid) and time.time() < deadline:
        time.sleep(0.25)
    if _alive(pid):
        os.kill(pid, signal.SIGTERM)
        deadline = time.time() + 10
        while _alive(pid) and time.time() < deadline:
            time.sleep(0.25)
    if _alive(pid):
        os.kill(pid, signal.SIGKILL)
        time.sleep(0.5)


class Checker:
    def __init__(self):
        self.fails = []

    def __call__(self, name, cond, detail=''):
        print(('PASS ' if cond else 'FAIL ') + name)
        if not cond:
            self.fails.append(name)
            if detail:
                print('  detail: ' + str(detail).strip()[:400])

    def result(self):
        print('RESULT: %d failures' % len(self.fails))
        return 1 if self.fails else 0
