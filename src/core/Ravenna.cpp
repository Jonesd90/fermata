#include "Ravenna.h"
#include <thread>
#include <map>
#include <algorithm>

namespace td
{
// ------------------------------------------------------------------------------------------------ WebSocket
juce::MemoryBlock WebSocketClient::makeFrame (int opcode, const void* data, size_t size, const juce::uint8 mask[4])
{
    juce::MemoryBlock f;
    auto put = [&f] (juce::uint8 b) { f.append (&b, 1); };
    put ((juce::uint8) (0x80 | (opcode & 0x0f)));
    if (size < 126) put ((juce::uint8) (0x80 | size));
    else if (size < 65536) { put (0x80 | 126); put ((juce::uint8) (size >> 8)); put ((juce::uint8) size); }
    else { put (0x80 | 127); for (int s = 56; s >= 0; s -= 8) put ((juce::uint8) ((juce::uint64) size >> s)); }
    for (int i = 0; i < 4; ++i) put (mask[i]);
    auto* p = static_cast<const juce::uint8*> (data);
    for (size_t i = 0; i < size; ++i) put ((juce::uint8) (p[i] ^ mask[i & 3]));
    return f;
}

bool WebSocketClient::writeAll (const void* src, size_t n)
{
    if (socket == nullptr) return false;
    auto* p = static_cast<const char*> (src);
    size_t done = 0;
    while (done < n)
    {
        const int w = socket->write (p + done, (int) (n - done));
        if (w <= 0) return false;
        done += (size_t) w;
    }
    return true;
}

bool WebSocketClient::readExact (void* dst, size_t n, int timeoutMs)
{
    auto* p = static_cast<char*> (dst);
    size_t got = 0;
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (1, timeoutMs);
    while (got < n)
    {
        if (socket == nullptr) return false;
        const int left = (int) end - (int) juce::Time::getMillisecondCounter();
        if (left <= 0) return false;
        const int ready = socket->waitUntilReady (true, juce::jmin (left, 200));
        if (ready < 0) return false;
        if (ready == 0) continue;
        const int r = socket->read (p + got, (int) (n - got), false);
        if (r <= 0) return false;
        got += (size_t) r;
    }
    return true;
}

bool WebSocketClient::connect (const juce::String& host, int port, const juce::String& path, const juce::String& origin, int timeoutMs, juce::String& error)
{
    close();
    auto s = std::make_unique<juce::StreamingSocket>();
    if (! s->connect (host, port, timeoutMs)) { error = "Cannot reach " + host; return false; }
    { const juce::ScopedLock sl (sockLock); socket = std::move (s); }
    juce::uint8 keyBytes[16]; juce::Random r; for (auto& b : keyBytes) b = (juce::uint8) r.nextInt (256);
    const auto key = juce::Base64::toBase64 (keyBytes, 16);
    juce::String req;
    req << "GET " << path << " HTTP/1.1\r\nHost: " << host << "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\nSec-WebSocket-Version: 13\r\n";
    if (origin.isNotEmpty()) req << "Origin: " << origin << "\r\n";
    req << "\r\n";
    const auto u = req.toRawUTF8();
    if (! writeAll (u, std::strlen (u))) { error = "Connection lost"; close(); return false; }
    // read the reply headers up to the blank line
    juce::String head;
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! head.endsWith ("\r\n\r\n"))
    {
        char c;
        if (! readExact (&c, 1, juce::jmax (1, (int) end - (int) juce::Time::getMillisecondCounter()))) { error = "No answer from " + host; close(); return false; }
        head << juce::String::charToString ((juce::juce_wchar) (juce::uint8) c);
        if (head.length() > 8192) { error = "Strange answer from " + host; close(); return false; }
    }
    if (! head.startsWith ("HTTP/1.1 101")) { error = "The device refused the connection (" + head.upToFirstOccurrenceOf ("\r\n", false, false) + ")"; close(); return false; }
    return true;
}

bool WebSocketClient::sendText (const juce::String& text)
{
    const juce::ScopedLock sl (writeLock);
    if (socket == nullptr) return false;
    juce::uint8 mask[4]; juce::Random r; for (auto& b : mask) b = (juce::uint8) r.nextInt (256);
    const auto u = text.toRawUTF8();
    auto f = makeFrame (1, u, std::strlen (u), mask);
    return writeAll (f.getData(), f.getSize());
}

