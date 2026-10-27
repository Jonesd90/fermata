#include "AafWrite.h"
#include "AafModel.h"
#include <map>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>

namespace td::aafwrite
{
namespace
{
using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
using Bytes = std::vector<u8>;

// ============================================================================ little helpers
void put16 (Bytes& b, u16 v) { b.push_back ((u8) v); b.push_back ((u8) (v >> 8)); }
void put32 (Bytes& b, u32 v) { put16 (b, (u16) v); put16 (b, (u16) (v >> 16)); }
void put64 (Bytes& b, u64 v) { put32 (b, (u32) v); put32 (b, (u32) (v >> 32)); }
void putUtf16 (Bytes& b, const juce::String& s)       // UTF-16LE with its terminating zero, as every AAF string is stored
{
    auto p = s.toUTF16();
    for (auto* c = p.getAddress(); *c != 0; ++c) put16 (b, (u16) *c);
    put16 (b, 0);
}
Bytes utf16 (const juce::String& s) { Bytes b; putUtf16 (b, s); return b; }

/** An AUID's 16 stored bytes from its text "0d010101-0101-2f00-060e-2b3402060101": the first three parts little-endian, the last two as written. */
Bytes auidBytes (const char* text)
{
    juce::String s (text); s = s.removeCharacters ("-");
    Bytes b (16, 0);
    if (s.length() != 32) return b;
    auto hex = [&] (int i) { return (u8) s.substring (i * 2, i * 2 + 2).getHexValue32(); };
    b[0] = hex (3); b[1] = hex (2); b[2] = hex (1); b[3] = hex (0);
    b[4] = hex (5); b[5] = hex (4);
    b[6] = hex (7); b[7] = hex (6);
    for (int i = 8; i < 16; ++i) b[(size_t) i] = hex (i);
    return b;
}
Bytes auidBytes (const juce::String& s) { return auidBytes (s.toRawUTF8()); }

/** A SMPTE mob id (32 bytes) with a random material number: the same shape as the ones pyaaf2 and Avid write. */
Bytes newMobId()
{
    static const u8 label[12] = { 0x06, 0x0a, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x0f, 0x20 };
    Bytes b (label, label + 12);
    b.push_back (0x13); b.push_back (0); b.push_back (0); b.push_back (0);
    const auto u = juce::Uuid();
    for (int i = 0; i < 16; ++i) b.push_back (u.getRawData()[i]);
    return b;
}
Bytes nullMobId() { return Bytes (32, 0); }

Bytes timeStamp (const juce::Time& t)
{
    Bytes b;
    put16 (b, (u16) t.getYear()); b.push_back ((u8) t.getMonth() + 1); b.push_back ((u8) t.getDayOfMonth());
    b.push_back ((u8) t.getHours()); b.push_back ((u8) t.getMinutes()); b.push_back ((u8) t.getSeconds()); b.push_back ((u8) (t.getMilliseconds() / 10));
    return b;
}

// ============================================================================ the model (AafModel.h) indexed
struct Model
{
    std::unordered_map<std::string, const aafmodel::ClassDef*> classByName, classByAuid;
    std::unordered_map<std::string, const aafmodel::TypeDef*> typeByAuid, typeByName;
    Model()
    {
        for (auto& c : aafmodel::classes) { classByName[c.name] = &c; classByAuid[c.auid] = &c; }
        for (auto& t : aafmodel::types) { typeByAuid[t.auid] = &t; typeByName[t.name] = &t; }
    }
    const aafmodel::ClassDef* cls (const char* name) const { auto it = classByName.find (name); jassert (it != classByName.end()); return it == classByName.end() ? nullptr : it->second; }
    const aafmodel::TypeDef* type (const char* auid) const { auto it = typeByAuid.find (auid); return it == typeByAuid.end() ? nullptr : it->second; }
    /** The property of a class (or of one of its ancestors) by name. */
    const aafmodel::PropDef* prop (const aafmodel::ClassDef* c, const char* name) const
    {
        for (; c != nullptr; c = c->parent != nullptr ? classByAuid.count (c->parent) ? classByAuid.at (c->parent) : nullptr : nullptr)
            for (int i = 0; i < c->numProps; ++i)
                if (std::strcmp (c->props[i].name, name) == 0) return &c->props[i];
        jassertfalse;      // (a property name the model does not know: a typo here)
        return nullptr;
    }
    /** The type with Rename unwrapped. */
    const aafmodel::TypeDef* plain (const aafmodel::TypeDef* t) const
    {
        while (t != nullptr && t->kind == aafmodel::TypeKind::Rename) t = type (t->ref);
        return t;
    }
    /** The stored form of a property's value, from its type: a strong or weak reference, one of their collections, or plain data. */
    u16 formOf (const aafmodel::PropDef* p) const
    {
        using K = aafmodel::TypeKind;
        auto* t = plain (type (p->type));
        if (t == nullptr) return 0x82;
        if (t->kind == K::StrongRef) return 0x22;
        if (t->kind == K::WeakRef) return 0x02;
        if (t->kind == K::Set || t->kind == K::VarArray)
        {
            auto* e = plain (type (t->ref));
            if (e != nullptr && e->kind == K::StrongRef) return t->kind == K::Set ? 0x3a : 0x32;
            if (e != nullptr && e->kind == K::WeakRef) return t->kind == K::Set ? 0x1a : 0x12;
        }
        return 0x82;
    }
};

/** The storage names: "<property>-<pid in hex>", shortened from the middle when too long for a compound file's 31 characters. (As pyaaf2.) */
juce::String squeeze (const juce::String& name, int size)
{
    if (name.length() <= size) return name;
    const int half = size / 2;
    juce::String out;
    for (int i = 0; i < size; ++i)
        out << (i < half ? name[i] : i == half ? juce::juce_wchar ('-') : name[name.length() - (size - i)]);
    return out;
}
juce::String mangle (const char* name, u16 pid, int size)
{
    const auto p = juce::String::toHexString ((int) pid);
    return squeeze (name, size - p.length() - 2) + "-" + p;
}

// ============================================================================ the objects of the file, built in memory
struct Obj;
using ObjPtr = std::unique_ptr<Obj>;
struct Obj
{
    const aafmodel::ClassDef* cls = nullptr;
    struct Prop { u16 pid, form; Bytes data; };
    std::vector<Prop> props;
    std::vector<std::pair<juce::String, ObjPtr>> kids;        // child storages (strongly referenced objects)
    std::vector<std::pair<juce::String, Bytes>> streams;      // index streams of the collections
    /** The "properties" stream: byte order, version, count, the entries, then the values. */
    Bytes propertiesStream() const
    {
        Bytes b; b.push_back (0x4c); b.push_back (32); put16 (b, (u16) props.size());
        for (auto& p : props) { put16 (b, p.pid); put16 (b, p.form); put16 (b, (u16) p.data.size()); }
        for (auto& p : props) b.insert (b.end(), p.data.begin(), p.data.end());
        return b;
    }
};

/** Builds the objects: the setters know the model, so a caller names properties and never thinks about pids or stored forms. */
struct Builder
{
    const Model& m;
    std::vector<std::vector<u16>> weakPaths;     // the "referenced properties" table: the pid path to each set that weak references point into
    explicit Builder (const Model& model) : m (model) {}

