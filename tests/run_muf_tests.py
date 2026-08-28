#!/usr/bin/env python3
# Runs tests/string_prims.muf against a protomuck server on a minimal db
# and checks each TAG:value line. Usage:
#   run_muf_tests.py <path-to-protomuck-binary> <game-dir> <port>
# game-dir needs data/ (with parmfile pinned to <port>) and a minimal db
# at minimal.db. Exits nonzero on any failure.
import socket, sys, time, select, os, subprocess, shutil

ARRAY_EXPECT = [
    'AMAKE:x-y', 'AGETI:v2', 'AEXPL:b', 'AVALS:a-b', 'AKEYS:k',
    'ACOUNT:3', 'AFIRST:0', 'ALAST:1', 'ANEXT:1', 'APREV:0',
    'ASETI:x-z', 'AINSI:x-z-y', 'AAPPI:x-z', 'AGETR:b-c',
    'ASETR:a-X-c', 'AINSR:a-X-b', 'ADELI:a-c', 'ADELR:a-d',
    'ANUNI:a-b-c', 'ANINT:b', 'ANDIF:e-f', 'AUNIO:a-b-c', 'AINTE:b',
    'ADIFF:e-f', 'AREVE:b-a', 'ASORT:a-b-c', 'ASRTI:a', 'APROPV:v1',
    'APROPD:a', 'APROPL:l1-l2', 'AREFL:#1-#0', 'AFIND:0-2', 'AEXCL:1',
    'AEXPA:a-b', 'ACUT:a|b-c', 'ACOMP:0', 'AINTR:q5', 'AFFLG:1',
    'ANGET:qux', 'ANSET:q', 'ANDEL:1', 'ASUM:6', 'AFRAG:Gre-eti-ngs',
    'APRAR:1', 'AFPRP:1', 'AFSMT:1', 'ARGMK:1', 'ARGMV:1', 'ARGSB:aXc',
    'ARGFP:1', 'ADESC:1', 'AONLN:1', 'ANOTF:ok', 'AANSI:ok',
    'ADONE:ok',
]

PROP_EXPECT = [
    'PSTR:hello', 'PINT:42', 'PFLT:3.5', 'PREF:1', 'PLOK:1',
    'POVER:7', 'POVERSTR:7.', 'PDIRVAL:topval', 'PDIRCHILD:childval',
    'PDIRP:1', 'PDIRP2:0', 'PDEEP:deep', 'PDEEPDIR:1',
    'PORD:.d|2n|_u|A|b|C1|c2|x y|~t|',
    'PCASE:one', 'PCASENAME:MiXeD', 'PCASE2:two',
    'PRMDIR:0', 'PRMDEEP:gone', 'PZERO:keep', 'PEMPTY:keep',
    'PADD:apval', 'PADDV:99', 'PENV:fromzero1',
    'PBIG137:v137', 'PBIGN:200', 'PMPI:plain-mpi', 'PI64:1',
    'PDONE:ok',
]

EXPECT = [
    'NUMBERP:1', 'STRINGCMP:0', 'STRCMP:0', 'STRNCMP:0', 'STRLEN:4',
    'STRCAT:concat', 'ATOI:42', 'ANSINOTIFY:ok', 'INTOSTR:7',
    'EXPLODE:ba2', 'SUBST:xa', 'INSTR:2', 'RINSTR:4', 'PRONOUN:One!',
    'TOUPPER:MIXED', 'TOLOWER:mixed', 'UNPARSE:Room Zero',
    'SMATCH:1', 'STRIPLEAD:x', 'STRIPTAIL:xy', 'STRINGPFX:1',
    'CRYPT:secret', 'HTMLDONE:ok', 'MIDSTR:bcd', 'CTOI:65', 'ITOC:B',
    'STOD:#5', 'SPLIT:ab', 'RSPLIT:a-bc', 'TOKEN:abcd:', 'FMT:x5',
    'PARSEANSI:hi', 'UNPARSEANSI:hi', 'ESCAPE:^^RED^^x', 'ANSILEN:2',
    'ANSICUT:ab', 'ANSIMID:bc', 'TEXTATTR:2', 'PNEON:ok',
    'DESCRNOTIFY:ok', 'ANSIDESCR:ok', 'NEXCL:ok', 'F2C:L', 'NDC:done',
    'AFMT:v', 'AUNPARSE:Room Zero', 'ANSINAME:One', 'B64E:aGk=',
    'B64D:hi', 'S2H:4142', 'H2S:AB', 'H2B:QUI=',
]

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

    def wait_for(marker, timeout=15.0):
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
    for name in ('strtest', 'arrtest', 'proptest'):
        src = open(os.path.join(here, {'strtest': 'string_prims.muf',
                                       'arrtest': 'array_prims.muf',
                                       'proptest': 'prop_prims.muf'}[name]),
                   encoding='latin-1').read()
        sendl('@prog %s.muf' % name)
        sendl('i')
        for line in src.split('\n'):
            sendl(line)
        sendl('.')
        sendl('c')
        sendl('q')
        if not wait_for(('%s.muf' % name).encode() and b'Compiler done'):
            print('FAIL: %s did not compile' % name)
            srv.kill()
            return 1
        sendl('@set %s.muf=L' % name)
        sendl('@act %s=me' % name)
        sendl('@link %s=%s.muf' % (name, name))
        sendl(name)
        sendl('say DONE-%s' % name)
        wait_for(('DONE-%s' % name).encode())
    pump(0.5)
    try:
        pidf = open('protomuck.pid').read().strip()
        os.kill(int(pidf), 9)
    except Exception:
        pass
    srv.kill()

    text = buf.decode('latin-1', 'replace')
    open(os.path.join(gamedir, 'muftest_capture.txt'), 'w', encoding='latin-1').write(text)
    fails = 0
    for want in EXPECT + ARRAY_EXPECT + PROP_EXPECT:
        if want in text:
            print('PASS', want)
        else:
            print('FAIL', want)
            fails += 1
    total = len(EXPECT) + len(ARRAY_EXPECT) + len(PROP_EXPECT)
    print('%d/%d passed' % (total - fails, total))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