int WebSocketClient::receive (juce::String& out, int timeoutMs)
{
    if (socket == nullptr) return -1;
    juce::MemoryBlock message;
    bool inMessage = false;
    auto wait = timeoutMs;
    for (;;)
    {
        if (! inMessage)                                     // wait for the first byte of the next frame
        {
            const int ready = socket->waitUntilReady (true, wait);
            if (ready < 0) { close(); return -1; }
            if (ready == 0) return 0;
        }
        juce::uint8 h[2];
        if (! readExact (h, 2, 5000)) { close(); return -1; }
        const int opcode = h[0] & 0x0f; const bool fin = (h[0] & 0x80) != 0;
        juce::uint64 len = h[1] & 0x7f;
        if (len == 126) { juce::uint8 b[2]; if (! readExact (b, 2, 5000)) { close(); return -1; } len = ((juce::uint64) b[0] << 8) | b[1]; }
        else if (len == 127) { juce::uint8 b[8]; if (! readExact (b, 8, 5000)) { close(); return -1; } len = 0; for (auto x : b) len = (len << 8) | x; }
        juce::uint8 mask[4] = {};
        const bool masked = (h[1] & 0x80) != 0;
        if (masked && ! readExact (mask, 4, 5000)) { close(); return -1; }
        if (len > 64ull * 1024 * 1024) { close(); return -1; }
        juce::MemoryBlock payload ((size_t) len);
        if (len > 0 && ! readExact (payload.getData(), (size_t) len, 20000)) { close(); return -1; }
        if (masked) { auto* p = static_cast<juce::uint8*> (payload.getData()); for (size_t i = 0; i < payload.getSize(); ++i) p[i] ^= mask[i & 3]; }

        if (opcode == 8) { close(); return -1; }
        if (opcode == 9)                                     // ping: answer with the same bytes
        {
            const juce::ScopedLock sl (writeLock);
            juce::uint8 m[4] = { 1, 2, 3, 4 };
            auto f = makeFrame (10, payload.getData(), payload.getSize(), m);
            writeAll (f.getData(), f.getSize());
            inMessage = false; continue;
        }
        if (opcode == 10) { inMessage = false; continue; }
        if (opcode == 1 || opcode == 2 || opcode == 0)
        {
            if (opcode != 0) message.reset();
            message.append (payload.getData(), payload.getSize());
            if (fin) { out = juce::String::fromUTF8 (static_cast<const char*> (message.getData()), (int) message.getSize()); return 1; }
            inMessage = true; wait = 5000;
        }
    }
}

void WebSocketClient::close()
{
    const juce::ScopedLock sl (sockLock);
    if (socket != nullptr) { socket->close(); socket.reset(); }
}

void WebSocketClient::interrupt()
{
    const juce::ScopedLock sl (sockLock);              // the socket object is only ever destroyed under this lock
    if (socket != nullptr) socket->close();
}

// ------------------------------------------------------------------------------------------------ device
RavennaDevice::RavennaDevice (juce::String name, juce::String host, int port)
    : juce::Thread ("Ravenna " + name), deviceName (std::move (name)), hostName (std::move (host)), portNumber (port) {}

void RavennaDevice::stop()
{
    requestStop();
    stopThread (-1);                                   // never kill the thread: it may be in the middle of using this object
    ws.close();
}

juce::String RavennaDevice::getStatus() const { const juce::ScopedLock sl (lock); return status; }
int RavennaDevice::numChannels() const { const juce::ScopedLock sl (lock); return (int) refs.size(); }

static bool boolOf (const juce::var& v) { return (bool) v; }

/** A fixed line input with its own gain (the Anubis jack inputs 3 / 4): no mic gain, but a line gain. */
static bool isLineOnlyModule (const juce::var& m)
{
    const auto& caps = m["custom"]["ins"]["capabilities"]["channel"];
    if ((bool) caps["micGain"]) return false;
    if ((bool) caps["lineGain"]) return true;
    const auto* arr = m["custom"]["ins"]["channels"].getArray();
    if (arr == nullptr || arr->isEmpty()) return false;
    const auto* o = (*arr)[0].getDynamicObject();
    return o != nullptr && o->hasProperty ("lineGain") && ! o->hasProperty ("micGain");
}

/** The name of the Line / Instrument switch in a channel: the device's z_in, or anything called instrument / hi-z. "" if there is none. */
static juce::String instrumentKeyOf (const juce::var& ch)
{
    const auto* o = ch.getDynamicObject();
    if (o == nullptr) return {};
    if (o->hasProperty ("z_in")) return "z_in";
    for (auto& p : o->getProperties())
    {
        const auto k = p.name.toString().toLowerCase();
        if ((k.contains ("instr") || k.contains ("hiz") || k.contains ("hi_z")) && (p.value.isBool() || p.value.isInt())) return p.name.toString();
    }
    return {};
}

HwPreamp RavennaDevice::readChannel (const juce::var& module, int index)
{
    HwPreamp h;
    h.moduleId = (int) module["id"]; h.index = index; h.name = module["name"].toString();
    const auto ch = module["custom"]["ins"]["channels"][index];
    h.m48V = boolOf (ch["m48V"]); h.pad = boolOf (ch["pad"]); h.lowCut = boolOf (ch["lowCut"]); h.phase = boolOf (ch["phase"]); h.cut = boolOf (ch["cut"]); h.lift = boolOf (ch["lift"]); h.zIn = boolOf (ch["z_in"]);
    h.line = (int) ch["inputMode"] == 1;
    h.micGain = (int) ch["micGain"]; h.lineGain = (int) ch["lineGain"];
    if (isLineOnlyModule (module))
    {
        h.lineOnly = true; h.line = true; h.instrKey = instrumentKeyOf (ch);
        h.m48V = h.pad = h.lift = false;
        h.zIn = h.instrKey.isNotEmpty() && boolOf (ch[juce::Identifier (h.instrKey)]);
    }
    return h;
}