    ObjPtr make (const char* className) { auto o = std::make_unique<Obj>(); o->cls = m.cls (className); return o; }
    Obj::Prop& set (Obj& o, const char* name, Bytes data, int formOverride = -1)
    {
        auto* p = m.prop (o.cls, name);
        for (auto& e : o.props) if (p != nullptr && e.pid == p->pid) { e.data = std::move (data); return e; }
        o.props.push_back ({ p != nullptr ? p->pid : (u16) 0, formOverride >= 0 ? (u16) formOverride : m.formOf (p), std::move (data) });
        return o.props.back();
    }
    void setInt (Obj& o, const char* name, juce::int64 v)
    {
        auto* p = m.prop (o.cls, name); auto* t = m.plain (m.type (p->type));
        int size = 4;
        if (t != nullptr && t->kind == aafmodel::TypeKind::Int) size = t->size;
        else if (t != nullptr && t->kind == aafmodel::TypeKind::Enum) { auto* e = m.plain (m.type (t->ref)); size = e != nullptr ? e->size : 1; }
        Bytes b; for (int i = 0; i < size; ++i) b.push_back ((u8) ((u64) v >> (8 * i)));
        set (o, name, std::move (b));
    }
    void setBool (Obj& o, const char* name, bool v) { set (o, name, Bytes { (u8) (v ? 1 : 0) }); }
    void setString (Obj& o, const char* name, const juce::String& s) { set (o, name, utf16 (s)); }
    void setAuid (Obj& o, const char* name, const char* auid) { set (o, name, auidBytes (auid)); }
    void setBytes (Obj& o, const char* name, Bytes b) { set (o, name, std::move (b)); }
    void setRational (Obj& o, const char* name, juce::int32 num, juce::int32 den) { Bytes b; put32 (b, (u32) num); put32 (b, (u32) den); set (o, name, std::move (b)); }
    void setTime (Obj& o, const char* name, const juce::Time& t) { set (o, name, timeStamp (t)); }
    /** A single strongly referenced child: the property holds the child's storage name. */
    Obj& setChild (Obj& o, const char* name, ObjPtr child)
    {
        auto* p = m.prop (o.cls, name);
        const auto storage = mangle (name, p->pid, 32);
        set (o, name, utf16 (storage));
        o.kids.emplace_back (storage, std::move (child));
        return *o.kids.back().second;
    }
    /** A strong reference vector or set: the children are "<name>{i}", with an index stream. A set's index also carries each child's key. */
    struct Collection { Obj* owner; const aafmodel::PropDef* prop; juce::String name; bool isSet; u16 keyPid; u8 keySize; std::vector<std::pair<u32, Bytes>> entries; };
    Collection collection (Obj& o, const char* name, u16 keyPid = 0, u8 keySize = 0)
    {
        auto* p = m.prop (o.cls, name);
        Collection c { &o, p, mangle (name, p->pid, 32 - 10), m.formOf (p) == 0x3a, keyPid, keySize, {} };
        set (o, name, utf16 (c.name));
        return c;
    }
    Obj& add (Collection& c, ObjPtr child, Bytes key = {})
    {
        const u32 local = (u32) c.entries.size();
        c.owner->kids.emplace_back (c.name + "{" + juce::String::toHexString ((int) local) + "}", std::move (child));
        c.entries.push_back ({ local, std::move (key) });
        return *c.owner->kids.back().second;
    }
    void finish (Collection& c)
    {
        Bytes b; put32 (b, (u32) c.entries.size()); put32 (b, (u32) c.entries.size()); put32 (b, 0xffffffffu);
        if (c.isSet) { put16 (b, c.keyPid); b.push_back (c.keySize); }
        for (auto& e : c.entries)
        {
            put32 (b, e.first);
            if (c.isSet) { put32 (b, 1); b.insert (b.end(), e.second.begin(), e.second.end()); }
        }
        c.owner->streams.emplace_back (c.name + " index", std::move (b));
    }
    /** The tag of a weak reference's target set (its place in the "referenced properties" table). */
    u16 tagFor (std::vector<u16> path)
    {
        for (size_t i = 0; i < weakPaths.size(); ++i) if (weakPaths[i] == path) return (u16) i;
        weakPaths.push_back (std::move (path));
        return (u16) (weakPaths.size() - 1);
    }
    void setWeak (Obj& o, const char* name, u16 tag, u16 keyPid, const Bytes& key)
    {
        Bytes b; put16 (b, tag); put16 (b, keyPid); b.push_back ((u8) key.size()); b.insert (b.end(), key.begin(), key.end());
        set (o, name, std::move (b));
    }
    /** A weak reference vector: the property names an index stream holding the keys. */
    void setWeakVector (Obj& o, const char* name, u16 tag, u16 keyPid, const std::vector<Bytes>& keys)
    {
        auto* p = m.prop (o.cls, name);
        const auto nm = mangle (name, p->pid, 32 - 10);
        set (o, name, utf16 (nm));
        Bytes b; put32 (b, (u32) keys.size()); put16 (b, tag); put16 (b, keyPid); b.push_back (keys.empty() ? (u8) 16 : (u8) keys.front().size());
        for (auto& k : keys) b.insert (b.end(), k.begin(), k.end());
        o.streams.emplace_back (nm + " index", std::move (b));
    }
    Bytes referencedProperties() const
    {
        Bytes b; b.push_back (0x4c); put16 (b, (u16) weakPaths.size());
        u32 n = 0; for (auto& p : weakPaths) n += (u32) p.size() + 1;
        put32 (b, n);
        for (auto& p : weakPaths) { for (auto pid : p) put16 (b, pid); put16 (b, 0); }
        return b;
    }
};

// the pids of the places weak references point into (Root -> MetaDictionary -> ClassDefinitions ...): the paths in the "referenced properties" table
constexpr u16 pidMetaDictionary = 0x0001, pidHeader = 0x0002, pidClassDefinitions = 0x0003, pidTypeDefinitions = 0x0004, pidMetaIdentification = 0x0005;
constexpr u16 pidDictionary = 0x3b04, pidDataDefinitions = 0x2605, pidOperationDefinitions = 0x2603, pidContainerDefinitions = 0x2608, pidDefIdentification = 0x1b01;
constexpr u16 pidMobId = 0x4401;

// the definitions used (SMPTE registered ids, from pyaaf2's tables)
constexpr const char* kDataDefSound = "01030202-0200-0000-060e-2b3404010101";
constexpr const char* kDataDefLegacyTimecode = "7f275e81-77e5-11d2-807f-006008143e6f";
constexpr const char* kOpDefMonoAudioDissolve = "0c3bea41-fc05-11d2-8a29-0050040ef7d2";
constexpr const char* kOperationCategoryEffect = "0d010102-0101-0100-060e-2b3404010101";
constexpr const char* kContainerRiffWave = "0d011301-0101-0100-060e-2b3404010106";
constexpr const char* kContainerAaf = "4313b571-d8ba-11d2-809b-006008143e6f";
constexpr const char* kUsageTopLevel = "0d010102-0101-0700-060e-2b3404010101";
constexpr const char* kFermataProductId = "6d1f3c2a-7b84-4e0b-9a51-0c6f2d8e4b17";      // Fermata's own, made once for this purpose
constexpr const char* kClassRoot = "b3b398a5-1c90-11d4-8053-080036210804";
constexpr const char* kTypeHeaderRef = "05022800-0000-0000-060e-2b3401040101";
constexpr const char* kTypeMetaDictionaryRef = "05022700-0000-0000-060e-2b3401040101";

// ============================================================================ the meta dictionary: the model written as objects
void buildMetaDictionary (Builder& b, Obj& md)
{
    using K = aafmodel::TypeKind;
    const auto classTag = b.tagFor ({ pidMetaDictionary, pidClassDefinitions });
    const auto typeTag = b.tagFor ({ pidMetaDictionary, pidTypeDefinitions });
    auto classes = b.collection (md, "ClassDefinitions", pidMetaIdentification, 16);
    for (auto& c : aafmodel::classes)
    {
        if (std::strcmp (c.auid, kClassRoot) == 0) continue;      // (the root is the file itself, not a class the file describes - Pyramix leaves it out too)
        auto cd = b.make ("ClassDefinition");
        b.setAuid (*cd, "Identification", c.auid);
        b.setString (*cd, "Name", c.name);
        b.setWeak (*cd, "ParentClass", classTag, pidMetaIdentification, auidBytes (c.parent != nullptr ? c.parent : c.auid));     // (InterchangeObject's parent is itself)
        b.setBool (*cd, "IsConcrete", c.concrete);
        auto props = b.collection (*cd, "Properties", pidMetaIdentification, 16);
        for (int i = 0; i < c.numProps; ++i)
        {
            auto& p = c.props[i];
            auto pd = b.make ("PropertyDefinition");
            b.setAuid (*pd, "Identification", p.auid);
            b.setString (*pd, "Name", p.name);
            b.setAuid (*pd, "Type", p.type);
            b.setInt (*pd, "LocalIdentification", p.pid);
            b.setBool (*pd, "IsOptional", p.optional);
            b.setBool (*pd, "IsUniqueIdentifier", p.unique);
            b.add (props, std::move (pd), auidBytes (p.auid));
        }
        b.finish (props);
        b.add (classes, std::move (cd), auidBytes (c.auid));
    }
    b.finish (classes);

    auto types = b.collection (md, "TypeDefinitions", pidMetaIdentification, 16);
    for (auto& t : aafmodel::types)
    {
        if (std::strcmp (t.auid, kTypeHeaderRef) == 0 || std::strcmp (t.auid, kTypeMetaDictionaryRef) == 0) continue;     // (the root's own two types)
        const char* className = nullptr;
        switch (t.kind)
        {
            case K::Int:        className = "TypeDefinitionInteger"; break;
            case K::Enum:       className = "TypeDefinitionEnumeration"; break;
            case K::Record:     className = "TypeDefinitionRecord"; break;
            case K::FixedArray: className = "TypeDefinitionFixedArray"; break;
            case K::VarArray:   className = "TypeDefinitionVariableArray"; break;
            case K::Rename:     className = "TypeDefinitionRename"; break;
            case K::String:     className = "TypeDefinitionString"; break;
            case K::Stream:     className = "TypeDefinitionStream"; break;
            case K::Opaque:     className = "TypeDefinitionOpaque"; break;
            case K::ExtEnum:    className = "TypeDefinitionExtendibleEnumeration"; break;
            case K::Character:  className = "TypeDefinitionCharacter"; break;
            case K::Indirect:   className = "TypeDefinitionIndirect"; break;
            case K::Set:        className = "TypeDefinitionSet"; break;
            case K::StrongRef:  className = "TypeDefinitionStrongObjectReference"; break;
            case K::WeakRef:    className = "TypeDefinitionWeakObjectReference"; break;
        }
        auto td = b.make (className);
        b.setAuid (*td, "Identification", t.auid);
        b.setString (*td, "Name", t.name);
        auto stringArray = [] (const char* const* names, int n) { Bytes out; for (int i = 0; i < n; ++i) putUtf16 (out, names[i]); return out; };
        auto auidArray = [] (const char* const* auids, int n) { Bytes out; for (int i = 0; i < n; ++i) { auto a = auidBytes (auids[i]); out.insert (out.end(), a.begin(), a.end()); } return out; };
        switch (t.kind)
        {
            case K::Int:        b.setInt (*td, "Size", t.size); b.setBool (*td, "IsSigned", t.isSigned); break;
            case K::Enum:       b.setWeak (*td, "ElementType", typeTag, pidMetaIdentification, auidBytes (t.ref));
                                b.setBytes (*td, "ElementNames", stringArray (t.names, t.numNames));
                                { Bytes v; for (int i = 0; i < t.numNames; ++i) put64 (v, (u64) t.values[i]); b.setBytes (*td, "ElementValues", std::move (v)); }
                                break;
            case K::Record:     { std::vector<Bytes> keys; for (int i = 0; i < t.numAuids; ++i) keys.push_back (auidBytes (t.auids[i]));
                                  b.setWeakVector (*td, "MemberTypes", typeTag, pidMetaIdentification, keys); }
                                b.setBytes (*td, "MemberNames", stringArray (t.names, t.numNames));
                                break;
            case K::FixedArray: b.setWeak (*td, "ElementType", typeTag, pidMetaIdentification, auidBytes (t.ref)); b.setInt (*td, "ElementCount", t.size); break;
            case K::VarArray:
            case K::Set:
            case K::String:     b.setWeak (*td, "ElementType", typeTag, pidMetaIdentification, auidBytes (t.ref)); break;
            case K::Rename:     b.setWeak (*td, "RenamedType", typeTag, pidMetaIdentification, auidBytes (t.ref)); break;
            case K::ExtEnum:    b.setBytes (*td, "ElementNames", stringArray (t.names, t.numNames)); b.setBytes (*td, "ElementValues", auidArray (t.auids, t.numAuids)); break;
            case K::StrongRef:  b.setWeak (*td, "ReferencedType", classTag, pidMetaIdentification, auidBytes (t.ref)); break;
            case K::WeakRef:    b.setWeak (*td, "ReferencedType", classTag, pidMetaIdentification, auidBytes (t.ref)); b.setBytes (*td, "TargetSet", auidArray (t.auids, t.numAuids)); break;
            default: break;
        }
        b.add (types, std::move (td), auidBytes (t.auid));
    }
    b.finish (types);
}

// ============================================================================ the compound file (Microsoft structured storage, version 4: 4096-byte sectors)
struct Cfb
{
    static constexpr u32 kSector = 4096, kMini = 64, kEnd = 0xfffffffeu, kFree = 0xffffffffu, kFatSect = 0xfffffffdu, kNoStream = 0xffffffffu;
    struct Entry
    {
        juce::String name; u8 type = 1;      // 1 storage, 2 stream, 5 root
        u32 left = kNoStream, right = kNoStream, child = kNoStream;
        Bytes clsid = Bytes (16, 0);
        Bytes data;                          // streams
        u32 start = 0; u64 size = 0;                 // (a storage's start is 0; an empty stream's is end-of-chain)
    };
    std::vector<Entry> entries;

