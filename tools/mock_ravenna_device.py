import socket,struct,hashlib,base64,json,threading,sys,time
dump=json.load(open('/tmp/settings_dump.json'))
mods=[m for m in dump['_modules'] if m['id'] in (154,155,156,157,158)]
dump={'_modules':mods}
LOG=open('/tmp/rt/mock.log','w')
def frame(op,data,mask=False):
    b=bytes([0x80|op]); n=len(data)
    if n<126: b+=bytes([n])
    elif n<65536: b+=bytes([126])+struct.pack('>H',n)
    else: b+=bytes([127])+struct.pack('>Q',n)
    return b+data
def readn(c,n):
    d=b''
    while len(d)<n:
        x=c.recv(n-len(d))
        if not x: raise EOFError
        d+=x
    return d
def readframe(c):
    h=readn(c,2); op=h[0]&15; ln=h[1]&127
    if ln==126: ln=struct.unpack('>H',readn(c,2))[0]
    elif ln==127: ln=struct.unpack('>Q',readn(c,8))[0]
    m=readn(c,4) if h[1]&128 else None
    p=readn(c,ln)
    if m: p=bytes(p[i]^m[i%4] for i in range(ln))
    return op,p
def handle(c):
    req=b''
    while b'\r\n\r\n' not in req: req+=c.recv(1024)
    key=[l.split(b': ')[1] for l in req.split(b'\r\n') if l.lower().startswith(b'sec-websocket-key')][0]
    acc=base64.b64encode(hashlib.sha1(key+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
    c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+acc+b'\r\n\r\n')
    send=lambda o: c.sendall(frame(1,json.dumps(o,separators=(',',':')).encode()))
    try:
        while True:
            op,p=readframe(c)
            if op==8: break
            if op!=1: continue
            for m in json.loads(p):
                ch=m['channel']
                if ch=='/meta/handshake':
                    send([{"channel":"/meta/handshake","successful":True,"clientId":"0xAAA"}])
                    send([{"id":m['id'],"channel":"/meta/handshake","successful":True,"clientId":"0xBBB"}])
                elif ch=='/meta/connect':
                    time.sleep(0.05); send([{"id":m['id'],"clientId":"0xBBB","channel":"/meta/connect","successful":True}])
                elif ch=='/meta/subscribe':
                    send([{"id":m['id'],"channel":"/meta/subscribe","successful":True}])
                elif ch=='/service/ravenna/commands':
                    send([{"id":m['id'],"channel":ch,"successful":True}])
                    send([{"channel":"/ravenna/settings","data":{"path":"$","value":dump}}])
                elif ch=='/service/ravenna/settings':
                    LOG.write(json.dumps(m['data'])+'\n'); LOG.flush()
                    send([{"id":m['id'],"channel":ch,"successful":True}])
                    send([{"channel":"/ravenna/settings","data":m['data']}])
    except Exception as e: pass
    c.close()
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(('127.0.0.1',int(sys.argv[1]))); s.listen(5)
while True:
    c,_=s.accept(); threading.Thread(target=handle,args=(c,),daemon=True).start()