/** Applies a path such as ".custom.ins.channels[1]" (relative to a module) to it. Objects are merged, anything else replaces. */
bool RavennaDevice::applyPath (juce::var& module, const juce::String& rel, const juce::var& value)
{
    juce::var cur = module;
    juce::var parent; juce::String key; int idx = -1;
    int i = 0;
    while (i < rel.length())
    {
        const auto c = rel[i];
        parent = cur;
        if (c == '.')
        {
            int j = i + 1; while (j < rel.length() && rel[j] != '.' && rel[j] != '[') ++j;
            key = rel.substring (i + 1, j); idx = -1; i = j;
            cur = cur[juce::Identifier (key)];
        }
        else if (c == '[')
        {
            const int j = rel.indexOfChar (i, ']'); if (j < 0) return false;
            idx = rel.substring (i + 1, j).getIntValue(); key = {}; i = j + 1;
            cur = cur[idx];
        }
        else return false;
    }
    if (parent.isVoid() && ! rel.isEmpty()) return false;
    if (rel.isEmpty())                                       // the whole module
    {
        if (auto* o = module.getDynamicObject()) if (auto* v = value.getDynamicObject()) { for (auto& p : v->getProperties()) o->setProperty (p.name, p.value); return true; }
        module = value; return true;
    }
    if (cur.getDynamicObject() != nullptr && value.getDynamicObject() != nullptr)
    {
        for (auto& p : value.getDynamicObject()->getProperties()) cur.getDynamicObject()->setProperty (p.name, p.value);
        return true;
    }
    if (cur.getArray() != nullptr && value.getArray() != nullptr && cur.getArray()->size() == value.getArray()->size())     // a list of channels: merge channel by channel, so a field the device left out is kept
    {
        auto* ca = cur.getArray(); auto* va = value.getArray(); bool allObjects = true;
        for (int k = 0; k < ca->size(); ++k) allObjects = allObjects && (*ca)[k].getDynamicObject() != nullptr && (*va)[k].getDynamicObject() != nullptr;
        if (allObjects)
        {
            for (int k = 0; k < ca->size(); ++k)
                for (auto& p : (*va)[k].getDynamicObject()->getProperties()) (*ca)[k].getDynamicObject()->setProperty (p.name, p.value);
            return true;
        }
    }
    if (idx >= 0) { if (auto* a = parent.getArray()) { if (idx < a->size()) a->set (idx, value); else return false; return true; } return false; }
    if (auto* o = parent.getDynamicObject()) { o->setProperty (juce::Identifier (key), value); return true; }
    return false;
}

/** Is this module one whose channels have a preamp that Fermata controls? 'why' says in words why not (for the log). */
static bool usablePreampModule (const juce::var& m, juce::String& why)
{
    const auto* arr = m["custom"]["ins"]["channels"].getArray();
    if (arr == nullptr) { why = "no input channels"; return false; }
    const bool lineOnly = isLineOnlyModule (m);
    if (! (bool) m["custom"]["ins"]["capabilities"]["channel"]["micGain"] && ! lineOnly) { why = "no gain control (not a preamp)"; return false; }      // only modules that have a mic or line gain
    const auto n = m["name"].toString();
    if (n.containsIgnoreCase ("split") || n.containsIgnoreCase ("built-in")) { why = "the Anubis lists its inputs twice (or this is the built-in mic): the plain ones are used"; return false; }
    why = lineOnly ? "used: a fixed line input with its own gain" : "used";
    return true;
}

void RavennaDevice::rebuildChannelList()
{
    // the modules that count, in the order of the numbers in their names ("Combo 1/2", "Jack 3/4") when every one has a different number: that is the order of the
    // inputs on the front panel and in ANEMAN. Otherwise in the order of the device's own module numbers.
    struct Mod { int id; int firstNumber; };
    std::vector<Mod> mods;
    bool numbered = true;
    for (auto& kv : modules)
    {
        juce::String why;
        if (! usablePreampModule (kv.second, why)) continue;
        const auto name = kv.second["name"].toString();
        int num = -1;
        for (int i = 0; i < name.length() && num < 0; ++i) if (juce::CharacterFunctions::isDigit (name[i])) num = name.substring (i).getIntValue();
        if (num < 0) numbered = false;
        mods.push_back ({ kv.first, num });
    }
    if (numbered)
    {
        for (size_t a = 0; a < mods.size() && numbered; ++a) for (size_t b = a + 1; b < mods.size(); ++b) if (mods[a].firstNumber == mods[b].firstNumber) { numbered = false; break; }
        if (numbered) std::stable_sort (mods.begin(), mods.end(), [] (const Mod& x, const Mod& y) { return x.firstNumber < y.firstNumber; });
    }
    std::vector<Ref> r;
    for (auto& md : mods)
    {
        const auto* arr = modules[md.id]["custom"]["ins"]["channels"].getArray();
        for (int i = 0; i < arr->size(); ++i) r.push_back ({ md.id, i });
    }
    refs = std::move (r);
}