    /** The directory tree: storages hold their children in a balanced binary tree ordered the compound-file way (shorter names first, then by
        upper-case letters). */
    static bool before (const juce::String& a, const juce::String& b)
    {
        if (a.length() != b.length()) return a.length() < b.length();
        return a.toUpperCase().compare (b.toUpperCase()) < 0;
    }
    u32 addObject (const Obj& o, const juce::String& name, bool isRoot)
    {
        const u32 me = (u32) entries.size();
        entries.push_back ({});
        entries[me].name = name; entries[me].type = isRoot ? 5 : 1; entries[me].clsid = auidBytes (o.cls->auid);
        std::vector<u32> children;
        {
            Entry e; e.name = "properties"; e.type = 2; e.data = o.propertiesStream();
            children.push_back ((u32) entries.size()); entries.push_back (std::move (e));
        }
        for (auto& s : o.streams) { Entry e; e.name = s.first; e.type = 2; e.data = s.second; children.push_back ((u32) entries.size()); entries.push_back (std::move (e)); }
        for (auto& k : o.kids) children.push_back (addObject (*k.second, k.first, false));
        entries[me].child = tree (children);
        return me;
    }
    u32 addStream (u32 parent, const juce::String& name, Bytes data)
    {
        Entry e; e.name = name; e.type = 2; e.data = std::move (data);
        const u32 id = (u32) entries.size(); entries.push_back (std::move (e));
        // (joins the parent's existing tree: rebuilt from the list of its children)
        std::vector<u32> children; collect (entries[parent].child, children); children.push_back (id);
        entries[parent].child = tree (children);
        return id;
    }
    void collect (u32 id, std::vector<u32>& out) const
    {
        if (id == kNoStream) return;
        collect (entries[id].left, out); out.push_back (id); collect (entries[id].right, out);
    }
    u32 tree (std::vector<u32> ids)
    {
        std::sort (ids.begin(), ids.end(), [this] (u32 a, u32 b) { return before (entries[a].name, entries[b].name); });
        return build (ids, 0, (int) ids.size());
    }
    u32 build (const std::vector<u32>& ids, int from, int to)
    {
        if (from >= to) return kNoStream;
        const int mid = (from + to) / 2;
        entries[ids[(size_t) mid]].left = build (ids, from, mid);
        entries[ids[(size_t) mid]].right = build (ids, mid + 1, to);
        return ids[(size_t) mid];
    }

