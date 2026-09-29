# A minimal websocket client for the test suites, able to send exactly
# what a test needs: masked or unmasked frames, fragments, and control
# frames, and to read back every frame the server sends in order.
import os, socket, base64, struct, json, time


def frame(payload, opcode=1, fin=True, mask=True):
    """One client frame. Clients must mask (RFC 6455 5.1); mask=False
    exists to test that the server refuses an unmasked one."""
    p = payload.encode('utf-8') if isinstance(payload, str) else payload
    n = len(p)
    hdr = bytes([(0x80 if fin else 0) | opcode])
    mbit = 0x80 if mask else 0
    if n < 126:
        hdr += bytes([mbit | n])
    elif n <= 0xFFFF:
        hdr += bytes([mbit | 126]) + struct.pack('>H', n)
    else:
        hdr += bytes([mbit | 127]) + struct.pack('>Q', n)
    if not mask:
        return hdr + p
    m = os.urandom(4)
    return hdr + m + bytes(c ^ m[i % 4] for i, c in enumerate(p))


class WS:
    def __init__(self, port, extra_headers=''):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /ws HTTP/1.1\r\nHost: h\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n%s\r\n"
                        % (key, extra_headers)).encode())
        self.buf = b''
        self.closed = False
        self.pending = []
        deadline = time.time() + 5
        while b'\r\n\r\n' not in self.buf and time.time() < deadline:
            self._read(0.5)
        head, _, self.buf = self.buf.partition(b'\r\n\r\n')
        self.status = head.split(b'\r\n', 1)[0]

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
        while time.time() < end:
            self._read(0.2)
            self._parse()
            while self.pending:
                f = self.pending.pop(0)
                got.append(f)
                if pred(f):
                    return got, f
            if self.closed:
                break
        return got, None

    def send(self, data):
        try:
            self.s.sendall(data)
        except OSError:
            self.closed = True

    def cmd(self, text):
        self.send(frame(json.dumps({"muck": {"command": text}})))

    def sb(self, cmd, data=None, raw=None):
        body = {"cmd": cmd}
        if data is not None:
            body["data"] = data
        self.send(frame(raw if raw is not None
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


def close_code(f):
    """The status code of a Close frame, or None."""
    op, payload = f
    if op != 8 or len(payload) < 2:
        return None
    return struct.unpack('>H', payload[:2])[0]