bool RavennaDevice::getChannel (int i, HwPreamp& out) const
{
    const juce::ScopedLock sl (lock);
    if (! juce::isPositiveAndBelow (i, (int) refs.size())) return false;
    auto it = modules.find (refs[(size_t) i].moduleId);
    if (it == modules.end()) return false;
    out = readChannel (it->second, refs[(size_t) i].index);
    return true;
}

bool RavennaDevice::sendChannelFields (int i, const juce::var& fields)
{
    juce::String path; juce::String json;
    {
        const juce::ScopedLock sl (lock);
        if (! connected.load() || ! juce::isPositiveAndBelow (i, (int) refs.size())) return false;
        const auto ref = refs[(size_t) i];
        auto it = modules.find (ref.moduleId);
        if (it == modules.end()) return false;
        const auto rel = ".custom.ins.channels[" + juce::String (ref.index) + "]";
        applyPath (it->second, rel, fields);                  // keep our copy current so the next change is compared against it
        path = "$._modules[?(@.id==" + juce::String (ref.moduleId) + ")][0]" + rel;
        auto* msg = new juce::DynamicObject();
        auto* data = new juce::DynamicObject();
        data->setProperty ("path", path); data->setProperty ("value", fields);
        msg->setProperty ("channel", "/service/ravenna/settings"); msg->setProperty ("data", juce::var (data));
        msg->setProperty ("id", nextId()); msg->setProperty ("clientId", clientId);
        json = juce::JSON::toString (juce::var (msg), true);
        outgoing.add (json);
        if (fields.getDynamicObject() != nullptr)       // for the log: every preamp change sent to a device
            juce::Logger::writeToLog ("Device " + hostName + " SENT: " + path + " = " + juce::JSON::toString (fields, true));
    }
    return true;
}

void RavennaDevice::sendRaw (const juce::String& json) { ws.sendText (json); }

void RavennaDevice::handleSettings (const juce::var& data)
{
    const auto path = data["path"].toString();
    const auto& value = data["value"];
    bool changed = false;
    if (path != "$" && path.containsIgnoreCase ("channels") && value.getArray() != nullptr)                   // for the log: a whole list of channels reported (the first two)
    {
        juce::String t;
        for (int k = 0; k < juce::jmin (2, value.getArray()->size()); ++k)
        {
            const auto& c = (*value.getArray())[k];
            t << " [" << k << "] inputMode=" << (c.getDynamicObject() != nullptr && c.getDynamicObject()->hasProperty ("inputMode") ? c["inputMode"].toString() : juce::String ("missing"))
              << " micGain=" << c["micGain"].toString() << " lineGain=" << c["lineGain"].toString();
        }
        juce::Logger::writeToLog ("Device " + hostName + " REPORTED LIST: " + path + t);
    }
    else if (path != "$" && path.containsIgnoreCase ("channels") && value.getDynamicObject() != nullptr)          // for the log: every preamp change reported by a device
        juce::Logger::writeToLog ("Device " + hostName + " REPORTED: " + path + " = " + juce::JSON::toString (value, true));
    {
        const juce::ScopedLock sl (lock);
        if (path == "$")
        {
            if (auto* arr = value["_modules"].getArray())
            {
                modules.clear();
                for (auto& m : *arr) modules[(int) m["id"]] = m;
                rebuildChannelList();
                for (auto& kv : modules)                           // for the log: every module the device has with input channels, and whether its channels count as preamps
                {
                    juce::String why;
                    const bool used = usablePreampModule (kv.second, why);
                    const auto* arr = kv.second["custom"]["ins"]["channels"].getArray();
                    if (arr == nullptr) continue;
                    juce::Logger::writeToLog ("Device " + hostName + " MODULE " + juce::String (kv.first) + " '" + kv.second["name"].toString() + "': " + juce::String (arr->size()) + " input channel(s), "
                                              + (used ? "controlled" : "NOT controlled") + " (" + why + ")");
                    if (arr->size() > 0)
                        juce::Logger::writeToLog ("Device " + hostName + " MODULE " + juce::String (kv.first) + " capabilities: " + juce::JSON::toString (kv.second["custom"]["ins"]["capabilities"], true)
                                                  + " | first channel: " + juce::JSON::toString ((*arr)[0], true));
                }
                status = "Connected: " + juce::String ((int) refs.size()) + " preamp channels";
                changed = true;
            }
        }
        else
        {
            const juce::String prefix = "$._modules[?(@.id==";
            if (path.startsWith (prefix))
            {
                const int close = path.indexOf (")][0]");
                const int id = path.substring (prefix.length(), close).getIntValue();
                auto it = modules.find (id);
                if (close > 0 && it != modules.end())
                {
                    changed = applyPath (it->second, path.substring (close + 5), value);
                    if (changed && path.substring (close + 5).containsIgnoreCase ("channels")) {} else if (changed && path.substring (close + 5).isEmpty()) rebuildChannelList();
                }
            }
        }
    }
    if (changed && onUpdate) onUpdate();
}