    /** Lays the file out and writes it. */
    juce::String write (const juce::File& file, const Bytes& headerClsid)
    {
        for (auto& e : entries) if (e.name.length() > 31) return "A name inside the AAF is too long: " + e.name;
        // the small streams go into the mini stream (64-byte pieces); the big ones and the directory get sectors of their own
        Bytes mini; std::vector<u32> miniFat;
        std::vector<Bytes> sectors;        // the file's sectors after the header, in order; the FAT is appended at the end
        std::vector<u32> fat;
        auto addChain = [&] (const Bytes& data) -> u32      // appends a chain of sectors, returns its first
        {
            if (data.empty()) return kEnd;
            const u32 first = (u32) sectors.size();
            for (size_t off = 0; off < data.size(); off += kSector)
            {
                Bytes s (kSector, 0); std::memcpy (s.data(), data.data() + off, juce::jmin ((size_t) kSector, data.size() - off));
                sectors.push_back (std::move (s));
                fat.push_back ((u32) sectors.size());      // the next sector (fixed to end-of-chain below)
            }
            fat.back() = kEnd;
            return first;
        };
        for (auto& e : entries)
        {
            if (e.type != 2) continue;
            e.size = e.data.size();
            if (e.data.empty()) { e.start = kEnd; continue; }
            if (e.data.size() < kSector)
            {
                e.start = (u32) (mini.size() / kMini);
                for (size_t off = 0; off < e.data.size(); off += kMini)
                {
                    Bytes s (kMini, 0); std::memcpy (s.data(), e.data.data() + off, juce::jmin ((size_t) kMini, e.data.size() - off));
                    mini.insert (mini.end(), s.begin(), s.end());
                    miniFat.push_back ((u32) (mini.size() / kMini));
                }
                miniFat.back() = kEnd;
            }
            else e.start = addChain (e.data);
        }
        // the directory
        const size_t dirBytes = ((entries.size() * 128 + kSector - 1) / kSector) * kSector;
        Bytes dir (dirBytes, 0);
        // the mini stream belongs to the root entry
        const u32 miniStart = addChain (mini);
        entries[0].start = miniStart; entries[0].size = mini.size();
        Bytes miniFatBytes; for (auto v : miniFat) put32 (miniFatBytes, v);
        while (miniFatBytes.size() % kSector != 0) put32 (miniFatBytes, kFree);
        const u32 miniFatStart = addChain (miniFatBytes);
        const u32 numMiniFat = miniFatBytes.empty() ? 0 : (u32) (miniFatBytes.size() / kSector);
        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto& e = entries[i]; u8* d = dir.data() + i * 128;
            Bytes nm = utf16 (e.name); std::memcpy (d, nm.data(), juce::jmin ((size_t) 64, nm.size()));
            d[64] = (u8) nm.size(); d[65] = (u8) (nm.size() >> 8);
            d[66] = e.type; d[67] = 1;      // black
            auto w32 = [&] (int at, u32 v) { d[at] = (u8) v; d[at + 1] = (u8) (v >> 8); d[at + 2] = (u8) (v >> 16); d[at + 3] = (u8) (v >> 24); };
            w32 (68, e.left); w32 (72, e.right); w32 (76, e.child);
            std::memcpy (d + 80, e.clsid.data(), 16);
            w32 (116, e.start); w32 (120, (u32) e.size); w32 (124, (u32) (e.size >> 32));
        }
        for (size_t i = entries.size(); i * 128 < dirBytes; ++i)      // unused entries: no left / right / child
        {
            u8* d = dir.data() + i * 128; std::memset (d + 68, 0xff, 12); std::memset (d + 116, 0, 4);
        }
        const u32 dirStart = addChain (dir);
        const u32 numDir = (u32) (dirBytes / kSector);
        // the FAT itself: as many sectors as it takes to cover everything including the FAT's own sectors
        u32 numFat = 0;
        while (true)
        {
            const u32 total = (u32) sectors.size() + numFat;
            const u32 need = (total + kSector / 4 - 1) / (kSector / 4);
            if (need <= numFat) break;
            numFat = need;
        }
        if (numFat > 109) return "The AAF would be too big (more than 440 MB).";
        const u32 fatStart = (u32) sectors.size();
        for (u32 i = 0; i < numFat; ++i) fat.push_back (kFatSect);
        while (fat.size() % (kSector / 4) != 0) fat.push_back (kFree);
        Bytes fatBytes; for (auto v : fat) put32 (fatBytes, v);
        for (u32 i = 0; i < numFat; ++i) { Bytes s (fatBytes.begin() + (long) (i * kSector), fatBytes.begin() + (long) ((i + 1) * kSector)); sectors.push_back (std::move (s)); }
        // the header (512 bytes, then zeros to the first sector boundary)
        Bytes h;
        static const u8 magic[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        h.insert (h.end(), magic, magic + 8);
        h.insert (h.end(), headerClsid.begin(), headerClsid.end());
        put16 (h, 0x3e); put16 (h, 4); put16 (h, 0xfffe); put16 (h, 12); put16 (h, 6);
        for (int i = 0; i < 6; ++i) h.push_back (0);
        put32 (h, numDir); put32 (h, numFat); put32 (h, dirStart); put32 (h, 0); put32 (h, kSector);
        put32 (h, miniFatStart); put32 (h, numMiniFat); put32 (h, kEnd); put32 (h, 0);
        for (u32 i = 0; i < 109; ++i) put32 (h, i < numFat ? fatStart + i : kFree);
        h.resize (kSector, 0);
        // out it goes
        juce::TemporaryFile tmp (file);
        {
            juce::FileOutputStream out (tmp.getFile());
            if (! out.openedOk()) return "Cannot write " + file.getFullPathName();
            out.write (h.data(), h.size());
            for (auto& s : sectors) out.write (s.data(), s.size());
            out.flush();
            if (out.getStatus().failed()) return "Writing " + file.getFullPathName() + " failed: " + out.getStatus().getErrorMessage();
        }
        if (! tmp.overwriteTargetFileWithTemporary()) return "Cannot replace " + file.getFullPathName();
        return {};
    }
};

// ============================================================================ the document as mobs
juce::String fileUrl (const juce::File& f)
{
    // file:///C:/Audio/Take%20003.wav - the form Pyramix and Pro Tools write (a path with forward slashes, the special characters escaped)
    auto path = f.getFullPathName().replaceCharacter ('\\', '/');
    if (! path.startsWith ("/")) path = "/" + path;
    return "file://" + juce::URL::addEscapeChars (path, false, false).replace ("%2F", "/").replace ("%3A", ":");
}

/** The WAV header a WAVEDescriptor's Summary carries (everything but the samples): RIFF, WAVE, a plain PCM fmt chunk, the data chunk's header. */
Bytes wavSummary (const SourceFile& f)
{
    Bytes b;
    const u32 bytesPerFrame = (u32) f.channels * (u32) ((f.bits + 7) / 8);
    const u64 dataBytes = (u64) f.lengthSamples * bytesPerFrame;
    auto tag = [&] (const char* t) { b.insert (b.end(), t, t + 4); };
    tag ("RIFF"); put32 (b, (u32) juce::jmin<u64> (0xffffffffu, dataBytes + 36)); tag ("WAVE");
    tag ("fmt "); put32 (b, 16); put16 (b, 1); put16 (b, (u16) f.channels); put32 (b, (u32) std::lround (f.sampleRate));
    put32 (b, (u32) std::lround (f.sampleRate) * bytesPerFrame); put16 (b, (u16) bytesPerFrame); put16 (b, (u16) f.bits);
    tag ("data"); put32 (b, (u32) juce::jmin<u64> (0xffffffffu, dataBytes));
    return b;
}

struct Mobs
{
    Builder& b; const Document& doc; const juce::Time now;
    u16 dataTag = 0, containerTag = 0, operationTag = 0;
    Bytes tapeId;
    std::vector<Bytes> masterIds;      // per file
    Mobs (Builder& bb, const Document& d) : b (bb), doc (d), now (juce::Time::getCurrentTime()) {}

