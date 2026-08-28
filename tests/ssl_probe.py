import socket, time, select, sys
port = int(sys.argv[1])
s = socket.create_connection(('127.0.0.1', port), timeout=10)
s.setblocking(False)
buf = b''
def pump(d=0.3):
    global buf
    e = time.time() + d
    while time.time() < e:
        r, _, _ = select.select([s], [], [], 0.05)
        if r:
            try:
                x = s.recv(65536)
                if x: buf += x
            except BlockingIOError: pass
def sendl(l):
    s.sendall(l.encode('latin-1') + b'\r\n'); pump(0.25)

sendl('connect One potrzebie'); pump(1)

# Fetch https://example.com over MUF SSL: nbsockopen, sockcheck loop,
# socksecure loop, socksend, nbsockrecv. Reports each stage.
prog = r''': main
  "example.com" 443 nbsockopen pop var! sk
  0 var! ok
  1 20 1 for pop
    sk @ sockcheck dup 1 = if pop 1 ok ! break then
    -1 = if 0 ok ! break then
    1 sleep
  repeat
  ok @ not if me @ "STAGE:connect-failed" notify exit then
  me @ "STAGE:connected" notify
  -1 var! sres
  1 30 1 for pop
    sk @ socksecure dup sres !
    dup 0 = if pop break then
    dup 2 = over 3 = or if pop 1 sleep continue then
    pop break
  repeat
  me @ "SECURE:" sres @ intostr strcat notify
  sres @ 0 = if
    sk @ "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n" socksend pop
    "" var! acc
    1 40 1 for pop
      sk @ nbsockrecv swap pop
      dup "" strcmp if acc @ swap strcat acc ! else pop then
      acc @ strlen 200 > if break then
      1 sleep
    repeat
    me @ "GOT:" acc @ 15 strcut pop strcat notify
  then
  sk @ 2 sockshutdown pop
  "example.com" 443 ssl_sockopen
  "ONESHOT:" swap strcat me @ swap notify
  dup socket? if 2 sockshutdown pop else pop then
  me @ "STAGE:done" notify
;'''
sendl('@prog sslt.muf'); sendl('i')
for l in prog.split('\n'): sendl(l)
sendl('.'); sendl('c'); sendl('q')
sendl('@set sslt.muf=L'); sendl('@act sslt=me'); sendl('@link sslt=sslt.muf')
sendl('sslt')
for _ in range(40):
    pump(1.0)
    if b'STAGE:done' in buf or b'STAGE:connect-failed' in buf: break
out = buf.decode('latin-1', 'replace')
for line in out.split('\n'):
    l = line.strip()
    if l.startswith(('STAGE:', 'SECURE:', 'GOT:', 'ONESHOT:')) or 'Error' in l:
        print(l)