void RavennaDevice::handleMessage (const juce::var& m)
{
    const auto channel = m["channel"].toString();
    if (channel == "/meta/connect") { needConnect = true; return; }
    if (channel == "/ravenna/settings") handleSettings (m["data"]);
}

void RavennaDevice::session()
{
    juce::String err;
    { const juce::ScopedLock sl (lock); status = "Connecting to " + hostName; }
    if (! ws.connect (hostName, portNumber, "/cometd/handshake", "http://" + hostName, 4000, err))
    {
        const juce::ScopedLock sl (lock); status = err; return;
    }
    auto sendObj = [this] (const juce::String& json) { ws.sendText (json); };
    sendObj ("[{\"version\":\"1.0\",\"minimumVersion\":\"0.9\",\"channel\":\"/meta/handshake\",\"supportedConnectionTypes\":[\"websocket\",\"long-polling\",\"callback-polling\"],\"advice\":{\"timeout\":60000,\"interval\":0},\"id\":\"1\"}]");
    // wait for the handshake answer that carries our id
    const auto t0 = juce::Time::getMillisecondCounter();
    clientId = {};
    while (clientId.isEmpty() && ! threadShouldExit() && juce::Time::getMillisecondCounter() - t0 < 6000)
    {
        juce::String msg;
        const int r = ws.receive (msg, 500);
        if (r < 0) { const juce::ScopedLock sl (lock); status = "Connection lost during handshake"; return; }
        if (r == 0) continue;
        auto v = juce::JSON::parse (msg);
        if (auto* a = v.getArray())
            for (auto& m : *a)
                if (m["channel"].toString() == "/meta/handshake" && m["id"].toString() == "1" && (bool) m["successful"]) clientId = m["clientId"].toString();
    }
    if (clientId.isEmpty()) { const juce::ScopedLock sl (lock); status = "The device did not answer the handshake"; ws.close(); return; }

    sendObj ("[{\"channel\":\"/meta/connect\",\"connectionType\":\"websocket\",\"advice\":{\"timeout\":0},\"id\":\"2\",\"clientId\":\"" + clientId + "\"}]");
    sendObj ("[{\"channel\":\"/meta/subscribe\",\"subscription\":\"/ravenna/settings\",\"id\":\"3\",\"clientId\":\"" + clientId + "\"},"
             "{\"channel\":\"/meta/subscribe\",\"subscription\":\"/ravenna/errors\",\"id\":\"4\",\"clientId\":\"" + clientId + "\"}]");
    sendObj ("[{\"channel\":\"/service/ravenna/commands\",\"data\":{\"command\":\"update\"},\"id\":\"5\",\"clientId\":\"" + clientId + "\"}]");
    { const juce::ScopedLock sl (lock); status = "Reading the device..."; }
    needConnect = false; lastConnectMs = (juce::int64) juce::Time::getMillisecondCounter();
    connected = true;

    while (! threadShouldExit() && ws.isOpen())
    {
        juce::String msg;
        const int r = ws.receive (msg, 100);
        if (r < 0) break;
        if (r == 1)
        {
            auto v = juce::JSON::parse (msg);
            if (auto* a = v.getArray()) for (auto& m : *a) handleMessage (m);
            else if (v.isObject()) handleMessage (v);
        }
        juce::StringArray out;
        { const juce::ScopedLock sl (lock); out = outgoing; outgoing.clear(); }
        for (auto& o : out) ws.sendText ("[" + o + "]");
        const auto now = (juce::int64) juce::Time::getMillisecondCounter();
        if (needConnect && now - lastConnectMs > 80)
        {
            needConnect = false; lastConnectMs = now;
            ws.sendText ("[{\"channel\":\"/meta/connect\",\"connectionType\":\"websocket\",\"id\":\"" + nextId() + "\",\"clientId\":\"" + clientId + "\"}]");
        }
        else if (! needConnect && now - lastConnectMs > 15000)     // nothing has come for a while: ask again
        {
            lastConnectMs = now;
            ws.sendText ("[{\"channel\":\"/meta/connect\",\"connectionType\":\"websocket\",\"id\":\"" + nextId() + "\",\"clientId\":\"" + clientId + "\"}]");
        }
    }
    connected = false;
    ws.close();
}

void RavennaDevice::run()
{
    while (! threadShouldExit())
    {
        session();
        connected = false;
        { const juce::ScopedLock sl (lock); modules.clear(); refs.clear(); if (! threadShouldExit()) status = status + " (retrying)"; }
        if (onUpdate) onUpdate();
        for (int i = 0; i < 40 && ! threadShouldExit(); ++i) wait (100);
    }
}