    void weakDef (Obj& o, const char* prop, const char* auid, u16 tag) { b.setWeak (o, prop, tag, pidDefIdentification, auidBytes (auid)); }
    void component (Obj& o, juce::int64 length, const char* dataDef) { weakDef (o, "DataDefinition", dataDef, dataTag); b.setInt (o, "Length", length); }
    ObjPtr sourceClip (const Bytes& mobId, u32 slotId, juce::int64 start, juce::int64 length)
    {
        auto c = b.make ("SourceClip");
        component (*c, length, kDataDefSound);
        b.setBytes (*c, "SourceID", mobId); b.setInt (*c, "SourceMobSlotID", slotId); b.setInt (*c, "StartTime", start);
        return c;
    }
    ObjPtr filler (juce::int64 length) { auto f = b.make ("Filler"); component (*f, length, kDataDefSound); return f; }
    ObjPtr sequenceOf (std::vector<ObjPtr> parts, juce::int64 length)
    {
        auto s = b.make ("Sequence"); component (*s, length, kDataDefSound);
        auto comps = b.collection (*s, "Components");
        for (auto& p : parts) b.add (comps, std::move (p));
        b.finish (comps);
        return s;
    }
    void mobHeader (Obj& mob, const juce::String& name, const Bytes& id)
    {
        b.setBytes (mob, "MobID", id); b.setString (mob, "Name", name); b.setTime (mob, "CreationTime", now); b.setTime (mob, "LastModified", now);
    }
    Obj& timelineSlot (Builder::Collection& slots, u32 slotId, const juce::String& name, juce::int32 rateNum, juce::int32 rateDen, u32 physical, ObjPtr segment)
    {
        auto s = b.make ("TimelineMobSlot");
        b.setInt (*s, "SlotID", slotId); b.setString (*s, "SlotName", name); b.setRational (*s, "EditRate", rateNum, rateDen); b.setInt (*s, "Origin", 0);
        b.setInt (*s, "PhysicalTrackNumber", physical);
        b.setChild (*s, "Segment", std::move (segment));
        return b.add (slots, std::move (s));
    }
    ObjPtr timecode (juce::int64 startFrames, juce::int64 lengthFrames)
    {
        auto t = b.make ("Timecode");
        component (*t, lengthFrames, kDataDefLegacyTimecode);
        b.setInt (*t, "Start", startFrames); b.setInt (*t, "FPS", (juce::int64) std::lround (doc.fps)); b.setBool (*t, "Drop", doc.drop);
        return t;
    }
    juce::int32 fpsNum() const { return doc.drop || std::fabs (doc.fps - std::round (doc.fps)) > 0.01 ? (juce::int32) std::lround (doc.fps * 1001.0) : (juce::int32) std::lround (doc.fps); }
    juce::int32 fpsDen() const { return doc.drop || std::fabs (doc.fps - std::round (doc.fps)) > 0.01 ? 1001 : 1; }
    juce::int32 rate() const { return (juce::int32) std::lround (doc.sampleRate); }

