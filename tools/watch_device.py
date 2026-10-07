"""Prints every setting change a Merging device reports, so you can see what a button in its web page changes.
Usage:   python watch_device.py 165.165.1.40        (then click things in the device's web page and read the lines that appear)
Needs only Python 3 (no installs)."""
import socket, struct, base64, os, json, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else "165.165.1.40"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 80

def frame(text):
    data = text.encode(); mask = os.urandom(4); n = len(data)
    head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))

def readn(s, n):
    d = b""
    while len(d) < n:
        x = s.recv(n - len(d))
        if not x: raise EOFError
        d += x
    return d

def read_message(s):
    msg = b""
    while True:
        h = readn(s, 2); op = h[0] & 15; fin = h[0] & 0x80; ln = h[1] & 127
        if ln == 126: ln = struct.unpack(">H", readn(s, 2))[0]
        elif ln == 127: ln = struct.unpack(">Q", readn(s, 8))[0]
        p = readn(s, ln)
        if op == 8: raise EOFError
        if op in (1, 0):
            msg += p
            if fin: return msg.decode("utf-8", "replace")

s = socket.create_connection((host, port), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(f"GET /cometd/handshake HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: http://{host}\r\n\r\n".encode())
head = b""
while b"\r\n\r\n" not in head: head += s.recv(1)
if b" 101 " not in head.split(b"\r\n")[0]: sys.exit("The device refused the connection: " + head.split(b"\r\n")[0].decode())
s.sendall(frame('[{"version":"1.0","minimumVersion":"0.9","channel":"/meta/handshake","supportedConnectionTypes":["websocket"],"advice":{"timeout":60000,"interval":0},"id":"1"}]'))
cid = None
while cid is None:
    for m in json.loads(read_message(s)):
        if m.get("channel") == "/meta/handshake" and m.get("id") == "1" and m.get("successful"): cid = m["clientId"]
send = lambda o: s.sendall(frame(json.dumps([o])))
send({"channel": "/meta/connect", "connectionType": "websocket", "advice": {"timeout": 0}, "id": "2", "clientId": cid})
send({"channel": "/meta/subscribe", "subscription": "/ravenna/settings", "id": "3", "clientId": cid})
send({"channel": "/service/ravenna/commands", "data": {"command": "update"}, "id": "5", "clientId": cid})
print(f"Connected to {host}. Now click things in the device's web page (e.g. Boost) - changes appear below. Ctrl+C to stop.\n")
s.settimeout(None); n = 10; last = time.time()
while True:
    for m in json.loads(read_message(s)):
        ch = m.get("channel")
        if ch == "/meta/connect":                                   # always ask again, or the device stops sending updates
            time.sleep(max(0.0, 0.08 - (time.time() - last)))
            n += 1; send({"channel": "/meta/connect", "connectionType": "websocket", "id": str(n), "clientId": cid}); last = time.time()
        elif ch == "/ravenna/settings":
            d = m.get("data", {})
            if d.get("path") == "$": print("(received the full settings dump: %d bytes)" % len(json.dumps(d))); continue
            print(time.strftime("%H:%M:%S"), d.get("path"), "=", json.dumps(d.get("value")))
        elif not (ch or "").startswith("/meta/"):
            print(time.strftime("%H:%M:%S"), "other message:", ch, json.dumps(m)[:400])      # anything else the device says