// ------------------------------------------------------------------------------------------------ driver
RavennaPreampDriver::RavennaPreampDriver (std::vector<PreampDeviceCfg> cfg)
{
    for (auto& c : cfg)
    {
        auto d = std::make_unique<Dev>();
        d->cfg = c;
        d->dev = std::make_unique<RavennaDevice> (c.name, c.host);
        d->dev->onUpdate = [l = link] { std::lock_guard<std::mutex> g (l->m); if (l->owner != nullptr) l->owner->hardwareChanged(); };
        devs.push_back (std::move (d));
    }
    link->owner = this;
    for (auto& d : devs) d->dev->start();
}

RavennaPreampDriver::~RavennaPreampDriver()
{
    { std::lock_guard<std::mutex> g (link->m); link->owner = nullptr; }     // no device thread calls back into this object from now on
    *alive = false;
    // Shutting a device down can take seconds (a connection attempt cannot be interrupted), so it is finished on a helper thread:
    // the window that replaced the driver does not freeze, and nothing is ever killed half-way.
    auto keep = std::make_shared<std::vector<std::unique_ptr<Dev>>> (std::move (devs));
    for (auto& d : *keep) d->dev->requestStop();
    std::thread ([keep] { for (auto& d : *keep) d->dev->stop(); keep->clear(); }).detach();
}

juce::String RavennaPreampDriver::getName() const
{
    int n = 0, total = 0;
    for (auto& d : devs) { if (d->dev->isConnected()) ++n; total += d->dev->numChannels(); }
    return devs.empty() ? "No preamp devices set up" : juce::String (n) + " of " + juce::String ((int) devs.size()) + " devices connected, " + juce::String (total) + " preamp channels";
}

bool RavennaPreampDriver::isConnected() const { for (auto& d : devs) if (d->dev->isConnected()) return true; return false; }

PreampSettings RavennaPreampDriver::toSettings (const HwPreamp& h)
{
    PreampSettings s;
    s.line = h.line; s.gainDb = (float) (h.line ? h.lineGain : h.micGain) / 10.0f;
    s.phantom = h.m48V; s.lowCut = h.lowCut; s.polarity = h.phase; s.boost = h.lift; s.pad = h.pad; s.zHigh = h.zIn;
    s.lineOnly = h.lineOnly; s.hasInstrument = h.lineOnly && h.instrKey.isNotEmpty();
    return s;
}

bool RavennaPreampDriver::locate (int inputIndex, Dev*& dev, int& channel) const
{
    for (auto& d : devs)
    {
        const int n = d->dev->numChannels();
        for (int c = 0; c < n; ++c) if (d->cfg.inputFor (c) == inputIndex) { dev = d.get(); channel = c; return true; }
    }
    return false;
}

bool RavennaPreampDriver::read (int inputIndex, PreampSettings& out) const
{
    Dev* d = nullptr; int ch = 0; HwPreamp h;
    if (! locate (inputIndex, d, ch) || ! d->dev->getChannel (ch, h)) return false;
    out = toSettings (h); return true;
}

bool RavennaPreampDriver::apply (int inputIndex, const PreampSettings& s)
{
    Dev* d = nullptr; int ch = 0; HwPreamp h;
    if (! locate (inputIndex, d, ch) || ! d->dev->getChannel (ch, h)) return false;
    auto* o = new juce::DynamicObject();                      // only what really differs is sent
    bool any = false;
    const bool modeChange = ! h.lineOnly && s.line != h.line;                  // a fixed line input has no Mic / Line switch
    if (modeChange) { o->setProperty ("inputMode", s.line ? 1 : 0); any = true; }
    // Switching Mic / Line sends the switch ALONE, exactly like the device's own web app: the gain on the screen still belongs to the OLD mode,
    // and sending it along made the device undo the switch. The device's own gain for the new mode comes back and is shown.
    if (! modeChange)
    {
        const int gain = juce::jlimit (0, 660, (int) std::lround (s.gainDb * 10.0f));
        const bool lineGain = h.lineOnly || s.line;
        const int cur = lineGain ? h.lineGain : h.micGain;
        if (gain != cur) { o->setProperty (lineGain ? "lineGain" : "micGain", gain); any = true; }
    }
    if (! h.lineOnly && s.phantom != h.m48V) { o->setProperty ("m48V", s.phantom); any = true; }
    if (s.lowCut != h.lowCut)  { o->setProperty ("lowCut", s.lowCut); any = true; }
    if (s.polarity != h.phase) { o->setProperty ("phase", s.polarity); any = true; }
    if (! h.lineOnly && s.boost != h.lift)   { o->setProperty ("lift", s.boost); any = true; }
    if (! h.lineOnly && s.pad != h.pad)      { o->setProperty ("pad", s.pad); any = true; }
    if (h.lineOnly) { if (h.instrKey.isNotEmpty() && s.zHigh != h.zIn) { o->setProperty (juce::Identifier (h.instrKey), s.zHigh); any = true; } }
    else if (s.zHigh != h.zIn) { o->setProperty ("z_in", s.zHigh); any = true; }
    juce::var v (o);
    if (! any) return true;
    lastSentMs[inputIndex] = juce::Time::getMillisecondCounterHiRes();
    return d->dev->sendChannelFields (ch, v);
}