    void build (Builder::Collection& mobs)
    {
        const juce::int64 dayFrames = (juce::int64) std::llround (86400.0 * doc.fps);
        const juce::int64 daySamples = (juce::int64) std::llround (86400.0 * doc.sampleRate);
        // ---- the generic source: a 24-hour tape whose timecode starts at 00:00:00:00, so a position on it is a time of day
        {
            auto tape = b.make ("SourceMob");
            tapeId = newMobId();
            mobHeader (*tape, "Generic Source Mob", tapeId);
            auto slots = b.collection (*tape, "Slots");
            timelineSlot (slots, 1, "TimeCode Track", fpsNum(), fpsDen(), 1, timecode (0, dayFrames));
            std::vector<ObjPtr> parts; parts.push_back (sourceClip (nullMobId(), 0, 0, daySamples));
            timelineSlot (slots, 3, "", rate(), 1, 0, sequenceOf (std::move (parts), daySamples));
            b.finish (slots);
            auto desc = b.make ("SoundDescriptor");
            b.setRational (*desc, "SampleRate", rate(), 1); b.setInt (*desc, "Length", daySamples);
            weakDef (*desc, "ContainerFormat", kContainerAaf, containerTag);
            b.setRational (*desc, "AudioSamplingRate", rate(), 1); b.setInt (*desc, "Channels", 1); b.setInt (*desc, "QuantizationBits", 24);
            b.setChild (*tape, "EssenceDescription", std::move (desc));
            b.add (mobs, std::move (tape), tapeId);
        }
        // ---- every file: a source mob (the file and where it sits on the tape) and a master mob (the clip the timeline refers to)
        for (auto& f : doc.files)
        {
            const auto fileId = newMobId(), masterId = newMobId();
            masterIds.push_back (masterId);
            const juce::int64 onTape = f.startSeconds >= 0.0 ? (juce::int64) std::llround (f.startSeconds * doc.sampleRate) : 0;
            auto src = b.make ("SourceMob");
            mobHeader (*src, f.name, fileId);
            auto slots = b.collection (*src, "Slots");
            for (int ch = 0; ch < f.channels; ++ch)
            {
                std::vector<ObjPtr> parts; parts.push_back (sourceClip (tapeId, 3, onTape, f.lengthSamples));
                timelineSlot (slots, (u32) ch + 1, "", rate(), 1, (u32) ch + 1, sequenceOf (std::move (parts), f.lengthSamples));
            }
            b.finish (slots);
            auto desc = b.make ("WAVEDescriptor");
            b.setRational (*desc, "SampleRate", rate(), 1); b.setInt (*desc, "Length", f.lengthSamples);
            weakDef (*desc, "ContainerFormat", kContainerRiffWave, containerTag);
            b.setBytes (*desc, "Summary", wavSummary (f));
            auto locs = b.collection (*desc, "Locator");
            auto loc = b.make ("NetworkLocator"); b.setString (*loc, "URLString", fileUrl (f.file));
            b.add (locs, std::move (loc)); b.finish (locs);
            b.setChild (*src, "EssenceDescription", std::move (desc));
            b.add (mobs, std::move (src), fileId);

            auto master = b.make ("MasterMob");
            mobHeader (*master, f.name, masterId);
            auto mslots = b.collection (*master, "Slots");
            for (int ch = 0; ch < f.channels; ++ch)
            {
                std::vector<ObjPtr> parts; parts.push_back (sourceClip (fileId, (u32) ch + 1, 0, f.lengthSamples));
                timelineSlot (mslots, (u32) ch + 1, "", rate(), 1, (u32) ch + 1, sequenceOf (std::move (parts), f.lengthSamples));
            }
            b.finish (mslots);
            b.add (mobs, std::move (master), masterId);
        }
        // ---- the timeline
        {
            auto comp = b.make ("CompositionMob");
            const auto compId = newMobId();
            mobHeader (*comp, doc.name, compId);
            b.setAuid (*comp, "UsageCode", kUsageTopLevel);
            auto slots = b.collection (*comp, "Slots");
            const juce::int64 lengthFrames = (juce::int64) std::llround ((double) doc.length / doc.sampleRate * doc.fps);
            timelineSlot (slots, 1, "TimeCode Track", fpsNum(), fpsDen(), 1, timecode ((juce::int64) std::llround (doc.timelineStartSeconds * doc.fps), lengthFrames));
            u32 slotId = 3, physical = 1;
            for (auto& t : doc.tracks)
            {
                std::vector<ObjPtr> parts;
                juce::int64 pos = 0;
                for (size_t i = 0; i < t.clips.size(); ++i)
                {
                    const auto& c = t.clips[i];
                    if (c.file >= doc.files.size()) continue;
                    juce::int64 overlap = pos - c.at;       // > 0: this clip begins before the previous one ends - a crossfade
                    const bool afterClip = ! parts.empty() && std::strcmp (parts.back()->cls->name, "SourceClip") == 0;
                    if (overlap > 0 && afterClip && overlap <= c.length)
                    {
                        // the previous clip already runs to its end; the transition sits over the last 'overlap' samples of it and the first of this
                        auto tr = b.make ("Transition");
                        component (*tr, overlap, kDataDefSound);
                        b.setInt (*tr, "CutPoint", c.cutPoint >= 0 ? juce::jlimit<juce::int64> (0, overlap, c.cutPoint) : overlap / 2);
                        auto og = b.make ("OperationGroup");
                        component (*og, overlap, kDataDefSound);
                        weakDef (*og, "Operation", kOpDefMonoAudioDissolve, operationTag);
                        b.setChild (*tr, "OperationGroup", std::move (og));
                        // the previous clip's fade-out and this one's fade-in say the same thing again, for readers that skip transitions
                        b.setInt (*parts.back(), "FadeOutLength", overlap); b.setInt (*parts.back(), "FadeOutType", 2);
                        parts.push_back (std::move (tr));
                        pos -= overlap;
                    }
                    else if (c.at > pos) { parts.push_back (filler (c.at - pos)); pos = c.at; }
                    else if (c.at < pos) continue;         // (overlapping more than its own length, or out of order: cannot be represented, left out)
                    auto clip = sourceClip (masterIds[c.file], (u32) c.channel + 1, c.sourceStart, c.length);
                    if (overlap > 0 && afterClip && overlap <= c.length) { b.setInt (*clip, "FadeInLength", overlap); b.setInt (*clip, "FadeInType", 2); }
                    else if (c.fadeIn > 0) { b.setInt (*clip, "FadeInLength", juce::jmin (c.fadeIn, c.length)); b.setInt (*clip, "FadeInType", 2); }
                    if (c.fadeOut > 0) { b.setInt (*clip, "FadeOutLength", juce::jmin (c.fadeOut, c.length)); b.setInt (*clip, "FadeOutType", 2); }
                    parts.push_back (std::move (clip));
                    pos += c.length;
                }
                if (pos < doc.length) { parts.push_back (filler (doc.length - pos)); pos = doc.length; }
                timelineSlot (slots, slotId++, t.name, rate(), 1, physical++, sequenceOf (std::move (parts), pos));
            }
            b.finish (slots);
            b.add (mobs, std::move (comp), compId);
        }
    }
};
} // namespace

juce::String writeAaf (const Document& doc, const juce::File& out)
{
    if (doc.sampleRate <= 0.0 || doc.fps <= 0.0) return "The AAF needs a sample rate and a frame rate.";
    for (auto& t : doc.tracks) for (auto& c : t.clips) if (c.file >= doc.files.size()) return "A clip refers to a file the document does not have.";
    static const Model model;
    Builder b (model);
    Obj root; root.cls = model.classByAuid.at (kClassRoot);

    // the root's two children
    auto& md = b.setChild (root, "MetaDictionary", b.make ("MetaDictionary"));
    auto& header = b.setChild (root, "Header", b.make ("Header"));
    buildMetaDictionary (b, md);

    const auto now = juce::Time::getCurrentTime();
    b.setInt (header, "ByteOrder", 0x4949); b.setTime (header, "LastModified", now);
    b.setBytes (header, "Version", Bytes { 1, 1 }); b.setInt (header, "ObjectModelVersion", 1);
    auto idents = b.collection (header, "IdentificationList");
    {
        auto id = b.make ("Identification");
        b.setString (*id, "CompanyName", "Fermata"); b.setString (*id, "ProductName", "Fermata");
        b.setString (*id, "ProductVersionString", doc.productVersion.isNotEmpty() ? doc.productVersion : juce::String ("0"));
        b.setAuid (*id, "ProductID", kFermataProductId); b.setTime (*id, "Date", now);
        b.setString (*id, "Platform", juce::SystemStats::getOperatingSystemName());
        auto gen = juce::Uuid(); b.setBytes (*id, "GenerationAUID", Bytes (gen.getRawData(), gen.getRawData() + 16));
        b.add (idents, std::move (id));
    }
    b.finish (idents);

    // the dictionary: the definitions the mobs refer to
    auto& dict = b.setChild (header, "Dictionary", b.make ("Dictionary"));
    Mobs mobs (b, doc);
    mobs.dataTag = b.tagFor ({ pidHeader, pidDictionary, pidDataDefinitions });
    mobs.containerTag = b.tagFor ({ pidHeader, pidDictionary, pidContainerDefinitions });
    mobs.operationTag = b.tagFor ({ pidHeader, pidDictionary, pidOperationDefinitions });
    {
        auto defs = b.collection (dict, "DataDefinitions", pidDefIdentification, 16);
        for (auto d : { std::make_pair (kDataDefSound, "DataDef_Sound"), std::make_pair (kDataDefLegacyTimecode, "DataDef_LegacyTimecode") })
        {
            auto o = b.make ("DataDefinition"); b.setAuid (*o, "Identification", d.first); b.setString (*o, "Name", d.second); b.setString (*o, "Description", "");
            b.add (defs, std::move (o), auidBytes (d.first));
        }
        b.finish (defs);
        auto conts = b.collection (dict, "ContainerDefinitions", pidDefIdentification, 16);
        for (auto d : { std::make_pair (kContainerRiffWave, "ContainerDef_RIFFWAVE"), std::make_pair (kContainerAaf, "ContainerDef_AAF") })
        {
            auto o = b.make ("ContainerDefinition"); b.setAuid (*o, "Identification", d.first); b.setString (*o, "Name", d.second); b.setString (*o, "Description", "");
            b.add (conts, std::move (o), auidBytes (d.first));
        }
        b.finish (conts);
        auto ops = b.collection (dict, "OperationDefinitions", pidDefIdentification, 16);
        {
            auto o = b.make ("OperationDefinition");
            b.setAuid (*o, "Identification", kOpDefMonoAudioDissolve); b.setString (*o, "Name", "Mono Audio Dissolve"); b.setString (*o, "Description", "");
            mobs.weakDef (*o, "DataDefinition", kDataDefSound, mobs.dataTag);
            b.setBool (*o, "IsTimeWarp", false); b.setInt (*o, "Bypass", 1); b.setInt (*o, "NumberInputs", 2); b.setAuid (*o, "OperationCategory", kOperationCategoryEffect);
            b.add (ops, std::move (o), auidBytes (kOpDefMonoAudioDissolve));
        }
        b.finish (ops);
    }
    // the content: the mobs
    auto& content = b.setChild (header, "Content", b.make ("ContentStorage"));
    auto mobSet = b.collection (content, "Mobs", pidMobId, 32);
    mobs.build (mobSet);
    b.finish (mobSet);

    // the compound file
    Cfb cfb;
    const u32 rootEntry = cfb.addObject (root, "Root Entry", true);
    cfb.addStream (rootEntry, "referenced properties", b.referencedProperties());
    return cfb.write (out, auidBytes ("0d010201-0200-0000-060e-2b3403020101"));      // (the signature of an AAF in a 4096-byte-sector compound file)
}
} // namespace td::aafwrite
