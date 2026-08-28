#!/usr/bin/env python3
# Uploads tests/prop_bench.muf to a server on a minimal db and prints
# the BENCH: lines. Usage:
#   run_muf_bench.py <path-to-protomuck-binary> <game-dir> <port>
# Same game-dir contract as run_muf_tests.py.
import socket, sys, time, select, os, subprocess, shutil


def main():
    binpath, gamedir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    os.chdir(gamedir)
    shutil.copy('minimal.db', 'live.db')
    srv = subprocess.Popen([binpath, 'live.db', 'out.db', str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.setblocking(False)
    buf = b''

    def pump(dur=0.05):
        nonlocal buf
        r, _, _ = select.select([s], [], [], dur)
        if r:
            try:
                d = s.recv(1 << 20)
                if d:
                    buf += d
            except BlockingIOError:
                pass

    def wait_for(marker, timeout=60.0):
        end = time.time() + timeout
        while time.time() < end:
            if marker in buf:
                return True
            pump(0.05)
        return False

    def sendl(line):
        s.sendall(line.encode('latin-1') + b'\r\n')

    sendl('connect One potrzebie')
    wait_for(b'Room Zero', 5)
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, 'prop_bench.muf'),
               encoding='latin-1').read()
    sendl('@prog bench.muf')
    sendl('i')
    for line in src.split('\n'):
        sendl(line)
    sendl('.')
    sendl('c')
    sendl('q')
    if not wait_for(b'Compiler done'):
        print('FAIL: bench did not compile')
        srv.kill()
        return 1
    sendl('@set bench.muf=W')
    sendl('@act bench=me')
    sendl('@link bench=bench.muf')
    sendl('bench')
    wait_for(b'BENCH:done:1', 600)
    pump(0.5)
    try:
        os.kill(int(open('protomuck.pid').read().strip()), 9)
    except Exception:
        pass
    srv.kill()

    for line in buf.decode('latin-1', 'replace').splitlines():
        if 'BENCH:' in line:
            print(line[line.index('BENCH:'):])
    return 0


if __name__ == '__main__':
    sys.exit(main())