std::vector<RavennaPreampDriver::Info> RavennaPreampDriver::info() const
{
    std::vector<Info> v;
    for (auto& d : devs)
    {
        int first = d->cfg.firstInput, count = d->dev->numChannels();
        if (! d->cfg.map.empty()) { first = 1 << 30; count = 0; for (auto& p : d->cfg.map) if (p.first < d->dev->numChannels() || ! d->dev->isConnected()) { first = juce::jmin (first, p.second); ++count; } }
        v.push_back ({ d->cfg.name, d->cfg.host, d->dev->getStatus(), d->dev->isConnected(), count, first, (int) d->cfg.map.size() });
    }
    return v;
}

void RavennaPreampDriver::hardwareChanged()
{
    // runs on a device thread: collect what the hardware has, then hand it to the message thread
    auto list = std::make_shared<std::vector<std::pair<int, PreampSettings>>>();
    for (auto& d : devs)
    {
        const int n = d->dev->numChannels();
        for (int c = 0; c < n; ++c) { HwPreamp h; const int in = d->cfg.inputFor (c); if (in >= 0 && d->dev->getChannel (c, h)) list->push_back ({ in, toSettings (h) }); }
    }
    auto flag = alive;
    if (! juce::MessageManager::getInstanceWithoutCreating()) return;
    juce::MessageManager::callAsync ([this, list, flag]
    {
        if (! *flag) return;
        bool skipped = false;
        for (auto& p : *list)
        {
            // the device's answer in the moment after this program changed the input may still show the OLD state: ignore it, and look again a moment later
            auto sent = lastSentMs.find (p.first);
            if (sent != lastSentMs.end() && juce::Time::getMillisecondCounterHiRes() - sent->second < 1200.0) { skipped = true; continue; }
            auto it = lastReported.find (p.first);
            const auto& a = p.second;
            if (it != lastReported.end() && it->second.gainDb == a.gainDb && it->second.phantom == a.phantom && it->second.line == a.line
                && it->second.lowCut == a.lowCut && it->second.polarity == a.polarity && it->second.boost == a.boost && it->second.pad == a.pad && it->second.zHigh == a.zHigh) continue;
            lastReported[p.first] = a;
            if (onHardware) onHardware (p.first, a);
        }
        if (skipped && ! recheckPending)
        {
            recheckPending = true;
            juce::Timer::callAfterDelay (1300, [this, flag] { if (! *flag) return; recheckPending = false; hardwareChanged(); });
        }
    });
}

// ------------------------------------------------------------------------------------------------ ANEMAN
namespace
{
bool isPreampGroup (const juce::var& g)
{
    if (g["Type"].toString() != "Audio" || g["Sub_Type"].toString() != "io") return false;
    auto* in = g["Inputs"].getArray(); if (in == nullptr || in->isEmpty()) return false;
    const auto n = g["Name"].toString().toLowerCase();
    return ! (n.contains ("aes") || n.contains ("spdif") || n.contains ("s/pdif") || n.contains ("built-in") || n.contains ("adat") || n.contains ("madi"));
}
}

AnemanPlan planFromAneman (const juce::var& devices, const juce::var& connections)
{
    AnemanPlan plan;
    auto* devs = devices.getArray(); auto* conns = connections.getArray();
    if (devs == nullptr || conns == nullptr) { plan.error = "ANEMAN's answer was not in the expected form."; return plan; }

    const juce::var* asio = nullptr;
    for (auto& d : *devs) if (d["Identity"]["Product"].toString().equalsIgnoreCase ("ASIO")) { asio = &d; break; }
    if (asio == nullptr) { plan.error = "ANEMAN does not list an ASIO device. Is the Merging ASIO driver (MAD) running?"; return plan; }
    const int asioId = (int) (*asio)["Id"];

    auto findDev = [&] (int id) -> const juce::var* { for (auto& d : *devs) if ((int) d["Id"] == id) return &d; return nullptr; };

    // where each patched input of a device lands on the ASIO device: device id -> (device channel -> ASIO input), 0-based
    std::map<int, std::map<int, int>> patched;
    for (auto& c : *conns)
    {
        if ((int) c["Output"]["DeviceId"] != asioId) continue;
        const int src = (int) c["Input"]["DeviceId"]; if (src == asioId) continue;
        const auto* d = findDev (src); if (d == nullptr) continue;
        int base = 0, ch = -1;
        if (auto* groups = (*d)["IOGroups"].getArray())
            for (auto& g : *groups)
            {
                if (! isPreampGroup (g)) continue;
                if ((int) g["Id"] == (int) c["Input"]["IOGroupId"]) { ch = base + (int) c["Input"]["InputId"]; break; }
                base += g["Inputs"].getArray()->size();
            }
        if (ch >= 0) patched[src][ch] = (int) c["Output"]["OutputId"];
    }

    struct Found { PreampDeviceCfg cfg; int count = 0; };
    std::vector<Found> found;
    for (auto& kv : patched)
    {
        const auto* d = findDev (kv.first);
        PreampDeviceCfg cfg; cfg.name = (*d)["Identity"]["Name"].toString();
        if (auto* ifs = (*d)["Network"]["Interfaces"].getArray()) for (auto& i : *ifs) { cfg.host = i["Address"].toString(); if (cfg.host.isNotEmpty()) break; }
        if (cfg.host.isEmpty()) { plan.warnings.add (cfg.name + ": ANEMAN gives no address for it, so it was left out."); continue; }
        cfg.map = kv.second;                                            // exactly as patched: device channel -> ASIO input
        cfg.firstInput = cfg.map.begin()->second;
        for (auto& p : cfg.map) cfg.firstInput = juce::jmin (cfg.firstInput, p.second);
        const int count = (int) cfg.map.size();
        // a readable description: runs of channels that go to consecutive ASIO inputs
        juce::String desc;
        for (auto it = cfg.map.begin(); it != cfg.map.end();)
        {
            auto runEnd = it;
            while (std::next (runEnd) != cfg.map.end() && std::next (runEnd)->first == runEnd->first + 1 && std::next (runEnd)->second == runEnd->second + 1) ++runEnd;
            if (desc.isNotEmpty()) desc << ", ";
            if (runEnd == it) desc << "ch " << (it->first + 1) << " > ASIO " << (it->second + 1);
            else desc << "ch " << (it->first + 1) << "-" << (runEnd->first + 1) << " > ASIO " << (it->second + 1) << "-" << (runEnd->second + 1);
            it = std::next (runEnd);
        }
        plan.summary.add (cfg.name + " (" + cfg.host + "): " + desc);
        found.push_back ({ cfg, count });
    }
    std::sort (found.begin(), found.end(), [] (const Found& a, const Found& b) { return a.cfg.firstInput < b.cfg.firstInput; });
    for (auto& f : found) plan.devices.push_back (f.cfg);

    // outputs, for the summary only
    std::map<int, juce::String> outs;
    for (auto& c : *conns)
    {
        if ((int) c["Input"]["DeviceId"] != asioId) continue;
        const auto* d = findDev ((int) c["Output"]["DeviceId"]); if (d == nullptr) continue;
        juce::String grp;
        if (auto* groups = (*d)["IOGroups"].getArray()) for (auto& g : *groups) if ((int) g["Id"] == (int) c["Output"]["IOGroupId"]) grp = g["Name"].toString();
        outs[(int) c["Input"]["InputId"]] = (*d)["Identity"]["Name"].toString() + " " + grp + (" " + juce::String ((int) c["Output"]["OutputId"] + 1));
    }
    if (! outs.empty())
    {
        juce::String line = "ASIO outputs:";
        for (auto& o : outs) line << "  " << (o.first + 1) << " > " << o.second << ";";
        plan.summary.add (line);
    }
    if (plan.devices.empty()) plan.error = "No device is patched to the ASIO inputs in ANEMAN yet.";
    plan.ok = plan.devices.size() > 0;
    return plan;
}

AnemanPlan fetchAnemanPlan (const juce::String& preferred)
{
    juce::StringArray hosts;
    if (preferred.trim().isNotEmpty()) hosts.add (preferred.trim());
    hosts.add ("127.0.0.1");
    for (auto& a : juce::IPAddress::getAllAddresses()) if (! a.isIPv6) hosts.addIfNotAlreadyThere (a.toString());
    juce::StringArray tried;
    for (auto h : hosts)
    {
        if (! h.contains (":")) h += ":6167";
        auto get = [&] (const juce::String& path, juce::var& out)
        {
            juce::URL url ("http://" + h + "/get");
            url = url.withPOSTData ("{\"Path\":\"" + path + "\"}");
            std::unique_ptr<juce::InputStream> in (url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                                                              .withExtraHeaders ("Content-Type: application/json\r\n").withConnectionTimeoutMs (1500)));
            if (in == nullptr) return false;
            out = juce::JSON::parse (in->readEntireStreamAsString());
            return out.isArray();
        };
        juce::var devs, conns;
        if (! get ("$.Devices", devs) || ! get ("$.Connections", conns)) { tried.add (h); continue; }
        auto plan = planFromAneman (devs, conns);
        plan.source = h;
        return plan;
    }
    AnemanPlan p;
    p.error = "Could not reach ANEMAN (tried " + tried.joinIntoString (", ") + "). Is ANEMAN running on this PC? Type its address in the box if it is somewhere else.";
    return p;
}
} // namespace td
