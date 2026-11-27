// TW3SE core - Lua on top of the RTTI: the game's objects, fields, enums and native functions, live from Lua.
//
//   lua <code>          console: runs a chunk, replies with what it printed and returned
//   luafile <name>      console: runs tw3se\lua\<name>.lua
//   tw3se\lua\autorun\*.lua   run once, at the first tick of the game
//
// In Lua (table `w3`):
//   local p = w3.player()              the player object (nil in the main menu); w3.global("theGame") ...
//   p.someField, p.someField = 5       read / write a field by name (inherited ones too)
//   p:SomeNative(1, "a")               call a native method; w3.call("GlobalNative", ...) a global one
//   w3.enum("EDeathType")              { EDT_Default = 0, ... }
//   w3.class("CR4Player")              { name, parent, size, fields = {name = {type, offset}}, methods = {...} }
//   w3.find("weather")                 names of the types that contain the text
//   w3.fields(obj), w3.classof(obj)    what an object has / its class name
//   w3.on_tick(function() ... end)     called once per game tick
//
// Values follow the RTTI type of the field or return value (docs/engine/scripting.md section 11): numbers and
// bools, CName and String as Lua strings, enums as their value name, handles and pointers as objects, embedded
// structs as views into the object, arrays as Lua tables (a copy). Writes: numbers, bools, enums, CName.
//
// What a call can pass: int, float, bool, string and name as literals (push-opcodes measured in game, section 2);
// enums, objects (as handles) and plain structs through a nested call to TW3SE_LuaArg<i> (opcode 0x1c, measured).
// A function is only called when the scripts declared it (import: its parameters are known), it is
// native and not latent; anything else is refused with the reason, never guessed (a wrong opcode can corrupt the
// VM).
// Every memory read goes through SafeRead; a fault inside a native is caught and reported.
//
// Lua runs on the main thread only (console commands and ticks both run there). Lua is compiled as C++, so a Lua
// error unwinds C++ frames properly.
#include "core.h"
#include <algorithm>
#include <set>
#include <cmath>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <regex>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "lua_ui.h"
namespace tw3se {
void LuaProcOpen(lua_State* L);  // lua_proc.cpp
void LuaLinkOpen(lua_State* L);  // lua_link.cpp
void LuaFsOpen(lua_State* L);    // lua_fs.cpp
void LuaInputOpen(lua_State* L); // lua_input.cpp
}

namespace tw3se {
void InstallPluginFns(lua_State* L);  // w3.plugin.<name> from the plugins (defined below, outside the unnamed namespace)
namespace {

lua_State* g_L = nullptr;
bool g_luaOn = false;  // InstallLua ran (the REDkit editor may register only the natives)
std::string* g_out = nullptr;  // where print goes while a console command runs
bool g_autorunDone = false;
const char* const kObj = "tw3se.object";

struct Obj {
    void* ptr;
    void* cls;
    bool isStruct;  // a struct view: fields at ptr + offset, no script-data redirect, no methods
    void* block;    // the handle block {refs, flags, object} the object came through, or null (needed to pass it)
    bool owns;      // holds one reference on block, given back when Lua collects the value
    void* ownerBlock;  // a struct view: the object it lies in, alive while this block still points at ownerPtr
    void* ownerPtr;
};
// The object a view (a struct or an array inside it) lies in, set while its field or element is pushed. A view
// checks it at each use: written through a view of a gone object, the handle went into memory that was another's by
// then - an 8-byte pointer in the middle of PhysX's data, the game fell at a world's start minutes later (27.11.).
struct ViewOwner {
    void* block;
    void* ptr;
};
thread_local ViewOwner g_viewOwner{};
bool OwnerGone(void* block, void* ptr) {
    void* now = nullptr;
    return block && (!SafeRead(static_cast<char*>(block) + 0x08, &now, 8) || now != ptr);
}
// A view of a gone object used: the error, and the first 20 into tw3se.log with the Lua stack (most such calls run
// under pcall, which swallows the error - the log names the line that would have written into another's memory)
[[noreturn]] void ViewGone(lua_State* L, const std::string& what) {
    static int logged = 0;
    if (logged < 20) {
        ++logged;
        luaL_traceback(L, L, what.c_str(), 1);
        Log("lua: %s (mod %s)", lua_tostring(L, -1), CurrentLuaMod()[0] ? CurrentLuaMod() : "-");
        lua_pop(L, 1);
    }
    luaL_error(L, "%s", what.c_str());
    for (;;) {}
}
struct OwnerScope {  // the owner for the views pushed in this scope
    ViewOwner was;
    explicit OwnerScope(ViewOwner now) : was(g_viewOwner) { g_viewOwner = now; }
    ~OwnerScope() { g_viewOwner = was; }
};

template <class T> T Rd(const void* base, size_t off, T def = T()) {
    T v;
    return SafeRead(static_cast<const char*>(base) + off, &v, sizeof v) ? v : def;
}

bool Plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7fffffffffffULL;
}

void* ClassOfGuarded(void* obj) {
    __try {
        return GetObjectClass(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string NameOfId(uint32_t id) {
    const char* s = GetCNameString(id);
    return s ? s : "";
}

// --- RTTI walks (CClass: parent +0x10, fields +0x30/+0x38, functions +0x48/+0x50; CProperty: type +0x08,
// name +0x10, offset +0x20, flags +0x24; CFunction: name +0x10, flags +0x18, return +0x20, params +0x28/+0x30)

template <class F> void ForEachClassItem(void* cls, size_t listOff, F visit) {
    for (int depth = 0; Plausible(cls) && depth < 64; ++depth, cls = Rd<void*>(cls, 0x10)) {
        void** arr = Rd<void**>(cls, listOff);
        uint32_t n = Rd<uint32_t>(cls, listOff + 8);
        if (!Plausible(arr) || n > 8192) continue;
        for (uint32_t i = 0; i < n; ++i) {
            void* e = Rd<void*>(arr, i * sizeof(void*));
            if (Plausible(e) && visit(e, cls)) return;
        }
    }
}

// A field or method lookup is a walk up the parent chain scanning name ids; it runs on every p.field and p:m()
// access. The result is cached by (class, name). A script reload frees the old CFunctions and CProperties, so
// LuaScriptsRecompiling clears both caches (a cached GetMoney called after a live reload would run freed memory,
// leave the engine allocator's lock taken and freeze the game).
std::unordered_map<void*, std::unordered_map<std::string, void*>> g_propCache;
std::unordered_map<void*, std::unordered_map<std::string, std::vector<void*>>> g_methodCache;

void* FindProp(void* cls, const char* name) {
    auto& slot = g_propCache[cls];
    auto it = slot.find(name);
    if (it != slot.end()) return it->second;
    void* found = nullptr;
    ForEachClassItem(cls, 0x30, [&](void* p, void*) {
        if (NameOfId(Rd<uint32_t>(p, 0x10)) != name) return false;
        found = p;
        return true;
    });
    slot[name] = found;  // a miss is cached too (null), so a wrong name is not re-scanned every frame
    return found;
}

const std::vector<void*>& FindMethods(void* cls, const char* name) {
    auto& slot = g_methodCache[cls];
    auto it = slot.find(name);
    if (it != slot.end()) return it->second;
    std::vector<void*>& out = slot[name];
    ForEachClassItem(cls, 0x48, [&](void* f, void*) {
        if (NameOfId(Rd<uint32_t>(f, 0x10)) == name) out.push_back(f);
        return false;
    });
    return out;
}

void* FieldAddr(const Obj& o, void* prop) {
    char* base = static_cast<char*>(o.ptr);
    if (!o.isStruct && (Rd<uint32_t>(prop, 0x24) & 0x20)) {  // script var: in the script-data block at +0x20
        base = Rd<char*>(o.ptr, 0x20);
        if (!Plausible(base)) return nullptr;
    }
    return base + Rd<uint32_t>(prop, 0x20);
}

// --- values

// --- object lifetime. A handle block is {int refs, int flags, object*}; the object's own pointer at +0x08
// counts as one ref. The engine's release (seen in CInputManager::RegisterListener): refs - 1; at 1 with flag
// 0x2 it destroys the object (vtable slot 0x18), at 0 it frees the block. A Lua object holds one ref, so the block
// outlives a destroyed object and the access sees the object pointer gone. Lua's release never takes the last refs
// (the engine's own steps stay with the engine): at worst an object or a block lives on.
void* BlockOf(void* ptr) {  // a scriptable's handle block (object+0x08, pointing back at it), or null
    void* b = Rd<void*>(ptr, 0x08);
    return Plausible(b) && Rd<void*>(b, 0x08) == ptr ? b : nullptr;
}

bool DiscardGuarded(void* obj) {  // the engine's own last step: vtable slot 0x18 of the object
    __try {
        using Discard_t = void (*)(void*);
        (*reinterpret_cast<Discard_t**>(obj))[3](obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Gives back one reference. The engine's own release discards a flag-0x2 object when its count falls to 1 (vtable
// slot 0x18). Lua takes that step only for objects w3.new made (g_newBlocks): done for every object, it would discard
// world entities the engine still holds (raycasts and positions go wrong after a garbage collection over a few
// thousand entities). Every other block keeps at least 2 refs when Lua lets go (at worst an object lives on).
std::unordered_set<void*> g_newBlocks;  // blocks of objects made by w3.new while Lua still holds them
std::vector<void*> g_discardQueue;      // their discards, waiting for the start of the next tick
void DiscardNow(void* block);

void ReleaseRef(void* block) {
    auto refs = static_cast<volatile LONG*>(block);
    bool ours = g_newBlocks.count(block) != 0;
    LONG r = 0;
    for (;;) {
        uint32_t fl = 0;
        if (!SafeRead(block, &r, 4) || !SafeRead(static_cast<char*>(block) + 4, &fl, 4)) return;
        if (!ours && !(r > 2 || (r == 2 && !(fl & 2)))) return;
        if (ours && r <= 1) return;
        if (InterlockedCompareExchange(refs, r - 1, r) == r) break;
    }
    if (!ours || r - 1 != 1) return;
    g_newBlocks.erase(block);
    // Not here: a release can come from Lua's garbage collector in the middle of any call (while arguments are
    // being built), and the discard runs engine code. It waits for the start of the next tick (DiscardPending).
    g_discardQueue.push_back(block);
}

// The discards Lua's releases left for a quiet moment (the start of a tick, no Lua running): an object whose count is
// still at its own ref is discarded the way the engine's release does it.
void DiscardPending() {
    std::vector<void*> q;
    q.swap(g_discardQueue);
    for (void* block : q) {
        LONG r = 0;
        if (!SafeRead(block, &r, 4) || r != 1) continue;  // someone took it in the meantime
        DiscardNow(block);
    }
}

void DiscardNow(void* block) {
    auto flags = reinterpret_cast<volatile LONG*>(static_cast<char*>(block) + 4);
    void* obj = Rd<void*>(block, 0x08);
    LONG f = 0;
    if (!obj || !SafeRead(const_cast<LONG*>(flags), &f, 4) || !(f & 2)) return;
    if (InterlockedCompareExchange(flags, f & ~2, f) != f) return;  // someone else changed it: theirs to finish
    static int logged = 0;
    if (logged < 50) {  // rare and worth seeing: which objects Lua was the last to hold
        ++logged;
        Log("lua: last handle gone, the game discards a %s (block %p)", RttiName(ClassOfGuarded(obj)).c_str(), block);
    }
    if (!DiscardGuarded(obj)) Log("lua: an object's discard faulted (block %p)", block);
}

// A handle the game handed over with a ref of its own (a return value, an out value, an argument it read): the Lua
// object took its own ref, the game's goes back.
void ReleaseHandedHandle(const void* addr, void* type) {
    if (RttiKind(type) != 7) return;
    void* block = Rd<void*>(addr, 0);
    if (Plausible(block)) ReleaseRef(block);
}

void PushObject(lua_State* L, void* ptr, void* cls, bool isStruct, void* block = nullptr) {
    if (!Plausible(ptr)) {
        lua_pushnil(L);
        return;
    }
    if (!cls) cls = ClassOfGuarded(ptr);
    if (!Plausible(cls)) {
        lua_pushnil(L);
        return;
    }
    if (!isStruct && !block) block = BlockOf(ptr);
    if (block && Rd<void*>(block, 0x08) != ptr) block = nullptr;  // a block of another (or a gone) object
    auto o = static_cast<Obj*>(lua_newuserdatauv(L, sizeof(Obj), 0));
    *o = {ptr, cls, isStruct, block, false};
    if (isStruct) o->ownerBlock = g_viewOwner.block, o->ownerPtr = g_viewOwner.ptr;
    if (block && !isStruct) {
        InterlockedIncrement(static_cast<volatile LONG*>(block));
        o->owns = true;
    }
    luaL_setmetatable(L, kObj);
}

void PushString(lua_State* L, const void* addr) {  // a WitcherScript string: {char* data, uint32 length incl. \0}
    const char* data = Rd<const char*>(addr, 0);
    uint32_t len = Rd<uint32_t>(addr, 8);
    if (!Plausible(data) || !len || len > (1u << 20)) {
        lua_pushliteral(L, "");
        return;
    }
    std::string s(len - 1, '\0');
    if (len > 1 && !SafeRead(data, &s[0], len - 1)) s.clear();
    lua_pushlstring(L, s.data(), strlen(s.c_str()));
}

// A struct value copied into memory Lua owns (behind the Obj in the same userdata): for return values, whose
// buffer is gone after the call.
void PushStructCopy(lua_State* L, const void* addr, void* type) {
    uint32_t size = RttiSize(type);
    if (!size || size > 4096) {
        lua_pushnil(L);
        return;
    }
    const size_t head = (sizeof(Obj) + 15) & ~size_t(15);
    char* mem = static_cast<char*>(lua_newuserdatauv(L, head + size, 0));
    if (!SafeRead(addr, mem + head, size)) memset(mem + head, 0, size);
    *reinterpret_cast<Obj*>(mem) = {mem + head, type, true, nullptr};
    luaL_setmetatable(L, kObj);
}

void PushValue(lua_State* L, const void* addr, void* type, int depth = 0, bool copy = false);

const char* const kArr = "tw3se.array";  // a live view on a TDynArray field {data@0, count@8}
struct Arr {
    void* addr;      // the array struct, read fresh each access so it reflects the live count/data
    void* inner;     // element type
    void* type;      // the array type itself
    uint32_t fixed;  // a fixed array ([N]Type): N elements right at addr, no count field; 0 for a TDynArray
    void* ownerBlock;  // the object it lies in (g_viewOwner)
    void* ownerPtr;
};
uint32_t ArrCount(const Arr* a) { return a->fixed ? a->fixed : Rd<uint32_t>(a->addr, 8); }
char* ArrData(const Arr* a) { return a->fixed ? static_cast<char*>(a->addr) : Rd<char*>(a->addr, 0); }

void* ArrayInner(const std::string& typeName) {  // "array:2,0,<inner>"
    size_t c = typeName.find(',', typeName.find(',') + 1);
    return c == std::string::npos ? nullptr : RttiFindType(typeName.c_str() + c + 1);
}

// A snapshot of an array as a plain Lua table (for temporary memory: return values, nested array elements).
void PushArrayCopy(lua_State* L, const void* addr, void* inner, int depth, bool copy) {
    uint32_t esize = RttiSize(inner);
    char* data = Rd<char*>(addr, 0);
    uint32_t n = Rd<uint32_t>(addr, 8);
    lua_createtable(L, n < 4096 ? n : 4096, 0);
    if (!inner || !esize || !Plausible(data) || depth > 4) return;
    for (uint32_t i = 0; i < n && i < 4096; ++i) {
        PushValue(L, data + static_cast<size_t>(i) * esize, inner, depth + 1, copy);
        lua_rawseti(L, -2, i + 1);
    }
}

// A live view on an array field: #a is the count, a[i] reads an element (1-based), a[i] = v writes it in place,
// a:to_table() snapshots it. Growing/shrinking (insert/remove) needs the engine allocator and is not here yet.
void PushArrayView(lua_State* L, const void* addr, void* inner, void* type) {
    auto a = static_cast<Arr*>(lua_newuserdatauv(L, sizeof(Arr), 0));
    *a = {const_cast<void*>(addr), inner, type, 0, g_viewOwner.block, g_viewOwner.ptr};
    luaL_setmetatable(L, kArr);
}

// A fixed array ([N]Type): N elements one after the other, read and written in place.
void PushFixedArrayView(lua_State* L, const void* addr, void* inner, uint32_t n) {
    auto a = static_cast<Arr*>(lua_newuserdatauv(L, sizeof(Arr), 0));
    *a = {const_cast<void*>(addr), inner, nullptr, n, g_viewOwner.block, g_viewOwner.ptr};
    luaL_setmetatable(L, kArr);
}

void PushMapView(lua_State* L, const void* addr, void* type);
void PushMapCopy(lua_State* L, const void* addr, void* type, int depth);

// `copy`: the memory at addr is temporary (a return value), structs are copied instead of viewed.
void PushValue(lua_State* L, const void* addr, void* type, int depth, bool copy) {
    std::string tn = RttiName(type);
    switch (RttiKind(type)) {
    case 11:  // fundamental
        if (tn == "Bool") lua_pushboolean(L, Rd<uint8_t>(addr, 0) != 0);
        else if (tn == "Float") lua_pushnumber(L, Rd<float>(addr, 0));
        else if (tn == "Double") lua_pushnumber(L, Rd<double>(addr, 0));
        else if (tn == "Int8") lua_pushinteger(L, Rd<int8_t>(addr, 0));
        else if (tn == "Uint8") lua_pushinteger(L, Rd<uint8_t>(addr, 0));
        else if (tn == "Int16") lua_pushinteger(L, Rd<int16_t>(addr, 0));
        else if (tn == "Uint16") lua_pushinteger(L, Rd<uint16_t>(addr, 0));
        else if (tn == "Int32") lua_pushinteger(L, Rd<int32_t>(addr, 0));
        else if (tn == "Uint32") lua_pushinteger(L, Rd<uint32_t>(addr, 0));
        else lua_pushinteger(L, Rd<int64_t>(addr, 0));  // Int64, Uint64
        return;
    case 0:  // simple
        if (tn == "CName") lua_pushstring(L, NameOfId(Rd<uint32_t>(addr, 0)).c_str());
        else if (tn == "String") PushString(L, addr);
        else if (tn == "CGUID") {  // 16 bytes, four uint32: a readable hex id (entity GUIDs use this)
            char g[40];
            sprintf_s(g, "%08x-%08x-%08x-%08x", Rd<uint32_t>(addr, 0), Rd<uint32_t>(addr, 4), Rd<uint32_t>(addr, 8),
                      Rd<uint32_t>(addr, 12));
            lua_pushstring(L, g);
        } else if (tn == "TagList") {  // {TDynArray<CName>}: a live array of names (insert/remove work, 4-byte cells)
            static void* cname = RttiFindType("CName");
            if (cname) PushArrayView(L, const_cast<void*>(addr), cname, nullptr);
            else lua_pushnil(L);
        } else if (tn == "LocalizedString") {  // a text id: the text in the game's language
            uint32_t id = Rd<uint32_t>(addr, 0);
            int top = lua_gettop(L);
            bool ok = false;
            if (id) {
                lua_getglobal(L, "w3");
                lua_getfield(L, -1, "call");
                lua_pushstring(L, "GetLocStringById");
                lua_pushinteger(L, id);
                ok = lua_pcall(L, 2, 1, 0) == LUA_OK && lua_isstring(L, -1) && lua_rawlen(L, -1) > 0;
            }
            if (ok) {
                lua_replace(L, top + 1);
                lua_settop(L, top + 1);
            } else {
                lua_settop(L, top);
                if (id) lua_pushfstring(L, "#%d", static_cast<int>(id));
                else lua_pushnil(L);
            }
        } else if (tn == "CDateTime") {  // a packed date and time, as its number
            lua_pushinteger(L, Rd<int64_t>(addr, 0));
        } else if (tn == "IdTag") {  // 20 bytes: as hex
            char h[48] = {};
            for (int i = 0; i < 20; ++i) sprintf_s(h + i * 2, 3, "%02x", Rd<uint8_t>(addr, i));
            lua_pushstring(L, h);
        } else if (tn == "EntityHandle") {  // a pointer to CPersistentEntityHandleData {vtable, handle block of the
            // entity at +0x08, ...}: the entity while it is there, else nil (measured: horseWithInventory empty,
            // as GetHorseWithInventory() says; the next such object held the horse manager)
            void* data = Rd<void*>(addr, 0);
            void* block = Plausible(data) ? Rd<void*>(data, 0x08) : nullptr;
            void* ent = Plausible(block) ? Rd<void*>(block, 0x08) : nullptr;
            if (Plausible(ent)) PushObject(L, ent, nullptr, false, block);
            else lua_pushnil(L);
        } else if (tn == "EngineTransform") {  // a pointer to {position, scale, rotation (Roll, Pitch, Yaw)}, null =
            // identity (measured on the player: position = GetWorldPosition, Yaw = GetHeading)
            const float* d = Rd<const float*>(addr, 0);
            float v[12] = {0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0};
            if (Plausible(d)) SafeRead(d, v, sizeof v);
            lua_createtable(L, 0, 3);
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, v[0]); lua_setfield(L, -2, "X");
            lua_pushnumber(L, v[1]); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, v[2]); lua_setfield(L, -2, "Z");
            lua_setfield(L, -2, "pos");
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, v[4]); lua_setfield(L, -2, "X");
            lua_pushnumber(L, v[5]); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, v[6]); lua_setfield(L, -2, "Z");
            lua_setfield(L, -2, "scale");
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, v[8]); lua_setfield(L, -2, "Roll");
            lua_pushnumber(L, v[9]); lua_setfield(L, -2, "Pitch");
            lua_pushnumber(L, v[10]); lua_setfield(L, -2, "Yaw");
            lua_setfield(L, -2, "rot");
        } else if (tn == "EngineQsTransform") {  // 48 bytes in place: translation, rotation (a quaternion), scale
            float v[12] = {};
            SafeRead(addr, v, sizeof v);
            const char* parts[3] = {"pos", "rot", "scale"};
            lua_createtable(L, 0, 3);
            for (int k = 0; k < 3; ++k) {
                lua_createtable(L, 0, 4);
                lua_pushnumber(L, v[k * 4 + 0]); lua_setfield(L, -2, "X");
                lua_pushnumber(L, v[k * 4 + 1]); lua_setfield(L, -2, "Y");
                lua_pushnumber(L, v[k * 4 + 2]); lua_setfield(L, -2, "Z");
                lua_pushnumber(L, v[k * 4 + 3]); lua_setfield(L, -2, "W");
                lua_setfield(L, -2, parts[k]);
            }
        } else if (RttiSize(type) == 8 && !Rd<void*>(addr, 0)) {  // an empty engine pointer type
            lua_pushnil(L);
        } else lua_pushfstring(L, "<%s>", tn.c_str());
        return;
    case 12:  // a map: a live view (m[key], m[key] = v, #m, pairs), a table for a temporary one
        if (copy) PushMapCopy(L, addr, type, depth);
        else PushMapView(L, addr, type);
        return;
    case 4: {  // a static array "static:N,Type": the count (u32) at +0, the elements from +0x10 (read in
               // CRTTIStaticArrayType's element accessor); a view on the current elements
        void* inner = Rd<void*>(type, 0x08);
        uint32_t n = Rd<uint32_t>(addr, 0), cap = Rd<uint32_t>(type, 0x10), es = RttiSize(inner);
        if (!Plausible(inner) || !es || n > cap || cap > 4096) {
            lua_pushfstring(L, "<%s>", tn.c_str());
        } else if (copy) {
            lua_createtable(L, static_cast<int>(n), 0);
            for (uint32_t i = 0; i < n && depth < 4; ++i) {
                PushValue(L, static_cast<const char*>(addr) + 0x10 + static_cast<size_t>(i) * es, inner, depth + 1, copy);
                lua_rawseti(L, -2, i + 1);
            }
        } else {
            PushFixedArrayView(L, static_cast<const char*>(addr) + 0x10, inner, n);
        }
        return;
    }
    case 1: {  // enum: its value name, or the number when the value has none
        uint32_t size = RttiSize(type);
        int32_t v = size == 1 ? Rd<int8_t>(addr, 0) : size == 2 ? Rd<int16_t>(addr, 0) : Rd<int32_t>(addr, 0);
        std::string name;
        if (RttiEnumName(type, v, name)) lua_pushstring(L, name.c_str());
        else lua_pushinteger(L, v);
        return;
    }
    case 9: {  // bitfield: the number (w3.enum gives the bit names with their values)
        uint32_t size = RttiSize(type);
        lua_pushinteger(L, size == 1 ? Rd<uint8_t>(addr, 0) : size == 2 ? Rd<uint16_t>(addr, 0) : Rd<uint32_t>(addr, 0));
        return;
    }
    case 8:  // soft handle (32 bytes): the resource's path, a String at +0x10 (measured on
             // CGameResource.defaultPlayerTemplate); nil when it names none
        if (Rd<uint32_t>(addr, 0x18) > 1) PushString(L, static_cast<const char*>(addr) + 0x10);
        else lua_pushnil(L);
        return;
    case 2:  // an embedded struct: a view into this memory, or a copy of a temporary one
        if (copy) PushStructCopy(L, addr, type);
        else PushObject(L, const_cast<void*>(addr), type, true);
        return;
    case 3:
        if (copy) PushArrayCopy(L, addr, ArrayInner(tn), depth, copy);
        else PushArrayView(L, addr, ArrayInner(tn), type);
        return;
    case 5: {  // a fixed array "[N]Type": N elements in place
        size_t close = tn.find(']');
        uint32_t n = static_cast<uint32_t>(strtoul(tn.c_str() + 1, nullptr, 10));
        void* inner = close == std::string::npos ? nullptr : RttiFindType(tn.c_str() + close + 1);
        uint32_t es = RttiSize(inner);
        if (!inner || !es || !n || n > 4096) {
            lua_pushfstring(L, "<%s>", tn.c_str());
        } else if (copy) {
            lua_createtable(L, static_cast<int>(n), 0);
            for (uint32_t i = 0; i < n && depth < 4; ++i) {
                PushValue(L, static_cast<const char*>(addr) + static_cast<size_t>(i) * es, inner, depth + 1, copy);
                lua_rawseti(L, -2, i + 1);
            }
        } else {
            PushFixedArrayView(L, addr, inner, n);
        }
        return;
    }
    case 6:  // pointer
        PushObject(L, Rd<void*>(addr, 0), nullptr, false);
        return;
    case 7: {  // handle: a block {refs, flags, object at +8}
        void* block = Rd<void*>(addr, 0);
        PushObject(L, Plausible(block) ? Rd<void*>(block, 8) : nullptr, nullptr, false, block);
        return;
    }
    default:
        lua_pushfstring(L, "<%s>", tn.c_str());
    }
}

bool SafeWrite(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string MapAssign(lua_State* L, int idx, void* addr, void* type);
std::string StructAssign(lua_State* L, int idx, void* addr, void* type);
std::string ArrayAssign(lua_State* L, int idx, void* addr, void* type);

// Writes the Lua value at `idx` into a field. Returns an error text, or "" when written.
std::string WriteValue(lua_State* L, int idx, void* addr, void* type) {
    std::string tn = RttiName(type);
    int kind = RttiKind(type);
    uint32_t size = RttiSize(type);
    uint8_t bytes[8] = {};
    if (kind == 11) {
        if (tn == "Bool") {
            bytes[0] = lua_toboolean(L, idx) ? 1 : 0;
        } else if (!lua_isnumber(L, idx)) {
            return "a number is needed for " + tn;
        } else if (tn == "Float") {
            float v = static_cast<float>(lua_tonumber(L, idx));
            if (!std::isfinite(v)) return "a finite number is needed for Float (not NaN, not beyond 3.4e38)";
            memcpy(bytes, &v, 4);
        } else if (tn == "Double") {
            double v = lua_tonumber(L, idx);
            if (!std::isfinite(v)) return "a finite number is needed for Double (not NaN)";
            memcpy(bytes, &v, 8);
        } else {
            lua_Integer v = lua_isinteger(L, idx) ? lua_tointeger(L, idx) : static_cast<lua_Integer>(lua_tonumber(L, idx));
            memcpy(bytes, &v, 8);  // little endian: the low bytes come first, whatever the size
        }
    } else if (kind == 9) {  // bitfield: a number, a bit name or a table of bit names
        int32_t v = 0;
        if (lua_isinteger(L, idx)) {
            v = static_cast<int32_t>(lua_tointeger(L, idx));
        } else if (lua_type(L, idx) == LUA_TSTRING) {
            if (!RttiEnumValue(type, lua_tostring(L, idx), &v)) return std::string("no bit ") + lua_tostring(L, idx) + " in " + tn;
        } else if (lua_istable(L, idx)) {
            for (int i = 1, n = static_cast<int>(lua_rawlen(L, idx)); i <= n; ++i) {
                lua_rawgeti(L, idx, i);
                int32_t bit = 0;
                const char* nm = lua_tostring(L, -1);
                bool ok = nm && RttiEnumValue(type, nm, &bit);
                lua_pop(L, 1);
                if (!ok) return "no bit " + std::string(nm ? nm : "?") + " in " + tn;
                v |= bit;
            }
        } else {
            return "a number, a bit name or a list of bit names is needed for " + tn;
        }
        memcpy(bytes, &v, 4);
    } else if (kind == 1) {
        int32_t v = 0;
        if (lua_type(L, idx) == LUA_TSTRING) {
            if (!RttiEnumValue(type, lua_tostring(L, idx), &v)) return std::string("no value ") + lua_tostring(L, idx) + " in " + tn;
        } else if (lua_isinteger(L, idx)) {
            v = static_cast<int32_t>(lua_tointeger(L, idx));
        } else {
            return "a value name or number is needed for " + tn;
        }
        memcpy(bytes, &v, 4);
    } else if (tn == "CName") {
        const char* s = lua_tostring(L, idx);
        if (!s) return "a string is needed for a name";
        uint32_t id = InternName(s);
        memcpy(bytes, &id, 4);
    } else if (tn == "String") {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        if (!s) return "a string is needed for " + tn;
        return WriteStringField(addr, s, len) ? "" : "the string could not be written";
    } else if (kind == 7) {  // a handle field: stores the handle block pointer
        if (lua_isnil(L, idx)) {
            void* z = nullptr;
            return SafeWrite(addr, &z, 8) ? "" : "the handle could not be cleared";  // old ref kept (safe, a leak)
        }
        auto o = static_cast<Obj*>(luaL_testudata(L, idx, kObj));
        if (!o || o->isStruct) return "an object is needed for " + tn;
        if (!o->block) return "this object came without a handle (from a pointer), it cannot be stored";
        std::string want = tn.substr(7);
        bool fits = false;
        for (void* c = o->cls; Plausible(c) && !fits; c = Rd<void*>(c, 0x10)) fits = RttiName(c) == want;
        if (!fits) return "a " + want + " is needed, this is a " + RttiName(o->cls);
        InterlockedIncrement(static_cast<volatile LONG*>(o->block));  // hold a reference; the old one is kept (safe)
        return SafeWrite(addr, &o->block, 8) ? "" : "the handle could not be written";
    } else if (kind == 12) {
        return MapAssign(L, idx, addr, type);
    } else if (kind == 2) {  // a struct field: from a table of its fields or a struct value of the same type
        return StructAssign(L, idx, addr, type);
    } else if (kind == 3) {  // a whole array: from a table or another array of the same element type
        return ArrayAssign(L, idx, addr, type);
    } else if (kind == 6) {  // a pointer field: the object itself (no copy, no reference; its owner keeps it alive)
        if (lua_isnil(L, idx)) {
            void* z = nullptr;
            return SafeWrite(addr, &z, 8) ? "" : "the pointer could not be cleared";
        }
        auto o = static_cast<Obj*>(luaL_testudata(L, idx, kObj));
        if (!o || o->isStruct || !o->ptr) return "an object is needed for " + tn;
        std::string want = tn.substr(4);  // "ptr:<class>"
        bool fits = false;
        for (void* c = o->cls; Plausible(c) && !fits; c = Rd<void*>(c, 0x10)) fits = RttiName(c) == want;
        if (!fits) return "a " + want + " is needed, this is a " + RttiName(o->cls);
        if (!SafeWrite(addr, &o->ptr, 8)) return "the pointer could not be written";
        // A w3.new object behind a pointer is the holder's now: a pointer takes no reference, so Lua's discard after
        // its last handle would leave the holder pointing at freed memory (w3.anim.with_sets' CAnimAnimsetsParam in
        // templateParams: the game's gameplay storage task reads the template's parameters on a worker thread and
        // crashes 45 s to 5 min later). Lua never discards it (at worst it lives on).
        if (o->block) g_newBlocks.erase(o->block);
        return "";
    } else {
        return "fields of type " + tn + " are read-only from Lua";
    }
    if (!size || size > 8) return "unexpected size of " + tn;
    return SafeWrite(addr, bytes, size) ? "" : "the field could not be written";
}

// --- passing any value: a nested call to TW3SE_LuaArg<i>(slot) as the argument
//
// Compiled bytecode of a nested native call, read with TW3SE_ProbeArg (scripts/tw3se_probe_call.ws):
// 0x1c, uint16 count of the bytes that follow, uint16 script line, CFunction* (8 bytes), its arguments, 0x20.
// TW3SE_LuaArg<i> hands back the bytes Lua prepared for argument position i. Its return type is set to the
// parameter's type before the call and its one int parameter declared, both borrowed from game functions with
// that return type / parameter, so the engine sees the same metadata a compiled call would carry.
constexpr int kArgSlots = 8;
std::vector<uint8_t> g_slots[kArgSlots];
std::unordered_map<void*, void*> g_retProps;  // type -> a __return CProperty of that type
void* g_intParams = nullptr;  // a parameter array holding one Int32 CProperty
// Both point into other functions' signatures. A script reload (the REDkit editor) frees and rebuilds those, so the
// compiler's declare hook bumps this and the next call scans again.
std::atomic<int> g_scriptGen{0};
int g_scannedGen = -1;

// TW3SE_LuaArg<n> borrows a return property and a parameter array for one call (PrepareArgFn). Its own fields go
// back right after the call: left pointing at another function's signature, the editor's script reload freed
// those objects a second time through it and crashed (Play -> Stop -> Reload scripts).
struct ArgFnFields { void* ret; void* params; uint32_t count; };
std::unordered_map<void*, ArgFnFields> g_argFnOrig;  // each LuaArg CFunction -> its fields before the first borrow
std::vector<void*> g_argFnPatched;                    // borrowed in the current call

void RestoreArgFns() {
    for (void* fn : g_argFnPatched) {
        const ArgFnFields& o = g_argFnOrig[fn];
        SafeWrite(static_cast<char*>(fn) + 0x20, &o.ret, 8);
        SafeWrite(static_cast<char*>(fn) + 0x28, &o.params, 8);
        SafeWrite(static_cast<char*>(fn) + 0x30, &o.count, 4);
    }
    g_argFnPatched.clear();
}

void LuaArgNative(int i, void* frame, void* result) {
    int32_t slot = -1;
    ReadScriptParam(frame, &slot);
    EndScriptParams(frame);
    if (result && slot == i && !g_slots[i].empty()) memcpy(result, g_slots[i].data(), g_slots[i].size());
}
template <int I> void LuaArgN(void*, void* frame, void* result) { LuaArgNative(I, frame, result); }
void (*const kLuaArgNatives[kArgSlots])(void*, void*, void*) = {&LuaArgN<0>, &LuaArgN<1>, &LuaArgN<2>, &LuaArgN<3>,
                                                                &LuaArgN<4>, &LuaArgN<5>, &LuaArgN<6>, &LuaArgN<7>};

// The sink for calling a WitcherScript function: its one argument is a nested call to the target, so reading the
// argument makes the VM execute that call; the return lands in `result`. Its declared return/param type do not
// matter - the VM reads the real return of the nested call.
void LuaSink(void*, void* frame, void* result) {
    ReadScriptParam(frame, result);  // executes the nested 0x1c call, writes its return value into result
    EndScriptParams(frame);          // the sink's own end-of-arguments marker
}

void ScanFunctions() {
    RttiFunctions([](void* fn, void*) {
        void* ret = Rd<void*>(fn, 0x20);
        void* type = Plausible(ret) ? Rd<void*>(ret, 0x08) : nullptr;
        if (Plausible(type) && !g_retProps.count(type)) g_retProps[type] = ret;
        void** params = Rd<void**>(fn, 0x28);
        if (!g_intParams && Rd<uint32_t>(fn, 0x30) == 1 && Plausible(params) &&
            RttiName(Rd<void*>(Rd<void*>(params, 0), 0x08)) == "Int32")
            g_intParams = params;
    }, nullptr);
}

bool IsPlainStruct(void* type, int depth);

void* PrepareArgFn(int i, void* type, std::string& err) {
    char name[24];
    sprintf_s(name, "TW3SE_LuaArg%d", i);
    void* fn = OurNative(name);
    if (!fn) {
        err = "TW3SE_LuaArg is not registered (natives off?)";
        return nullptr;
    }
    if (g_scannedGen != g_scriptGen.load()) {
        g_retProps.clear();
        g_intParams = nullptr;
        g_scannedGen = g_scriptGen.load();
        ScanFunctions();
    }
    void* ret = nullptr;
    auto it = g_retProps.find(type);
    if (it != g_retProps.end()) {
        ret = it->second;
    } else {
        // No game function returns exactly this type: one of the same kind and size (another enum of the same
        // size, any handle, a plain struct of the same size) carries the same copy rules.
        int kind = RttiKind(type);
        uint32_t size = RttiSize(type);
        for (auto& [t, prop] : g_retProps) {
            if (RttiKind(t) != kind || RttiSize(t) != size) continue;
            if (kind == 1 || kind == 7 || (kind == 2 && IsPlainStruct(t, 0))) {
                ret = prop;
                break;
            }
        }
    }
    if (!ret || !g_intParams) {
        err = "no game function returns a " + RttiName(type) + ", Lua cannot pass one yet";
        return nullptr;
    }
    if (!g_argFnOrig.count(fn))
        g_argFnOrig[fn] = {Rd<void*>(fn, 0x20), Rd<void*>(fn, 0x28), Rd<uint32_t>(fn, 0x30)};
    if (std::find(g_argFnPatched.begin(), g_argFnPatched.end(), fn) == g_argFnPatched.end()) g_argFnPatched.push_back(fn);
    uint32_t one = 1;
    if (!SafeWrite(static_cast<char*>(fn) + 0x20, &ret, 8) || !SafeWrite(static_cast<char*>(fn) + 0x28, &g_intParams, 8) ||
        !SafeWrite(static_cast<char*>(fn) + 0x30, &one, 4)) {
        err = "TW3SE_LuaArg could not be prepared";
        return nullptr;
    }
    return fn;
}

// A struct of numbers, bools, enums and names only (nested ones too): its bytes can be copied as they are.
bool IsPlainStruct(void* type, int depth) {
    if (depth > 4 || RttiKind(type) != 2 || Plausible(Rd<void*>(type, 0x10))) return false;  // a parent: an object
    bool plain = true;
    ForEachClassItem(type, 0x30, [&](void* p, void*) {
        void* t = Rd<void*>(p, 0x08);
        int k = RttiKind(t);
        plain = k == 11 || k == 1 || k == 9 || RttiName(t) == "CName" || (k == 2 && IsPlainStruct(t, depth + 1));
        return !plain;
    });
    return plain;
}

// The refs EncodeArg took on object arguments for one call. The called native gives each back once; a call that
// never happens (a later argument was wrong) has to give them back itself.
std::vector<void*> g_argRefs;

// The bytes of a value of `type` (enum, handle, plain struct) from the Lua value at idx. Returns an error text.
std::string EncodeArg(lua_State* L, int idx, void* type, std::vector<uint8_t>& out) {
    int kind = RttiKind(type);
    std::string tn = RttiName(type);
    out.assign(RttiSize(type), 0);
    if (out.empty()) return "the size of " + tn + " is unknown";
    // enums, bitfields and the small number types (Uint8, Int16 ...; CVisualDebug.AddText's `line : byte`)
    if (kind == 1 || kind == 9 || kind == 11) return WriteValue(L, idx, out.data(), type);
    if (kind == 7) {
        if (lua_isnil(L, idx)) return "";  // a null handle
        auto o = static_cast<Obj*>(luaL_testudata(L, idx, kObj));
        if (!o || o->isStruct) return "an object is needed for " + tn;
        if (!o->block) return "this object came without a handle (from a pointer), it cannot be passed";
        std::string want = tn.substr(7);
        bool fits = false;
        for (void* c = o->cls; Plausible(c) && !fits; c = Rd<void*>(c, 0x10)) fits = RttiName(c) == want;
        if (!fits) return "a " + want + " is needed, this is a " + RttiName(o->cls);
        InterlockedIncrement(static_cast<volatile LONG*>(o->block));  // the native releases its parameter once
        g_argRefs.push_back(o->block);
        // A w3.new object handed to the game is the game's now: it may keep it without a reference (ForceAIBehavior
        // keeps the action; a mod's CAIActionLoop and CAIPlayAnimationSlotAction were discarded half a second after
        // the scene's stop, the game called into them in its tick 3 to 16 s later and crashed). Lua never discards it, as for a pointer field (at worst it lives on).
        g_newBlocks.erase(o->block);
        memcpy(out.data(), &o->block, sizeof(void*));
        return "";
    }
    if (kind == 2) {
        if (!IsPlainStruct(type, 0)) return tn + " holds strings, arrays or objects (Lua passes plain structs so far)";
        auto o = static_cast<Obj*>(luaL_testudata(L, idx, kObj));
        if (o && o->isStruct) {
            if (RttiName(o->cls) != tn) return "a " + tn + " is needed, this is a " + RttiName(o->cls);
            return SafeRead(o->ptr, out.data(), out.size()) ? "" : "the struct could not be read";
        }
        if (!lua_istable(L, idx)) return "a " + tn + " or a table of its fields is needed";
        std::string err;
        ForEachClassItem(type, 0x30, [&](void* p, void*) {
            lua_getfield(L, idx, NameOfId(Rd<uint32_t>(p, 0x10)).c_str());
            if (!lua_isnil(L, -1)) err = WriteValue(L, -1, out.data() + Rd<uint32_t>(p, 0x20), Rd<void*>(p, 0x08));
            lua_pop(L, 1);
            return !err.empty();
        });
        return err;
    }
    return "Lua cannot pass a " + tn + " yet";
}

// --- out / by-reference arguments (opcode 0x0f + a CProperty*, FINDINGS_OUTPARAM3.md)
//
// WitcherScript passes an array (and an explicit `out`) by reference: the compiler emits 0x0f + the 8-byte
// address of a CProperty-shaped descriptor (not the variable's own address - a first attempt that embedded the
// buffer's address directly faulted the VM, see FINDINGS_OUTPARAM2/3.md). GScriptNativeTable[0x0f] resolves the
// descriptor against ScriptFrame+0x20 ("object"): `*(prop+0x24) & 0x20 ? *(object+0x20) : object`, then adds
// `*(prop+0x20)` (the offset) - the same object+CProperty recipe as any other property read (scripting.md
// section 8), just applied to a frame field instead of a live object. So each call that uses 0x0f needs: one
// contiguous "locals block" (one TDynArray-sized slot per by-reference argument) that ScriptFrame+0x20 points
// at, and one small synthetic CProperty per argument (type = the real declared parameter type, offset = this
// argument's byte offset within the locals block, flags = 0 - no extra object indirection wanted).
//
// For a Lua call, each array parameter gets a TDynArray {data@0, count@8} slot in that locals block: a Lua
// table fills it in with one exact allocation (input), nil or an empty table leaves it empty (output). After the
// call the array is read back and returned as an extra Lua value, then its data is freed. So:
//   local ents = w3.global("theGame"):GetEntitiesByTag("novigrad_citizen", {})   -- {} is the out array
struct OutArg {
    uint8_t* slot;   // this argument's 16-byte slot inside g_localsBase (a TDynArray for an array, the value for a scalar)
    void* type;      // an array's element type (read back with PushArrayCopy) or an out-scalar's own type (PushValue)
    bool isArray;
    bool borrowed = false;  // the {data, count} of a live array handed on: read back, never released or freed
    void* origin = nullptr; // borrowed: where it came from; what the callee did to {data, count} goes back there
};
uint8_t* g_localsBase = nullptr;    // ScriptFrame+0x20 for one call: N * 16 bytes, one TDynArray slot each
std::vector<uint8_t*> g_propStore;  // owns the synthetic CProperty descriptors (0x30 bytes each) for one call
std::vector<OutArg> g_outArgs;      // the array arguments of one call, read back afterwards
// ScriptFrame+0x28 for one call: structs with strings, arrays or objects handed by value. Opcode 0x10 + a synthetic
// CProperty reads them from there, the way the engine's own C++ calls pass every argument (orig() of an
// OnSpawned hook passes its SEntitySpawnData this way). The slots own their copies.
uint8_t* g_paramsBlock = nullptr;
std::vector<std::pair<uint8_t*, void*>> g_paramVals;  // each slot and its type, released after the call

void* AllocBytes(size_t n) { return EngineAlloc(n, 8); }
void FreeBytes(void* p) {
    if (p)
        if (auto f = Fn<void (*)(void*)>("Memory::Free")) f(p);
}

void ReleasePendingArgRefs() {  // the call did not happen: the refs taken for it go back
    for (void* b : g_argRefs) ReleaseRef(b);
    g_argRefs.clear();
}

void WriteBackBorrowed() {  // a borrowed array's {data, count} after the call, back where it came from
    for (OutArg& o : g_outArgs)
        if (o.borrowed && o.origin) {
            SafeWrite(o.origin, o.slot, 12);
            o.origin = nullptr;
        }
}

void ReleaseValue(void* addr, void* type, int depth);

void ReleaseParamsBlock() {  // the by-value struct slots of one call give back what they own
    for (auto& [slot, type] : g_paramVals) ReleaseValue(slot, type, 0);
    g_paramVals.clear();
    delete[] g_paramsBlock;
    g_paramsBlock = nullptr;
    g_invokeParams = nullptr;
}

void ClearOutArgs() {
    ReleasePendingArgRefs();  // empty after a call (the native took them), full after an error before it
    RestoreArgFns();
    ReleaseParamsBlock();
    delete[] g_localsBase;
    g_localsBase = nullptr;
    for (uint8_t* p : g_propStore) delete[] p;
    g_propStore.clear();
    g_outArgs.clear();
}

// --- calls

// The argument bytecode for `fn`'s declared parameters from Lua values idx.., with the measured literal opcodes.
// Returns an error text, or "" with the bytecode in `bc`.
std::string BuildArgs(lua_State* L, int first, void* fn, std::vector<uint8_t>& bc) {
    void** params = Rd<void**>(fn, 0x28);
    uint32_t np = Rd<uint32_t>(fn, 0x30);
    int given = lua_gettop(L) - first + 1;
    if (np > 32 || (np && !Plausible(params))) return "its parameters cannot be read";
    if (given > static_cast<int>(np)) return "it takes at most " + std::to_string(np) + " argument(s), " + std::to_string(given) + " given";
    if (!lua_checkstack(L, static_cast<int>(np) + 1)) return "no room on the Lua stack";  // a left-off argument's index stays acceptable (none)
    auto put = [&](const void* p, size_t n) { bc.insert(bc.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    int nested = 0;
    // Pre-scan: the by-reference arguments (arrays and out values, a trailing one left off too: it is never
    // omitted, see below). They share one locals block (ScriptFrame+0x20), one 16-byte TDynArray slot each,
    // addressed by a per-argument synthetic CProperty offset (FINDINGS_OUTPARAM3.md) - allocated up front.
    std::vector<uint32_t> refOffset(np ? np : 1, 0);  // each by-reference argument's byte offset in the locals block
    uint32_t blockSize = 0;
    for (uint32_t i = 0; i < np; ++i) {
        void* p = Rd<void*>(params, i * sizeof(void*));
        void* pt = Rd<void*>(p, 0x08);
        bool isArr = RttiName(pt).rfind("array:", 0) == 0;
        if (!(isArr || (Rd<uint32_t>(p, 0x24) & 0x200))) continue;  // arrays and out-params share the locals block
        uint32_t sz = 16;  // a TDynArray slot or a scalar fits 16; an out-struct needs a slot its own size
        if (!isArr && RttiKind(pt) == 2) sz = (RttiSize(pt) + 15) & ~15u;
        refOffset[i] = blockSize;
        blockSize += sz;
    }
    if (blockSize) g_localsBase = new uint8_t[blockSize]();
    std::vector<uint32_t> valOffset(np ? np : 1, 0);  // each by-value struct with strings/arrays/objects in g_paramsBlock
    uint32_t valSize = 0;
    for (uint32_t i = 0; i < np && static_cast<int>(i) < given; ++i) {
        void* p = Rd<void*>(params, i * sizeof(void*));
        void* pt = Rd<void*>(p, 0x08);
        if (RttiKind(pt) != 2 || (Rd<uint32_t>(p, 0x24) & 0x200)) continue;  // every by-value struct may need one
        valOffset[i] = valSize;
        valSize += (RttiSize(pt) + 15) & ~15u;
    }
    if (valSize) g_paramsBlock = new uint8_t[valSize]();
    // a struct by value through g_paramsBlock: its own copy there, read with 0x10 + a synthetic CProperty
    auto byValue = [&](uint32_t i, void* ptype, int idx, const std::string& pn) -> std::string {
        uint32_t off = valOffset[i];
        uint8_t* slot = g_paramsBlock + off;
        g_paramVals.push_back({slot, ptype});
        std::string e = lua_isnil(L, idx) ? "" : StructAssign(L, idx, slot, ptype);
        if (!e.empty()) return "argument " + pn + ": " + e;
        uint8_t* synth = new uint8_t[0x30]();
        memcpy(synth + 0x08, &ptype, 8);
        memcpy(synth + 0x20, &off, 4);
        g_propStore.push_back(synth);
        bc.push_back(0x10);
        put(&synth, 8);
        return "";
    };
    for (uint32_t i = 0; i < np; ++i) {
        void* prop = Rd<void*>(params, i * sizeof(void*));
        std::string tn = RttiName(Rd<void*>(prop, 0x08));
        std::string pn = NameOfId(Rd<uint32_t>(prop, 0x10));
        int idx = first + static_cast<int>(i);
        // a trailing argument the caller left off, or nil for an optional one: the "omitted" opcode (0x00), the
        // function sees its default (FindGameplayEntitiesInRange(arr, p, r, n, nil, nil, nil, 'CActor'))
        bool optional = (Rd<uint32_t>(prop, 0x24) & 0x400) != 0;  // bit 10 (docs/engine/scripting.md, parameter flags)
        // A by-reference parameter (an array, an out value) is never omitted: the callee writes its result through
        // it, and an omitted one had nowhere to go (F6: inv:GetAllItems() without its out array, Lua got
        // nil and the game crashed 2 s later). Left off, it gets an empty slot of its own and comes back as an extra
        // return value, as if nil was given.
        bool byRef = tn.rfind("array:", 0) == 0 || (Rd<uint32_t>(prop, 0x24) & 0x200) != 0;
        if (!byRef && (static_cast<int>(i) >= given || (optional && lua_isnil(L, idx)))) {
            bc.push_back(0x00);
            continue;
        }
        // By-reference parameters (CProperty flag bit 9 = 0x200; arrays are always by-reference whether or not the
        // bit is set) go through the 0x0f/synthetic-CProperty path: a slot in the locals block plus a synthetic
        // CProperty the handler resolves against ScriptFrame+0x20. Arrays get a TDynArray slot (below); an
        // out-scalar (out string/name/int/...) gets a plain value slot here. A by-reference struct (out Vector) is
        // bigger than one slot and still refused.
        bool isArr = tn.rfind("array:", 0) == 0;
        if (!isArr && (Rd<uint32_t>(prop, 0x24) & 0x200)) {  // an out value by reference: a scalar or a plain struct
            void* ptype = Rd<void*>(prop, 0x08);
            int pk = RttiKind(ptype);
            uint32_t psz = RttiSize(ptype);
            bool scalar = (pk == 11 || pk == 0 || pk == 1) && psz > 0 && psz <= 16;  // fundamental, String/CName, enum
            bool plain = pk == 2 && psz > 0 && psz <= 256 && IsPlainStruct(ptype, 0);  // out Vector/Matrix/Box ...
            // a struct with strings, arrays or objects (OnGameCameraTick's SCameraMovementData): its slot gets its
            // own copy field by field
            bool rich = pk == 2 && psz > 0 && psz <= 4096 && !plain;
            if (!scalar && !plain && !rich)
                return "argument " + pn + " is an out/by-reference " + tn + " (not supported yet)";
            uint32_t slotOffset = refOffset[i];
            uint8_t* slot = g_localsBase + slotOffset;  // zeroed; a number, enum or plain struct given goes in first
            // An out parameter is a reference: the function sees the caller's value (GetUserMapPinLimits
            // reads its two "out" ints as the limits to set; zeroed slots would set them to 0). Strings and names
            // stay out-only, their slot would own the copy.
            if (!lua_isnoneornil(L, idx) && (pk == 11 || pk == 1 || plain || rich)) {
                std::string err = (plain || rich) ? StructAssign(L, idx, slot, ptype) : WriteValue(L, idx, slot, ptype);
                if (!err.empty() && rich) {  // never a half-copied struct (orig() would write it back over the caller's)
                    ReleaseValue(slot, ptype, 0);
                    return "argument " + pn + ": " + err;
                }
                if (!err.empty()) memset(slot, 0, psz);  // a placeholder of another kind: a pure out value, as before
            }
            uint8_t* synth = new uint8_t[0x30]();        // same synthetic CProperty recipe as the array path
            memcpy(synth + 0x08, &ptype, 8);
            memcpy(synth + 0x20, &slotOffset, 4);
            g_propStore.push_back(synth);
            g_outArgs.push_back({slot, ptype, false});
            bc.push_back(0x0f);
            put(&synth, 8);
            continue;
        }
        if (tn == "Int32") {
            if (!lua_isnumber(L, idx)) return "argument " + pn + " needs an int";
            bc.push_back(0x04);
            int32_t v = static_cast<int32_t>(lua_isinteger(L, idx) ? lua_tointeger(L, idx) : static_cast<lua_Integer>(lua_tonumber(L, idx)));
            put(&v, 4);
        } else if (tn == "Float") {
            if (!lua_isnumber(L, idx)) return "argument " + pn + " needs a number";
            float v = static_cast<float>(lua_tonumber(L, idx));
            // NaN and 1e15 (infinite as a float) would go into the game as they are: hours per minute NaN
            if (!std::isfinite(v)) return "argument " + pn + " needs a finite number (not NaN, not beyond 3.4e38)";
            bc.push_back(0x06);
            put(&v, 4);
        } else if (tn == "Bool") {
            bc.push_back(lua_toboolean(L, idx) ? 0x0a : 0x0b);
        } else if (tn == "String" || tn == "CName") {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            if (!s) return "argument " + pn + " needs a string";
            if (tn == "String") {
                bc.push_back(0x07);
                uint32_t l = static_cast<uint32_t>(len);
                put(&l, 4);
                put(s, len);
            } else {
                bc.push_back(0x08);
                uint32_t id = InternName(s);
                put(&id, 4);
            }
        } else if (tn.rfind("array:", 0) == 0) {  // by-reference: a TDynArray slot we own, read back after the call
            void* elemType = ArrayInner(tn);
            uint32_t esize = RttiSize(elemType);
            if (!esize) return "argument " + pn + ": unknown element size of " + tn;
            uint32_t slotOffset = refOffset[i];
            uint8_t* tda = g_localsBase + slotOffset;  // this argument's slot inside the shared locals block
            bool borrowed = false;
            if (auto av = static_cast<Arr*>(luaL_testudata(L, idx, kArr))) {
                // a live array (a hook's argument, a field): handed on as it is, {data, count} borrowed - the
                // callee sees the same elements, nothing is copied or freed afterwards (orig() of
                // OnDialogChoicesSet hands on its array<SSceneChoice> this way)
                if (av->fixed || RttiName(av->type) != tn) return "argument " + pn + ": a " + tn + " is needed";
                if (!SafeRead(av->addr, tda, 12)) return "argument " + pn + ": the array could not be read";
                borrowed = true;
            } else if (lua_istable(L, idx)) {
                int n = static_cast<int>(lua_rawlen(L, idx));
                if (n > 0) {
                    void* data = AllocBytes(static_cast<size_t>(n) * esize);
                    if (!data) return "argument " + pn + ": could not allocate the array";
                    memset(data, 0, static_cast<size_t>(n) * esize);
                    for (int k = 1; k <= n; ++k) {
                        lua_rawgeti(L, idx, k);
                        std::string e = WriteValue(L, -1, static_cast<uint8_t*>(data) + static_cast<size_t>(k - 1) * esize, elemType);
                        lua_pop(L, 1);
                        if (!e.empty()) { FreeBytes(data); return "argument " + pn + " element " + std::to_string(k) + ": " + e; }
                    }
                    memcpy(tda, &data, 8);
                    uint32_t cnt = static_cast<uint32_t>(n);
                    memcpy(tda + 8, &cnt, 4);
                }
            } else if (!lua_isnoneornil(L, idx)) {
                return "argument " + pn + " needs a table, an array or nil for " + tn;
            }
            g_outArgs.push_back({tda, elemType, true, borrowed,
                                 borrowed ? static_cast<Arr*>(lua_touserdata(L, idx))->addr : nullptr});
            // The 0x0f operand is a CProperty*, not the slot's address (FINDINGS_OUTPARAM3.md): type = this
            // argument's real declared type (from `prop`, already read above - no extra RTTI lookup needed),
            // offset = this slot's byte offset within g_localsBase, flags = 0 (no extra object indirection -
            // ScriptFrame+0x20 already points straight at the locals block).
            uint8_t* synth = new uint8_t[0x30]();
            void* ptype = Rd<void*>(prop, 0x08);
            memcpy(synth + 0x08, &ptype, 8);
            memcpy(synth + 0x20, &slotOffset, 4);
            g_propStore.push_back(synth);
            bc.push_back(0x0f);  // out / by-reference: a CProperty* resolved against ScriptFrame+0x20
            put(&synth, 8);
        } else if (RttiKind(Rd<void*>(prop, 0x08)) == 2 && !IsPlainStruct(Rd<void*>(prop, 0x08), 0)) {
            // a struct with strings, arrays or objects, by value
            std::string e = byValue(i, Rd<void*>(prop, 0x08), idx, pn);
            if (!e.empty()) return e;
        } else {
            void* type = Rd<void*>(prop, 0x08);
            if (nested >= kArgSlots) return "more than " + std::to_string(kArgSlots) + " arguments of other types";
            std::vector<uint8_t> bytes;
            std::string e = EncodeArg(L, idx, type, bytes);
            void* argFn = e.empty() ? PrepareArgFn(nested, type, e) : nullptr;
            if (!argFn && RttiKind(type) == 2) {  // a plain struct no game function returns (SEntitySpawnData)
                e = byValue(i, type, idx, pn);
                if (!e.empty()) return e;
                continue;
            }
            if (!argFn) return "argument " + pn + ": " + e;
            g_slots[nested] = bytes;
            bc.push_back(0x1c);  // nested call: TW3SE_LuaArg<nested>(nested)
            uint16_t follow = 2 + 8 + 5 + 1, line = 0;
            put(&follow, 2);
            put(&line, 2);
            put(&argFn, 8);
            bc.push_back(0x04);
            int32_t slot = nested++;
            put(&slot, 4);
            bc.push_back(0x20);
        }
    }
    bc.push_back(0x20);  // end of arguments
    g_invokeParams = g_paramsBlock;  // the frame of the call these arguments go to (0x10 reads ScriptFrame+0x28)
    return "";
}

// A nested call to `target` (its CFunction pointer embedded directly, the way the compiler writes it, measured,
// docs/engine/bytecode.md): 0x1c, u16 bytes-that-follow, u16 line, the pointer, the argument bytecode
// (which ends in its own 0x20). Works for a native and for a WitcherScript function alike (the VM's 0x1c handler
// runs either). `args` already ends in 0x20.
std::vector<uint8_t> NestedCall(void* target, const std::vector<uint8_t>& args) {
    std::vector<uint8_t> bc;
    bc.push_back(0x1c);
    uint16_t follow = static_cast<uint16_t>(2 + 8 + args.size()), line = 0;
    auto put = [&](const void* p, size_t n) { bc.insert(bc.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    put(&follow, 2);
    put(&line, 2);
    put(&target, 8);
    bc.insert(bc.end(), args.begin(), args.end());
    return bc;
}

// Calls `target` with argument bytecode `args` (ending in 0x20) and writes the return into `result`. A native is
// invoked directly; a WitcherScript function is run by feeding a nested call to it as the single argument of
// TW3SE_LuaSink, whose handler reads that argument - which makes the VM execute the function. `self` is the context
// (null for a global function, the object for a method).
bool RunCall(void* target, bool isNative, void* self, const std::vector<uint8_t>& args, void* result, bool* faulted) {
    if (isNative) return InvokeNative(target, self, args.data(), result, faulted, g_localsBase);
    void* sink = OurNative("TW3SE_LuaSink");
    if (!sink) {
        if (faulted) *faulted = false;
        return false;
    }
    std::vector<uint8_t> frame = NestedCall(target, args);
    frame.push_back(0x20);  // end of the sink's own argument list
    return InvokeNative(sink, self, frame.data(), result, faulted, g_localsBase);
}

// --- latent functions. For a latent WitcherScript function the VM's dispatcher builds the
// callee's frame (0x120 bytes, arguments bound) and appends it to the frame stack of the calling frame's script
// thread (callingFrame+0x40, thread+0x10); it never starts a thread itself. Lua has no thread, so the sink frame
// carries a stand-in thread whose stack only collects that frame; TW3SE then starts it on a real thread with
// StartScriptThread (the way the engine's own entry points do: frame +0x40/+0x110 cleared) and hands Lua a latent
// object: th.done (the thread is gone, seen in the CScriptThread destructor), th:wait() inside w3.run. A latent
// native waits inside a script thread the VM steps; for those with a non-latent twin (ActionMoveTo ->
// ActionMoveToAsync, FadeIn -> FadeInAsync) the twin is called and done is the actor's current action ending
// (IsCurrentActionInProgress) or the fade (IsFading).
const char* const kLatent = "tw3se.latent";
struct Latent {
    int kind;         // 1 a script thread, 2 an actor's action, 3 a fade, 0 done at once
    uint64_t id;      // kind 1: the thread's id in g_latentLive
    bool result;      // kinds 2/3: what the twin returned (false: it did not start)
    uint64_t until;   // kinds 2/3: not done before this tick (GetTickCount64)
};
std::mutex g_latentMx;
std::unordered_map<void*, uint64_t> g_latentLive;  // script threads started by Lua -> their id, while they live
std::unordered_set<uint64_t> g_latentIds;
uint64_t g_latentNext = 1;
// The return value of a latent call from Lua (docs/engine/scripting.md section 17): StartScriptThread's 5th
// argument is where the root frame's `return` writes. The buffer lives until the thread has ended and Lua let go of
// the latent object (orphan: Lua went first, the thread frees it).
struct LatentResult {
    std::unique_ptr<uint8_t[]> buf;
    void* type = nullptr;
    bool orphan = false;
};
std::unordered_map<uint64_t, LatentResult> g_latentResults;  // by the latent object's id
void FreeLatentResult(LatentResult& r);  // below (needs the value helpers)
// The stand-in thread: only its frame stack (+0x10 data, +0x18 count) is used. The dispatcher grows that array
// with the engine's allocator; the block stays and is reused, the count goes back to 0 after each call.
alignas(16) uint8_t g_latentHost[0x78] = {};
using ThreadDelete_t = void* (*)(void*, uint32_t);
using StartThread_t = void* (*)(void*, void* context, void*, void* frame, void* entry);
ThreadDelete_t oThreadDelete = nullptr;

// A finished thread goes through its deleting destructor (vtable slot 2, TickScriptThreads calls it with 1). That
// one has the destructor body inlined: a hook on ~CScriptThread never saw a thread end.
void* ThreadDeleteHook(void* self, uint32_t flags) {
    uint64_t ended = 0;
    {
        std::lock_guard<std::mutex> l(g_latentMx);
        auto it = g_latentLive.find(self);
        if (it != g_latentLive.end()) {
            ended = it->second;
            g_latentIds.erase(it->second);
            g_latentLive.erase(it);
        }
    }
    void* r = oThreadDelete(self, flags);
    if (ended) {  // Lua let go of the latent object before the thread ended: its result buffer goes now
        auto res = g_latentResults.find(ended);
        if (res != g_latentResults.end() && res->second.orphan) {
            FreeLatentResult(res->second);
            g_latentResults.erase(res);
        }
    }
    return r;
}
bool EnsureThreadHooks() { return Address("StartScriptThread") != 0; }
// Hooked through the first thread Lua starts (its vtable), before it can end: threads are stepped on the main
// thread, which is here.
void EnsureDeleteHook(void* thread) {
    static int state = 0;  // 0 not tried, 1 in, -1 failed
    if (state) return;
    void** vt = Rd<void**>(thread, 0);
    void* del = vt && MemReadable(vt + 2, sizeof(void*)) ? vt[2] : nullptr;
    bool ok = del && Hook(del, reinterpret_cast<void*>(&ThreadDeleteHook), reinterpret_cast<void**>(&oThreadDelete));
    Log("lua: script thread end hook %s (latent functions, witcher3.exe+0x%llx)", ok ? "in" : "NOT in",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(del) - ImageBase()));
    state = ok ? 1 : -1;
}

void PushLatent(lua_State* L, int kind, uint64_t id, bool result, int objIdx) {
    objIdx = lua_absindex(L, objIdx);
    auto t = static_cast<Latent*>(lua_newuserdatauv(L, sizeof(Latent), 1));
    *t = {kind, id, result, 0};
    if (objIdx) lua_pushvalue(L, objIdx);
    else lua_pushnil(L);
    lua_setiuservalue(L, -2, 1);  // the object whose action or fade it waits for
    luaL_setmetatable(L, kLatent);
}

int LLatentIndex(lua_State* L) {
    auto t = static_cast<Latent*>(luaL_checkudata(L, 1, kLatent));
    const char* key = luaL_checkstring(L, 2);
    if (!strcmp(key, "wait")) {  // th:wait(): w3.wait_until(th.done), inside w3.run
        lua_getglobal(L, "w3");
        lua_getfield(L, -1, "_latent_wait");
        return 1;
    }
    if (!strcmp(key, "result")) {
        if (t->kind == 2 || t->kind == 3) {
            lua_pushboolean(L, t->result);
            return 1;
        }
        bool running;
        {
            std::lock_guard<std::mutex> l(g_latentMx);
            running = t->kind == 1 && g_latentIds.count(t->id);
        }
        auto res = t->kind == 1 && !running ? g_latentResults.find(t->id) : g_latentResults.end();
        if (res != g_latentResults.end() && res->second.type) PushValue(L, res->second.buf.get(), res->second.type, 0, true);
        else lua_pushnil(L);  // still running, or the function returns nothing
        return 1;
    }
    if (strcmp(key, "done") != 0) return 0;
    bool done = true;
    if (t->kind == 1) {
        std::lock_guard<std::mutex> l(g_latentMx);
        done = !g_latentIds.count(t->id);
    } else if ((t->kind == 2 || t->kind == 3) && t->result && GetTickCount64() < t->until) {
        done = false;
    } else if ((t->kind == 2 || t->kind == 3) && t->result) {
        lua_getiuservalue(L, 1, 1);  // the actor, or the game
        int obj = lua_gettop(L);
        lua_getfield(L, obj, t->kind == 2 ? "IsCurrentActionInProgress" : "IsFading");
        lua_pushvalue(L, obj);
        done = lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_toboolean(L, -1);
        lua_settop(L, obj - 1);
    }
    lua_pushboolean(L, done);
    return 1;
}

void FreeLatentResult(LatentResult& r) {  // what the return left in the buffer: a handle's ref, a string's bytes
    uint8_t* slot = r.buf.get();
    if (slot && r.type) {
        ReleaseHandedHandle(slot, r.type);
        if (RttiName(r.type) == "String") FreeBytes(Rd<void*>(slot, 0));
        else if (RttiKind(r.type) == 2 && !IsPlainStruct(r.type, 0)) ReleaseValue(slot, r.type, 0);
    }
    r.buf.reset();
}

int LLatentGc(lua_State* L) {  // Lua lets go of a latent object: its result buffer, now or when its thread ends
    auto t = static_cast<Latent*>(luaL_checkudata(L, 1, kLatent));
    if (t->kind != 1) return 0;
    auto res = g_latentResults.find(t->id);
    if (res == g_latentResults.end()) return 0;
    bool running;
    {
        std::lock_guard<std::mutex> l(g_latentMx);
        running = g_latentIds.count(t->id) != 0;
    }
    if (running) {
        res->second.orphan = true;
    } else {
        FreeLatentResult(res->second);
        g_latentResults.erase(res);
    }
    return 0;
}

int LLatentToStr(lua_State* L) {
    auto t = static_cast<Latent*>(luaL_checkudata(L, 1, kLatent));
    static const char* kinds[] = {"done", "script thread", "action", "fade"};
    lua_pushfstring(L, "latent call (%s)", kinds[t->kind >= 0 && t->kind <= 3 ? t->kind : 0]);
    return 1;
}

// --- natives without an import line: about 100 natives are registered from C++ and no script imports them,
// so their CFunction has no parameters to read and w3.call could pass them nothing. w3.declare gives one a signature
// in WitcherScript's words: CProperties of our own (type +0x08, flags +0x24: 0x200 out, 0x400 optional) and a stand-in
// CFunction (+0x18 flags, +0x20 the return, +0x28/+0x30 the parameters) that BuildArgs reads in its place. The game's
// handler is what runs. A wrong signature makes the handler read its arguments wrong: the call is guarded, but only
// the declaration's author knows whether it fits.
struct Declared {
    std::vector<uint8_t*> props;  // one per parameter, then the return's (kept for the session)
    uint8_t* meta = nullptr;
    std::string text;
};
std::unordered_map<void*, Declared> g_declared;  // the native's CFunction -> its signature

void* DeclType(std::string t, std::string& err) {  // "int", "name", "array<name>", "CEntity", "Vector" -> RTTI type
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    while (!t.empty() && t.back() == ' ') t.pop_back();
    static const std::pair<const char*, const char*> kWords[] = {
        {"int", "Int32"}, {"float", "Float"}, {"bool", "Bool"}, {"string", "String"}, {"name", "CName"},
        {"byte", "Uint8"}, {"Int32", "Int32"}, {"Uint64", "Uint64"}, {"Uint32", "Uint32"}};
    for (const auto& [w, r] : kWords)
        if (t == w) return RttiFindType(r);
    if (t.rfind("array<", 0) == 0 && t.back() == '>') {
        void* inner = DeclType(t.substr(6, t.size() - 7), err);
        if (!inner) return nullptr;
        std::string n = "array:2,0," + RttiName(inner);
        if (void* a = RttiFindType(n.c_str())) return a;
        err = "no array type " + n + " in the game";
        return nullptr;
    }
    if (void* direct = RttiFindType(t.c_str())) {
        if (RttiKind(direct) == 2) {  // a class is passed as a handle (a struct has no handle type: Vector stays)
            std::string h = "handle:" + t;
            if (void* ht = RttiFindType(h.c_str())) return ht;
        }
        return direct;
    }
    err = "unknown type '" + t + "'";
    return nullptr;
}

uint8_t* DeclProp(void* type, uint32_t flags) {
    auto* p = new uint8_t[0x40]();
    memcpy(p + 0x08, &type, 8);
    memcpy(p + 0x24, &flags, 4);
    return p;
}

// w3.declare("Name" or "Class.Name", "int, out array<name>, optional float -> bool"): see above. Returns true; an
// error names what does not fit.
int LDeclare(lua_State* L) {
    std::string full = luaL_checkstring(L, 1);
    std::string sig = luaL_checkstring(L, 2);
    void* fn = nullptr;
    size_t dot = full.find('.');
    if (dot == std::string::npos) {
        fn = FindGlobalFn(full.c_str());
    } else {
        void* cls = FindClassByName(full.substr(0, dot).c_str());
        if (!cls) return luaL_error(L, "w3.declare: no class %s", full.substr(0, dot).c_str());
        for (void* f : FindMethods(cls, full.substr(dot + 1).c_str()))
            if ((Rd<uint32_t>(f, 0x18) & 9) == 1) fn = f;  // the native registered from C++
    }
    if (!fn) return luaL_error(L, "w3.declare: no native %s", full.c_str());
    uint32_t fl = Rd<uint32_t>(fn, 0x18);
    if (!(fl & 1)) return luaL_error(L, "w3.declare: %s is a WitcherScript function, w3.call knows its parameters", full.c_str());
    if ((fl & 8) || Rd<uint32_t>(fn, 0x30) != 0)
        return luaL_error(L, "w3.declare: %s is imported by the scripts, w3.call knows its parameters", full.c_str());
    Declared d;
    d.text = sig;
    std::string params = sig, ret;
    size_t arrow = sig.find("->");
    if (arrow != std::string::npos) {
        params = sig.substr(0, arrow);
        ret = sig.substr(arrow + 2);
    }
    std::string err;
    std::vector<uint8_t*> ps;
    size_t start = 0;
    while (params.find_first_not_of(' ') != std::string::npos && start <= params.size()) {
        size_t comma = params.find(',', start);
        std::string tok = params.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        uint32_t flags = 0;
        for (;;) {
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            if (tok.rfind("out ", 0) == 0) flags |= 0x200, tok.erase(0, 4);
            else if (tok.rfind("optional ", 0) == 0) flags |= 0x400, tok.erase(0, 9);
            else break;
        }
        void* t = DeclType(tok, err);
        if (!t) return luaL_error(L, "w3.declare %s: parameter %d: %s", full.c_str(), static_cast<int>(ps.size()) + 1, err.c_str());
        ps.push_back(DeclProp(t, flags));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (ps.size() > 32) return luaL_error(L, "w3.declare %s: at most 32 parameters", full.c_str());
    uint8_t* retProp = nullptr;
    if (ret.find_first_not_of(' ') != std::string::npos) {
        void* t = DeclType(ret, err);
        if (!t) return luaL_error(L, "w3.declare %s: the return: %s", full.c_str(), err.c_str());
        retProp = DeclProp(t, 0);
    }
    d.props = ps;
    if (retProp) d.props.push_back(retProp);
    auto** list = new void*[ps.empty() ? 1 : ps.size()]();
    for (size_t i = 0; i < ps.size(); ++i) list[i] = ps[i];
    d.meta = new uint8_t[0x100]();
    uint32_t mflags = fl & ~8u;
    uint32_t np = static_cast<uint32_t>(ps.size());
    memcpy(d.meta + 0x18, &mflags, 4);
    memcpy(d.meta + 0x20, &retProp, 8);
    memcpy(d.meta + 0x28, &list, 8);
    memcpy(d.meta + 0x30, &np, 4);
    g_declared[fn] = d;  // a second declaration replaces the first (the old blocks stay: a call may hold them)
    lua_pushboolean(L, 1);
    return 1;
}

// Calls one of `candidates` (the same function name; a native method can appear twice: registered from C++ and
// declared by the scripts' import) with the Lua arguments from `first` on. Pushes the return value.
int CallCandidates(lua_State* L, const char* name, const std::vector<void*>& candidates, void* self, int first) {
    // candidates come most-derived first (FindMethods walks child -> parent). Embedding a CFunction pointer is
    // static dispatch, so the most-derived override is the right one to pick - keep the first of each kind.
    void* decl = nullptr;     // the imported declaration: native, with params/return metadata
    void* nativeTwin = nullptr;  // a native registered from C++ without an import line
    void* scriptFn = nullptr; // a pure WitcherScript function (run through the sink)
    for (void* f : candidates) {
        uint32_t fl = Rd<uint32_t>(f, 0x18);
        if ((fl & 8) && !decl) decl = f;
        else if ((fl & 1) && !nativeTwin) nativeTwin = f;
        else if (!(fl & 1) && !scriptFn) scriptFn = f;
    }
    void* meta = decl ? decl : scriptFn ? scriptFn : nativeTwin;  // whose parameters/return we read
    if (!decl && !scriptFn && nativeTwin) {  // no import: a signature from w3.declare, if one was given
        auto d = g_declared.find(nativeTwin);
        if (d != g_declared.end()) meta = d->second.meta;
    }
    if (!meta) return luaL_error(L, "%s: no candidate to call", name);
    bool latent = (Rd<uint32_t>(meta, 0x18) & 0x40) != 0;
    if (latent && (decl || nativeTwin)) {  // a latent native: LMethod routes the ones with an Async twin
        if (!strcmp(name, "Sleep") || !strcmp(name, "SleepOneFrame"))
            return luaL_error(L, "%s is latent: inside w3.run use w3.wait(seconds) or w3.wait_frames(n)", name);
        return luaL_error(L, "%s is a latent native without a non-latent twin (it needs a script thread)", name);
    }
    if (latent && !EnsureThreadHooks()) return luaL_error(L, "%s is latent and the script thread hooks are missing", name);
    ClearOutArgs();
    std::vector<uint8_t> bc;
    std::string err = BuildArgs(L, first, meta, bc);
    if (!err.empty()) { ClearOutArgs(); return luaL_error(L, "%s: %s", name, err.c_str()); }
    void* retProp = Rd<void*>(meta, 0x20);
    void* retType = Plausible(retProp) ? Rd<void*>(retProp, 0x08) : nullptr;
    if (retType && RttiSize(retType) > 512) { ClearOutArgs(); return luaL_error(L, "%s: return value too large", name); }
    alignas(16) uint8_t result[512] = {};
    bool called = false, faulted = false;
    if (decl || nativeTwin) {  // a native: call its handler directly
        if (decl) called = InvokeNative(decl, self, bc.data(), result, &faulted, g_localsBase);
        if (!called && !faulted && nativeTwin && nativeTwin != decl)
            called = InvokeNative(nativeTwin, self, bc.data(), result, &faulted, g_localsBase);
    } else {  // a WitcherScript function: run it through the sink (a latent one: its frame lands on the stand-in)
        if (latent) g_invokeThread = g_latentHost;
        called = RunCall(scriptFn, false, self, bc, result, &faulted);
        g_invokeThread = nullptr;
    }
    void* started = nullptr;  // the script thread a latent call runs on
    std::unique_ptr<uint8_t[]> latentResult;  // its return value's buffer (kept in g_latentResults by id)
    if (latent && called) {
        uint32_t& n = *reinterpret_cast<uint32_t*>(g_latentHost + 0x18);
        void** frames = *reinterpret_cast<void***>(g_latentHost + 0x10);
        void* frame = n && frames ? frames[n - 1] : nullptr;
        n = 0;
        if (frame) {
            *reinterpret_cast<void**>(static_cast<uint8_t*>(frame) + 0x40) = nullptr;   // the thread sets itself here
            *reinterpret_cast<void**>(static_cast<uint8_t*>(frame) + 0x110) = nullptr;  // as StartScriptThread's callers
            auto start = reinterpret_cast<StartThread_t>(Address("StartScriptThread"));
            if (retType) {  // where the root frame's return writes (th.result)
                uint32_t rs = RttiSize(retType);
                latentResult.reset(new uint8_t[(rs < 16 ? 16 : rs) + 16]());
            }
            started = start(nullptr, Rd<void*>(frame, 0), nullptr, frame, latentResult.get());
            if (started) EnsureDeleteHook(started);
        }
    }
    if (called || faulted) g_argRefs.clear();  // the native took them (after a fault: maybe - a leak is the safe side)
    if (faulted) { ClearOutArgs(); return luaL_error(L, "%s faulted inside the game (caught)", name); }
    if (!called) { ClearOutArgs(); return luaL_error(L, "%s has no native handler", name); }
    if (latent) {  // the thread the VM started for it, or done at once when none was seen
        ClearOutArgs();
        uint64_t id = 0;
        if (started) {
            std::lock_guard<std::mutex> l(g_latentMx);
            id = g_latentNext++;
            g_latentLive[started] = id;
            g_latentIds.insert(id);
        }
        if (id && latentResult) g_latentResults[id] = {std::move(latentResult), retType, false};
        PushLatent(L, id ? 1 : 0, id, true, 0);
        return 1;
    }
    int rets = 0;
    if (retType) {
        PushValue(L, result, retType, 0, true);
        ReleaseHandedHandle(result, retType);  // the returned handle's ref
        ++rets;
    }
    WriteBackBorrowed();
    for (const OutArg& o : g_outArgs) {  // each by-reference argument, read back as an extra value in parameter order
        if (o.borrowed) {  // the caller's own array: its elements as they are now, nothing given back
            PushArrayCopy(L, o.slot, o.type, 0, true);
        } else if (o.isArray) {
            PushArrayCopy(L, o.slot, o.type, 0, true);
            // each handle in the array holds a ref of the game's, each string its buffer; both are given back
            // here
            uint8_t* data = Rd<uint8_t*>(o.slot, 0);
            uint32_t n = Rd<uint32_t>(o.slot, 8), esize = RttiSize(o.type);
            bool str = RttiName(o.type) == "String";
            for (uint32_t k = 0; data && esize && k < n && k < 100000; ++k) {
                ReleaseHandedHandle(data + static_cast<size_t>(k) * esize, o.type);
                if (str) FreeBytes(Rd<void*>(data + static_cast<size_t>(k) * esize, 0));
            }
            FreeBytes(data);  // the array's data buffer
        } else {
            PushValue(L, o.slot, o.type, 0, true);
            ReleaseHandedHandle(o.slot, o.type);
            if (RttiName(o.type) == "String") FreeBytes(Rd<void*>(o.slot, 0));  // the out-string's heap buffer
            if (RttiKind(o.type) == 2 && !IsPlainStruct(o.type, 0)) ReleaseValue(o.slot, o.type, 0);  // its fields
        }
        ++rets;
    }
    ClearOutArgs();
    return rets;
}

Obj* CheckObj(lua_State* L, int idx) { return static_cast<Obj*>(luaL_checkudata(L, idx, kObj)); }

// An object Lua reads, writes or calls: a held object whose block lost it was destroyed in the game.
Obj* CheckLive(lua_State* L, int idx) {
    Obj* o = CheckObj(L, idx);
    if (o->owns && Rd<void*>(o->block, 0x08) != o->ptr)
        luaL_error(L, "the %s is gone (destroyed in the game; w3.valid tells before use)", RttiName(o->cls).c_str());
    if (o->isStruct && OwnerGone(o->ownerBlock, o->ownerPtr))
        ViewGone(L, "the object this " + RttiName(o->cls) + " lies in is gone (fetch it anew from a live object)");
    return o;
}
// the owner of the views pushed from a field of o: o itself, or the object a struct view lies in
ViewOwner OwnerOf(const Obj* o) {
    if (o->isStruct) return {o->ownerBlock, o->ownerPtr};
    return o->owns ? ViewOwner{o->block, o->ptr} : ViewOwner{};
}

// w3.discard(obj): an object w3.new made goes at the next tick, not when Lua's collector gets to the value.
// It gives back this value's reference; the object goes when no other Lua value holds it (true then). An object of
// the game's is refused: the engine still holds it, and Lua's discard of world entities broke the game once.
int LDiscard(lua_State* L) {
    Obj* o = CheckObj(L, 1);
    if (o->isStruct || !o->block || !g_newBlocks.count(o->block))
        return luaL_error(L, "w3.discard: only an object w3.new made (the game owns this one; an entity: e:Destroy())");
    if (o->owns) {
        o->owns = false;
        ReleaseRef(o->block);
    }
    lua_pushboolean(L, !g_newBlocks.count(o->block));
    return 1;
}

int LGc(lua_State* L) {  // Lua collects the value: its reference goes back
    Obj* o = CheckObj(L, 1);
    if (o->owns) ReleaseRef(o->block);
    o->owns = false;
    return 0;
}

int LMethod(lua_State* L) {  // upvalue 1: the method name; arg 1: the object
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    Obj* o = CheckLive(L, 1);
    if (o->isStruct) return luaL_error(L, "%s: a struct has no methods", name);
    std::vector<void*> fns = FindMethods(o->cls, name);
    // a latent native with a non-latent twin (ActionMoveTo -> ActionMoveToAsync): the twin starts it, a latent object
    // waits for its end
    bool latentNative = false;
    for (void* f : fns) latentNative |= (Rd<uint32_t>(f, 0x18) & 0x41) == 0x41;
    if (latentNative) {
        std::string twin = std::string(name) + "Async";
        std::vector<void*> tf = FindMethods(o->cls, twin.c_str());
        if (!tf.empty()) {
            int before = lua_gettop(L);
            int n = CallCandidates(L, twin.c_str(), tf, o->ptr, 2);
            bool started = n == 0 || lua_toboolean(L, before + 1);  // FadeOutAsync returns nothing
            lua_settop(L, before);
            bool fade = !strcmp(name, "FadeIn") || !strcmp(name, "FadeOut");
            bool actor = !strncmp(name, "Action", 6);
            // IsFading turns true one frame after FadeOutAsync (measured): done waits the fade's time at least
            double secs = fade ? (lua_isnumber(L, 2) ? lua_tonumber(L, 2) : 1.0) : 0.1;
            PushLatent(L, fade ? 3 : actor ? 2 : 0, 0, started, 1);
            static_cast<Latent*>(lua_touserdata(L, -1))->until =
                GetTickCount64() + static_cast<uint64_t>((secs > 0 && secs < 3600 ? secs : 0) * 1000);
            return 1;
        }
    }
    return CallCandidates(L, name, fns, o->ptr, 2);
}

int LIndex(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (void* prop = FindProp(o->cls, key)) {
        void* addr = FieldAddr(*o, prop);
        if (!addr) return 0;
        OwnerScope scope(OwnerOf(o));
        PushValue(L, addr, Rd<void*>(prop, 0x08));
        return 1;
    }
    if (!o->isStruct && !FindMethods(o->cls, key).empty()) {
        lua_pushstring(L, key);
        lua_pushcclosure(L, &LMethod, 1);
        return 1;
    }
    return 0;
}

int LNewIndex(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    const char* key = luaL_checkstring(L, 2);
    void* prop = FindProp(o->cls, key);
    if (!prop) return luaL_error(L, "%s has no field %s", RttiName(o->cls).c_str(), key);
    void* addr = FieldAddr(*o, prop);
    if (!addr) return luaL_error(L, "field %s: no script data", key);
    std::string err = WriteValue(L, 3, addr, Rd<void*>(prop, 0x08));
    if (!err.empty()) return luaL_error(L, "%s.%s: %s", RttiName(o->cls).c_str(), key, err.c_str());
    return 0;
}

int LToString(lua_State* L) {
    Obj* o = CheckObj(L, 1);
    lua_pushfstring(L, "%s%s", RttiName(o->cls).c_str(), o->isStruct ? " (struct)" : "");
    return 1;
}

int LEq(lua_State* L) {
    lua_pushboolean(L, CheckObj(L, 1)->ptr == CheckObj(L, 2)->ptr);
    return 1;
}

// --- array views

Arr* CheckArr(lua_State* L, int idx) {
    auto a = static_cast<Arr*>(luaL_checkudata(L, idx, kArr));
    if (OwnerGone(a->ownerBlock, a->ownerPtr)) ViewGone(L, "the object this array lies in is gone (fetch it anew)");
    return a;
}

int LArrLen(lua_State* L) {
    lua_pushinteger(L, ArrCount(CheckArr(L, 1)));
    return 1;
}

int LArrToTable(lua_State* L) {
    Arr* a = CheckArr(L, 1);
    if (a->fixed) {
        uint32_t es = RttiSize(a->inner);
        lua_createtable(L, static_cast<int>(a->fixed), 0);
        for (uint32_t i = 0; es && i < a->fixed; ++i) {
            PushValue(L, static_cast<char*>(a->addr) + static_cast<size_t>(i) * es, a->inner, 1, true);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    PushArrayCopy(L, a->addr, a->inner, 0, true);
    return 1;
}

// --- array growth, any element (before: 4-byte elements only, through the one TDynArray::GrowBy
// instance, which divides the allocator's block size by 4). An array is {data@0, count@8}; its capacity is the
// allocator's block size, there is no field for it (FINDINGS_OUTPARAM2.md). Growing takes a new block from the engine
// allocator (Memory::AllocDefault, what the out-arrays of every call already use), copies the elements, zeroes the
// rest and frees the old block (Memory::Free). The engine grows and frees such a block later like its own.
// What an element owns goes with it: a handle its reference, a string its buffer, a struct or array everything in it.

// Gives back what the value at addr owns and zeroes it (an element that leaves its array).
void ReleaseValue(void* addr, void* type, int depth) {
    if (!addr || !type || depth > 8) return;
    int kind = RttiKind(type);
    if (kind == 7) {
        void* block = Rd<void*>(addr, 0);
        if (Plausible(block)) ReleaseRef(block);
    } else if (kind == 0 && RttiName(type) == "String") {
        FreeBytes(Rd<void*>(addr, 0));
    } else if (kind == 0 && RttiName(type) == "TagList") {
        FreeBytes(Rd<void*>(addr, 0));
    } else if (kind == 2) {
        ForEachClassItem(type, 0x30, [&](void* p, void*) {
            ReleaseValue(static_cast<char*>(addr) + Rd<uint32_t>(p, 0x20), Rd<void*>(p, 0x08), depth + 1);
            return false;
        });
        return;  // the struct's own bytes stay; its fields are zeroed one by one
    } else if (kind == 3) {
        void* inner = ArrayInner(RttiName(type));
        char* data = Rd<char*>(addr, 0);
        uint32_t n = Rd<uint32_t>(addr, 8), es = RttiSize(inner);
        for (uint32_t i = 0; Plausible(data) && es && i < n; ++i) ReleaseValue(data + static_cast<size_t>(i) * es, inner, depth + 1);
        FreeBytes(data);
    } else {
        return;  // numbers, names, enums, bitfields: nothing owned
    }
    std::vector<uint8_t> zero(RttiSize(type), 0);
    SafeWrite(addr, zero.data(), zero.size());
}

// Whether elements of this type can be added and removed: everything whose ownership TW3SE knows.
bool ArrElemMovable(void* inner, std::string& why) {
    int kind = RttiKind(inner);
    std::string tn = RttiName(inner);
    if (!RttiSize(inner)) { why = "the size of " + tn + " is unknown"; return false; }
    if (kind == 11 || kind == 1 || kind == 9 || kind == 7 || kind == 2 || kind == 3 || kind == 6) return true;
    if (kind == 0 && (tn == "CName" || tn == "String" || tn == "CGUID" || tn == "TagList")) return true;
    why = "elements of type " + tn + " cannot be added or removed yet";
    return false;
}

int ArrGrowOne(const Arr* a) {  // appends one zeroed slot, returns its 0-based index, or -1 on failure
    uint32_t n = Rd<uint32_t>(a->addr, 8), es = RttiSize(a->inner);
    char* old = Rd<char*>(a->addr, 0);
    if (!es || n >= (1u << 24) || (old && !Plausible(old))) return -1;
    uint32_t cap = n + 1 + (n + 1) / 2;
    if (cap < 4) cap = 4;
    char* nb = static_cast<char*>(EngineAlloc(static_cast<size_t>(cap) * es, 16));
    if (!nb) return -1;
    memset(nb, 0, static_cast<size_t>(cap) * es);
    if (old && n) memcpy(nb, old, static_cast<size_t>(n) * es);
    uint32_t newN = n + 1;
    if (!SafeWrite(a->addr, &nb, 8) || !SafeWrite(static_cast<char*>(a->addr) + 8, &newN, 4)) {
        FreeBytes(nb);
        return -1;
    }
    if (old) FreeBytes(old);
    return static_cast<int>(n);
}

std::string AssignValue(lua_State* L, int idx, void* addr, void* type);

// A struct from a table of its fields (through the struct's own field writes: `light.color = {Red = 255}`)
// or from a struct value of the same type (a plain struct: its bytes).
std::string StructAssign(lua_State* L, int idx, void* addr, void* type) {
    idx = lua_absindex(L, idx);
    std::string tn = RttiName(type);
    if (auto o = static_cast<Obj*>(luaL_testudata(L, idx, kObj))) {
        if (!o->isStruct || o->cls != type) return "a " + tn + " or a table of its fields is needed";
        if (o->ptr == addr) return "";
        if (!IsPlainStruct(type, 0)) {
            // strings, arrays or objects inside: field by field through their own writes (a choice copied
            // with to_table() goes back to OnDialogChoicesSet this way)
            std::string err;
            int top = lua_gettop(L);
            ForEachClassItem(type, 0x30, [&](void* p, void*) {
                void* ft = Rd<void*>(p, 0x08);
                uint32_t off = Rd<uint32_t>(p, 0x20);
                PushValue(L, static_cast<const char*>(o->ptr) + off, ft, 0, true);
                err = AssignValue(L, -1, static_cast<char*>(addr) + off, ft);
                lua_settop(L, top);
                if (!err.empty()) err = tn + "." + NameOfId(Rd<uint32_t>(p, 0x10)) + ": " + err;
                return !err.empty();
            });
            return err;
        }
        std::vector<uint8_t> b(RttiSize(type));
        if (!SafeRead(o->ptr, b.data(), b.size())) return "the struct could not be read";
        return SafeWrite(addr, b.data(), b.size()) ? "" : "the struct could not be written";
    }
    if (!lua_istable(L, idx)) return "a " + tn + " or a table of its fields is needed";
    return AssignValue(L, idx, addr, type);
}

// Writes the Lua value at idx into an element: a struct from a table of its fields (through the struct's own field
// writes), everything else through WriteValue.
std::string AssignValue(lua_State* L, int idx, void* addr, void* type) {
    idx = lua_absindex(L, idx);
    if (RttiKind(type) == 2 && (lua_istable(L, idx) || lua_isnil(L, idx))) {
        if (lua_isnil(L, idx)) return "";  // a zeroed struct
        int top = lua_gettop(L);
        PushObject(L, addr, type, true);  // the struct's view: view[key] = value writes the field
        int view = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, idx)) {  // stack: view, key, value
            lua_pushcfunction(L, [](lua_State* S) -> int { lua_settable(S, 1); return 0; });  // f(view, key, value)
            lua_pushvalue(L, view);
            lua_pushvalue(L, -4);  // key
            lua_pushvalue(L, -4);  // value
            if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
                std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "a field could not be written";
                lua_settop(L, top);
                return err;
            }
            lua_pop(L, 1);  // the value; the key stays for lua_next
        }
        lua_settop(L, top);
        return "";
    }
    return WriteValue(L, idx, addr, type);
}

// A whole array (copying a template's fields into a new one): from a Lua table or an array view of the
// same element type. The new elements go into one fresh block; the old ones are released with what they own. Plain
// elements (numbers, enums, bitfields) from an array are copied as bytes, the rest one by one through their writes.
std::string ArrayAssign(lua_State* L, int idx, void* addr, void* type) {
    idx = lua_absindex(L, idx);
    void* inner = ArrayInner(RttiName(type));
    std::string why;
    if (!inner) return "the element type of " + RttiName(type) + " is unknown";
    if (!ArrElemMovable(inner, why)) return why;
    auto src = static_cast<Arr*>(luaL_testudata(L, idx, kArr));
    if (!src && !lua_istable(L, idx)) return "a table or an array of " + RttiName(inner) + " is needed";
    if (src && src->inner != inner)
        return "an array of " + RttiName(inner) + " is needed, this one holds " + RttiName(src->inner);
    if (src && src->addr == addr) return "";
    uint32_t es = RttiSize(inner);
    size_t n = src ? ArrCount(src) : lua_rawlen(L, idx);
    if (n >= (1u << 24)) return "too many elements";
    char* nb = nullptr;
    if (n) {
        nb = static_cast<char*>(EngineAlloc(n * es, 16));
        if (!nb) return "no memory for the elements";
        memset(nb, 0, n * es);
    }
    int ik = RttiKind(inner);
    bool plain = src && (ik == 11 || ik == 1 || ik == 9);
    if (plain && n && !SafeRead(ArrData(src), nb, n * es)) {
        FreeBytes(nb);
        return "the source array could not be read";
    }
    ReleaseValue(addr, type, 0);  // the old elements and their block; {data, count} zeroed
    uint32_t count = static_cast<uint32_t>(n);
    if (!SafeWrite(addr, &nb, 8) || !SafeWrite(static_cast<char*>(addr) + 8, &count, 4)) return "the array could not be written";
    if (plain) return "";
    int top = lua_gettop(L);
    for (size_t i = 0; i < n; ++i) {
        if (src) PushValue(L, ArrData(src) + i * es, inner, 1, true);
        else lua_rawgeti(L, idx, static_cast<lua_Integer>(i + 1));
        std::string err = AssignValue(L, -1, nb + i * es, inner);
        lua_settop(L, top);
        if (!err.empty()) return "element " + std::to_string(i + 1) + ": " + err;
    }
    return "";
}

// --- maps. A map field (THashMap, 72 bytes) is an open-addressing hash table: key type +0x00,
// value type +0x08 (bound at the first insert), slots +0x18 (40 bytes each: state byte 1 = used, hash +0x04, key*
// +0x08, value* +0x18; keys and values live in blocks of their own), capacity +0x20, count +0x24. Read in
// CRTTIMapType's methods (vtable at .?AVCRTTIMapType@@): slot 30 finds a key's value, slot 34 inserts a key (a new
// key is constructed and copied from ours, its value constructed empty; both return the value's address), slot 36
// deletes a key. Reading walks the slots; changes go through those methods.
const char* const kMap = "tw3se.map";
struct MapView {
    void* addr;
    void* type;  // the map type (kind 12): key type +0x08, value type +0x10
};
void* MapKeyType(void* t) { return Rd<void*>(t, 0x08); }
void* MapValueType(void* t) { return Rd<void*>(t, 0x10); }

template <class F> void ForEachMapEntry(const void* m, F f) {  // f(key*, value*) -> true stops
    const char* slots = Rd<const char*>(m, 0x18);
    uint32_t cap = Rd<uint32_t>(m, 0x20);
    if (!Plausible(slots) || cap > (1u << 24)) return;
    for (uint32_t i = 0; i < cap; ++i) {
        const char* e = slots + static_cast<size_t>(i) * 40;
        if (Rd<uint8_t>(e, 0) != 1) continue;
        void* k = Rd<void*>(e, 0x08);
        void* v = Rd<void*>(e, 0x18);
        if (Plausible(k) && Plausible(v) && f(k, v)) return;
    }
}

using MapFind_t = void* (*)(void* type, void* map, const void* key);
using MapInsert_t = bool (*)(void* type, void* map, const void* key, void** value);
using MapDelete_t = bool (*)(void* type, void* map, const void* key);
// (the slots are 5.01's; RttiSlot gives them in this build: 5.02 has one more from slot 10 on)
void* MapFindG(void* t, void* m, const void* k) {
    int s = RttiSlot(t, 30);
    if (s < 0) return nullptr;
    __try {
        return (*reinterpret_cast<MapFind_t**>(t))[s](t, m, k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void* MapInsertG(void* t, void* m, const void* k) {  // the value's address (new or existing), null on a fault
    int s = RttiSlot(t, 34);
    if (s < 0) return nullptr;
    __try {
        void* v = nullptr;
        (*reinterpret_cast<MapInsert_t**>(t))[s](t, m, k, &v);
        return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool MapDeleteG(void* t, void* m, const void* k) {
    int s = RttiSlot(t, 36);
    if (s < 0) return false;
    __try {
        return (*reinterpret_cast<MapDelete_t**>(t))[s](t, m, k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A key from Lua in a buffer of the key type; "" or an error. The caller releases it with ReleaseValue.
std::string MapKey(lua_State* L, int idx, void* kt, std::vector<uint8_t>& kb) {
    uint32_t ks = RttiSize(kt);
    if (!ks || ks > 256) return "the map's key type " + RttiName(kt) + " is not usable";
    kb.assign(ks, 0);
    if (lua_isnil(L, idx)) return "a map key is needed";
    return WriteValue(L, idx, kb.data(), kt);
}

void PushMapView(lua_State* L, const void* addr, void* type) {
    auto m = static_cast<MapView*>(lua_newuserdatauv(L, sizeof(MapView), 0));
    *m = {const_cast<void*>(addr), type};
    luaL_setmetatable(L, kMap);
}

void PushMapCopy(lua_State* L, const void* addr, void* type, int depth) {
    void* kt = MapKeyType(type);
    void* vt = MapValueType(type);
    lua_newtable(L);
    if (depth > 4) return;
    ForEachMapEntry(addr, [&](void* k, void* v) {
        PushValue(L, k, kt, depth + 1, true);
        PushValue(L, v, vt, depth + 1, true);
        if (lua_isnil(L, -2)) lua_pop(L, 2);
        else lua_settable(L, -3);
        return false;
    });
}

MapView* CheckMap(lua_State* L, int idx) { return static_cast<MapView*>(luaL_checkudata(L, idx, kMap)); }

int LMapToTable(lua_State* L) {
    MapView* m = CheckMap(L, 1);
    PushMapCopy(L, m->addr, m->type, 0);
    return 1;
}

int LMapKeys(lua_State* L) {
    MapView* m = CheckMap(L, 1);
    void* kt = MapKeyType(m->type);
    lua_newtable(L);
    int i = 0;
    ForEachMapEntry(m->addr, [&](void* k, void*) {
        PushValue(L, k, kt, 1, true);
        lua_rawseti(L, -2, ++i);
        return false;
    });
    return 1;
}

int LMapClear(lua_State* L) {  // every key out, through the map's own delete
    MapView* m = CheckMap(L, 1);
    void* kt = MapKeyType(m->type);
    std::vector<std::vector<uint8_t>> keys;  // copies of the keys first: deleting changes the slots
    ForEachMapEntry(m->addr, [&](void* k, void*) {
        std::vector<uint8_t> kb(RttiSize(kt));
        if (SafeRead(k, kb.data(), kb.size())) keys.push_back(std::move(kb));
        return false;
    });
    int n = 0;
    for (auto& kb : keys) n += MapDeleteG(m->type, m->addr, kb.data()) ? 1 : 0;  // a copied key is only compared
    lua_pushinteger(L, n);
    return 1;
}

int LMapIndex(lua_State* L) {  // m[key]: the value, nil when the key is not there; m.to_table/keys/clear
    MapView* m = CheckMap(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {  // the three methods come first (a key of that name: m:to_table()[k])
        const char* k = lua_tostring(L, 2);
        lua_CFunction f = !strcmp(k, "to_table") ? &LMapToTable : !strcmp(k, "keys") ? &LMapKeys : !strcmp(k, "clear") ? &LMapClear : nullptr;
        if (f) {
            lua_pushcfunction(L, f);
            return 1;
        }
    }
    void* kt = MapKeyType(m->type);
    std::vector<uint8_t> kb;
    std::string err = MapKey(L, 2, kt, kb);
    if (!err.empty()) return luaL_error(L, "map key: %s", err.c_str());
    void* v = MapFindG(m->type, m->addr, kb.data());
    ReleaseValue(kb.data(), kt, 0);
    if (!v) return 0;
    PushValue(L, v, MapValueType(m->type));
    return 1;
}

int LMapNewIndex(lua_State* L) {  // m[key] = value adds or changes; m[key] = nil deletes
    MapView* m = CheckMap(L, 1);
    void* kt = MapKeyType(m->type);
    void* vt = MapValueType(m->type);
    std::vector<uint8_t> kb;
    std::string err = MapKey(L, 2, kt, kb);
    if (!err.empty()) return luaL_error(L, "map key: %s", err.c_str());
    if (lua_isnil(L, 3)) {
        MapDeleteG(m->type, m->addr, kb.data());
        ReleaseValue(kb.data(), kt, 0);
        return 0;
    }
    std::string why;
    if (!ArrElemMovable(vt, why) || !ArrElemMovable(kt, why)) {
        ReleaseValue(kb.data(), kt, 0);
        return luaL_error(L, "map<%s, %s>: %s", RttiName(kt).c_str(), RttiName(vt).c_str(), why.c_str());
    }
    bool existed = MapFindG(m->type, m->addr, kb.data()) != nullptr;
    void* v = MapInsertG(m->type, m->addr, kb.data());
    if (!v) {
        ReleaseValue(kb.data(), kt, 0);
        return luaL_error(L, "the map could not take the key");
    }
    err = AssignValue(L, 3, v, vt);
    if (!err.empty() && !existed) MapDeleteG(m->type, m->addr, kb.data());  // a value that does not fit: no new key
    ReleaseValue(kb.data(), kt, 0);  // the map made its own copy of the key
    if (!err.empty()) return luaL_error(L, "map value: %s", err.c_str());
    return 0;
}

int LMapLen(lua_State* L) {
    lua_pushinteger(L, Rd<uint32_t>(CheckMap(L, 1)->addr, 0x24));
    return 1;
}

int LMapPairs(lua_State* L) {  // pairs(m): over a snapshot (changing the map inside the loop is fine)
    MapView* m = CheckMap(L, 1);
    lua_getglobal(L, "next");
    PushMapCopy(L, m->addr, m->type, 0);
    lua_pushnil(L);
    return 3;
}

int LMapToStr(lua_State* L) {
    MapView* m = CheckMap(L, 1);
    lua_pushfstring(L, "map<%s, %s> (%d)", RttiName(MapKeyType(m->type)).c_str(), RttiName(MapValueType(m->type)).c_str(),
                    static_cast<int>(Rd<uint32_t>(m->addr, 0x24)));
    return 1;
}

// p.mapField = {k = v, ...}: the map gets exactly these entries.
std::string MapAssign(lua_State* L, int idx, void* addr, void* type) {
    idx = lua_absindex(L, idx);
    if (luaL_testudata(L, idx, kMap)) {  // a map view of the same field: nothing to do; another: its entries
        MapView* src = static_cast<MapView*>(lua_touserdata(L, idx));
        if (src->addr == addr) return "";
        PushMapCopy(L, src->addr, src->type, 0);
        std::string e = MapAssign(L, -1, addr, type);
        lua_pop(L, 1);
        return e;
    }
    if (!lua_istable(L, idx)) return "a table of keys and values is needed for a map";
    int top = lua_gettop(L);
    PushMapView(L, addr, type);
    int view = lua_gettop(L);
    lua_pushcfunction(L, &LMapClear);
    lua_pushvalue(L, view);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::string e = lua_tostring(L, -1) ? lua_tostring(L, -1) : "the map could not be cleared";
        lua_settop(L, top);
        return e;
    }
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pushcfunction(L, &LMapNewIndex);
        lua_pushvalue(L, view);
        lua_pushvalue(L, -4);
        lua_pushvalue(L, -4);
        if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
            std::string e = lua_tostring(L, -1) ? lua_tostring(L, -1) : "an entry could not be written";
            lua_settop(L, top);
            return e;
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return "";
}

int LArrInsert(lua_State* L) {  // a:insert(value) appends; a:insert(pos, value) inserts before pos
    Arr* a = CheckArr(L, 1);
    std::string why;
    if (a->fixed) return luaL_error(L, "a fixed array (%d elements) cannot grow", static_cast<int>(a->fixed));
    if (!ArrElemMovable(a->inner, why)) return luaL_error(L, "array<%s>: %s", RttiName(a->inner).c_str(), why.c_str());
    uint32_t n = Rd<uint32_t>(a->addr, 8), es = RttiSize(a->inner);
    lua_Integer pos;
    int valIdx;
    if (lua_gettop(L) >= 3) { pos = luaL_checkinteger(L, 2); valIdx = 3; } else { pos = n + 1; valIdx = 2; }
    if (pos < 1 || pos > static_cast<lua_Integer>(n) + 1)
        return luaL_error(L, "insert position %d out of range 1..%d", (int)pos, n + 1);
    if (ArrGrowOne(a) < 0) return luaL_error(L, "array could not grow");
    char* data = Rd<char*>(a->addr, 0);
    uint32_t newN = Rd<uint32_t>(a->addr, 8);
    if (!Plausible(data) || newN != n + 1) return luaL_error(L, "array grow left it inconsistent");
    char* slot = data + static_cast<size_t>(pos - 1) * es;
    if (static_cast<uint32_t>(pos) < newN) {  // open a gap at pos, then zero it (it holds a copy of its neighbour)
        memmove(slot + es, slot, static_cast<size_t>(newN - pos) * es);
        memset(slot, 0, es);
    }
    std::string err = AssignValue(L, valIdx, slot, a->inner);
    if (!err.empty()) {  // take the slot out again: the array is as it was
        ReleaseValue(slot, a->inner, 0);
        memmove(slot, slot + es, static_cast<size_t>(newN - pos) * es);
        SafeWrite(static_cast<char*>(a->addr) + 8, &n, 4);
        return luaL_error(L, "insert: %s", err.c_str());
    }
    return 0;
}

int LArrRemove(lua_State* L) {  // a:remove([pos]) removes the element and returns it (a struct holding objects: true)
    Arr* a = CheckArr(L, 1);
    std::string why;
    if (a->fixed) return luaL_error(L, "a fixed array (%d elements) cannot shrink", static_cast<int>(a->fixed));
    if (!ArrElemMovable(a->inner, why)) return luaL_error(L, "array<%s>: %s", RttiName(a->inner).c_str(), why.c_str());
    uint32_t n = Rd<uint32_t>(a->addr, 8), es = RttiSize(a->inner);
    if (n == 0) return 0;
    lua_Integer pos = luaL_optinteger(L, 2, n);
    if (pos < 1 || pos > static_cast<lua_Integer>(n)) return luaL_error(L, "remove position %d out of range 1..%d", (int)pos, n);
    char* data = Rd<char*>(a->addr, 0);
    if (!Plausible(data)) return luaL_error(L, "array element cannot be removed");
    char* slot = data + static_cast<size_t>(pos - 1) * es;
    int kind = RttiKind(a->inner);
    if (kind == 2 && !IsPlainStruct(a->inner, 0)) lua_pushboolean(L, 1);  // its copy would point at what is freed now
    else if (kind == 3) PushArrayCopy(L, slot, ArrayInner(RttiName(a->inner)), 0, true);
    else PushValue(L, slot, a->inner, 0, true);  // a handle: the Lua object takes its own reference
    ReleaseValue(slot, a->inner, 0);
    if (static_cast<uint32_t>(pos) < n) memmove(slot, slot + es, static_cast<size_t>(n - pos) * es);
    memset(data + static_cast<size_t>(n - 1) * es, 0, es);
    uint32_t newN = n - 1;
    SafeWrite(static_cast<char*>(a->addr) + 8, &newN, 4);
    return 1;
}

int LArrIndex(lua_State* L) {
    Arr* a = CheckArr(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        if (!strcmp(key, "to_table")) { lua_pushcfunction(L, &LArrToTable); return 1; }
        if (!strcmp(key, "insert")) { lua_pushcfunction(L, &LArrInsert); return 1; }
        if (!strcmp(key, "remove")) { lua_pushcfunction(L, &LArrRemove); return 1; }
        return 0;
    }
    lua_Integer i = luaL_checkinteger(L, 2);
    uint32_t n = ArrCount(a);
    uint32_t esize = RttiSize(a->inner);
    char* data = ArrData(a);
    if (i < 1 || i > static_cast<lua_Integer>(n) || !esize || !Plausible(data)) return 0;
    OwnerScope scope({a->ownerBlock, a->ownerPtr});
    PushValue(L, data + static_cast<size_t>(i - 1) * esize, a->inner, 0, false);
    return 1;
}

int LArrNewIndex(lua_State* L) {
    Arr* a = CheckArr(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    uint32_t n = ArrCount(a);
    uint32_t esize = RttiSize(a->inner);
    char* data = ArrData(a);
    if (i == static_cast<lua_Integer>(n) + 1 && !a->fixed) {  // a[#a+1] = v appends: the same as a:insert(v)
        lua_pushcfunction(L, &LArrInsert);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 3);
        lua_call(L, 2, 0);
        return 0;
    } else if (i < 1 || i > static_cast<lua_Integer>(n)) {
        return luaL_error(L, "array index %d out of range 1..%d (use a:insert(v) to grow)", (int)i, n);
    }
    if (!esize || !Plausible(data)) return luaL_error(L, "array element cannot be written");
    std::string err = AssignValue(L, 3, data + static_cast<size_t>(i - 1) * esize, a->inner);
    if (!err.empty()) return luaL_error(L, "array element %d: %s", (int)i, err.c_str());
    return 0;
}

int LArrToStr(lua_State* L) {
    Arr* a = CheckArr(L, 1);
    lua_pushfstring(L, "array<%s>[%d]", RttiName(a->inner).c_str(), (int)ArrCount(a));
    return 1;
}

bool PCall(lua_State* L, int nargs, int nres, std::string& err);  // defined below (used by the hook dispatcher)

// A hook handler or a Lua native that ran into the time limit or the memory limit would do it again at its next
// call: an endless loop in a hook of a function the game calls every frame stood the game still for 5 s a frame.
// It is taken off at the next tick (OnTick, DropRunaways); an ordinary error leaves it on, as before.
bool IsRunaway(const std::string& err) {
    return err.find("without coming back and was stopped") != std::string::npos ||
           err.find("not enough memory") != std::string::npos;
}
// Skipped at once (the game calls a hooked function many times in one tick, and each call stood 5 s), taken off at
// the next tick.
std::set<int> g_dropHooks;    // hook handler ids
std::set<int> g_dropNatives;  // indexes in g_luaNatives

// --- hooks (stage 1): replace a native's table entry with a dispatcher that runs the original, then lets a Lua
// function observe the call (the object it ran on) and optionally override the return value. The original always
// runs (side effects + the bytecode cursor handled by it), so stage 1 parses no arguments. Per-hook reentrancy
// guard; Lua runs only on the main thread.
std::string g_curMod;  // the Lua mod loading or running right now ("" = none: the console, WitcherScript ...)
std::map<std::string, std::wstring> g_modDirs;  // "mods/<name>" -> its folder (ScanMods, RunEarlyNatives)
void OwnerTable(lua_State* L, const char* list);  // who added which function (the mod loader below)
struct ModScope {  // the current mod for the length of a call
    std::string prev;
    explicit ModScope(const std::string& m) : prev(g_curMod) { g_curMod = m; }
    ~ModScope() { g_curMod = prev; }
};

// --- profiler: the time Lua spends per mod and kind (on_tick, a hook, a native ...). Each scope counts its own
// time only: a hook that fires inside an on_tick function counts for the hook, and the game's original under orig()
// counts for nobody. `luaprof` shows it, w3.profile() returns it.
struct ProfStat {
    double ms = 0, maxMs = 0;
    uint64_t calls = 0;
};
std::map<std::pair<std::string, std::string>, ProfStat> g_prof;  // (mod, kind)
uint64_t g_profTicks = 0;      // ticks since the start or the last reset
double g_profSince = 0;        // when that was (QpcMs)
std::vector<double> g_profChild;  // per open scope: the time of the scopes inside it

double QpcMs() {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / perMs;
}

struct ProfScope {  // record = false: the time is taken from the enclosing scope and kept by nobody
    std::string mod, kind;
    double t0;
    bool record;
    ProfScope(std::string m, std::string k, bool rec = true)
        : mod(std::move(m)), kind(std::move(k)), t0(QpcMs()), record(rec) {
        g_profChild.push_back(0);
    }
    ~ProfScope() {
        double d = QpcMs() - t0;
        double self = d - g_profChild.back();
        g_profChild.pop_back();
        if (!g_profChild.empty()) g_profChild.back() += d;
        if (!record) return;
        ProfStat& s = g_prof[{mod, kind}];
        s.ms += self;
        ++s.calls;
        if (self > s.maxMs) s.maxMs = self;
    }
};

constexpr int kMaxHooks = 64;  // hooked functions (each holds any number of Lua handlers)
struct HookHandler {
    int id;             // the handle w3.hook returned
    int ref;            // the Lua function
    int priority;       // higher runs first (outermost)
    std::string owner;  // the mod that set it: its handler runs as that mod
};
struct HookRec {
    void** slot = nullptr;   // the table entry we replaced (&GScriptNativeTable[op] or the method entry's byte 0)
    void* orig = nullptr;    // the handler that was there (null for a script hook: its fresh slot was empty)
    void* fn = nullptr;      // the CFunction (to read parameter and return types, and to re-invoke the original)
    void* retType = nullptr; // the function's return type, to show/override the result
    std::vector<HookHandler> handlers;  // the chain, highest priority first (several mods on one function)
    bool inProgress = false;
    bool used = false;
    bool parked = false;     // taken off for a script reload; put back on the new function by cls + name
    std::string cls, name;   // what w3.hook was given (cls empty = a global function)
    bool isScript = false;   // a WitcherScript function: hooked by flagging it native + a fresh opcode, orig via Dispatch
    uint32_t origFlags = 0;  // CFunction+0x18 before the hook (to restore the native bit on unhook)
    uint32_t origOpcode = 0; // CFunction+0xe0 before the hook (to restore on unhook)
};
HookRec g_hooks[kMaxHooks];
int g_nextHandlerId = 1;

// frame cursor (ScriptFrame+0x30) helpers - lua.cpp does not include the ScriptFrame struct
uint8_t* FrameCursor(void* frame) { return Rd<uint8_t*>(frame, 0x30); }
void SetFrameCursor(void* frame, uint8_t* p) { SafeWrite(static_cast<char*>(frame) + 0x30, &p, 8); }

// Reads the arguments of the intercepted call once (evaluating them exactly as the original would), pushing each as
// a Lua value onto a fresh table left on top of the stack. Advances the frame cursor to just past the arguments.
// An array argument the engine hands over by value (a call from C++ such as OnDialogChoicesSet: the reader
// writes {data, count} itself, where a script's call gives the array's address) is kept here while the hook's chain
// runs, so the view and orig() see it. DispatchHook trims the list back when its chain ends.
std::deque<std::array<uint8_t, 16>> g_byValueArrays;

void ReadCallArgs(lua_State* L, void* fn, void* frame) {
    lua_newtable(L);
    void** params = Rd<void**>(fn, 0x28);
    uint32_t np = Rd<uint32_t>(fn, 0x30);
    if (!Plausible(params) || np > 32) return;
    for (uint32_t i = 0; i < np; ++i) {
        void* prop = Rd<void*>(params, i * sizeof(void*));
        void* type = Rd<void*>(prop, 0x08);
        std::string tn = RttiName(type);
        alignas(16) uint8_t buf[512] = {};
        ReadScriptParam(frame, buf);  // the real frame resolves every argument type correctly
        if (tn.rfind("array:", 0) == 0) {  // an array: buf holds the TDynArray's address, or the array itself
            void* addr = nullptr;
            memcpy(&addr, buf, 8);
            uint32_t count = 0;
            memcpy(&count, buf + 8, 4);
            if (count != 0 && count < (1u << 24)) {  // by value: {data, count} right in buf
                auto& held = g_byValueArrays.emplace_back();
                memcpy(held.data(), buf, 16);
                PushArrayView(L, held.data(), ArrayInner(tn), type);
            } else if (Plausible(addr)) {
                PushArrayView(L, addr, ArrayInner(tn), type);
            } else {
                lua_pushnil(L);
            }
        } else if (Rd<uint32_t>(prop, 0x24) & 0x200) {  // an out-scalar by-reference: buf holds the address
            void* addr = nullptr;
            memcpy(&addr, buf, 8);
            if (Plausible(addr)) PushValue(L, addr, type, 0, false);
            else lua_pushnil(L);
        } else {
            PushValue(L, buf, type, 0, true);
            ReleaseHandedHandle(buf, type);  // the intercepted call's argument: read once, here
        }
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
}

// The chain of one intercepted call. Level k is handler k; the level after the last runs the original. Each
// handler gets (self, args, orig), where orig() runs the next level with the (possibly edited) args table and returns
// its result. A handler that calls nothing and returns nil, or raises an error, passes the call on to the next level;
// one that returns a value without calling orig() replaces everything below it.
struct ChainCtx {
    int hook;
    void* ctx;
    void* result;
    int argsRef;
    int selfRef;
    std::vector<int> refs;    // the handlers' functions at the time of the call
    std::vector<int> ids;     // their ids (w3.unhook)
    std::vector<std::string> owners;  // their mods
    std::vector<char> called; // level k's orig() was called (index = the level it ran)
    std::vector<int> valRef;  // what that level returned
    bool origRan = false;     // the original ran (a chain that fails before it runs it once with the call's own args)
    // A function with out parameters: orig() runs it over the caller's own frame, its arguments read again from
    // there, so each out value lands in the caller's variable the way the game passes it (a rebuilt
    // OnGameCameraTick would write its SCameraMovementData to the wrong place and crash the camera code).
    bool hasOut = false;
    void* frame = nullptr;
    uint8_t* beforeArgs = nullptr;
};

int RunOriginal(lua_State* L, ChainCtx* c) {  // the original with the args table; pushes its result
    HookRec& h = g_hooks[c->hook];
    if (c->hasOut) {  // the caller's frame, its own arguments (the args table's edits do not apply here)
        uint8_t* end = FrameCursor(c->frame);
        SetFrameCursor(c->frame, c->beforeArgs);
        {
            ProfScope game("", "", false);
            if (h.isScript) RunScriptOriginal(h.fn, c->ctx, c->frame, c->result);
            else CallRawHandler(h.orig, c->ctx, c->frame, c->result);
        }
        SetFrameCursor(c->frame, end);  // DispatchHook ends the call past the arguments, as for every hook
        c->origRan = true;
        if (h.retType) PushValue(L, c->result, h.retType, 0, true);
        else lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->argsRef);
    int argsTbl = lua_gettop(L);
    int n = static_cast<int>(lua_rawlen(L, argsTbl));
    int base = lua_gettop(L) + 1;
    for (int k = 1; k <= n; ++k) lua_rawgeti(L, argsTbl, k);  // unpack onto the stack for BuildArgs
    std::vector<uint8_t> bc;
    std::string err = BuildArgs(L, base, h.fn, bc);
    lua_settop(L, argsTbl - 1);
    // this call's argument blocks are held here, so a Lua call inside the original (a hook's handler calling the
    // game) cannot free them while the original still writes to them
    uint8_t* paramsBlock = g_paramsBlock;
    uint8_t* localsBase = g_localsBase;
    std::vector<std::pair<uint8_t*, void*>> paramVals;
    std::vector<OutArg> outArgs;
    std::vector<uint8_t*> propStore;
    paramVals.swap(g_paramVals);
    outArgs.swap(g_outArgs);
    propStore.swap(g_propStore);
    g_paramsBlock = nullptr;
    g_localsBase = nullptr;
    auto releaseArgs = [&] {
        for (auto& [slot, type] : paramVals) ReleaseValue(slot, type, 0);
        delete[] paramsBlock;
        delete[] localsBase;
        for (uint8_t* p : propStore) delete[] p;
    };
    if (!err.empty()) {
        releaseArgs();
        g_invokeParams = nullptr;
        RestoreArgFns();
        return luaL_error(L, "orig(): %s", err.c_str());
    }
    bool faulted = false;
    {
        ProfScope game("", "", false);  // the game's own time, not the handler's
        // c->ctx came from the dispatcher already this-adjusted; do not apply the adjustment again (applyThisAdjust=false).
        if (h.isScript) InvokeScript(h.fn, c->ctx, bc.data(), c->result, &faulted, localsBase);
        else InvokeNative(h.fn, c->ctx, bc.data(), c->result, &faulted, localsBase, false);
    }
    c->origRan = true;
    for (OutArg& o : outArgs)  // an array handed on: what the callee did to {data, count} goes back
        if (o.borrowed && o.origin) SafeWrite(o.origin, o.slot, 12);
    releaseArgs();
    RestoreArgFns();
    if (h.retType) PushValue(L, c->result, h.retType, 0, true);
    else lua_pushnil(L);
    return 1;
}

int ChainLevel(lua_State* L, ChainCtx* c, int level);

int ChainNext(lua_State* L) {  // the orig() a handler gets; upvalues: the ChainCtx, the level it runs
    auto c = static_cast<ChainCtx*>(lua_touserdata(L, lua_upvalueindex(1)));
    int level = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
    c->called[level] = 1;
    ChainLevel(L, c, level);
    if (c->valRef[level] != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, c->valRef[level]);
    lua_pushvalue(L, -1);
    c->valRef[level] = luaL_ref(L, LUA_REGISTRYINDEX);
    return 1;
}

int ChainLevel(lua_State* L, ChainCtx* c, int level) {  // pushes the level's result
    if (level >= static_cast<int>(c->refs.size())) return RunOriginal(L, c);
    if (g_dropHooks.count(c->ids[level])) return ChainLevel(L, c, level + 1);  // ran into a limit: passed on
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->refs[level]);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->selfRef);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->argsRef);
    lua_pushlightuserdata(L, c);
    lua_pushinteger(L, level + 1);
    lua_pushcclosure(L, &ChainNext, 2);
    std::string err;
    bool ok;
    {
        const std::string& owner = c->owners[level];
        ModScope scope(owner);
        const HookRec& h = g_hooks[c->hook];
        ProfScope prof(owner, "hook " + (h.cls.empty() ? h.name : h.cls + "::" + h.name));
        ok = PCall(L, 3, 1, err);
    }
    bool nextRan = c->called[level + 1] != 0;
    if (!ok) {
        Log("lua: hook error (%s, handler %d): %s\nscript stack, innermost first:\n%s", g_hooks[c->hook].name.c_str(),
            level + 1, err.c_str(), ScriptStackText(false).c_str());
        if (IsRunaway(err)) g_dropHooks.insert(c->ids[level]);
    }
    if (ok && !lua_isnil(L, -1)) return 1;  // the handler's value
    if (ok) lua_pop(L, 1);
    if (nextRan) {  // observed (or failed after orig()): the next level's value stands
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->valRef[level + 1]);
        return 1;
    }
    return ChainLevel(L, c, level + 1);  // passed on
}

// Level 0 under a protected call: an error that escapes the chain (orig() with arguments that do not fit, raised in
// RunOriginal past every handler) ends here and leaves the result slot as it is.
int ChainEntry(lua_State* L) {
    auto c = static_cast<ChainCtx*>(lua_touserdata(L, 1));
    return ChainLevel(L, c, 0);
}

void DispatchHook(int i, void* ctx, void* frame, void* result) {
    HookRec& h = g_hooks[i];
    Crumb("lua: hook", static_cast<uintptr_t>(i), reinterpret_cast<uintptr_t>(ctx));
    DoingScope doing("a Lua hook", static_cast<uintptr_t>(i));
    if (h.inProgress || !OnMainThread() || !g_L || h.handlers.empty()) {  // reentrancy / off-thread: the original
        if (h.isScript) RunScriptOriginal(h.fn, ctx, frame, result);  // real frame: advances the cursor correctly
        else CallRawHandler(h.orig, ctx, frame, result);
        return;
    }
    h.inProgress = true;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    size_t heldMark = g_byValueArrays.size();  // by-value array arguments of this call live until its chain ends
    ChainCtx c;
    c.hook = i;
    c.ctx = ctx;
    c.result = result;
    for (const HookHandler& hh : h.handlers) {
        c.refs.push_back(hh.ref);
        c.ids.push_back(hh.id);
        c.owners.push_back(hh.owner);
    }
    c.called.assign(c.refs.size() + 2, 0);
    c.valRef.assign(c.refs.size() + 2, LUA_NOREF);
    PushObject(L, ctx, nullptr, false);  // self (nil for a global)
    c.selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    uint8_t* beforeArgs = FrameCursor(frame);
    c.frame = frame;
    c.beforeArgs = beforeArgs;
    {
        void** params = Rd<void**>(h.fn, 0x28);
        uint32_t np = Rd<uint32_t>(h.fn, 0x30);
        for (uint32_t k = 0; Plausible(params) && k < np && k < 32; ++k) {
            void* prop = Rd<void*>(params, k * sizeof(void*));
            if ((Rd<uint32_t>(prop, 0x24) & 0x200) && RttiKind(Rd<void*>(prop, 0x08)) != 3) c.hasOut = true;
        }
    }
    ReadCallArgs(L, h.fn, frame);        // args table (reads/evaluates the call's arguments once)
    uint8_t* afterArgs = FrameCursor(frame);  // points at the end-of-arguments marker
    c.argsRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushcfunction(L, &ChainEntry);
    lua_pushlightuserdata(L, &c);
    std::string err;
    if (!PCall(L, 1, 1, err)) {
        Log("lua: hook chain (%s): %s", h.name.c_str(), err.c_str());
        if (!c.origRan) {
            // the game's code still runs, with the call's own arguments (read again from the frame): an event a
            // mod's hook broke would otherwise never happen
            Log("lua: hook chain (%s): the original runs with the call's own arguments", h.name.c_str());
            SetFrameCursor(frame, beforeArgs);
            if (h.isScript) RunScriptOriginal(h.fn, ctx, frame, result);
            else CallRawHandler(h.orig, ctx, frame, result);
            afterArgs = FrameCursor(frame) - 1;
        }
    } else if (!lua_isnil(L, -1) && h.retType && result) {
        // the chain's value is the result. A handle already in the slot stays as it is (writing it again would take
        // one more reference each call). A caller that drops the result passes no slot (each such call was a
        // write to address 0 that a guard took).
        bool same = RttiKind(h.retType) == 7 && luaL_testudata(L, -1, kObj) &&
                    static_cast<Obj*>(lua_touserdata(L, -1))->block == Rd<void*>(result, 0);
        if (!same) WriteValue(L, -1, result, h.retType);
    }
    SetFrameCursor(frame, afterArgs + 1);  // consume the end-of-arguments marker: the intercepted call is complete
    for (int r : c.valRef)
        if (r != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, r);
    luaL_unref(L, LUA_REGISTRYINDEX, c.argsRef);
    luaL_unref(L, LUA_REGISTRYINDEX, c.selfRef);
    lua_settop(L, top);
    g_byValueArrays.resize(heldMark);  // (an array argument is good during its hook, like the call's other arguments)
    h.inProgress = false;
}

template <int I> void HookTramp(void* ctx, void* frame, void* result) { DispatchHook(I, ctx, frame, result); }
using HookFn = void (*)(void*, void*, void*);
template <size_t... I> constexpr std::array<HookFn, sizeof...(I)> MakeTramps(std::index_sequence<I...>) {
    return {&HookTramp<I>...};
}
const std::array<HookFn, kMaxHooks> g_tramps = MakeTramps(std::make_index_sequence<kMaxHooks>());

// The method-table entry for a method opcode (24-byte entries, handler at byte 0), or the native-table entry for a
// global. Returns a pointer to the 8-byte handler slot, or null.
void** HandlerSlot(bool global, uint32_t opcode) {
    if (global) {
        auto t = reinterpret_cast<void**>(Address("GScriptNativeTable"));
        return t ? &t[opcode] : nullptr;
    }
    auto m = reinterpret_cast<char*>(Address("GScriptMethodTable"));
    return m ? reinterpret_cast<void**>(m + static_cast<size_t>(opcode) * 24) : nullptr;
}

// Puts hook slot i on the function cls.name (cls empty = a global). Fills g_hooks[i], keeps its handlers. Returns an
// error text, or "" when installed.
std::string InstallHook(int i, const std::string& cls, const std::string& name) {
    const char* nm = name.c_str();
    void* fn = nullptr;
    if (!cls.empty()) {
        void* c = FindClassByName(cls.c_str());
        if (!c) return "no class " + cls;
        const std::vector<void*>& ms = FindMethods(c, nm);
        if (!ms.empty()) fn = ms.front();
    } else {
        fn = FindGlobalFn(nm);
    }
    if (!fn) return "function " + name + " not found";
    uint32_t flags = Rd<uint32_t>(fn, 0x18), opcode = Rd<uint32_t>(fn, 0xe0);
    if (flags & 0x40) return name + " is latent, cannot hook yet";
    bool isScript = !(flags & 1);  // a WitcherScript function has no own table handler; the dispatcher interprets it
    if (isScript && !Address("Dispatch"))
        return name + " is a WitcherScript function; script hooks need Dispatch, missing on this build";
    if (isScript) EnsureScriptCallHook();  // events the engine raises from C++ go through the hook too
    bool global = (flags & 2) != 0;
    HookFn tramp = g_tramps[i];
    void* retProp = Rd<void*>(fn, 0x20);
    HookRec rec{};
    rec.fn = fn;
    rec.retType = Plausible(retProp) ? Rd<void*>(retProp, 0x08) : nullptr;
    rec.used = true;
    rec.isScript = isScript;
    rec.origFlags = flags;
    rec.origOpcode = opcode;
    rec.cls = cls;
    rec.name = name;
    if (!isScript) {
        // a native: point its own table entry at our trampoline, remember the handler to restore on unhook
        void** slot = HandlerSlot(global, opcode);
        if (!slot) return name + ": handler table not available";
        void* orig = nullptr;
        if (!SafeRead(slot, &orig, 8) || !orig) return name + ": no handler to hook";
        if (!SafeWrite(slot, &tramp, 8)) return name + ": handler table is not writable";
        rec.slot = slot;
        rec.orig = orig;
    } else {
        // a WitcherScript function: give it a fresh, unused opcode pointing at our trampoline, then set the native
        // bit so the dispatcher routes the call through the table (our trampoline) instead of the interpreter. The
        // original body still runs on request via InvokeScript (a native-bit-cleared copy through the dispatcher).
        uint32_t fresh = 4000 + static_cast<uint32_t>(i);  // above the native opcode range (2850..3269), below 4096
        void** slot = HandlerSlot(global, fresh);
        if (!slot) return name + ": handler table not available";
        void* existing = nullptr;
        if (!SafeRead(slot, &existing, 8)) return name + ": handler table not readable";
        if (existing) return name + ": fresh opcode " + std::to_string(fresh) + " unexpectedly in use";
        uint32_t nf = flags | 1;  // the native bit
        if (!SafeWrite(slot, &tramp, 8) || !SafeWrite(static_cast<char*>(fn) + 0xe0, &fresh, 4) ||
            !SafeWrite(static_cast<char*>(fn) + 0x18, &nf, 4))
            return name + ": could not install the script hook";
        if (!global) {  // a method's fresh 24-byte entry: make sure its this-adjust (byte 8) is zero
            int32_t zero = 0;
            SafeWrite(reinterpret_cast<char*>(slot) + 8, &zero, 4);
        }
        rec.slot = slot;
    }
    rec.handlers = std::move(g_hooks[i].handlers);
    g_hooks[i] = std::move(rec);
    return "";
}

// Takes hook i off its function: the table slot and the CFunction get back what they had.
void RemoveHookPatch(HookRec& h) {
    if (h.isScript) {  // clear the fresh table slot and restore the CFunction's flags (native bit) and opcode
        void* nul = nullptr;
        SafeWrite(h.slot, &nul, 8);
        SafeWrite(static_cast<char*>(h.fn) + 0x18, &h.origFlags, 4);
        SafeWrite(static_cast<char*>(h.fn) + 0xe0, &h.origOpcode, 4);
    } else {
        SafeWrite(h.slot, &h.orig, 8);  // restore the original handler
    }
}

// Before a script reload, while the old functions still exist: every hook comes off. ReviveHooks puts it back on
// the new function of the same name after the reload.
void ParkHooks() {
    for (int i = 0; i < kMaxHooks; ++i) {
        HookRec& h = g_hooks[i];
        if (!h.used || h.parked) continue;
        RemoveHookPatch(h);
        h.fn = nullptr;
        h.slot = nullptr;
        h.parked = true;
    }
}

bool g_hooksParked = false;

void ReviveHooks() {
    if (!g_hooksParked) return;
    g_hooksParked = false;
    for (int i = 0; i < kMaxHooks; ++i) {
        HookRec& h = g_hooks[i];
        if (!h.used || !h.parked) continue;
        std::string cls = h.cls, name = h.name;
        std::string err = InstallHook(i, cls, name);
        if (err.empty()) {
            Log("lua: hook %d (%s%s%s) back on after the script reload", i, cls.c_str(), cls.empty() ? "" : ".",
                name.c_str());
            continue;
        }
        Log("lua: hook %d (%s%s%s) dropped after the script reload: %s", i, cls.c_str(), cls.empty() ? "" : ".",
            name.c_str(), err.c_str());
        if (g_L)
            for (const HookHandler& hh : h.handlers) luaL_unref(g_L, LUA_REGISTRYINDEX, hh.ref);
        h = HookRec{};
    }
}

// w3.hook([className,] functionName, fn [, priority]): add a handler. className nil/absent = a global function.
// The handler is fn(self, args, orig): read or change args, call orig() to run the rest of the chain (the original
// at its end), return a value to use as the result. Several handlers on one function form a chain, the highest
// priority first (default 0; equal priorities in the order they were added). Works on native functions (own table
// handler redirected) and on WitcherScript functions (flagged native with a fresh opcode). Returns a handle for
// w3.unhook, which removes that handler only.
int LHook(lua_State* L) {
    const char* cls = nullptr;
    const char* name = nullptr;
    int fnIdx;
    if (lua_type(L, 1) == LUA_TSTRING && lua_type(L, 2) == LUA_TSTRING) {  // (class, name, fn)
        cls = lua_tostring(L, 1);
        name = lua_tostring(L, 2);
        fnIdx = 3;
    } else if (lua_isnil(L, 1)) {  // (nil, name, fn)
        name = luaL_checkstring(L, 2);
        fnIdx = 3;
    } else {  // (name, fn)
        name = luaL_checkstring(L, 1);
        fnIdx = 2;
    }
    luaL_checktype(L, fnIdx, LUA_TFUNCTION);
    int priority = static_cast<int>(luaL_optinteger(L, fnIdx + 1, 0));
    ReviveHooks();  // a hook parked by a reload gets its slot back first
    std::string c = cls ? cls : "";
    int i = 0;
    for (; i < kMaxHooks; ++i)  // the function hooked already: one more handler in its chain
        if (g_hooks[i].used && g_hooks[i].cls == c && g_hooks[i].name == name) break;
    if (i == kMaxHooks) {
        for (i = 0; i < kMaxHooks && g_hooks[i].used; ++i) {}
        if (i == kMaxHooks) return luaL_error(L, "too many hooked functions (max %d)", kMaxHooks);
        std::string err = InstallHook(i, c, name);
        if (!err.empty()) {
            g_hooks[i] = HookRec{};
            return luaL_error(L, "%s", err.c_str());
        }
    }
    lua_pushvalue(L, fnIdx);
    HookHandler hh{g_nextHandlerId++, luaL_ref(L, LUA_REGISTRYINDEX), priority, g_curMod};
    auto& v = g_hooks[i].handlers;
    auto pos = std::find_if(v.begin(), v.end(), [&](const HookHandler& o) { return o.priority < priority; });
    v.insert(pos, hh);
    if (!g_curMod.empty()) {  // the hook belongs to the mod that set it (taken out with it)
        OwnerTable(L, "hooks");
        lua_pushstring(L, g_curMod.c_str());
        lua_rawseti(L, -2, hh.id);
        lua_pop(L, 1);
    }
    lua_pushinteger(L, hh.id);  // a handle for w3.unhook
    return 1;
}

bool UnhookId(lua_State* L, int id) {  // that handler leaves the chain; the last one takes the hook off
    for (int i = 0; i < kMaxHooks; ++i) {
        HookRec& h = g_hooks[i];
        if (!h.used) continue;
        auto it = std::find_if(h.handlers.begin(), h.handlers.end(), [&](const HookHandler& o) { return o.id == id; });
        if (it == h.handlers.end()) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
        h.handlers.erase(it);
        if (h.handlers.empty()) {
            if (!h.parked) RemoveHookPatch(h);
            h = HookRec{};
        }
        return true;
    }
    return false;
}

int LUnhook(lua_State* L) {  // w3.unhook(handle)
    lua_pushboolean(L, UnhookId(L, static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

// --- latent script functions watched (docs/engine/scripting.md section 17): w3.watch_latent(name, fn) runs
// fn(e) when a latent WitcherScript function starts (e.phase "start", e.args) and when its frame ends (e.phase "end",
// e.finished: ran to its end or was killed, e.result). e.frame matches the two. The call itself is left as it is: the
// start comes from the Dispatch detour (scriptstack.cpp) right after the frame went onto its thread, the end from
// CFunction::DestroyLatentFrame, where every latent frame ends. Both run on the main thread inside the game's script
// threads tick: the function reads; a latent call from it would land on the same thread (w3.after(0) for that).
// Watches are kept by name: a script reload makes new CFunctions, the map is made again then.
struct LatentWatch {
    int id;
    std::string cls, name;  // cls "" for a global function
    int ref;
    std::string owner;
};
std::vector<LatentWatch> g_latentWatches;
std::unordered_map<void*, std::vector<int>> g_latentByFn;  // CFunction -> indexes in g_latentWatches
int g_latentMapGen = -1;
int g_latentNextId = 1;
using DestroyLatent_t = void (*)(void* fn, void* frame);
DestroyLatent_t oDestroyLatent = nullptr;

void* LatentFn(const std::string& cls, const std::string& name) {  // the latent script function, or null
    std::vector<void*> fns;
    if (cls.empty()) {
        if (void* f = FindGlobalFn(name.c_str())) fns.push_back(f);
    } else if (void* c = FindClassByName(cls.c_str())) {
        fns = FindMethods(c, name.c_str());
    }
    for (void* f : fns)
        if ((Rd<uint32_t>(f, 0x18) & 0x41) == 0x40) return f;
    return nullptr;
}

void LatentMap() {
    if (g_latentMapGen == g_scriptGen.load()) return;
    g_latentMapGen = g_scriptGen.load();
    g_latentByFn.clear();
    for (size_t i = 0; i < g_latentWatches.size(); ++i)
        if (void* f = LatentFn(g_latentWatches[i].cls, g_latentWatches[i].name))
            g_latentByFn[f].push_back(static_cast<int>(i));
}

void LatentEvent(void* fn, void* ctx, void* frame, bool start) {
    lua_State* L = g_L;
    if (!L || g_hooksParked || !OnMainThread() || g_latentWatches.empty()) return;
    LatentMap();
    auto it = g_latentByFn.find(fn);
    if (it == g_latentByFn.end()) return;
    std::vector<LatentWatch> ws;
    for (int i : it->second)
        if (i < static_cast<int>(g_latentWatches.size()) && g_latentWatches[i].ref != LUA_NOREF) ws.push_back(g_latentWatches[i]);
    for (const LatentWatch& w : ws) {
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, w.ref);
        lua_createtable(L, 0, 6);
        lua_pushstring(L, start ? "start" : "end");
        lua_setfield(L, -2, "phase");
        lua_pushstring(L, (w.cls.empty() ? w.name : w.cls + "." + w.name).c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(frame)));
        lua_setfield(L, -2, "frame");
        void* self = ctx ? ctx : Rd<void*>(frame, 0x00);
        PushObject(L, Plausible(self) ? self : nullptr, nullptr, false, Plausible(self) ? BlockOf(self) : nullptr);
        lua_setfield(L, -2, "self");
        if (start) {  // the bound arguments: the locals buffer at frame+0x20, each at its CProperty offset
            uint8_t* locals = Rd<uint8_t*>(frame, 0x20);
            void** params = Rd<void**>(fn, 0x28);
            uint32_t np = Rd<uint32_t>(fn, 0x30);
            lua_createtable(L, static_cast<int>(np), 0);
            if (Plausible(locals) && np <= 32 && (np == 0 || Plausible(params)))
                for (uint32_t k = 0; k < np; ++k) {
                    void* prop = Rd<void*>(params, k * sizeof(void*));
                    void* type = Rd<void*>(prop, 0x08);
                    if (Plausible(type)) PushValue(L, locals + Rd<uint32_t>(prop, 0x20), type, 0, true);
                    else lua_pushnil(L);
                    lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
                }
            lua_setfield(L, -2, "args");
        } else {  // finished when the cursor stands at the end of the code
            uint32_t codeOff = FieldOr("CFunction.code", 0), sizeOff = FieldOr("CFunction.codeSize", 0);
            bool known = codeOff && sizeOff;
            uint8_t* code = known ? Rd<uint8_t*>(fn, codeOff) : nullptr;
            uint32_t size = known ? Rd<uint32_t>(fn, sizeOff) : 0;
            bool finished = known && code && Rd<uint8_t*>(frame, 0x30) == code + size;
            if (known) lua_pushboolean(L, finished);
            else lua_pushnil(L);
            lua_setfield(L, -2, "finished");
            void* result = Rd<void*>(frame, 0x110);
            if (!Plausible(result)) {  // a thread's root frame returns into the thread's buffer (+0x58), its stack empty
                void* thread = Rd<void*>(frame, 0x40);
                if (Plausible(thread) && Rd<uint32_t>(thread, 0x18) == 0) result = Rd<void*>(thread, 0x58);
            }
            void* retProp = Rd<void*>(fn, 0x20);
            void* retType = Plausible(retProp) ? Rd<void*>(retProp, 0x08) : nullptr;
            if (finished && Plausible(result) && Plausible(retType)) {
                PushValue(L, result, retType, 0, true);
                lua_setfield(L, -2, "result");
            }
        }
        std::string err;
        ModScope scope(w.owner);
        bool ok;
        {
            ProfScope prof(w.owner, "latent " + w.name);
            ok = PCall(L, 1, 0, err);
        }
        if (!ok) {
            Log("lua: latent watch %s (%d): %s", w.name.c_str(), w.id, err.c_str());
            if (IsRunaway(err))
                for (LatentWatch& x : g_latentWatches)
                    if (x.id == w.id) x.ref = LUA_NOREF;  // ran into a limit: off
        }
        lua_settop(L, top);
    }
}

void LatentStarted(void* fn, void* ctx, void* frame) { LatentEvent(fn, ctx, frame, true); }

void DestroyLatentHook(void* fn, void* frame) {
    if (!g_latentWatches.empty()) LatentEvent(fn, nullptr, frame, false);
    oDestroyLatent(fn, frame);
}

// w3.watch_latent("Name" or "Class.Name", fn) -> id; w3.unwatch_latent(id)
int LWatchLatent(lua_State* L) {
    std::string full = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    size_t dot = full.find('.');
    std::string cls = dot == std::string::npos ? "" : full.substr(0, dot);
    std::string name = dot == std::string::npos ? full : full.substr(dot + 1);
    if (!LatentFn(cls, name)) return luaL_error(L, "w3.watch_latent: %s is no latent WitcherScript function", full.c_str());
    if (!oDestroyLatent) {
        void* target = reinterpret_cast<void*>(Address("CFunction::DestroyLatentFrame"));
        if (!target || !Hook(target, reinterpret_cast<void*>(&DestroyLatentHook), reinterpret_cast<void**>(&oDestroyLatent)))
            return luaL_error(L, "w3.watch_latent: the end of latent frames is not known for this game build");
        SetLatentStartCallback(&LatentStarted);
    }
    lua_pushvalue(L, 2);
    int id = g_latentNextId++;
    g_latentWatches.push_back({id, cls, name, luaL_ref(L, LUA_REGISTRYINDEX), g_curMod});
    g_latentMapGen = -1;
    lua_pushinteger(L, id);
    return 1;
}
bool LatentUnwatchId(lua_State* L, int id) {
    for (auto it = g_latentWatches.begin(); it != g_latentWatches.end(); ++it)
        if (it->id == id) {
            if (it->ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
            g_latentWatches.erase(it);
            g_latentMapGen = -1;
            return true;
        }
    return false;
}
int LUnwatchLatent(lua_State* L) {
    lua_pushboolean(L, LatentUnwatchId(L, static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LatentUnwatchOwner(lua_State* L, const std::string& owner) {
    std::vector<int> ids;
    for (const LatentWatch& w : g_latentWatches)
        if (w.owner == owner) ids.push_back(w.id);
    for (int id : ids) LatentUnwatchId(L, id);
    return static_cast<int>(ids.size());
}

}  // namespace
}  // namespace tw3se

// --- engine functions by name: w3.engine_hook(name, fn) runs fn before the engine function the address
// list names, with its four register arguments as integers (rcx, rdx, r8, r9; pointers too) and the next four from
// the stack; a table fn returns replaces the four registers. Only on the main thread: a call on another thread passes
// by. The stubs are in enghook.asm, one per slot; a slot's MinHook hook stays for the session (another thread may be
// inside its stub), and w3.engine_unhook only takes its Lua functions off.
constexpr int kEngSlots = 16;
extern "C" void* g_enghookOrig[kEngSlots] = {};
#define TW3SE_ENGTHUNK(n) extern "C" void enghook_thunk_##n();
TW3SE_ENGTHUNK(0) TW3SE_ENGTHUNK(1) TW3SE_ENGTHUNK(2) TW3SE_ENGTHUNK(3) TW3SE_ENGTHUNK(4) TW3SE_ENGTHUNK(5)
TW3SE_ENGTHUNK(6) TW3SE_ENGTHUNK(7) TW3SE_ENGTHUNK(8) TW3SE_ENGTHUNK(9) TW3SE_ENGTHUNK(10) TW3SE_ENGTHUNK(11)
TW3SE_ENGTHUNK(12) TW3SE_ENGTHUNK(13) TW3SE_ENGTHUNK(14) TW3SE_ENGTHUNK(15)
#undef TW3SE_ENGTHUNK
static void (*const kEngThunks[kEngSlots])() = {
    &enghook_thunk_0, &enghook_thunk_1, &enghook_thunk_2, &enghook_thunk_3, &enghook_thunk_4, &enghook_thunk_5,
    &enghook_thunk_6, &enghook_thunk_7, &enghook_thunk_8, &enghook_thunk_9, &enghook_thunk_10, &enghook_thunk_11,
    &enghook_thunk_12, &enghook_thunk_13, &enghook_thunk_14, &enghook_thunk_15};

namespace tw3se {
namespace {
struct EngHandler {
    int id;
    int ref;
    std::string owner;
};
struct EngSlot {
    std::string name;
    void* target = nullptr;
    std::vector<EngHandler> handlers;  // main thread only
    std::atomic<uint64_t> calls{0};    // every call, any thread
    bool busy = false;                 // a handler of this slot runs (the function called again inside it)
};
EngSlot g_eng[kEngSlots];
int g_engNextId = 1;

void EngPre(int slot, uint64_t* regs, const uint64_t* stack) {
    EngSlot& s = g_eng[slot];
    s.calls.fetch_add(1, std::memory_order_relaxed);
    lua_State* L = g_L;
    if (!L || s.busy || s.handlers.empty() || !OnMainThread()) return;
    s.busy = true;
    std::vector<EngHandler> hs = s.handlers;  // a handler may unhook itself
    for (const EngHandler& h : hs) {
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
        lua_createtable(L, 8, 1);
        for (int i = 0; i < 4; ++i) {
            lua_pushinteger(L, static_cast<lua_Integer>(regs[i]));
            lua_rawseti(L, -2, i + 1);
        }
        for (int i = 0; i < 4; ++i) {  // the stack arguments: there or not, the function decides (read only)
            uint64_t v = 0;
            SafeRead(stack + i, &v, 8);
            lua_pushinteger(L, static_cast<lua_Integer>(v));
            lua_rawseti(L, -2, i + 5);
        }
        lua_pushstring(L, s.name.c_str());
        lua_setfield(L, -2, "name");
        std::string err;
        ModScope scope(h.owner);
        bool ok;
        {
            ProfScope prof(h.owner, "engine hook " + s.name);
            ok = PCall(L, 1, 1, err);
        }
        if (!ok) {
            Log("lua: engine hook %s (handler %d): %s", s.name.c_str(), h.id, err.c_str());
            if (IsRunaway(err))  // the time or memory limit: off at once
                for (EngHandler& x : s.handlers)
                    if (x.id == h.id) x.ref = LUA_NOREF;
        } else if (lua_istable(L, -1)) {  // new values for the four registers
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_isinteger(L, -1)) regs[i] = static_cast<uint64_t>(lua_tointeger(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_settop(L, top);
    }
    s.handlers.erase(std::remove_if(s.handlers.begin(), s.handlers.end(), [&](const EngHandler& x) {
                         if (x.ref != LUA_NOREF) return false;
                         Log("lua: engine hook %s handler %d taken off (it ran into a limit)", s.name.c_str(), x.id);
                         return true;
                     }),
                     s.handlers.end());
    s.busy = false;
}

// w3.engine_hook(name, fn) -> id: see above. The name is one of the address list's (GetAddress for plugins).
int LEngineHook(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::string why;
    void* target = reinterpret_cast<void*>(EngineResolve(name, &why));
    if (!target) return luaL_error(L, "w3.engine_hook: %s", why.c_str());
    int slot = -1;
    for (int i = 0; i < kEngSlots && slot < 0; ++i)
        if (g_eng[i].target == target) slot = i;
    if (slot < 0) {
        for (int i = 0; i < kEngSlots && slot < 0; ++i)
            if (!g_eng[i].target) slot = i;
        if (slot < 0) return luaL_error(L, "w3.engine_hook: all %d engine hook slots are taken", kEngSlots);
        if (!Hook(target, reinterpret_cast<void*>(kEngThunks[slot]), &g_enghookOrig[slot]))
            return luaL_error(L, "w3.engine_hook: %s could not be hooked", name.c_str());
        g_eng[slot].name = name;
        g_eng[slot].target = target;
        Log("lua: engine hook on %s (slot %d)", name.c_str(), slot);
    }
    lua_pushvalue(L, 2);
    int id = g_engNextId++;
    g_eng[slot].handlers.push_back({id, luaL_ref(L, LUA_REGISTRYINDEX), g_curMod});
    lua_pushinteger(L, id);
    return 1;
}

bool EngUnhookId(lua_State* L, int id) {
    for (EngSlot& s : g_eng)
        for (auto it = s.handlers.begin(); it != s.handlers.end(); ++it)
            if (it->id == id) {
                if (it->ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
                s.handlers.erase(it);
                return true;
            }
    return false;
}
int LEngineUnhook(lua_State* L) {
    lua_pushboolean(L, EngUnhookId(L, static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

// w3.mem: memory for w3.engine_call's out values and structs. alloc(n) -> address (zeroed, the engine's allocator, so
// the game may keep it), read(address, n) -> string (nil if unreadable), write(address, string), free(address).
int LMemAlloc(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n <= 0 || n > (1 << 30)) return luaL_error(L, "w3.mem.alloc: 1 to 1 GB");
    void* p = EngineAlloc(static_cast<size_t>(n), 16);
    if (!p) return luaL_error(L, "w3.mem.alloc: no engine memory");
    memset(p, 0, static_cast<size_t>(n));
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(p)));
    return 1;
}
int LMemRead(lua_State* L) {
    auto p = reinterpret_cast<const void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n < 0 || n > (1 << 30)) return luaL_error(L, "w3.mem.read: 0 to 1 GB");
    std::string s(static_cast<size_t>(n), '\0');
    if (!SafeRead(p, s.data(), s.size())) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
int LMemWrite(lua_State* L) {
    auto p = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    size_t n = 0;
    const char* s = luaL_checklstring(L, 2, &n);
    lua_pushboolean(L, SafeWrite(p, s, n));
    return 1;
}
int LMemFree(lua_State* L) {
    auto p = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    if (auto freeFn = Fn<void (*)(void*)>("Memory::Free")) freeFn(p);
    return 0;
}

// w3.engine_addr(name) -> the function's offset in witcher3.exe, or nil and why (names as w3.engine_hook takes them)
// w3.find_code(signature [, max]) -> {address, ...}: every match in witcher3.exe's code (at most max, 16)
int LFindCode(lua_State* L) {
    std::vector<uintptr_t> hits = SignatureScan(luaL_checkstring(L, 1), static_cast<size_t>(luaL_optinteger(L, 2, 16)));
    lua_createtable(L, static_cast<int>(hits.size()), 0);
    for (size_t i = 0; i < hits.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(hits[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
// w3.patch(place, bytes [, offset]) -> id, or nil and why. place: an address (below 0x10000000 an offset in
// witcher3.exe), an engine name or a signature that matches once; bytes: hex text ("90 90 C3") or {0x90, ...}
int LPatch(lua_State* L) {
    std::string err, name;
    uintptr_t at = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        name = lua_tostring(L, 1);
        at = PatchPlace(&name, 0, &err);
    } else {
        at = PatchPlace(nullptr, static_cast<uintptr_t>(luaL_checkinteger(L, 1)), &err);
    }
    std::vector<uint8_t> bytes;
    if (lua_istable(L, 2)) {
        for (lua_Integer i = 1, n = static_cast<lua_Integer>(lua_rawlen(L, 2)); i <= n; ++i) {
            lua_rawgeti(L, 2, i);
            bytes.push_back(static_cast<uint8_t>(luaL_checkinteger(L, -1)));
            lua_pop(L, 1);
        }
    } else {
        const char* s = luaL_checkstring(L, 2);
        for (const char* p = s; *p;) {
            if (*p == ' ') { ++p; continue; }
            if (!isxdigit(static_cast<unsigned char>(p[0])) || !isxdigit(static_cast<unsigned char>(p[1])))
                return luaL_argerror(L, 2, "hex pairs like \"90 90 C3\", or a table of numbers");
            bytes.push_back(static_cast<uint8_t>(strtoul(std::string(p, 2).c_str(), nullptr, 16)));
            p += 2;
        }
    }
    int id = 0;
    if (at) id = PatchBytes(at + static_cast<uintptr_t>(luaL_optinteger(L, 3, 0)), bytes, g_curMod, &err);
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}
int LUnpatch(lua_State* L) {  // w3.unpatch(id): the old bytes back
    lua_pushboolean(L, PatchRestore(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LEngineAddr(lua_State* L) {
    std::string why;
    uintptr_t a = EngineResolve(luaL_checkstring(L, 1), &why);
    if (!a) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(a - ImageBase()));
    return 1;
}

}  // namespace
}  // namespace tw3se
extern "C" uint64_t tw3se_engcall(void* fn, const uint64_t* regs, const uint64_t* stack, uint64_t nstack,
                                  uint64_t* xmm0Out);  // enghook.asm
namespace tw3se {
namespace {

int LObjectAt(lua_State* L);  // w3.object, below

// The call itself, under a guard: a fault in the engine function ends the call (no C++ objects in here).
bool EngCallGuarded(void* fn, const uint64_t* regs, const uint64_t* stack, uint64_t n, uint64_t* rax, uint64_t* xmm0,
                    DWORD* code) {
    __try {
        *rax = tw3se_engcall(fn, regs, stack, n, xmm0);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// w3.engine_call(fn, sig, ...) -> the result. fn: a name as w3.engine_hook takes it, or an offset in witcher3.exe
// (an integer below the image's size, as w3.vslot and w3.engine_addr give it), or an address. sig: one letter an
// argument, then '>' and the result's letter: i integer, p pointer (an integer, an object, nil = 0), s string (a
// C string for the call's time), b boolean, f float, d double; result also o (an object at the address), v nothing.
// "pf>i": a pointer and a float in, an integer out. Up to 16 arguments; by Windows x64 the first four go to rcx,
// rdx, r8, r9 or, for f and d, xmm0-xmm3. Main thread only; a fault ends the call with an error.
int LEngineCall(lua_State* L) {
    if (!OnMainThread()) return luaL_error(L, "w3.engine_call: only on the main thread");
    uintptr_t fn = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        std::string why;
        fn = EngineResolve(lua_tostring(L, 1), &why);
        if (!fn) return luaL_error(L, "w3.engine_call: %s", why.c_str());
    } else {
        fn = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
        if (fn && fn < 0x10000000) fn += ImageBase();  // an offset in the exe
    }
    if (!fn) return luaL_error(L, "w3.engine_call: no function");
    std::string sig = luaL_checkstring(L, 2);
    size_t gt = sig.find('>');
    std::string ins = sig.substr(0, gt);
    char ret = gt == std::string::npos || gt + 1 >= sig.size() ? 'v' : sig[gt + 1];
    if (ins.size() > 16) return luaL_error(L, "w3.engine_call: at most 16 arguments");
    uint64_t regs[8] = {}, stack[12] = {};
    std::vector<std::string> keep;  // the strings live until the call returns
    keep.reserve(ins.size());
    for (size_t k = 0; k < ins.size(); ++k) {
        int idx = static_cast<int>(k) + 3;
        uint64_t v = 0;
        bool isFloat = false;
        switch (ins[k]) {
        case 'i':
            v = static_cast<uint64_t>(luaL_checkinteger(L, idx));
            break;
        case 'b':
            v = lua_toboolean(L, idx) ? 1 : 0;
            break;
        case 'p':
            if (lua_isnoneornil(L, idx)) v = 0;
            else if (Obj* o = static_cast<Obj*>(luaL_testudata(L, idx, kObj))) {
                v = reinterpret_cast<uint64_t>(o->ptr);
                // A w3.new object handed to an engine function is the engine's now, as for a script native (below):
                // it may keep it without a reference (the witcher woman's skinning attachments, attached through
                // CMeshSkinningAttachment::vf43, stayed in the node's list; Lua's discard after the last handle left
                // them freed there, the world's unload called into them, 27.11.). Lua never discards it.
                if (o->block) g_newBlocks.erase(o->block);
            }
            else v = static_cast<uint64_t>(luaL_checkinteger(L, idx));
            break;
        case 's': {  // the whole string with its length: packed structs hold zero bytes
            size_t len = 0;
            const char* p = luaL_checklstring(L, idx, &len);
            keep.emplace_back(p, len);
            v = reinterpret_cast<uint64_t>(keep.back().c_str());
            break;
        }
        case 'f': {
            float f = static_cast<float>(luaL_checknumber(L, idx));
            uint32_t bits;
            memcpy(&bits, &f, 4);
            v = bits;
            isFloat = true;
            break;
        }
        case 'd': {
            double d = luaL_checknumber(L, idx);
            memcpy(&v, &d, 8);
            isFloat = true;
            break;
        }
        default:
            return luaL_error(L, "w3.engine_call: argument %d: '%c' is no type (i p s b f d)", static_cast<int>(k) + 1,
                              ins[k]);
        }
        if (k < 4) regs[isFloat ? 4 + k : k] = v;
        else stack[k - 4] = v;
    }
    uint64_t rax = 0, xmm0 = 0;
    DWORD code = 0;
    if (!EngCallGuarded(reinterpret_cast<void*>(fn), regs, stack, ins.size() > 4 ? ins.size() - 4 : 0, &rax, &xmm0,
                        &code)) {
        char msg[160];
        snprintf(msg, sizeof msg, "w3.engine_call: the function at witcher3.exe+0x%llx faulted (0x%08lx)",
                 static_cast<unsigned long long>(fn - ImageBase()), code);
        return luaL_error(L, "%s", msg);
    }
    switch (ret) {
    case 'v':
        return 0;
    case 'b':
        lua_pushboolean(L, (rax & 0xff) != 0);
        return 1;
    case 'f': {
        float f;
        uint32_t bits = static_cast<uint32_t>(xmm0);
        memcpy(&f, &bits, 4);
        lua_pushnumber(L, f);
        return 1;
    }
    case 'd': {
        double d;
        memcpy(&d, &xmm0, 8);
        lua_pushnumber(L, d);
        return 1;
    }
    case 's':
        if (!rax) lua_pushnil(L);
        else {
            char buf[1024] = {};
            for (size_t n = 0; n + 1 < sizeof buf && SafeRead(reinterpret_cast<const char*>(rax) + n, buf + n, 1) && buf[n];)
                ++n;
            lua_pushstring(L, buf);
        }
        return 1;
    case 'o':
        if (!rax) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushcfunction(L, LObjectAt);
        lua_pushinteger(L, static_cast<lua_Integer>(rax));
        lua_call(L, 1, 1);
        return 1;
    default:  // i, p
        lua_pushinteger(L, static_cast<lua_Integer>(rax));
        return 1;
    }
}
int EngUnhookOwner(lua_State* L, const std::string& owner) {  // a Lua mod is taken out
    std::vector<int> ids;
    for (EngSlot& s : g_eng)
        for (const EngHandler& h : s.handlers)
            if (h.owner == owner) ids.push_back(h.id);
    for (int id : ids) EngUnhookId(L, id);
    return static_cast<int>(ids.size());
}
// w3.engine_hooks() -> {{name, calls, handlers}, ...}: what is hooked and how often it ran (every thread)
int LEngineHooks(lua_State* L) {
    lua_newtable(L);
    int n = 0;
    for (const EngSlot& s : g_eng) {
        if (!s.target) continue;
        lua_createtable(L, 0, 3);
        lua_pushstring(L, s.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, static_cast<lua_Integer>(s.calls.load()));
        lua_setfield(L, -2, "calls");
        lua_pushinteger(L, static_cast<lua_Integer>(s.handlers.size()));
        lua_setfield(L, -2, "handlers");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
}  // namespace
}  // namespace tw3se

extern "C" void tw3se_enghook_pre(int slot, uint64_t* regs, const uint64_t* stack) {
    if (slot >= 0 && slot < kEngSlots) tw3se::EngPre(slot, regs, stack);
}

namespace tw3se {
namespace {

// --- natives written in Lua: w3.native(name, fn) gives WitcherScript a function `import function name(...)`.
// The parameters and the return come from the import line: the compiler fills the native's CFunction with them
// (+0x28 parameters, +0x30 count, +0x20 return), the handler reads each one with the VM's own reader and hands them to
// the Lua function; its value goes back as the return. Files in tw3se\lua\natives\ run at the game start, before the
// scripts load, so scripts find their natives at once; a native from a later w3.native is there after the next
// `reloadscripts force`. The same name again replaces the Lua function (no reload needed for that).
constexpr int kLuaNatives = 128;  // TW3SE's own script library (J3) takes about 30
struct LuaNative {
    std::string name;
    int ref = LUA_NOREF;
    std::string owner;  // the mod that made it
};
LuaNative g_luaNatives[kLuaNatives];

void DispatchLuaNative(int i, void* self, void* frame, void* result) {
    LuaNative& n = g_luaNatives[i];
    bool method = n.name.find('.') != std::string::npos;  // a method: the object goes first
    void* fn = method ? OurMethod(n.name) : OurNative(n.name.c_str());
    void** params = fn ? Rd<void**>(fn, 0x28) : nullptr;
    uint32_t np = fn ? Rd<uint32_t>(fn, 0x30) : 0;
    if (np > 16 || (np && !Plausible(params))) np = 0;
    lua_State* L = g_L;
    bool run = L && OnMainThread() && n.ref != LUA_NOREF && !g_dropNatives.count(i);
    int top = run ? lua_gettop(L) : 0;
    if (run) lua_rawgeti(L, LUA_REGISTRYINDEX, n.ref);
    if (run && method) PushObject(L, self, nullptr, false, BlockOf(self));
    for (uint32_t k = 0; k < np; ++k) {  // every argument is read (the VM's cursor), even when Lua does not run
        void* prop = Rd<void*>(params, k * sizeof(void*));
        void* type = Rd<void*>(prop, 0x08);
        alignas(16) uint8_t buf[512] = {};
        ReadScriptParam(frame, buf);
        if (!run) continue;
        if (Rd<uint32_t>(prop, 0x24) & 0x200) lua_pushnil(L);  // an out parameter: not handed to Lua
        else {
            PushValue(L, buf, type, 0, true);
            ReleaseHandedHandle(buf, type);
        }
    }
    EndScriptParams(frame);
    if (!run) return;
    std::string err;
    ModScope scope(n.owner);
    bool ok;
    {
        ProfScope prof(n.owner, "native " + n.name);
        ok = PCall(L, static_cast<int>(np) + (method ? 1 : 0), 1, err);
    }
    if (!ok) {
        Log("lua: native %s: %s", n.name.c_str(), err.c_str());
        if (IsRunaway(err)) g_dropNatives.insert(i);
    } else if (result && fn && !lua_isnil(L, -1)) {
        void* retProp = Rd<void*>(fn, 0x20);
        void* retType = Plausible(retProp) ? Rd<void*>(retProp, 0x08) : nullptr;
        if (retType) {
            std::string e = WriteValue(L, -1, result, retType);
            if (!e.empty()) Log("lua: native %s: its return: %s", n.name.c_str(), e.c_str());
        }
    }
    lua_settop(L, top);
}

template <int I> void LuaNativeTramp(void* self, void* frame, void* result) { DispatchLuaNative(I, self, frame, result); }
template <size_t... I> constexpr std::array<HookFn, sizeof...(I)> MakeNativeTramps(std::index_sequence<I...>) {
    return {&LuaNativeTramp<I>...};
}
const std::array<HookFn, kLuaNatives> g_nativeTramps = MakeNativeTramps(std::make_index_sequence<kLuaNatives>());

int LNative(lua_State* L) {  // w3.native(name, fn) -> true; the name again replaces the function
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int free = -1;
    for (int i = 0; i < kLuaNatives; ++i) {
        if (g_luaNatives[i].name == name) {  // replace the Lua side only
            luaL_unref(L, LUA_REGISTRYINDEX, g_luaNatives[i].ref);
            lua_pushvalue(L, 2);
            g_luaNatives[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            g_luaNatives[i].owner = g_curMod;
            lua_pushboolean(L, 1);
            return 1;
        }
        if (free < 0 && g_luaNatives[i].name.empty()) free = i;
    }
    if (free < 0) return luaL_error(L, "w3.native: at most %d natives from Lua", kLuaNatives);
    const char* dot = strchr(name, '.');
    if (dot) {  // "Class.Name": a method of a native class; the Lua function gets the object first
        std::string cls(name, dot), meth(dot + 1);
        // At the game start (a natives\ file) the engine's types cannot be asked yet: a FindType there faulted inside
        // ntdll. The class is checked when the method is registered (scripting.cpp, RegisterMethodOne).
        if (NativesRegistered()) {
            void* klass = FindClassByName(cls.c_str());
            if (!klass) return luaL_error(L, "w3.native: no class %s", cls.c_str());
            if (!FindMethods(klass, meth.c_str()).empty())
                return luaL_error(L, "w3.native: %s is a method of the game or of TW3SE already", name);
        }
        if (!RegisterNativeMethodAnytime(cls.c_str(), meth.c_str(), g_nativeTramps[free]))
            return luaL_error(L, "w3.native: %s was not registered (tw3se.log names why)", name);
    } else {
        // (at the game start the engine's names do not exist yet: the check waits for a native made later)
        if (NativesRegistered() && FindGlobalFn(name))
            return luaL_error(L, "w3.native: %s is a function of the game or of TW3SE already", name);
        if (!RegisterNativeAnytime(name, g_nativeTramps[free], 0)) return luaL_error(L, "w3.native: %s was not registered", name);
    }
    LuaNative& n = g_luaNatives[free];
    n.name = name;
    lua_pushvalue(L, 2);
    n.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    n.owner = g_curMod;
    if (g_curMod.rfind("mods/", 0) == 0) NoteModNative(name);
    Log("lua: native %s from Lua%s", name, g_curMod.empty() ? "" : (" (" + g_curMod + ")").c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// --- the w3 table

// A predefined global (GScriptGlobals field -> handle block -> object at +8, docs/engine/world.md), with its block.
void PushGlobal(lua_State* L, uint32_t off) {
    uintptr_t addr = Address("GScriptGlobals");
    void* base = addr ? Rd<void*>(reinterpret_cast<void*>(addr), 0) : nullptr;
    void* block = Plausible(base) ? Rd<void*>(base, off) : nullptr;
    PushObject(L, Plausible(block) ? Rd<void*>(block, 8) : nullptr, nullptr, false, block);
}

int LGlobal(lua_State* L) {
    uint32_t off = ScriptGlobalOffsetOf(luaL_checkstring(L, 1));
    if (!off) return luaL_error(L, "unknown global %s (theGame, thePlayer, theCamera, theSound, theInput ...)", lua_tostring(L, 1));
    PushGlobal(L, off);
    return 1;
}

int LPlayer(lua_State* L) {
    PushGlobal(L, ScriptGlobalOffsetOf("thePlayer"));
    return 1;
}

int LCall(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* fn = FindGlobalFn(name);
    if (!fn) return luaL_error(L, "no global function %s", name);
    return CallCandidates(L, name, {fn}, nullptr, 2);
}

// --- every object of a class: the engine keeps every CObject in one table (GObjectsMap: a pointer
// to {CObject** data, u32 count, ..., CRITICAL_SECTION at +0x20}, freed slots null; its GC walks it). A snapshot
// under that lock: TryEnter, so a thread that holds it while it waits for the main thread cannot hang the game (then
// the walk reads without it, every read guarded).
bool IsA(void* cls, void* want, std::unordered_map<void*, bool>& memo) {
    auto it = memo.find(cls);
    if (it != memo.end()) return it->second;
    bool yes = false;
    for (void* c = cls; Plausible(c) && !yes; c = Rd<void*>(c, 0x10)) yes = c == want;
    memo[cls] = yes;
    return yes;
}

// Whether an object lives inside an entity template (CObject parent at +0x30): every template holds an entity
// of its class as a pattern (a save had three W3PlayerWitcher: the player under its CDynamicLayer, two under
// CEntityTemplates), with its components under it.
bool InTemplate(void* o, std::unordered_map<void*, bool>& memo) {
    static void* tmpl = RttiFindType("CEntityTemplate");
    static void* entity = RttiFindType("CEntity");
    void* p = Rd<void*>(o, 0x30);
    // an entity in no layer is in no world either (a W3PlayerWitcher without a parent can turn up after an hour of
    // spawning and destroying)
    if (!Plausible(p)) {
        void* c = ClassOfGuarded(o);
        return Plausible(c) && Plausible(entity) && IsA(c, entity, memo);
    }
    for (int depth = 0; depth < 8 && Plausible(p); ++depth, p = Rd<void*>(p, 0x30)) {
        void* c = ClassOfGuarded(p);
        if (Plausible(c) && Plausible(tmpl) && IsA(c, tmpl, memo)) return true;
    }
    return false;
}

// The objects of class `want` (and its subclasses), at most `limit`. Main thread. Without `templates` the pattern
// objects inside entity templates are left out.
std::vector<void*> ObjectsOf(void* want, size_t limit, bool templates = false) {
    std::unordered_map<void*, bool> tmemo;
    std::vector<void*> out;
    uintptr_t gaddr = Address("GObjectsMap");
    char* map = gaddr ? Rd<char*>(reinterpret_cast<void*>(gaddr), 0) : nullptr;
    if (!Plausible(map) || !Plausible(want)) return out;
    auto cs = reinterpret_cast<CRITICAL_SECTION*>(map + 0x20);
    bool locked = TryEnterCriticalSection(cs) != 0;
    void** data = Rd<void**>(map, 0);
    uint32_t n = Rd<uint32_t>(map, 8);
    std::unordered_map<void*, bool> memo;
    if (Plausible(data) && n < 50000000) {
        std::vector<void*> chunk(4096);
        for (uint32_t i = 0; i < n && out.size() < limit; i += 4096) {
            uint32_t m = n - i < 4096 ? n - i : 4096;
            if (!SafeRead(data + i, chunk.data(), m * sizeof(void*))) break;
            for (uint32_t k = 0; k < m && out.size() < limit; ++k) {
                void* o = chunk[k];
                if (!Plausible(o)) continue;
                void* c = ClassOfGuarded(o);
                if (Plausible(c) && IsA(c, want, memo) && (templates || !InTemplate(o, tmemo))) out.push_back(o);
            }
        }
    }
    if (locked) LeaveCriticalSection(cs);
    return out;
}

// w3.all(className [, limit [, templates]]): every live object of the class and its subclasses (entities, components,
// quest objects, ...), at most `limit` (default 100000), as a table; `templates` true adds the pattern objects inside
// entity templates.
int LAll(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_Integer limit = luaL_optinteger(L, 2, 100000);
    bool templates = lua_toboolean(L, 3) != 0;
    void* cls = RttiFindType(name);
    if (RttiKind(cls) != 2) return luaL_error(L, "no class %s", name);
    if (!Address("GObjectsMap")) return luaL_error(L, "w3.all: the object table is not found in this build");
    std::vector<void*> objs = ObjectsOf(cls, limit > 0 ? static_cast<size_t>(limit) : 0, templates);
    lua_createtable(L, static_cast<int>(objs.size()), 0);
    int i = 0;
    for (void* o : objs) {
        PushObject(L, o, nullptr, false);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// w3._params(obj|nil, name): the parameters of a method of obj (nil: a global function) as
// { {name=, type=, kind=, optional=, out=}, ... }, or nil when there is no such function. The declaration of a
// native first, as CallCandidates reads it (J3: the script library converts text arguments with it).
int LParams(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    void* fn = nullptr;
    if (lua_isnil(L, 1)) {
        fn = FindGlobalFn(name);
    } else {
        const std::vector<void*>& ms = FindMethods(CheckLive(L, 1)->cls, name);
        for (void* f : ms)
            if (Rd<uint32_t>(f, 0x18) & 8) { fn = f; break; }
        if (!fn)
            for (void* f : ms)
                if (!(Rd<uint32_t>(f, 0x18) & 1)) { fn = f; break; }
        if (!fn && !ms.empty()) fn = ms.front();
    }
    if (!fn) return 0;
    void** params = Rd<void**>(fn, 0x28);
    uint32_t np = Rd<uint32_t>(fn, 0x30);
    if (np > 32 || (np && !Plausible(params))) return 0;
    lua_createtable(L, static_cast<int>(np), 0);
    for (uint32_t i = 0; i < np; ++i) {
        void* p = Rd<void*>(params, i * sizeof(void*));
        void* t = Rd<void*>(p, 0x08);
        uint32_t fl = Rd<uint32_t>(p, 0x24);
        lua_createtable(L, 0, 5);
        lua_pushstring(L, NameOfId(Rd<uint32_t>(p, 0x10)).c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, RttiName(t).c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, RttiKind(t));
        lua_setfield(L, -2, "kind");
        lua_pushboolean(L, (fl & 0x400) != 0);
        lua_setfield(L, -2, "optional");
        lua_pushboolean(L, (fl & 0x200) != 0);
        lua_setfield(L, -2, "out");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// w3.text.set(id, text): the game shows `text` wherever it shows the text with this string id (a choice, an item
// name; a story scene's line keeps the game's text); nil gives the game's own back. Belongs to the
// mod that set it (loctext.cpp).
int LTextSet(lua_State* L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id <= 0 || id > 0xffffffffLL) return luaL_error(L, "w3.text.set: a string id is a positive number");
    if (lua_isnoneornil(L, 2)) {
        lua_pushboolean(L, LocTextClear(static_cast<uint32_t>(id)));
        return 1;
    }
    if (!LocTextSet(static_cast<uint32_t>(id), luaL_checkstring(L, 2), g_curMod))
        return luaL_error(L, "w3.text.set: the game's text function is not found in this build");
    lua_pushboolean(L, 1);
    return 1;
}

// --- the quest graph, live (layout from docs/FINDINGS_STORY_SPRUNG.md S1c, checked in TickActiveBlocks
// and ActivateBlock of this build): a CQuestThread keeps its active blocks at +0x68 (count +0x70; 24-byte entries
// {block, data, state}) and its child threads at +0x90 (count +0x98). The offsets are below 0x80 (one-byte
// displacements in the code), so they stand here and not in addresses.inc.
int LQuestBlocks(lua_State* L) {  // w3._quest_blocks(thread): its active blocks
    Obj* t = CheckLive(L, 1);
    char* arr = Rd<char*>(t->ptr, 0x68);
    uint32_t n = Rd<uint32_t>(t->ptr, 0x70);
    lua_newtable(L);
    if (!Plausible(arr) || n > 4096) return 1;
    int k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        void* b = Rd<void*>(arr, static_cast<size_t>(i) * 24);
        PushObject(L, b, nullptr, false);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}
int LQuestChildren(lua_State* L) {  // w3._quest_children(thread): its child threads
    Obj* t = CheckLive(L, 1);
    void** arr = Rd<void**>(t->ptr, 0x90);
    uint32_t n = Rd<uint32_t>(t->ptr, 0x98);
    lua_newtable(L);
    if (!Plausible(arr) || n > 4096) return 1;
    int k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        PushObject(L, Rd<void*>(arr, i * sizeof(void*)), nullptr, false);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}
using ActivateBlock_t = void (*)(void* thread, void* block, const void* input);
bool ActivateBlockGuarded(ActivateBlock_t f, void* thread, void* block) {
    static const uint64_t noInput = 0;  // the 8 bytes a pending activation carries after its block: none
    __try {
        f(thread, block, &noInput);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// w3._quest_activate(thread, block): CQuestThread::ActivateBlock, as when a connection reaches the block. The block has
// to belong to the graph this thread runs (the engine returns at once for one that is active already).
int LQuestActivate(lua_State* L) {
    Obj* t = CheckLive(L, 1);
    Obj* b = CheckLive(L, 2);
    auto f = reinterpret_cast<ActivateBlock_t>(Address("CQuestThread::ActivateBlock"));
    if (!f) return luaL_error(L, "the quest block activation is not found in this build");
    if (!Plausible(Rd<void*>(t->ptr, 0x60))) return luaL_error(L, "the thread runs no graph");
    lua_pushboolean(L, ActivateBlockGuarded(f, t->ptr, b->ptr));
    return 1;
}

// w3._member(obj, name): "field", its type name and kind; "method"; or nil.
int LMember(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (void* prop = FindProp(o->cls, name)) {
        void* t = Rd<void*>(prop, 0x08);
        lua_pushliteral(L, "field");
        lua_pushstring(L, RttiName(t).c_str());
        lua_pushinteger(L, RttiKind(t));
        return 3;
    }
    if (!o->isStruct && !FindMethods(o->cls, name).empty()) {
        lua_pushliteral(L, "method");
        return 1;
    }
    return 0;
}

// w3.new_enum(name, {OPTION = value, ...}) -> true: an enum WitcherScript uses by its name and its value names,
// with no declaration, like the game's native enums. From a mod's natives\ file it is there when the scripts compile;
// later, after the next `reloadscripts force`. A name the game has already is refused.
int LNewEnum(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::pair<std::string, int32_t>> values;
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) != LUA_TSTRING || !lua_isinteger(L, -1)) {
            lua_pop(L, 2);
            return luaL_argerror(L, 2, "value names with whole numbers ({MM_Off = 0, MM_On = 1})");
        }
        lua_Integer v = lua_tointeger(L, -1);
        if (v < INT32_MIN || v > INT32_MAX) {
            lua_pop(L, 2);
            return luaL_argerror(L, 2, "a value that fits in 32 bits");
        }
        values.emplace_back(lua_tostring(L, -2), static_cast<int32_t>(v));
        lua_pop(L, 1);
    }
    if (values.empty()) return luaL_argerror(L, 2, "at least one value");
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (NativesRegistered() && RttiFindType(name.c_str()))  // (at the game start the types cannot be asked yet)
        return luaL_error(L, "w3.new_enum: the game has a type %s already", name.c_str());
    if (!RegisterEnumAnytime(name, values)) return luaL_error(L, "w3.new_enum: %s was not registered (tw3se.log names why)", name.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

int LEnum(lua_State* L) {
    void* t = RttiFindType(luaL_checkstring(L, 1));
    if (RttiKind(t) != 1 && RttiKind(t) != 9) return luaL_error(L, "no enum or bitfield %s", lua_tostring(L, 1));  // a bitfield: bit names, 1 << bit
    lua_newtable(L);
    RttiEnumValues(t, [&](const char* name, int32_t v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    });
    return 1;
}

void PushClassInfo(lua_State* L, void* cls) {
    lua_newtable(L);
    lua_pushstring(L, RttiName(cls).c_str());
    lua_setfield(L, -2, "name");
    void* parent = Rd<void*>(cls, 0x10);
    if (Plausible(parent)) {
        lua_pushstring(L, RttiName(parent).c_str());
        lua_setfield(L, -2, "parent");
    }
    lua_pushinteger(L, RttiSize(cls));
    lua_setfield(L, -2, "size");
    lua_newtable(L);
    ForEachClassItem(cls, 0x30, [&](void* p, void*) {
        lua_newtable(L);
        lua_pushstring(L, RttiName(Rd<void*>(p, 0x08)).c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, Rd<uint32_t>(p, 0x20));
        lua_setfield(L, -2, "offset");
        lua_setfield(L, -2, NameOfId(Rd<uint32_t>(p, 0x10)).c_str());
        return false;
    });
    lua_setfield(L, -2, "fields");
    lua_newtable(L);
    int i = 0;
    ForEachClassItem(cls, 0x48, [&](void* f, void*) {
        lua_pushstring(L, NameOfId(Rd<uint32_t>(f, 0x10)).c_str());
        lua_rawseti(L, -2, ++i);
        return false;
    });
    lua_setfield(L, -2, "methods");
}

int LClass(lua_State* L) {
    void* t = RttiFindType(luaL_checkstring(L, 1));
    if (RttiKind(t) != 2) return luaL_error(L, "no class %s", lua_tostring(L, 1));
    PushClassInfo(L, t);
    return 1;
}

int LFields(lua_State* L) {
    PushClassInfo(L, CheckObj(L, 1)->cls);
    lua_getfield(L, -1, "fields");
    return 1;
}

int LClassOf(lua_State* L) {
    lua_pushstring(L, RttiName(CheckObj(L, 1)->cls).c_str());
    return 1;
}

// Whether an object still looks alive: a handle-derived object is valid while its handle block still points at it
// (a destroyed object's block no longer does). Lets a mod holding an object across ticks check before use.
int LValid(lua_State* L) {
    auto o = static_cast<Obj*>(luaL_testudata(L, 1, kObj));
    bool ok = o && Plausible(o->ptr);
    if (ok && o->block) ok = Rd<void*>(o->block, 8) == o->ptr;
    lua_pushboolean(L, ok);
    return 1;
}

// w3.entities([className]): every tracked live entity (pre-placed objects currently loaded), optionally only those
// of a class (incl. subclasses). Uses the world register (worldedit), no new engine walk.
struct EntCtx {
    lua_State* L;
    const char* filter;  // class name or null
    int n;
};
void EntVisit(void* entity, void* block, void* ctx) {
    auto c = static_cast<EntCtx*>(ctx);
    void* cls = ClassOfGuarded(entity);
    if (!Plausible(cls)) return;
    if (c->filter) {
        bool fits = false;
        for (void* k = cls; Plausible(k) && !fits; k = Rd<void*>(k, 0x10)) fits = RttiName(k) == c->filter;
        if (!fits) return;
    }
    PushObject(c->L, entity, cls, false, block);
    lua_rawseti(c->L, -2, ++c->n);
}
int LEntities(lua_State* L) {
    EntCtx c{L, lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr, 0};
    lua_newtable(L);
    WorldForEachEntity(&EntVisit, &c);
    return 1;
}

int LFind(lua_State* L) {
    std::string needle = luaL_checkstring(L, 1);
    for (char& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    lua_newtable(L);
    int i = 0;
    RttiForEachType([&](const char* name, void* type) {
        int k = RttiKind(type);
        if (k != 1 && k != 2 && k != 9) return;
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower.find(needle) == std::string::npos || i >= 500) return;
        lua_pushstring(L, name);
        lua_rawseti(L, -2, ++i);
    });
    return 1;
}

// The loaded entity with a GUID (the pre-placed GUID from its layer, as four hex dwords: "aaaaaaaa-bbbbbbbb-...",
// the form w3.guid and a CGUID field give), or nil. Looks it up in the live register (worldedit.cpp).
int LByGuid(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    uint32_t g[4] = {0};
    if (sscanf_s(s, "%x-%x-%x-%x", &g[0], &g[1], &g[2], &g[3]) != 4 &&
        sscanf_s(s, "%x %x %x %x", &g[0], &g[1], &g[2], &g[3]) != 4)
        return luaL_error(L, "by_guid: a guid is four hex dwords like \"xxxxxxxx-xxxxxxxx-xxxxxxxx-xxxxxxxx\"");
    void* obj = WorldEntityByGuid(g);
    if (obj) PushObject(L, obj, nullptr, false);
    else lua_pushnil(L);
    return 1;
}

// The pre-placed GUID of an entity as "aaaaaaaa-bbbbbbbb-cccccccc-dddddddd", or nil for a run-time object (which
// has a counter GUID the register does not keep). The same value GetGuidHash reads and w3.by_guid takes.
int LGuid(lua_State* L) {
    Obj* o = CheckObj(L, 1);
    uint32_t g[4] = {0};
    if (o && !o->isStruct && o->ptr && WorldEntityGuid(o->ptr, g)) {
        char buf[40];
        sprintf_s(buf, "%08x-%08x-%08x-%08x", g[0], g[1], g[2], g[3]);
        lua_pushstring(L, buf);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// --- low-level debug reads (for reverse-engineering from the live game: follow a pointer chain, match an address
// against the analysis layer's module-relative offsets). Read-only; a bad address reads as nil, never crashes.
int LAddr(lua_State* L) {  // the raw address of an object (or of an array field's {data, count}), as an integer
    if (Arr* a = static_cast<Arr*>(luaL_testudata(L, 1, kArr))) {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(a->addr)));
        return 1;
    }
    Obj* o = static_cast<Obj*>(luaL_testudata(L, 1, kObj));
    if (!o) return luaL_error(L, "addr: expected an object or an array");
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(o->ptr)));
    return 1;
}
int LHwWatch(lua_State* L) {  // w3.hwwatch("w 1f2e3d4c50 8"): the `hwwatch` console command, its reply
    lua_pushstring(L, HwWatchCommand(luaL_checkstring(L, 1)).c_str());
    return 1;
}
int LPeek(lua_State* L) {  // the 8-byte value at base+offset, or nil if unreadable
    uintptr_t base = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
    lua_Integer off = luaL_optinteger(L, 2, 0);
    uint64_t v = 0;
    if (!SafeRead(reinterpret_cast<void*>(base + static_cast<uintptr_t>(off)), &v, 8)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    return 1;
}
// For measuring layouts from Lua (tools/measure_types.py): w3._type(name) is the address of a registered type,
// w3._cname(id) the text of a name id (nil when the id has none).
int LTypeAddr(lua_State* L) {
    void* t = RttiFindType(luaL_checkstring(L, 1));
    if (!t) lua_pushnil(L);
    else lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(t)));
    return 1;
}
int LCNameText(lua_State* L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id <= 0 || id > 0xffffff) { lua_pushnil(L); return 1; }
    const char* s = GetCNameString(static_cast<uint32_t>(id));
    if (s && *s) lua_pushstring(L, s);
    else lua_pushnil(L);
    return 1;
}
int LRva(lua_State* L) {  // an address as a module-relative offset (matches the analysis layer's RVAs), or nil
    uintptr_t p = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
    uintptr_t base = ImageBase();
    if (p < base || p > base + 0x10000000ull) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, static_cast<lua_Integer>(p - base));
    return 1;
}
int LScriptStack(lua_State* L) {  // w3.script_stack(): the script functions running now, innermost first (F3)
    ScriptStackFrameInfo frames[64];
    int n = ScriptStackRead(frames, 64);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, frames[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, frames[i].file.c_str());
        lua_setfield(L, -2, "file");
        lua_pushinteger(L, frames[i].line);
        lua_setfield(L, -2, "line");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
// --- hooks on virtual methods: w3.vhook(object, slot, fn). The slot of the object's vtable points at a
// stub (vhook.cpp); every object sharing that vtable runs through it. All script classes share one vtable, so the
// handler runs only for objects of the hooked object's class (and its subclasses); the rest go to the original. The
// handler gets self, the raw arguments {rdx, r8, r9, f1, f2, f3} (f: xmm1-xmm3 as floats) and orig().
struct LuaVHook {
    int ref = LUA_NOREF;
    std::string owner;
    void* cls = nullptr;
    bool inProgress = false;
    std::unordered_map<void*, bool> memo;
};
LuaVHook g_luaVh[64];

float LowFloat(uint64_t bits) {
    float f;
    uint32_t b = static_cast<uint32_t>(bits);
    memcpy(&f, &b, 4);
    return f;
}
uint64_t FloatBits(double d) {
    float f = static_cast<float>(d);
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

struct VCall {
    int id;
    VRegs* regs;
    uint64_t* stack;
    bool called = false;
    uint64_t result = 0;
};

void VArgsToRegs(lua_State* L, int args, VRegs* r) {  // what the handler changed in args goes into the registers
    uint64_t* ints[3] = {&r->rdx, &r->r8, &r->r9};
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, args, i + 1);
        if (lua_isinteger(L, -1)) *ints[i] = static_cast<uint64_t>(lua_tointeger(L, -1));
        else if (lua_isboolean(L, -1)) *ints[i] = lua_toboolean(L, -1) ? 1 : 0;
        else if (Obj* o = static_cast<Obj*>(luaL_testudata(L, -1, kObj))) *ints[i] = reinterpret_cast<uint64_t>(o->ptr);
        lua_pop(L, 1);
        lua_getfield(L, args, ("f" + std::to_string(i + 1)).c_str());
        if (lua_isnumber(L, -1)) r->xmm[i + 1] = FloatBits(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

int LVOrig(lua_State* L) {  // orig() inside a w3.vhook handler; upvalues: the VCall, the args table
    auto c = static_cast<VCall*>(lua_touserdata(L, lua_upvalueindex(1)));
    VArgsToRegs(L, lua_upvalueindex(2), c->regs);
    c->result = VHookCallOriginal(c->id, c->regs, c->stack);
    c->called = true;
    lua_pushinteger(L, static_cast<lua_Integer>(c->result));
    lua_pushnumber(L, LowFloat(c->regs->xmm[0]));
    return 2;
}

uint64_t LuaVHandler(int id, VRegs* regs, uint64_t* stack, void*) {
    LuaVHook& h = g_luaVh[id];
    void* self = reinterpret_cast<void*>(regs->rcx);
    if (h.inProgress || !OnMainThread() || !g_L || h.ref == LUA_NOREF) return VHookCallOriginal(id, regs, stack);
    void* cls = ClassOfGuarded(self);
    if (!Plausible(cls) || !IsA(cls, h.cls, h.memo)) return VHookCallOriginal(id, regs, stack);
    DoingScope doing("a Lua virtual hook", static_cast<uintptr_t>(id));
    h.inProgress = true;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    VCall c{id, regs, stack};
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
    PushObject(L, self, cls, false);
    lua_createtable(L, 3, 3);
    int args = lua_gettop(L);
    const uint64_t ints[3] = {regs->rdx, regs->r8, regs->r9};
    for (int i = 0; i < 3; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(ints[i]));
        lua_rawseti(L, args, i + 1);
        lua_pushnumber(L, LowFloat(regs->xmm[i + 1]));
        lua_setfield(L, args, ("f" + std::to_string(i + 1)).c_str());
    }
    lua_pushlightuserdata(L, &c);
    lua_pushvalue(L, args);
    lua_pushcclosure(L, &LVOrig, 2);
    std::string err;
    bool ok;
    {
        ModScope scope(h.owner);
        ProfScope prof(h.owner, "vhook " + std::to_string(id));
        ok = PCall(L, 3, 1, err);
    }
    uint64_t result;
    if (!ok) {
        Log("lua: virtual hook %d error: %s", id, err.c_str());
        result = c.called ? c.result : VHookCallOriginal(id, regs, stack);
    } else if (lua_isinteger(L, -1)) {
        result = static_cast<uint64_t>(lua_tointeger(L, -1));
    } else if (lua_isnumber(L, -1)) {
        result = FloatBits(lua_tonumber(L, -1));
    } else if (lua_isboolean(L, -1)) {
        result = lua_toboolean(L, -1) ? 1 : 0;
    } else if (Obj* o = static_cast<Obj*>(luaL_testudata(L, -1, kObj))) {
        result = reinterpret_cast<uint64_t>(o->ptr);
    } else {
        result = c.called ? c.result : VHookCallOriginal(id, regs, stack);  // nil: the original's value
    }
    lua_settop(L, top);
    h.inProgress = false;
    return result;
}

int LVHook(lua_State* L) {  // w3.vhook(object, slot, fn): returns a handle for w3.vunhook
    Obj* o = static_cast<Obj*>(luaL_checkudata(L, 1, kObj));
    int slot = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (o->isStruct) return luaL_error(L, "vhook: a struct has no vtable");
    void* vtable = nullptr;
    if (!SafeRead(o->ptr, &vtable, 8) || !Plausible(vtable)) return luaL_error(L, "vhook: the object has no vtable");
    std::string err;
    int id = VHookInstall(vtable, slot, &LuaVHandler, nullptr, err);
    if (id < 0) return luaL_error(L, "vhook: %s", err.c_str());
    LuaVHook& h = g_luaVh[id];
    lua_pushvalue(L, 3);
    h.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    h.owner = g_curMod;
    h.cls = o->cls;
    h.memo.clear();
    lua_pushinteger(L, id);
    return 1;
}

bool VUnhook(lua_State* L, int id) {
    if (id < 0 || id >= 64 || g_luaVh[id].ref == LUA_NOREF) return false;
    VHookRemove(id);
    luaL_unref(L, LUA_REGISTRYINDEX, g_luaVh[id].ref);
    g_luaVh[id].ref = LUA_NOREF;
    g_luaVh[id].owner.clear();
    return true;
}
int LVUnhook(lua_State* L) {
    lua_pushboolean(L, VUnhook(L, static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int VUnhookOwner(lua_State* L, const std::string& owner) {
    int n = 0;
    for (int i = 0; i < 64; ++i)
        if (g_luaVh[i].ref != LUA_NOREF && g_luaVh[i].owner == owner && VUnhook(L, i)) ++n;
    return n;
}
int LVSlot(lua_State* L) {  // w3.vslot(object, slot): the RVA of the function in that slot (for analysis)
    Obj* o = static_cast<Obj*>(luaL_checkudata(L, 1, kObj));
    int slot = static_cast<int>(luaL_checkinteger(L, 2));
    void* vtable = nullptr;
    void* fn = nullptr;
    if (!SafeRead(o->ptr, &vtable, 8) || !SafeRead(static_cast<void**>(vtable) + slot, &fn, 8)) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(fn) - ImageBase()));
    return 1;
}
// --- watched script lines (F4): w3.on_line(file, line, fn) runs fn(info) before the statement in that line
// runs, info = {file, line, name, self}. `file` is the end of the script's path ("playerwitcher.ws", or with folders).
struct LineWatch {
    std::string file;  // lower case, backslashes
    uint32_t line = 0;
    int ref = LUA_NOREF;
    std::string owner;
};
std::vector<LineWatch> g_lineWatches;  // index + 1 is the handle; a removed one keeps its slot with ref LUA_NOREF

std::string NormPath(std::string s) {
    for (char& ch : s) ch = ch == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return s;
}

// A function's parameters (CFunction +0x28 list, count +0x30) and locals (+0x34 list, count +0x3c, measured
// on ShowMeGoods: five locals with their offsets) are CProperties; their values lie in the frame's locals block
// (frame +0x20) at each property's offset.
template <class F> void ForEachVar(void* fn, F visit) {
    for (uint32_t listOff : {0x28u, 0x34u}) {
        void** list = nullptr;
        uint32_t n = 0;
        if (!SafeRead(static_cast<char*>(fn) + listOff, &list, 8) || !SafeRead(static_cast<char*>(fn) + listOff + 8, &n, 4) ||
            !Plausible(list) || n > 512)
            continue;
        for (uint32_t i = 0; i < n; ++i) {
            void* prop = nullptr;
            if (SafeRead(list + i, &prop, 8) && Plausible(prop)) visit(prop);
        }
    }
}

uint64_t g_lineToken = 0;  // the watched-line call running now; info.set refuses once it is over

int LLineSet(lua_State* L) {  // info.set(name, value); upvalues: fn, frame, token
    void* fn = lua_touserdata(L, lua_upvalueindex(1));
    void* frame = lua_touserdata(L, lua_upvalueindex(2));
    if (static_cast<uint64_t>(lua_tointeger(L, lua_upvalueindex(3))) != g_lineToken)
        return luaL_error(L, "info.set: only inside the on_line function");
    const char* name = luaL_checkstring(L, 1);
    char* base = Rd<char*>(frame, 0x20);
    std::string err = "no variable " + std::string(name);
    ForEachVar(fn, [&](void* prop) {
        if (NameOfId(Rd<uint32_t>(prop, 0x10)) != name || !Plausible(base)) return;
        err = WriteValue(L, 2, base + Rd<uint32_t>(prop, 0x20), Rd<void*>(prop, 0x08));
    });
    if (!err.empty()) return luaL_error(L, "info.set: %s", err.c_str());
    return 0;
}

// info = {file, line, name, self, vars, set} for a statement about to run (w3.on_line and the debugger's steps)
void PushLineInfo(lua_State* L, void* fn, void* ctx, void* frame, uint32_t line) {
    lua_createtable(L, 0, 4);
    lua_pushstring(L, ScriptFunctionFile(fn).c_str());
    lua_setfield(L, -2, "file");
    lua_pushinteger(L, line);
    lua_setfield(L, -2, "line");
    lua_pushstring(L, ScriptFunctionName(fn).c_str());
    lua_setfield(L, -2, "name");
    PushObject(L, ctx, nullptr, false);
    lua_setfield(L, -2, "self");
    lua_newtable(L);  // vars: every parameter and local by name, as values
    if (char* base = Rd<char*>(frame, 0x20); Plausible(base)) {
        ForEachVar(fn, [&](void* prop) {
            PushValue(L, base + Rd<uint32_t>(prop, 0x20), Rd<void*>(prop, 0x08), 0, true);
            lua_setfield(L, -2, NameOfId(Rd<uint32_t>(prop, 0x10)).c_str());
        });
    }
    lua_setfield(L, -2, "vars");
    uint64_t token = ++g_lineToken;
    lua_pushlightuserdata(L, fn);
    lua_pushlightuserdata(L, frame);
    lua_pushinteger(L, static_cast<lua_Integer>(token));
    lua_pushcclosure(L, &LLineSet, 3);
    lua_setfield(L, -2, "set");
}

void LuaLineFn(void* fn, void* ctx, void* frame, uint32_t line) {
    lua_State* L = g_L;
    if (!L || !OnMainThread()) return;
    std::string file;
    bool fileRead = false;
    for (size_t i = 0; i < g_lineWatches.size(); ++i) {
        if (g_lineWatches[i].ref == LUA_NOREF || g_lineWatches[i].line != line) continue;
        if (!fileRead) {
            file = NormPath(ScriptFunctionFile(fn));
            fileRead = true;
        }
        const std::string& want = g_lineWatches[i].file;
        // the script's path ends with the wanted one, or the other way round: the game knows a mod's script by a
        // shorter path than its place below content\scripts (local\tw3se_test_core.ws as tw3se_test_core.ws)
        bool ends = file.size() >= want.size() && file.compare(file.size() - want.size(), want.size(), want) == 0;
        bool within = !file.empty() && want.size() > file.size() && want[want.size() - file.size() - 1] == '\\' &&
                      want.compare(want.size() - file.size(), file.size(), file) == 0;
        if (!ends && !within) continue;
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_lineWatches[i].ref);
        PushLineInfo(L, fn, ctx, frame, line);
        std::string err;
        {
            ModScope scope(g_lineWatches[i].owner);
            ProfScope prof(g_lineWatches[i].owner, "on_line " + want + ":" + std::to_string(line));
            if (!PCall(L, 1, 0, err)) Log("lua: on_line %s:%u error: %s", want.c_str(), line, err.c_str());
        }
        ++g_lineToken;  // the frame may be gone after this statement: a kept info.set refuses from now on
        lua_settop(L, top);
    }
}

void RewatchLine(uint32_t line) {  // the line's bit stays while any watch still wants it
    bool any = false;
    for (const LineWatch& w : g_lineWatches) any = any || (w.ref != LUA_NOREF && w.line == line);
    ScriptLineWatch(line, any);
}

int LOnLine(lua_State* L) {  // w3.on_line(file, line, fn): a handle for w3.off_line
    std::string file = NormPath(luaL_checkstring(L, 1));
    lua_Integer line = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (line < 1 || line > 0xffff) return luaL_error(L, "on_line: line %d out of range", static_cast<int>(line));
    LineWatch w;
    w.file = file;
    w.line = static_cast<uint32_t>(line);
    lua_pushvalue(L, 3);
    w.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    w.owner = g_curMod;
    g_lineWatches.push_back(w);
    ScriptLineCallback(&LuaLineFn);
    ScriptLineWatch(w.line, true);
    lua_pushinteger(L, static_cast<lua_Integer>(g_lineWatches.size()));
    return 1;
}
bool OffLine(lua_State* L, size_t handle) {
    if (handle < 1 || handle > g_lineWatches.size() || g_lineWatches[handle - 1].ref == LUA_NOREF) return false;
    LineWatch& w = g_lineWatches[handle - 1];
    luaL_unref(L, LUA_REGISTRYINDEX, w.ref);
    w.ref = LUA_NOREF;
    RewatchLine(w.line);
    return true;
}
int LOffLine(lua_State* L) {
    lua_pushboolean(L, OffLine(L, static_cast<size_t>(luaL_checkinteger(L, 1))));
    return 1;
}

// --- the script debugger (F4/I7; lua\lib\debugger.lua drives it, debugger.cpp has the connection)
int g_stepRef = LUA_NOREF;

void LuaStepFn(void* fn, void* ctx, void* frame, uint32_t line) {
    lua_State* L = g_L;
    if (!L || !OnMainThread() || g_stepRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_stepRef);
    PushLineInfo(L, fn, ctx, frame, line);
    std::string err;
    if (!PCall(L, 1, 0, err)) Log("lua: debugger step error: %s", err.c_str());
    ++g_lineToken;
    lua_settop(L, top);
}

int LDbgOnStep(lua_State* L) {  // w3._dbg_on_step(fn): fn(info) at the statement a w3._dbg_step asked for
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (g_stepRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g_stepRef);
    lua_pushvalue(L, 1);
    g_stepRef = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptStepCallback(&LuaStepFn);
    return 0;
}
int LDbgStep(lua_State* L) {  // w3._dbg_step(mode): 1 next statement, 2 step over, 3 step out, 0 off
    ScriptStep(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

// The main thread's script calls, innermost first: {name, file, line} (file "" for a native)
int LDbgStack(lua_State* L) {
    ScriptStackFrameInfo f[64];
    int n = ScriptStackRead(f, 64);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, f[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, f[i].file.c_str());
        lua_setfield(L, -2, "file");
        lua_pushinteger(L, f[i].line);
        lua_setfield(L, -2, "line");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// One call's variables: {{name, value, type}, ...} in declaration order (parameters, then locals), and its object.
// Level 1 is the innermost call. Only while the main thread stands in the debugger: the frames live then.
int LDbgFrame(lua_State* L) {
    int level = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptStackFrameInfo f[64];
    int n = ScriptStackRead(f, 64);
    if (level < 1 || level > n) return 0;
    const ScriptStackFrameInfo& fr = f[level - 1];
    lua_newtable(L);
    char* base = fr.frame ? Rd<char*>(fr.frame, 0x20) : nullptr;
    if (Plausible(base)) {
        int i = 0;
        ForEachVar(fr.fn, [&](void* prop) {
            void* type = Rd<void*>(prop, 0x08);
            lua_createtable(L, 0, 3);
            lua_pushstring(L, NameOfId(Rd<uint32_t>(prop, 0x10)).c_str());
            lua_setfield(L, -2, "name");
            PushValue(L, base + Rd<uint32_t>(prop, 0x20), type, 0, true);
            lua_setfield(L, -2, "value");
            lua_pushstring(L, RttiName(type).c_str());
            lua_setfield(L, -2, "type");
            lua_rawseti(L, -2, ++i);
        });
    }
    PushObject(L, fr.ctx, nullptr, false);
    return 2;
}

int LDbgSetVar(lua_State* L) {  // w3._dbg_setvar(level, name, value): a variable of a standing call
    int level = static_cast<int>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    ScriptStackFrameInfo f[64];
    int n = ScriptStackRead(f, 64);
    if (level < 1 || level > n) return luaL_error(L, "no call at level %d", level);
    char* base = f[level - 1].frame ? Rd<char*>(f[level - 1].frame, 0x20) : nullptr;
    std::string err = "no variable " + std::string(name);
    if (Plausible(base))
        ForEachVar(f[level - 1].fn, [&](void* prop) {
            if (NameOfId(Rd<uint32_t>(prop, 0x10)) != name) return;
            err = WriteValue(L, 3, base + Rd<uint32_t>(prop, 0x20), Rd<void*>(prop, 0x08));
        });
    if (!err.empty()) return luaL_error(L, "%s", err.c_str());
    return 0;
}

int LDbgListen(lua_State* L) {  // w3._dbg_listen(port) -> true, or false when the port is taken
    lua_pushboolean(L, DebugListen(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LDbgRecv(lua_State* L) {  // w3._dbg_recv(ms): the next line from the client, nil after ms (0: only a look)
    std::string line;
    if (!DebugRecv(line, static_cast<int>(luaL_optinteger(L, 1, 0)))) return 0;
    lua_pushlstring(L, line.data(), line.size());
    return 1;
}
int LDbgSend(lua_State* L) {  // w3._dbg_send(line)
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    DebugSend(std::string(s, n));
    return 0;
}
int LDbgConnected(lua_State* L) {
    lua_pushboolean(L, DebugConnected());
    return 1;
}
void ResetLuaClock();  // below, with the time limit
int LDbgAlive(lua_State*) {  // the main thread stands in the debugger on purpose: no hang report, no time limit
    NoteMainThreadAlive();
    ResetLuaClock();
    return 0;
}
int OffLineOwner(lua_State* L, const std::string& owner) {
    int n = 0;
    for (size_t i = 0; i < g_lineWatches.size(); ++i)
        if (g_lineWatches[i].ref != LUA_NOREF && g_lineWatches[i].owner == owner && OffLine(L, i + 1)) ++n;
    return n;
}

// --- material parameters. A CMaterialInstance lists its parameters at +0x80 (24-byte entries {CName,
// IRTTIType*, void* value}, count +0x88) and keeps the renderer's copy, a CRenderMaterialParameters, at +0x68. A
// render proxy made later takes that copy; setting +0x68 to null makes the engine build a fresh one from the values
// at the next proxy (measured: a wraith spawned after the change showed the new lamp colour, the ones before kept
// theirs). The old copy stays with the proxies that hold it.
struct MatParam {
    std::string name;
    void* type;
    void* value;
};
std::vector<MatParam> MaterialParams(void* mi) {
    std::vector<MatParam> out;
    char* list = nullptr;
    uint32_t n = 0;
    if (!SafeRead(static_cast<char*>(mi) + 0x80, &list, 8) || !SafeRead(static_cast<char*>(mi) + 0x88, &n, 4) ||
        !Plausible(list) || n > 512)
        return out;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0;
        void* type = nullptr;
        void* value = nullptr;
        if (!SafeRead(list + i * 24, &id, 4) || !SafeRead(list + i * 24 + 8, &type, 8) ||
            !SafeRead(list + i * 24 + 16, &value, 8) || !Plausible(type) || !Plausible(value))
            continue;
        out.push_back({NameOfId(id), type, value});
    }
    return out;
}
void* CheckMaterial(lua_State* L, int idx) {
    Obj* o = static_cast<Obj*>(luaL_checkudata(L, idx, kObj));
    static void* mic = RttiFindType("CMaterialInstance");
    std::unordered_map<void*, bool> memo;
    if (o->isStruct || !mic || !IsA(o->cls, mic, memo)) luaL_error(L, "a CMaterialInstance is needed");
    return o->ptr;
}
int LMatParams(lua_State* L) {  // w3._mat_params(material): {name = value} (textures as their resource)
    void* mi = CheckMaterial(L, 1);
    lua_newtable(L);
    for (const MatParam& p : MaterialParams(mi)) {
        PushValue(L, p.value, p.type, 0, true);
        lua_setfield(L, -2, p.name.c_str());
    }
    return 1;
}
int LMatSet(lua_State* L) {  // w3._mat_set(material, name, value): the value, and the renderer's copy made again
    void* mi = CheckMaterial(L, 1);
    const char* name = luaL_checkstring(L, 2);
    for (const MatParam& p : MaterialParams(mi)) {
        if (p.name != name) continue;
        std::string err = WriteValue(L, 3, p.value, p.type);
        if (!err.empty()) return luaL_error(L, "material %s: %s", name, err.c_str());
        void* none = nullptr;
        SafeWrite(static_cast<char*>(mi) + 0x68, &none, 8);
        return 0;
    }
    return luaL_error(L, "the material has no parameter %s", name);
}

int LFilesReload(lua_State* L) {  // w3._files_reload(path): tw3se\files\<path> read again; true or nil, reason
    std::string err = ReloadLooseFile(luaL_checkstring(L, 1));
    if (err.empty()) { lua_pushboolean(L, 1); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
}
int LFilesSet(lua_State* L) {  // w3._files_set(path, bytes): a game file's new contents for this session (memory only)
    size_t n = 0;
    std::string p = luaL_checkstring(L, 1);
    const char* data = luaL_checklstring(L, 2, &n);
    for (char& c : p) c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // (the loaded resource stays: what the world uses is freed by the engine at the next save load)
    std::string err = FindDiskFile(p.c_str()) ? "" : NewDiskFile(p.c_str());  // a path the game lacks: a new file
    if (err.empty()) err = SetFileContents(p.c_str(), std::vector<uint8_t>(data, data + n), lua_toboolean(L, 3) != 0);
    if (err.empty()) { lua_pushboolean(L, 1); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
}
int LFilesClear(lua_State* L) {  // w3._files_clear(path): the game's own contents of a file again
    std::string p = luaL_checkstring(L, 1);
    for (char& c : p) c = c == '/' ? '\\' : c;
    std::string err = ClearFileContents(p.c_str(), lua_toboolean(L, 2) != 0);
    if (err.empty()) { lua_pushboolean(L, 1); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
}
int LDeformProbe(lua_State* L) {  // w3._deform_probe(index count): the bindings of such draws to the log
    DeformProbe(static_cast<uint32_t>(luaL_checkinteger(L, 1)));
    lua_pushinteger(L, static_cast<lua_Integer>(DeformBuffersSeen()));
    return 1;
}
// Blend shapes (blendshape.cpp); w3.deform in lua_lib.inc wraps these. A relative path is from TW3SE's folder.
std::wstring BlendPath(const char* p) {
    int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, p, -1, &w[0], n);
    bool absolute = w.size() > 1 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'));
    return absolute ? w : Dir() + L"\\" + w;
}
int LBlendLoad(lua_State* L) {  // w3._bs_load(render mesh, path, range) -> set id | nil, error
    std::string err;
    int id = BlendLoad(static_cast<uintptr_t>(luaL_checkinteger(L, 1)), BlendPath(luaL_checkstring(L, 2)),
                       static_cast<float>(luaL_optnumber(L, 3, 1.0)), &err);
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}
int LBlendSet(lua_State* L) {  // w3._bs_set(set, shape name | index from 1, weight) -> true | false
    int set = static_cast<int>(luaL_checkinteger(L, 1));
    int shape = lua_type(L, 2) == LUA_TSTRING ? BlendShapeIndex(set, lua_tostring(L, 2))
                                              : static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    lua_pushboolean(L, shape >= 0 && BlendWeight(set, shape, static_cast<float>(luaL_checknumber(L, 3))));
    return 1;
}
int LBlendNames(lua_State* L) {  // w3._bs_names(set) -> {name, ...}
    std::vector<std::string> names = BlendNames(static_cast<int>(luaL_checkinteger(L, 1)));
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
int LBlendState(lua_State* L) {
    lua_pushstring(L, BlendState(static_cast<int>(luaL_checkinteger(L, 1))).c_str());
    return 1;
}
int LBlendUnload(lua_State* L) {
    lua_pushboolean(L, BlendUnload(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LHideOpen(lua_State* L) {  // w3._hide_open(render mesh) -> hide id | nil, error
    std::string err;
    int id = HideOpen(static_cast<uintptr_t>(luaL_checkinteger(L, 1)), &err);
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}
int LHideBits(lua_State* L) {  // w3._hide_bits(hide, chunk from 0, bit field string) -> true | nil, error
    size_t n = 0;
    const char* b = luaL_checklstring(L, 3, &n);
    std::string err;
    if (!HideBits(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<uint32_t>(luaL_checkinteger(L, 2)),
                  reinterpret_cast<const uint8_t*>(b), n, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LHideClose(lua_State* L) {
    lua_pushboolean(L, HideClose(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LHideState(lua_State* L) {
    lua_pushstring(L, HideState(static_cast<int>(luaL_checkinteger(L, 1))).c_str());
    return 1;
}
int LHideForget(lua_State* L) {
    lua_pushinteger(L, HideForget(static_cast<uintptr_t>(luaL_checkinteger(L, 1))));
    return 1;
}
// The skinning shader hook (skinhook.cpp): w3.skinhook in lua_lib.inc wraps these.
int LSkinAdd(lua_State* L) {  // w3._skin_add(render mesh) -> slot | nil, error
    std::string err;
    int slot = SkinHookAdd(static_cast<uintptr_t>(luaL_checkinteger(L, 1)), &err, lua_toboolean(L, 2) != 0);
    if (!slot) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushinteger(L, slot);
    return 1;
}
int LSkinRemove(lua_State* L) {
    lua_pushboolean(L, SkinHookRemove(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LSkinCapsules(lua_State* L) {  // w3._skin_caps(slot, {ax, ay, az, r, bx, by, bz, strength, ...})
    int slot = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    float v[8 * 8] = {};
    int n = static_cast<int>(lua_rawlen(L, 2));
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        v[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, SkinHookCapsules(slot, v, n / 8));
    return 1;
}
int LSkinCapsulesEx(lua_State* L) {  // w3._skin_caps_ex(slot, {ax, ay, az, r, bx, by, bz, strength, falloff, mode, 0, 0, ...})
    int slot = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    float v[32 * 12] = {};
    int n = static_cast<int>(lua_rawlen(L, 2));
    if (n > 32 * 12) n = 32 * 12;
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        v[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, SkinHookCapsulesEx(slot, v, n / 12));
    return 1;
}
int LSkinWeights(lua_State* L) {  // w3._skin_weights(slot, chunk, {w, ...}) -> true | nil, error
    int slot = static_cast<int>(luaL_checkinteger(L, 1)), chunk = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<float> v(lua_rawlen(L, 3));
    for (size_t i = 0; i < v.size(); ++i) {
        lua_rawgeti(L, 3, static_cast<lua_Integer>(i + 1));
        v[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    std::string err;
    if (!SkinHookWeights(slot, chunk, v.data(), static_cast<int>(v.size()), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LSkinChunks(lua_State* L) {  // w3._skin_chunks(slot) -> {vertex count of chunk 0, of chunk 1, ...}
    std::vector<uint32_t> c = SkinHookChunks(static_cast<int>(luaL_checkinteger(L, 1)));
    lua_createtable(L, static_cast<int>(c.size()), 0);
    for (size_t i = 0; i < c.size(); ++i) {
        lua_pushinteger(L, c[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
int LSkinState(lua_State* L) {  // w3._skin_state(slot) -> "none" | "active" | "lost"
    int st = SkinHookState(static_cast<int>(luaL_checkinteger(L, 1)));
    lua_pushstring(L, st == 2 ? "active" : st == 3 ? "lost" : "none");
    return 1;
}
int LSkinStats(lua_State* L) {
    lua_pushstring(L, SkinHookStats().c_str());
    return 1;
}
int LBlendForget(lua_State* L) {  // w3._bs_forget(render mesh) -> how many sets ended (w3.deform.release)
    lua_pushinteger(L, BlendForget(static_cast<uintptr_t>(luaL_checkinteger(L, 1))));
    return 1;
}
int LBlendResetAll(lua_State* L) {  // w3._bs_reset_all() -> how many sets ended (before a render rebuild)
    lua_pushinteger(L, BlendResetAll());
    return 1;
}
int LRenderRebuild(lua_State* L) {  // w3._render_rebuild(phase): the plugins' callbacks, 0 before and 1 after
    FireRenderRebuild(static_cast<int>(luaL_checkinteger(L, 1)) ? 1 : 0);
    return 0;
}
int LBlendStats(lua_State* L) {
    lua_pushstring(L, BlendStats().c_str());
    return 1;
}
// Live meshes (blendshape.cpp); w3.deform.live in lua_lib.inc wraps these. Chunks count from 0, as in the interface.
int LLiveNew(lua_State* L) {  // w3._lm_new() -> id
    lua_pushinteger(L, LiveNew());
    return 1;
}
int LLiveAttach(lua_State* L) {  // w3._lm_attach(id, render mesh) -> true | nil, error
    std::string err;
    if (!LiveAttach(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<uintptr_t>(luaL_checkinteger(L, 2)), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LLiveFail(lua_State* L) {
    lua_pushboolean(L, LiveFail(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LLiveClose(lua_State* L) {
    lua_pushboolean(L, LiveClose(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LLiveInfo(lua_State* L) {  // w3._lm_info(id, chunk) -> vertex count, chunk count | nil
    int count = 0, chunks = 0;
    if (!LiveInfo(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_optinteger(L, 2, 0)), &count, &chunks)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, count);
    lua_pushinteger(L, chunks);
    return 2;
}
std::vector<float> LuaFloats(lua_State* L, int idx) {
    std::vector<float> v(lua_rawlen(L, idx));
    for (size_t i = 0; i < v.size(); ++i) {
        lua_rawgeti(L, idx, static_cast<lua_Integer>(i + 1));
        v[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return v;
}
int LLiveWrite(lua_State* L) {  // w3._lm_write(id, chunk, {x, y, z, ...}, {nx, ny, nz, ...} | nil) -> true | nil, error
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<float> pos = LuaFloats(L, 3), nrm;
    if (lua_istable(L, 4)) nrm = LuaFloats(L, 4);
    std::string err;
    if (!nrm.empty() && nrm.size() != pos.size()) err = "positions and normals differ in length";
    else if (!LiveWrite(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)), pos.data(),
                        nrm.empty() ? nullptr : nrm.data(), static_cast<int>(pos.size() / 3), &err) && err.empty())
        err = "not written";
    if (!err.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LLiveBounds(lua_State* L) {  // w3._lm_bounds(id, {x, y, z}, {x, y, z}) -> true | false
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<float> mn = LuaFloats(L, 2), mx = LuaFloats(L, 3);
    lua_pushboolean(L, mn.size() >= 3 && mx.size() >= 3 &&
                           LiveBounds(static_cast<int>(luaL_checkinteger(L, 1)), mn.data(), mx.data()));
    return 1;
}
// w3._tex_test(depot path, t [, w, h]): a test picture (moving rings) written into the texture -> true | nil, error
int LTexTest(lua_State* L) {
    int w = static_cast<int>(luaL_optinteger(L, 3, 512)), h = static_cast<int>(luaL_optinteger(L, 4, 512));
    std::vector<uint8_t> p = TexTestPicture(w, h, static_cast<float>(luaL_optnumber(L, 2, 0)));
    std::string err;
    if (!TextureWrite(luaL_checkstring(L, 1), p.data(), w, h, 1, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
// w3._tex_pin(depot path, DDS file under tw3se or absolute) -> true | nil, error; w3._tex_unpin(depot path) -> bool
int LTexPin(lua_State* L) {
    std::string err;
    if (!TexturePin(luaL_checkstring(L, 1), BlendPath(luaL_checkstring(L, 2)), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
// w3._tex_stamp(texture path, position map file, {layer, ...}): a layer {motif = file, x, y, z, nx, ny, nz, size,
// turn = degrees, opacity, flip, ink = {r, g, b}} - its frame from the normal and the turn (up toward +z)
// w3._tex_eye(depot path, layers file, {cat, veins, white, vein, outer, inner, pupil = {r, g, b} 0..255})
int LTexEye(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::wstring layers = BlendPath(luaL_checkstring(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    TexEyeLook k;
    lua_getfield(L, 3, "cat");
    k.cat = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 3, "veins");
    if (lua_isnumber(L, -1)) k.veins = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    auto rgb = [&](const char* name, float* out) {
        lua_getfield(L, 3, name);
        if (lua_istable(L, -1))
            for (int c = 0; c < 3; ++c) {
                lua_geti(L, -1, c + 1);
                out[c] = static_cast<float>(lua_tonumber(L, -1)) / 255.0f;
                lua_pop(L, 1);
            }
        lua_pop(L, 1);
    };
    rgb("white", k.white), rgb("vein", k.vein), rgb("outer", k.outer), rgb("inner", k.inner), rgb("pupil", k.pupil);
    std::string err;
    if (!TextureEye(path, layers, k, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LTexStamp(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::wstring pm = BlendPath(luaL_checkstring(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<TexStampLayer> layers;
    lua_Integer n = luaL_len(L, 3);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, 3, i);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        auto num = [&](const char* k, float def) {
            lua_getfield(L, -1, k);
            float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
            lua_pop(L, 1);
            return v;
        };
        TexStampLayer s;
        lua_getfield(L, -1, "motif");
        s.motif = BlendPath(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        s.centre[0] = num("x", 0), s.centre[1] = num("y", 0), s.centre[2] = num("z", 0);
        float nv[3] = {num("nx", 0), num("ny", 1), num("nz", 0)};
        float len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
        if (len < 1e-6f) nv[1] = 1, len = 1;
        for (float& v : nv) v /= len;
        float up[3] = {-nv[2] * nv[0], -nv[2] * nv[1], 1 - nv[2] * nv[2]};
        float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (ul < 1e-4f) up[0] = -nv[1] * nv[0], up[1] = 1 - nv[1] * nv[1], up[2] = -nv[1] * nv[2],
                        ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        for (float& v : up) v /= ul;
        float rt[3] = {up[1] * nv[2] - up[2] * nv[1], up[2] * nv[0] - up[0] * nv[2], up[0] * nv[1] - up[1] * nv[0]};
        float a = num("turn", 0) * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
        for (int k = 0; k < 3; ++k) {
            s.normal[k] = nv[k];
            s.up[k] = up[k] * c + rt[k] * sn;
            s.right[k] = rt[k] * c - up[k] * sn;
        }
        s.size = num("size", 0.1f), s.depth = num("depth", 0.04f), s.opacity = num("opacity", 1.0f);
        s.facing = num("facing", 0.15f);
        lua_getfield(L, -1, "flip");
        s.flip = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "ink");
        if (lua_istable(L, -1)) {
            s.tint = true;
            for (int k = 0; k < 3; ++k) {
                lua_geti(L, -1, k + 1);
                s.colour[k] = static_cast<float>(lua_tonumber(L, -1)) / 255.0f;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        layers.push_back(s);
        lua_pop(L, 1);
    }
    std::string err;
    if (!TextureStamp(path, pm, layers, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LTexUnpin(lua_State* L) {
    lua_pushboolean(L, TextureUnpin(luaL_checkstring(L, 1)));
    return 1;
}
int LBlendNormals(lua_State* L) {  // w3._bs_normals(set or live mesh, on) -> true | nil, error
    std::string err;
    if (!BlendNormals(static_cast<int>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2) ? 1 : 0, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LSkinCapture(lua_State* L) {  // w3._skin_capture(slot, path) -> true | nil, error
    std::string err;
    if (!SkinHookCapture(static_cast<int>(luaL_checkinteger(L, 1)), BlendPath(luaL_checkstring(L, 2)), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
// 19.11.: w3._pso_trace(frames, path): the pipelines the next frames set, how often, to a file (tw3se folder)
int LPsoTrace(lua_State* L) {
    int frames = static_cast<int>(luaL_checkinteger(L, 1));
    const char* name = luaL_optstring(L, 2, "pso_trace.txt");
    std::wstring path = Dir() + L"\\";
    for (const char* c = name; *c; ++c) path += static_cast<wchar_t>(*c);
    lua_pushboolean(L, RenderPsoTrace(frames, path));
    return 1;
}
int LSkinDqs(lua_State* L) {  // w3._skin_dqs(slot, on) -> true | nil, error
    std::string err;
    if (!SkinHookDqs(static_cast<int>(luaL_checkinteger(L, 1)), lua_isnone(L, 2) || lua_toboolean(L, 2), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LSkinMush(lua_State* L) {  // w3._skin_mush(slot, iterations) -> true | nil, error
    std::string err;
    if (!SkinHookDeltaMush(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_optinteger(L, 2, 8)), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LSkinMushMask(lua_State* L) {  // w3._skin_mush_mask(slot, chunk, {m, ...}) -> true | nil, error
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<float> m = LuaFloats(L, 3);
    std::string err;
    if (!SkinHookMushMask(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)), m.data(),
                          static_cast<int>(m.size()), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LMushDump(lua_State* L) {  // w3._skin_mush_dump(slot, path without extension) -> what was queued
    lua_pushstring(L, MushDump(static_cast<int>(luaL_checkinteger(L, 1)), BlendPath(luaL_checkstring(L, 2))).c_str());
    return 1;
}
// w3._skin_overlay_test(slot, size, t): the test picture (rings, alpha falling off from the middle) as the slot's
// skin overlay; size 0 takes it away
int LSkinOverlayTest(lua_State* L) {
    int slot = static_cast<int>(luaL_checkinteger(L, 1)), size = static_cast<int>(luaL_optinteger(L, 2, 512));
    std::vector<uint8_t> p = size ? TexTestPicture(size, size, static_cast<float>(luaL_optnumber(L, 3, 0)))
                                  : std::vector<uint8_t>();
    std::string err;
    if (!SkinHookOverlay(slot, p.data(), size, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
// w3._skin_material_test(slot, size, kind): a test material plane: 'wet', 'matte' (all of it), 'drops' (round
// drops: wet, a dome of height, and a milky colour); 'half' (left half wet, right half matte). size 0: off
// w3._skin_overlay_raw(slot, plane, size, bytes): a plane of the slot's overlay (0 colour, 1 material) from a
// string of 4 * size * size bytes RGBA8; bytes nil: the plane clear
int LSkinOverlayRaw(lua_State* L) {
    int slot = static_cast<int>(luaL_checkinteger(L, 1)), plane = static_cast<int>(luaL_checkinteger(L, 2));
    int size = static_cast<int>(luaL_checkinteger(L, 3));
    size_t n = 0;
    const char* b = lua_isnoneornil(L, 4) ? nullptr : luaL_checklstring(L, 4, &n);
    std::string err;
    if (plane < 0 || plane > 1 || size < 0 || (b && n != 4ull * size * size))
        err = "plane 0 or 1, and 4 * size * size bytes";
    else if (SkinHookOverlayPlane(slot, plane, b, size, &err))
        return lua_pushboolean(L, 1), 1;
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
}
int LSkinMaterialTest(lua_State* L) {
    int slot = static_cast<int>(luaL_checkinteger(L, 1)), size = static_cast<int>(luaL_optinteger(L, 2, 512));
    std::string kind = luaL_optstring(L, 3, "drops"), err;
    std::vector<uint8_t> m(4ull * size * size), c(4ull * size * size);
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    struct Drop { float x, y, r; };
    std::vector<Drop> drops;
    for (int i = 0; kind == "drops" && i < 60; ++i) drops.push_back({rnd() * size, rnd() * size, (0.01f + 0.03f * rnd()) * size});
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            uint8_t* p = &m[4ull * (y * size + x)];
            uint8_t* q = &c[4ull * (y * size + x)];
            if (kind == "wet") p[0] = 255;
            else if (kind == "matte") p[1] = 255;
            else if (kind == "half") (x < size / 2 ? p[0] : p[1]) = 255;
            float h = 0;
            for (const Drop& d : drops) {
                float dx = (x - d.x) / d.r, dy = (y - d.y) / d.r, r2 = dx * dx + dy * dy;
                if (r2 < 1) h = (std::max)(h, std::sqrt(1 - r2));
            }
            if (h > 0) {
                p[0] = 255;
                p[2] = static_cast<uint8_t>(h * 255);
                q[0] = 235, q[1] = 232, q[2] = 220, q[3] = static_cast<uint8_t>(40 + 60 * (std::min)(1.0f, h * 3));
            }
        }
    if ((kind == "drops" && !SkinHookOverlay(slot, size ? c.data() : nullptr, size, &err)) ||
        !SkinHookOverlayPlane(slot, 1, size ? m.data() : nullptr, size, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LIndexDump(lua_State* L) {  // w3._bs_index_dump(render mesh, path) -> what was queued
    lua_pushstring(L, IndexDump(static_cast<uintptr_t>(luaL_checkinteger(L, 1)), BlendPath(luaL_checkstring(L, 2)),
                                lua_toboolean(L, 3) != 0).c_str());
    return 1;
}
int LTexReadback(lua_State* L) {  // w3._tex_readback(depot path, file under tw3se) -> what was written
    lua_pushstring(L, TexReadback(luaL_checkstring(L, 1), BlendPath(luaL_checkstring(L, 2))).c_str());
    return 1;
}
int LTexStats(lua_State* L) {
    lua_pushstring(L, TexStats().c_str());
    return 1;
}
int LTexProbe(lua_State* L) {  // w3._tex_probe(depot path, file under tw3se) -> what was written
    lua_pushstring(L, TexProbe(luaL_checkstring(L, 1), BlendPath(luaL_checkstring(L, 2))).c_str());
    return 1;
}
int LBlendDump(lua_State* L) {  // w3._bs_dump(render mesh, path) -> what was queued, or why not
    lua_pushstring(L, BlendDump(static_cast<uintptr_t>(luaL_checkinteger(L, 1)), BlendPath(luaL_checkstring(L, 2)), lua_toboolean(L, 3) != 0).c_str());
    return 1;
}
// w3._shot_burst(dir, frames, x, y, w, h): a box of the picture in each of the next frames, raw files in dir
// (measuring: in which frame a change shows); w3._shot_burst_done() counts the files written
int LShotBurst(lua_State* L) {
    lua_pushboolean(L, UiShotBurst(luaL_checkstring(L, 1), static_cast<int>(luaL_checkinteger(L, 2)),
                                   static_cast<int>(luaL_checkinteger(L, 3)), static_cast<int>(luaL_checkinteger(L, 4)),
                                   static_cast<int>(luaL_checkinteger(L, 5)), static_cast<int>(luaL_checkinteger(L, 6))));
    return 1;
}
int LShotBurstDone(lua_State* L) {
    lua_pushinteger(L, UiShotBurstDone());
    return 1;
}
int LDeformStats(lua_State* L) {
    lua_pushstring(L, DeformStats().c_str());
    return 1;
}
// w3._memfind(value [, max]): the addresses of 8-byte-aligned qwords equal to value in the process's committed,
// writable private memory (analysis: where the engine keeps a pointer), at most max (16).
int LMemFind(lua_State* L) {
    uint64_t value = static_cast<uint64_t>(luaL_checkinteger(L, 1));
    int max = static_cast<int>(luaL_optinteger(L, 2, 16));
    lua_newtable(L);
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; n < max && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi); ) {
        uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
        bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        uintptr_t own = reinterpret_cast<uintptr_t>(&value);  // this thread's stack holds the value too
        if (rw && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) && !(own >= start && own < end)) {
            uint64_t chunk[512];
            for (uintptr_t p = start; p < end && n < max; p += sizeof chunk) {
                size_t len = (std::min)(sizeof chunk, static_cast<size_t>(end - p));
                if (!SafeRead(reinterpret_cast<void*>(p), chunk, len)) continue;
                for (size_t i = 0; i < len / 8 && n < max; ++i)
                    if (chunk[i] == value) {
                        lua_pushinteger(L, static_cast<lua_Integer>(p + i * 8));
                        lua_rawseti(L, -2, ++n);
                    }
            }
        }
        if (end <= a) break;
        a = end;
    }
    return 1;
}
int LDiskFile(lua_State* L) {  // w3._diskfile(path): the address of the depot's CDiskFile (analysis), nil if none
    void* df = FindDiskFile(luaL_checkstring(L, 1));
    if (!df) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(df)));
    return 1;
}

int LFilesAlias(lua_State* L) {  // w3._files_alias(path, new path): true, or nil and the reason
    std::string err = FileAlias(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    if (err.empty()) { lua_pushboolean(L, 1); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
}

// The mesh of a running mesh component: the component's own setter (CMeshComponent vtable slot 93 in 5.01):
// it takes a ref on the new mesh, drops the old one and builds the render proxy anew. Found by what it does,
// not by its slot: within its first 0x90 bytes it writes the handle of the field `mesh` (`mov [rbx + offset], r8`).
// The generic SetResource (slot 88) only checks the class and calls it; its write lies further on.
int LSetMesh(lua_State* L) {  // w3._set_mesh(component, mesh)
    Obj* c = CheckLive(L, 1);
    Obj* m = CheckLive(L, 2);
    static void* meshClass = RttiFindType("CMesh");
    std::unordered_map<void*, bool> memo;
    if (m->isStruct || !meshClass || !IsA(m->cls, meshClass, memo)) return luaL_error(L, "a CMesh is needed");
    void* prop = c->isStruct ? nullptr : FindProp(c->cls, "mesh");
    if (!prop) return luaL_error(L, "%s has no field mesh", RttiName(c->cls).c_str());
    uint32_t off = Rd<uint32_t>(prop, 0x20);
    void** vt = nullptr;
    if (!SafeRead(c->ptr, &vt, 8) || !vt) return luaL_error(L, "the component has no vtable");
    static std::unordered_map<void**, void*> setters;  // vtable -> setter
    void*& fn = setters[vt];
    if (!fn) {
        uint8_t pat[7] = {0x4c, 0x89, 0x83};
        memcpy(pat + 3, &off, 4);
        for (int s = 0; s < 260 && !fn; ++s) {
            void* f = nullptr;
            uint8_t code[0x90];
            if (!SafeRead(&vt[s], &f, 8) || !f || !SafeRead(f, code, sizeof code)) break;
            for (size_t i = 0; i + sizeof pat <= sizeof code; ++i)
                if (!memcmp(code + i, pat, sizeof pat)) {
                    fn = f;
                    Log("set_mesh: %s setter at vtable slot %d", RttiName(c->cls).c_str(), s);
                    break;
                }
        }
        if (!fn) return luaL_error(L, "the mesh setter of %s was not found in this build", RttiName(c->cls).c_str());
    }
    void* ob = BlockOf(c->ptr);
    reinterpret_cast<void (*)(void*, void*)>(fn)(c->ptr, m->ptr);
    // The setter discards the component's handles: the old block points at nothing, the component has a new one
    // (measured on a beard: the old block 13 refs -> 11 and no object, the new one only the component's
    // own ref). Every handle the game held goes dead with it, the appearance component's among them, and
    // ExcludeAppearanceTemplate would leave a component whose mesh was swapped standing after a change of clothes.
    // The old block goes back: the component's handles hold again. The new block, which
    // nothing else holds, keeps its one ref and no object (16 bytes stay; Lua never frees an engine block).
    void* fresh = Rd<void*>(c->ptr, 0x08);
    if (ob && fresh != ob && Plausible(fresh) && Rd<void*>(fresh, 0x08) == c->ptr && !Rd<void*>(ob, 0x08)) {
        void* obj = c->ptr;
        void* none = nullptr;
        InterlockedIncrement(static_cast<volatile LONG*>(ob));  // the component's own ref, as before the setter
        if (SafeWrite(static_cast<char*>(ob) + 0x08, &obj, 8) && SafeWrite(static_cast<char*>(c->ptr) + 0x08, &ob, 8))
            SafeWrite(static_cast<char*>(fresh) + 0x08, &none, 8);
    }
    // If the old block could not go back, the Lua value moves to the new block, so it stays usable.
    void* nb = BlockOf(c->ptr);
    if (nb && nb != c->block) {
        if (c->owns) {
            InterlockedIncrement(static_cast<volatile LONG*>(nb));
            ReleaseRef(c->block);
        }
        c->block = nb;
    }
    return 0;
}

// A drawable component's render proxy made anew from its current data (a material changed with w3.material.set
// shows on an entity that stands in the world already).
int LRefresh(lua_State* L) {  // w3._refresh(component) -> true, or false when the component is not in the world
    Obj* c = CheckLive(L, 1);
    static void* drawable = RttiFindType("CDrawableComponent");
    std::unordered_map<void*, bool> memo;
    if (c->isStruct || !drawable || !IsA(c->cls, drawable, memo)) return luaL_error(L, "a CDrawableComponent is needed");
    auto fn = reinterpret_cast<void (*)(void*)>(Address("CDrawableComponent::RefreshRenderProxies"));
    if (!fn) return luaL_error(L, "RefreshRenderProxies was not found in this build");
    uint32_t flags = 0;
    bool attached = SafeRead(static_cast<char*>(c->ptr) + 0x54, &flags, 4) && (flags & 0x100000);
    if (attached) fn(c->ptr);
    lua_pushboolean(L, attached);
    return 1;
}

int LSectorList(lua_State* L) {  // w3._sector(sector address) -> {{type, flags, radius, X, Y, Z}, ...} or nil
    std::vector<SectorObject> objs;
    if (SectorObjects(reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))), &objs) < 0) return 0;
    lua_createtable(L, static_cast<int>(objs.size()), 0);
    for (size_t i = 0; i < objs.size(); ++i) {
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, objs[i].type);
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, objs[i].flags);
        lua_setfield(L, -2, "flags");
        lua_pushinteger(L, objs[i].radius);
        lua_setfield(L, -2, "radius");
        const char* axes[3] = {"X", "Y", "Z"};
        for (int k = 0; k < 3; ++k) {
            lua_pushnumber(L, objs[i].pos[k]);
            lua_setfield(L, -2, axes[k]);
        }
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
// w3._sector_meshes(sector address [, x, y, z, radius | index from 1]) -> {{index (from 1), resource, hash (16 hex
// digits), m = {12}, box = {6}, min = {3}, max = {3}, collision}, ...} or nil: sector.cpp, SectorMeshes
int LSectorMeshes(lua_State* L) {
    std::vector<SectorMesh> ms;
    float c[3] = {};
    bool one = lua_gettop(L) == 2, around = !one && !lua_isnoneornil(L, 2);
    if (around) c[0] = static_cast<float>(luaL_checknumber(L, 2)), c[1] = static_cast<float>(luaL_checknumber(L, 3)),
              c[2] = static_cast<float>(luaL_checknumber(L, 4));
    if (SectorMeshes(reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))), &ms, around ? c : nullptr,
                     around ? static_cast<float>(luaL_checknumber(L, 5)) : 0, one ? luaL_checkinteger(L, 2) - 1 : -1) < 0)
        return 0;
    lua_createtable(L, static_cast<int>(ms.size()), 0);
    for (size_t i = 0; i < ms.size(); ++i) {
        const SectorMesh& m = ms[i];
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, m.index + 1);
        lua_setfield(L, -2, "index");
        lua_pushinteger(L, m.resource);
        lua_setfield(L, -2, "resource");
        lua_pushinteger(L, m.group);
        lua_setfield(L, -2, "group");
        char h[20];
        snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(m.hash));
        lua_pushstring(L, h);
        lua_setfield(L, -2, "hash");
        lua_createtable(L, 12, 0);
        for (int k = 0; k < 12; ++k) {
            lua_pushnumber(L, m.matrix[k]);
            lua_rawseti(L, -2, k + 1);
        }
        lua_setfield(L, -2, "m");
        lua_createtable(L, 6, 0);
        for (int k = 0; k < 6; ++k) {
            lua_pushnumber(L, m.box[k]);
            lua_rawseti(L, -2, k + 1);
        }
        lua_setfield(L, -2, "box");
        for (int w = 0; w < 2; ++w) {
            lua_createtable(L, 3, 0);
            for (int k = 0; k < 3; ++k) {
                lua_pushnumber(L, w ? m.max[k] : m.min[k]);
                lua_rawseti(L, -2, k + 1);
            }
            lua_setfield(L, -2, w ? "max" : "min");
        }
        lua_pushboolean(L, m.collision);
        lua_setfield(L, -2, "collision");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
int LMeshPath(lua_State* L) {  // w3._mesh_path(16 hex digits) -> the mesh's depot path, or nil
    uint64_t h = _strtoui64(luaL_checkstring(L, 1), nullptr, 16);
    std::string p = MeshPathOfHash(h);
    if (p.empty()) return 0;
    lua_pushstring(L, p.c_str());
    return 1;
}
int LSectorWorld(lua_State* L) {  // w3._sector_world(world) -> the address of its merged sector data, or nil
    void* s = WorldSectorData(CheckLive(L, 1)->ptr);
    if (!s) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(s)));
    return 1;
}
int LSectorSetPos(lua_State* L) {  // w3._sector_pos(sector address, index from 1, x, y, z) -> true | false
    float p[3] = {static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
                  static_cast<float>(luaL_checknumber(L, 5))};
    lua_pushboolean(L, SectorSetPos(reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))),
                                    static_cast<uint32_t>(luaL_checkinteger(L, 2) - 1), p));
    return 1;
}
// w3._sector_proxies(world, {index from 1, ...}) -> {[index] = render proxy address} of the objects of the world's
// block that are streamed in, or nil: sector.cpp, SectorProxies
int LSectorProxies(lua_State* L) {
    void* world = CheckLive(L, 1)->ptr;
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<uint32_t> ids;
    for (lua_Integer i = 1;; ++i) {
        if (lua_rawgeti(L, 2, i) == LUA_TNIL) { lua_pop(L, 1); break; }
        ids.push_back(static_cast<uint32_t>(luaL_checkinteger(L, -1) - 1));
        lua_pop(L, 1);
    }
    std::vector<void*> px(ids.size());
    if (SectorProxies(world, ids.data(), ids.size(), px.data()) < 0) return 0;
    lua_createtable(L, 0, static_cast<int>(ids.size()));
    for (size_t i = 0; i < ids.size(); ++i) {
        if (!px[i]) continue;
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(px[i])));
        lua_rawseti(L, -2, static_cast<lua_Integer>(ids[i]) + 1);
    }
    return 1;
}
// w3._sector_unstream(world, index from 1 [, on]): the world block's object (with its collision) out of the world at
// once and kept out (on false) or back (on true) -> objects switched, or nil (no streamer); without `on`: is it
// switched off (true/false): sector.cpp, SectorUnstream
int LSectorUnstream(lua_State* L) {
    void* world = CheckLive(L, 1)->ptr;
    lua_Integer i = luaL_checkinteger(L, 2);
    if (i < 1 || i > 0x7fffffff) return luaL_argerror(L, 2, "an index from 1");
    const uint32_t id = static_cast<uint32_t>(i - 1);
    if (lua_isnoneornil(L, 3)) {
        int r = SectorUnstreamed(world, id);
        if (r < 0) return 0;
        lua_pushboolean(L, r);
        return 1;
    }
    int n = SectorUnstream(world, id, lua_toboolean(L, 3) != 0);
    if (n < 0) return 0;
    lua_pushinteger(L, n);
    return 1;
}
// w3._proxy_lc(render proxy address, set bits, clear bits) -> the light channel byte before, or nil (not a render
// proxy): highlight.cpp, ProxyChannels
int LProxyLc(lua_State* L) {
    uint8_t before = 0;
    if (!ProxyChannels(reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))),
                       static_cast<uint8_t>(luaL_checkinteger(L, 2)), static_cast<uint8_t>(luaL_checkinteger(L, 3)), &before))
        return 0;
    lua_pushinteger(L, before);
    return 1;
}
// w3._focus_extra(slot 0 | 1 [, r, g, b, a [, stencil bit]]) -> the stencil bit of the slot's objects, nil when the
// extra colours are not there. Without a colour the slot goes off: highlight.cpp, FocusExtraColor
int LFocusExtra(lua_State* L) {
    float c[4];
    bool on = !lua_isnoneornil(L, 2);
    for (int k = 0; on && k < 4; ++k) c[k] = static_cast<float>(luaL_checknumber(L, 2 + k));
    uint8_t bit = FocusExtraColor(static_cast<int>(luaL_checkinteger(L, 1)), on ? c : nullptr,
                                  static_cast<uint8_t>(luaL_optinteger(L, 6, 0)));
    if (!bit) return 0;
    lua_pushinteger(L, bit);
    return 1;
}
// w3._focus_probe() -> the pipelines the focus effect's quads set at the last probe; asks for the next one
int LFocusProbe(lua_State* L) {
    std::string s = FocusProbe();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
// w3._focus_census([on]) -> {all = samples, [0x01] = samples ... [0x80]}, frames counted: highlight.cpp, FocusCensus
int LFocusCensus(lua_State* L) {
    int on = lua_isnoneornil(L, 1) ? -1 : lua_toboolean(L, 1) ? 1 : 0;
    uint64_t c[9];
    uint64_t frames = FocusCensus(on, c);
    lua_createtable(L, 0, 9);
    for (int k = 0; k < 8; ++k) {
        lua_pushinteger(L, static_cast<lua_Integer>(c[k]));
        lua_rawseti(L, -2, lua_Integer{1} << k);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(c[8]));
    lua_setfield(L, -2, "all");
    lua_pushinteger(L, static_cast<lua_Integer>(frames));
    return 2;
}
// w3._focus_color(group 0..3 [, r, g, b, a]) -> r, g, b, a (premultiplied) or nil: highlight.cpp, FocusGroupColor
int LFocusColor(lua_State* L) {
    int g = static_cast<int>(luaL_checkinteger(L, 1));
    float c[4], set[4];
    bool setting = !lua_isnoneornil(L, 2);
    for (int k = 0; setting && k < 4; ++k) set[k] = static_cast<float>(luaL_checknumber(L, 2 + k));
    if (!FocusGroupColor(g, c, setting ? set : nullptr)) return 0;
    for (float v : c) lua_pushnumber(L, v);
    return 4;
}

int LTerrainProbe(lua_State* L) {  // w3._terrain_probe(clipmap address, x, y [, spacing]) -> height, raw value
    float h = 0;
    uint32_t raw = 0;
    std::string err = TerrainProbe(reinterpret_cast<void*>(luaL_checkinteger(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                                   static_cast<float>(luaL_checknumber(L, 3)), static_cast<int>(luaL_optinteger(L, 4, 0)), &h, &raw);
    if (!err.empty()) return luaL_error(L, "terrain: %s", err.c_str());
    lua_pushnumber(L, h);
    lua_pushinteger(L, raw);
    return 2;
}

// w3._rest_bones(skeleton [, on | {names}]) -> true, or (nil) the stats with no skeleton: posehook.cpp, PoseRestBones.
// An address with on = false forgets a rig object that is gone.
int LRestBones(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushstring(L, PoseRestStats().c_str());
        return 1;
    }
    if (lua_isinteger(L, 1)) {
        PoseRestForget(reinterpret_cast<void*>(static_cast<uintptr_t>(lua_tointeger(L, 1))));
        lua_pushboolean(L, 1);
        return 1;
    }
    std::vector<std::string> names;
    bool list = lua_istable(L, 2);
    if (list)
        for (lua_Integer i = 1, n = luaL_len(L, 2); i <= n; ++i) {
            lua_geti(L, 2, i);
            if (!lua_isstring(L, -1)) return luaL_error(L, "w3.anim.rest_bones: the bones are names (strings)");
            names.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    std::string err = PoseRestBones(CheckLive(L, 1)->ptr, list || lua_isnoneornil(L, 2) || lua_toboolean(L, 2),
                                    list ? &names : nullptr);
    if (!err.empty()) return luaL_error(L, "w3.anim.rest_bones: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// w3._terrain_raise(clipmap address, x, y, radius, metres) -> height map texels, collision pieces (terrain.cpp)
int LTerrainRaise(lua_State* L) {
    int texels = 0, pieces = 0;
    std::string err = TerrainRaise(reinterpret_cast<void*>(luaL_checkinteger(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                                   static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
                                   static_cast<float>(luaL_checknumber(L, 5)), &texels, &pieces);
    if (!err.empty()) return luaL_error(L, "w3.world.terrain_raise: %s", err.c_str());
    lua_pushinteger(L, texels);
    lua_pushinteger(L, pieces);
    return 2;
}

// w3._scene_time(player) -> t, length, section address; w3._scene_pause(player, on); w3._scene_seek(player, t)
int LSceneTime(lua_State* L) {
    float t = 0, len = 0;
    void* sec = nullptr;
    std::string err = SceneTime(CheckLive(L, 1)->ptr, &t, &len, &sec);
    if (!err.empty()) return luaL_error(L, "w3.scene.time: %s", err.c_str());
    lua_pushnumber(L, t);
    lua_pushnumber(L, len);
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(sec)));
    return 3;
}
// w3._scene_elements(player) -> {{index, start, duration, t, running}, ...}, the current element's index (1-based)
int LSceneElements(lua_State* L) {
    std::vector<SceneElement> els;
    int cur = -1;
    std::string err = SceneElements(CheckLive(L, 1)->ptr, &els, &cur);
    if (!err.empty()) return luaL_error(L, "w3.scene.elements: %s", err.c_str());
    lua_createtable(L, static_cast<int>(els.size()), 0);
    for (size_t i = 0; i < els.size(); ++i) {
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, els[i].index + 1); lua_setfield(L, -2, "index");
        lua_pushnumber(L, els[i].start); lua_setfield(L, -2, "start");
        lua_pushnumber(L, els[i].duration); lua_setfield(L, -2, "duration");
        lua_pushnumber(L, els[i].local); lua_setfield(L, -2, "t");
        lua_pushboolean(L, els[i].running); lua_setfield(L, -2, "running");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_pushinteger(L, cur + 1);
    return 2;
}
int LScenePause(lua_State* L) {
    std::string err = ScenePause(CheckLive(L, 1)->ptr, lua_toboolean(L, 2) != 0);
    if (!err.empty()) return luaL_error(L, "w3.scene.pause: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}
int LSceneSeek(lua_State* L) {
    std::string err = SceneSeek(CheckLive(L, 1)->ptr, static_cast<float>(luaL_checknumber(L, 2)));
    if (!err.empty()) return luaL_error(L, "w3.scene.seek: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

int LTerrainOverlay(lua_State* L) {  // w3._terrain_overlay(clipmap address [, width, height, rgba string]) -> true
    void* clip = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    std::string err;
    if (lua_isnoneornil(L, 2)) {
        err = TerrainOverlay(clip, 0, 0, nullptr);
    } else {
        uint32_t w = static_cast<uint32_t>(luaL_checkinteger(L, 2)), h = static_cast<uint32_t>(luaL_checkinteger(L, 3));
        size_t n = 0;
        const char* px = luaL_checklstring(L, 4, &n);
        if (n != static_cast<size_t>(w) * h * 4) return luaL_error(L, "the pixels need width * height * 4 bytes (RGBA)");
        err = TerrainOverlay(clip, w, h, reinterpret_cast<const uint8_t*>(px));
    }
    if (!err.empty()) return luaL_error(L, "%s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// --- dynamic decals (decal.cpp): lua_lib.inc's w3.decal makes each decal a table of these keys
float DecalNum(lua_State* L, int t, const char* key, float def) {
    lua_getfield(L, t, key);
    float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}
void DecalVec(lua_State* L, int t, const char* key, float* out, int n) {  // {X, Y, Z} or {x, y, z} or {1, 2, 3}
    lua_getfield(L, t, key);
    if (lua_istable(L, -1)) {
        static const char* const up[4] = {"X", "Y", "Z", "W"};
        static const char* const lo[4] = {"x", "y", "z", "w"};
        for (int i = 0; i < n; ++i) {
            lua_getfield(L, -1, up[i]);
            if (!lua_isnumber(L, -1)) {
                lua_pop(L, 1);
                lua_getfield(L, -1, lo[i]);
            }
            if (!lua_isnumber(L, -1)) {
                lua_pop(L, 1);
                lua_rawgeti(L, -1, i + 1);
            }
            if (lua_isnumber(L, -1)) out[i] = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}
int LDecalSpawn(lua_State* L) {  // w3._decal_spawn(list of decal tables [, entities]) -> ids (0: failed), first error
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<void*> ents;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        for (lua_Integer i = 1, n = luaL_len(L, 2); i <= n; ++i) {
            lua_rawgeti(L, 2, i);
            ents.push_back(CheckLive(L, -1)->ptr);
            lua_pop(L, 1);
        }
    }
    const lua_Integer n = luaL_len(L, 1);
    lua_createtable(L, static_cast<int>(n), 0);
    const int ids = lua_gettop(L);
    std::string first;
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        const int t = lua_gettop(L);
        luaL_checktype(L, t, LUA_TTABLE);
        DecalDesc d = DecalDefaults();
        std::string err;
        DecalVec(L, t, "pos", d.pos, 3);
        DecalVec(L, t, "dir", d.dir, 3);
        DecalVec(L, t, "up", d.up, 3);
        DecalVec(L, t, "atlas", d.uv, 4);
        d.width = DecalNum(L, t, "width", d.width);
        d.height = DecalNum(L, t, "height", d.height);
        d.depth = DecalNum(L, t, "depth", d.depth);
        lua_getfield(L, t, "color");
        if (lua_isinteger(L, -1)) d.color = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, t, "spec_color");
        if (lua_isinteger(L, -1)) d.specularColor = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        d.specularity = DecalNum(L, t, "specularity", d.specularity);
        d.specularScale = DecalNum(L, t, "spec_scale", d.specularScale);
        d.specularBase = DecalNum(L, t, "spec_base", d.specularBase);
        d.lifetime = DecalNum(L, t, "life", 0);
        d.fadeIn = DecalNum(L, t, "fade_in", 0);
        d.fadeOut = DecalNum(L, t, "fade_out", 0);
        d.projection = static_cast<int32_t>(DecalNum(L, t, "projection", 0));
        d.doubleSided = static_cast<int32_t>(DecalNum(L, t, "double_sided", 0));
        d.priority = static_cast<int32_t>(DecalNum(L, t, "priority", 0));
        d.staticOnly = static_cast<int32_t>(DecalNum(L, t, "static_only", 0));
        d.depthFadePower = DecalNum(L, t, "depth_fade", d.depthFadePower);
        d.normalFadeBias = DecalNum(L, t, "normal_fade_bias", d.normalFadeBias);
        d.normalFadeScale = DecalNum(L, t, "normal_fade_scale", d.normalFadeScale);
        d.autoHideDistance = DecalNum(L, t, "hide_distance", d.autoHideDistance);
        d.raw7c = static_cast<int32_t>(DecalNum(L, t, "_b7c", -1));
        d.raw7e = static_cast<int32_t>(DecalNum(L, t, "_b7e", -1));
        lua_getfield(L, t, "diffuse");
        const char* dp = lua_tostring(L, -1);
        d.diffuse = dp ? DecalTexture(dp, err) : nullptr;
        if (!dp) err = "no diffuse texture";
        lua_pop(L, 1);
        lua_getfield(L, t, "normal");
        const char* np = lua_tostring(L, -1);
        if (np && *np) d.normal = DecalTexture(np, err);
        lua_pop(L, 1);
        uint32_t id = 0;
        if (d.diffuse && (!np || !*np || d.normal))
            id = DecalSpawn(d, ents.empty() ? nullptr : ents.data(), static_cast<int>(ents.size()), err);
        if (!id && first.empty()) first = err;
        lua_pushinteger(L, id);
        lua_rawseti(L, ids, i);
        lua_settop(L, ids);
    }
    if (first.empty()) return 1;
    lua_pushstring(L, first.c_str());
    return 2;
}
int LDecalRemove(lua_State* L) {  // w3._decal_remove(id) -> true when it was one of ours
    lua_pushboolean(L, DecalRemove(static_cast<uint32_t>(luaL_checkinteger(L, 1))));
    return 1;
}
int LDecalClear(lua_State* L) {  // w3._decal_clear([entity]) -> how many
    lua_pushinteger(L, DecalClear(lua_isnoneornil(L, 1) ? nullptr : CheckLive(L, 1)->ptr));
    return 1;
}
int LDecalPerMesh(lua_State* L) {  // w3._decal_per_mesh([n]) -> the limit before (nil: not in this build)
    int r = DecalPerMesh(static_cast<int>(luaL_optinteger(L, 1, 0)));
    if (r < 0) lua_pushnil(L);
    else lua_pushinteger(L, r);
    return 1;
}
int LDecalCount(lua_State* L) {
    lua_pushinteger(L, DecalCount());
    return 1;
}
int LDecalColor(lua_State* L) {  // w3._decal_color(id, color, spec colour, specularity, spec scale, spec base) -> ok
    lua_pushboolean(L, DecalSetColor(static_cast<uint32_t>(luaL_checkinteger(L, 1)),
                                     static_cast<uint32_t>(luaL_checkinteger(L, 2)),
                                     static_cast<uint32_t>(luaL_checkinteger(L, 3)),
                                     static_cast<float>(luaL_checknumber(L, 4)),
                                     static_cast<float>(luaL_checknumber(L, 5)),
                                     static_cast<float>(luaL_checknumber(L, 6))));
    return 1;
}

// --- network (J4): lua_lib.inc's w3.http / w3.net keep the callbacks and empty the queue each tick
int LNetHttp(lua_State* L) {  // w3._net_http(method, url, headers, body, timeout ms) -> id
    lua_pushinteger(L, NetHttp(luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_optstring(L, 3, ""),
                               lua_isnoneornil(L, 4) ? std::string() : std::string(luaL_checkstring(L, 4), luaL_len(L, 4)),
                               static_cast<int>(luaL_optinteger(L, 5, 30000))));
    return 1;
}
int LNetConnect(lua_State* L) {  // w3._net_connect(host, port) -> id (the "connect" event says how it went)
    int id = NetConnect(luaL_checkstring(L, 1), static_cast<int>(luaL_checkinteger(L, 2)));
    if (!id) return luaL_error(L, "no network");
    lua_pushinteger(L, id);
    return 1;
}
int LNetUdp(lua_State* L) {  // w3._net_udp(port) -> id, or nil and the reason
    std::string err;
    int id = NetUdp(static_cast<int>(luaL_optinteger(L, 1, 0)), err);
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}
int LNetSend(lua_State* L) {  // w3._net_send(id, data [, host, port]) -> true, or nil and the reason
    size_t n = 0;
    const char* d = luaL_checklstring(L, 2, &n);
    const char* host = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
    std::string err = NetSend(static_cast<int>(luaL_checkinteger(L, 1)), std::string(d, n), host,
                              static_cast<int>(luaL_optinteger(L, 4, 0)));
    if (!err.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
int LNetClose(lua_State* L) {
    NetClose(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}
int LNetPoll(lua_State* L) {  // w3._net_poll() -> {{id, kind, status, body, headers, error, host, port}, ...}
    std::vector<NetEvent> evs = NetPoll();
    lua_createtable(L, static_cast<int>(evs.size()), 0);
    for (size_t i = 0; i < evs.size(); ++i) {
        const NetEvent& e = evs[i];
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, e.id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, e.kind.c_str());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, e.status);
        lua_setfield(L, -2, "status");
        lua_pushlstring(L, e.body.data(), e.body.size());
        lua_setfield(L, -2, "body");
        lua_pushstring(L, e.headers.c_str());
        lua_setfield(L, -2, "headers");
        if (!e.error.empty()) {
            lua_pushstring(L, e.error.c_str());
            lua_setfield(L, -2, "error");
        }
        if (!e.host.empty()) {
            lua_pushstring(L, e.host.c_str());
            lua_setfield(L, -2, "host");
            lua_pushinteger(L, e.port);
            lua_setfield(L, -2, "port");
        }
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int LReport(lua_State* L) {  // w3.report(): the bug report as text (also in tw3se\out\report.txt)
    std::string r = BuildReport();
    lua_pushlstring(L, r.data(), r.size());
    return 1;
}

int LConfig(lua_State* L) {  // w3._config(group, name [, value]) -> the value as text, or nil and the reason
    std::string out, err;
    std::string value;
    const char* v = nullptr;
    if (!lua_isnoneornil(L, 3)) {
        value = lua_isboolean(L, 3) ? (lua_toboolean(L, 3) ? "true" : "false") : luaL_tolstring(L, 3, nullptr);
        v = value.c_str();
    }
    if (!EngineConfig(luaL_checkstring(L, 1), luaL_checkstring(L, 2), v, out, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushstring(L, out.c_str());
    return 1;
}
int LConfigList(lua_State* L) {  // w3._config_list([filter]) -> {"group/name\ttype", ...}
    std::vector<std::string> all = EngineConfigList(luaL_optstring(L, 1, ""));
    lua_createtable(L, static_cast<int>(all.size()), 0);
    for (size_t i = 0; i < all.size(); ++i) {
        lua_pushstring(L, all[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int LTypeName(lua_State* L) {  // w3._typename(address): the C++ type of the object there (RTTI), nil if none
    std::string n = ObjectTypeName(reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))));
    if (n.empty()) return 0;
    lua_pushstring(L, n.c_str());
    return 1;
}
int LPxScene(lua_State* L) {  // w3._px_scene(world address) -> the address of its PhysX scene, nil if none
    void* s = PhysxScene(reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))));
    if (!s) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(s)));
    return 1;
}
int LPxGravity(lua_State* L) {  // w3._px_gravity(scene [, x, y, z]) -> x, y, z (after setting)
    void* scene = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    bool set = !lua_isnoneornil(L, 2);
    float v[3] = {static_cast<float>(luaL_optnumber(L, 2, 0)), static_cast<float>(luaL_optnumber(L, 3, 0)),
                  static_cast<float>(luaL_optnumber(L, 4, 0))};
    for (float f : v)  // (PhysX takes NaN and 1e15 as they come, every body then flies off)
        if (set && !(f >= -1000.f && f <= 1000.f)) return luaL_error(L, "gravity: each axis -1000 to 1000 m/s^2");
    std::string err = PhysxGravity(scene, v, set);
    if (!err.empty()) return luaL_error(L, "%s", err.c_str());
    for (float f : v) lua_pushnumber(L, f);
    return 3;
}

int LDir(lua_State* L) {  // w3.dir(): TW3SE's folder (bin\x64_dx12\tw3se), UTF-8, for io.open
    const std::wstring& d = Dir();
    int n = WideCharToMultiByte(CP_UTF8, 0, d.c_str(), static_cast<int>(d.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, d.c_str(), static_cast<int>(d.size()), s.data(), n, nullptr, nullptr);
    lua_pushstring(L, s.c_str());
    return 1;
}

// w3.object(address [, className]): the object at a raw address (a vhook argument), nil if none. With a class: seen
// as that class (an object that reports only IScriptable, like the options menu's CInGameConfigWrapper).
int LObjectAt(lua_State* L) {
    void* cls = nullptr;
    if (lua_isstring(L, 2)) {
        cls = FindClassByName(lua_tostring(L, 2));
        if (!cls) return luaL_error(L, "w3.object: no class %s", lua_tostring(L, 2));
    }
    PushObject(L, reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1))), cls, false);
    return 1;
}

int LPoke(lua_State* L) {  // write the 8-byte integer value at base+offset; returns true on success (SEH-guarded)
    uintptr_t base = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
    lua_Integer off = luaL_checkinteger(L, 2);
    uint64_t v = static_cast<uint64_t>(luaL_checkinteger(L, 3));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(base + static_cast<uintptr_t>(off)), &v, 8));
    return 1;
}

int LPoseSelftest(lua_State* L) {  // w3.pose_selftest(componentPtr, boneIndex): twist a bone (comp=1 = broad, 0 = off)
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    int bone = static_cast<int>(luaL_checkinteger(L, 2));
    PoseSelftestTarget(comp, bone);
    return 0;
}

int LPoseMapdump(lua_State* L) {  // w3.pose_mapdump(componentPtr): log that component's A+0x18 remap once
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    PoseMapTarget(comp);
    return 0;
}

int LPoseWave(lua_State* L) {  // w3.pose_wave(componentPtr, bone, sigIdx, sigVal, on): oscillate a bone per frame
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    int bone = static_cast<int>(luaL_checkinteger(L, 2));
    int sigIdx = static_cast<int>(luaL_checkinteger(L, 3));
    int sigVal = static_cast<int>(luaL_checkinteger(L, 4));
    int on = lua_isnoneornil(L, 5) ? 1 : lua_toboolean(L, 5);
    PoseWave(comp, bone, sigIdx, sigVal, on);
    return 0;
}

int LPoseRestore(lua_State* L) {  // w3.pose_restore(componentPtr, bone, sigIdx, sigVal): write saved original back
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    int bone = static_cast<int>(luaL_checkinteger(L, 2));
    int sigIdx = static_cast<int>(luaL_checkinteger(L, 3));
    int sigVal = static_cast<int>(luaL_checkinteger(L, 4));
    PoseRestore(comp, bone, sigIdx, sigVal);
    return 0;
}

int LPoseMeshlist(lua_State* L) {  // w3.pose_meshlist(componentPtr, on): log each distinct mesh (combine) once
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    int on = lua_isnoneornil(L, 2) ? 1 : lua_toboolean(L, 2);
    PoseMeshList(comp, on);
    return 0;
}

int LPoseSelftestSig(lua_State* L) {  // w3.pose_selftest_sig(componentPtr, boneIndex, sigIdx, sigVal): twist one submesh
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    int bone = static_cast<int>(luaL_checkinteger(L, 2));
    int sigIdx = static_cast<int>(luaL_checkinteger(L, 3));
    int sigVal = static_cast<int>(luaL_checkinteger(L, 4));
    PoseSelftestSig(comp, bone, sigIdx, sigVal);
    return 0;
}

// w3.pose(entity): the whole skeleton in model space, from the last frame: { count, modelToWorld = {16},
// bones = { {name, parent, x, y, z}, ... } } (parent 0-based, -1 = root; x y z = the bone's position in the figure,
// metres). The first call starts watching the figure and returns nil; from the next frame on it has a pose.
// The figures w3.pose watches, with the time it last asked. A figure nobody asked about for 10 s is no longer watched
// (the combine hook works for each watched figure every frame).
std::unordered_map<void*, ULONGLONG> g_luaPoseAsked;

void ExpirePoseWatches() {  // the Lua tick
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next || g_luaPoseAsked.empty()) return;
    next = now + 1000;
    for (auto it = g_luaPoseAsked.begin(); it != g_luaPoseAsked.end();) {
        if (now - it->second < 10000) { ++it; continue; }
        PoseWatchBy(it->first, 0, 2);
        it = g_luaPoseAsked.erase(it);
    }
}

int LPoseStop(lua_State* L) {  // w3.pose_stop(entity): w3.pose stops watching the figure now
    Obj* o = CheckObj(L, 1);
    if (g_luaPoseAsked.erase(o->ptr)) PoseWatchBy(o->ptr, 0, 2);
    return 0;
}

// w3.face.set(actor, {track = weight, ...} [, "add"]): the face's weights by track name, written into every frame from
// now on (posehook.cpp, FaceSet); "add" puts them on top of the game's (a scene, lip sync), else they stand in its
// place. An empty table takes them off, as w3.face.clear(actor) does. w3.face.tracks(actor): the track names, once
// the face has been drawn (the first call starts watching it).
int LFaceSet(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const char* mode = luaL_optstring(L, 3, "set");
    bool add = strcmp(mode, "add") == 0;
    if (!add && strcmp(mode, "set") != 0) return luaL_argerror(L, 3, "\"set\" or \"add\"");
    std::vector<std::pair<std::string, float>> weights;
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) != LUA_TSTRING || !lua_isnumber(L, -1)) {
            lua_pop(L, 2);
            return luaL_argerror(L, 2, "track names with numbers ({jaw_open = 0.5})");
        }
        double v = lua_tonumber(L, -1);
        if (!std::isfinite(v) || std::fabs(v) > 100) {
            lua_pop(L, 2);
            return luaL_argerror(L, 2, "a weight between -100 and 100");
        }
        weights.emplace_back(lua_tostring(L, -2), static_cast<float>(v));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, FaceSet(o->ptr, weights, add, g_curMod) != 0);
    return 1;
}
int LFaceClear(lua_State* L) {
    lua_pushboolean(L, FaceClear(CheckLive(L, 1)->ptr) != 0);
    return 1;
}
int LFaceTracks(lua_State* L) {
    std::vector<std::string> names = FaceTracks(CheckLive(L, 1)->ptr);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int LPose(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    bool component = false;  // a component: its own pose (GetComponentPose; a dangle's rig too), no watch needed
    for (void* c = o->cls; Plausible(c) && !component; c = Rd<void*>(c, 0x10)) component = RttiName(c) == "CComponent";
    if (component) {
        TW3SE_Pose p{};
        if (!PoseGetComponent(o->ptr, &p)) {
            lua_pushnil(L);
            return 1;
        }
        lua_newtable(L);
        lua_pushinteger(L, p.boneCount);
        lua_setfield(L, -2, "count");
        lua_newtable(L);
        for (int i = 0; i < p.boneCount; ++i) {
            const float* m = p.local + static_cast<size_t>(i) * p.localStride;
            lua_newtable(L);
            lua_pushstring(L, p.boneNames ? p.boneNames[i] : "");
            lua_setfield(L, -2, "name");
            lua_pushinteger(L, p.parents[i]);
            lua_setfield(L, -2, "parent");
            lua_pushnumber(L, m[12]);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, m[13]);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, m[14]);
            lua_setfield(L, -2, "z");
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "bones");
        return 1;
    }
    const char* kindName = luaL_optstring(L, 2, "body");
    int kind = strcmp(kindName, "mimic") == 0 ? TW3SE_POSE_MIMIC : TW3SE_POSE_BODY;
    if (kind == TW3SE_POSE_BODY && strcmp(kindName, "body") != 0) return luaL_argerror(L, 2, "\"body\" or \"mimic\"");
    PoseWatchBy(o->ptr, 1, 2);  // idempotent: watched while w3.pose keeps asking
    g_luaPoseAsked[o->ptr] = GetTickCount64();
    TW3SE_Pose p{};
    if (!PoseGet(o->ptr, kind, &p)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, p.boneCount);
    lua_setfield(L, -2, "count");
    lua_newtable(L);
    for (int i = 0; i < 16; ++i) {
        lua_pushnumber(L, p.modelToWorld[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "modelToWorld");
    lua_newtable(L);
    for (int i = 0; i < p.boneCount; ++i) {
        const float* m = p.local + static_cast<size_t>(i) * p.localStride;
        lua_newtable(L);
        lua_pushstring(L, p.boneNames ? p.boneNames[i] : "");
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, p.parents[i]);
        lua_setfield(L, -2, "parent");
        lua_pushnumber(L, m[12]);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, m[13]);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, m[14]);
        lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "bones");
    return 1;
}

// w3._view_rect(x, y, w, h) or (false): the game's picture into that rectangle. w3._view_rect() -> x, y, w, h of
// the picture as drawn (its own aspect), screen width and height; or nil, width, height when it is off
int LViewRect(lua_State* L) {
    if (lua_gettop(L) >= 1) {
        if (lua_isboolean(L, 1) && !lua_toboolean(L, 1)) {
            OverlayViewRect(false, 0, 0, 0, 0);
        } else {
            float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
            float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
            if (!(w >= 2 && h >= 2) || !std::isfinite(x) || !std::isfinite(y)) return luaL_argerror(L, 3, "a rectangle of at least 2 x 2 pixels");
            OverlayViewRect(true, x, y, w, h);
        }
    }
    float x = 0, y = 0, w = 0, h = 0, sw = 0, sh = 0;
    bool on = OverlayViewGet(&x, &y, &w, &h, &sw, &sh);
    if (!on) {
        lua_pushnil(L);
        lua_pushnumber(L, sw);
        lua_pushnumber(L, sh);
        return 3;
    }
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    lua_pushnumber(L, sw);
    lua_pushnumber(L, sh);
    return 6;
}

// w3._anim_sample(component, name, t) -> a pose like w3.pose (with modelToWorld and duration), or nil and why;
// nil and "" while the animation's data is not in memory yet
int LAnimSample(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float t = static_cast<float>(luaL_checknumber(L, 3));
    TW3SE_Pose p{};
    float duration = 0;
    std::string err;
    if (!PoseSample(o->ptr, name, t, &p, &duration, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, p.boneCount);
    lua_setfield(L, -2, "count");
    lua_pushnumber(L, duration);
    lua_setfield(L, -2, "duration");
    lua_newtable(L);
    for (int i = 0; i < 16; ++i) {
        lua_pushnumber(L, p.modelToWorld[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "modelToWorld");
    lua_newtable(L);
    for (int i = 0; i < p.boneCount; ++i) {
        const float* m = p.local + static_cast<size_t>(i) * p.localStride;
        lua_newtable(L);
        lua_pushstring(L, p.boneNames ? p.boneNames[i] : "");
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, p.parents[i]);
        lua_setfield(L, -2, "parent");
        lua_pushnumber(L, m[12]);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, m[13]);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, m[14]);
        lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "bones");
    return 1;
}
// w3._anim_pose(component, {{name, t, weight}, ...}): laid on the component every frame; {} takes it off
// w3._anim_create(component, name, fps, {bone names}, raw): raw = frames x #bones x 10 float32 (position xyz,
// rotation xyzw, scale xyz, each bone local to its parent) -> how many of the bones the figure's rig has
int LAnimCreate(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float fps = static_cast<float>(luaL_checknumber(L, 3));
    luaL_checktype(L, 4, LUA_TTABLE);
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 5, &len);
    std::vector<std::string> bones;
    for (lua_Integer i = 1, n = luaL_len(L, 4); i <= n; ++i) {
        lua_rawgeti(L, 4, i);
        const char* b = lua_tostring(L, -1);
        bones.emplace_back(b ? b : "");
        lua_pop(L, 1);
    }
    size_t perFrame = bones.size() * 10 * 4;
    if (bones.empty() || len == 0 || len % perFrame) return luaL_error(L, "w3.anim.create: the data is not frames x %d bones x 10 floats", static_cast<int>(bones.size()));
    std::vector<float> data(len / 4);
    memcpy(data.data(), raw, len);
    uint32_t mapped = 0;
    std::string err = AnimCreate(o->ptr, name, fps, bones, data.data(), static_cast<uint32_t>(len / perFrame), &mapped);
    if (!err.empty()) return luaL_error(L, "w3.anim.create: %s", err.c_str());
    lua_pushinteger(L, mapped);
    return 1;
}

// w3._anim_publish(set, component, name): a made animation into a new CSkeletalAnimationSet and the engine's table of
// animation names. The set keeps one reference for good: its entries point at the made animation.
int LAnimPublish(lua_State* L) {
    Obj* set = CheckLive(L, 1);
    Obj* comp = CheckLive(L, 2);
    const char* name = luaL_checkstring(L, 3);
    void* hb = BlockOf(set->ptr);
    if (!hb) return luaL_error(L, "w3.anim.create: the set has no handle");
    std::string err = AnimPublish(set->ptr, comp->ptr, name);
    if (!err.empty()) return luaL_error(L, "w3.anim.create: %s", err.c_str());
    InterlockedIncrement(static_cast<volatile LONG*>(hb));
    lua_pushboolean(L, 1);
    return 1;
}

// w3._anim_shape(component, {bone = {scale = {x, y, z} or s, offset = {x, y, z}, rot = {x, y, z, w}}, ...})
int LAnimBones(lua_State* L) {  // w3._anim_bones(component) -> {names in rig order}, or nil and the reason
    Obj* o = CheckLive(L, 1);
    std::vector<std::string> names;
    std::string err = AnimBoneNames(o->ptr, &names);
    if (!err.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
// w3._anim_reach(component, hand "l" | "r" | nil, x, y, z, weight, ahead, out) -> true (posehook.cpp, AnimReachSet)
int LAnimReach(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    const char* hand = luaL_optstring(L, 2, "");
    float t[3] = {static_cast<float>(luaL_optnumber(L, 3, 0)), static_cast<float>(luaL_optnumber(L, 4, 0)),
                  static_cast<float>(luaL_optnumber(L, 5, 0))};
    std::string err = AnimReachSet(o->ptr, hand[0], t, static_cast<float>(luaL_optnumber(L, 6, 0)),
                                   static_cast<float>(luaL_optnumber(L, 7, 0.10)), static_cast<float>(luaL_optnumber(L, 8, 0.05)));
    if (!err.empty()) return luaL_error(L, "w3.anim.reach: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}
int LAnimShape(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::pair<std::string, std::array<float, 12>>> bones;
    auto vec = [&](int t, const char* key, float* out, int n) {  // a field: a number for every part or {x, y, ...}
        lua_getfield(L, t, key);
        if (lua_isnumber(L, -1)) {
            for (int k = 0; k < n; ++k) out[k] = static_cast<float>(lua_tonumber(L, -1));
        } else if (lua_istable(L, -1)) {
            for (int k = 0; k < n; ++k) {
                lua_rawgeti(L, -1, k + 1);
                if (lua_isnumber(L, -1)) out[k] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    };
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
            std::array<float, 12> v = {1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0};
            int t = lua_gettop(L);
            vec(t, "scale", &v[0], 3);
            vec(t, "offset", &v[3], 3);
            vec(t, "length", &v[10], 1);
            lua_getfield(L, t, "inherit");
            lua_getfield(L, t, "only");
            v[11] = lua_toboolean(L, -2) ? 1.0f : lua_toboolean(L, -1) ? 2.0f : 0.0f;
            lua_pop(L, 2);
            float r[4] = {0, 0, 0, 1};
            vec(t, "rot", r, 4);
            float len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
            for (int k = 0; k < 4; ++k) v[6 + k] = len > 1e-6f ? r[k] / len : (k == 3 ? 1.0f : 0.0f);
            bones.emplace_back(lua_tostring(L, -2), v);
        }
        lua_pop(L, 1);
    }
    std::string err = AnimShapeSet(o->ptr, bones);
    if (!err.empty()) return luaL_error(L, "w3.anim.shape: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// w3._anim_record(component, maxSeconds) / w3._anim_record_stop(component, fps) -> {fps, bones, raw}
int LAnimRecord(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    std::string err = AnimRecordStart(o->ptr, static_cast<float>(luaL_optnumber(L, 2, 60)));
    if (!err.empty()) return luaL_error(L, "w3.anim.record: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}
int LAnimRecordStop(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    float fps = static_cast<float>(luaL_optnumber(L, 2, 30));
    std::vector<std::string> bones;
    std::vector<float> data;
    std::string err = AnimRecordStop(o->ptr, fps, &bones, &data);
    if (!err.empty()) return luaL_error(L, "w3.anim.record_stop: %s", err.c_str());
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, fps);
    lua_setfield(L, -2, "fps");
    lua_createtable(L, static_cast<int>(bones.size()), 0);
    for (size_t i = 0; i < bones.size(); ++i) {
        lua_pushstring(L, bones[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    lua_setfield(L, -2, "bones");
    lua_pushlstring(L, reinterpret_cast<const char*>(data.data()), data.size() * 4);
    lua_setfield(L, -2, "raw");
    return 1;
}

int LAnimPose(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::tuple<std::string, float, float>> layers;
    lua_Integer n = luaL_len(L, 2);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        if (!lua_istable(L, -1)) return luaL_argerror(L, 2, "layers {name, t, weight}");
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        lua_rawgeti(L, -3, 3);
        const char* name = lua_tostring(L, -3);
        double t = lua_isnumber(L, -2) ? lua_tonumber(L, -2) : 0;
        double w = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 1;
        if (!name || !std::isfinite(t) || !std::isfinite(w)) return luaL_argerror(L, 2, "layers {name, t, weight}");
        layers.emplace_back(name, static_cast<float>(t), static_cast<float>(w));
        lua_pop(L, 4);
    }
    std::string err = PoseLayersSet(o->ptr, layers);
    if (!err.empty()) return luaL_error(L, "w3.anim.pose: %s", err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

int LPoseWatch(lua_State* L) {  // w3.pose_watch(componentPtr, on): watch a component so the pose callback fires
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(luaL_checkinteger(L, 1)));
    int on = lua_isnoneornil(L, 2) ? 1 : lua_toboolean(L, 2);
    lua_pushinteger(L, PoseWatch(comp, on));
    return 1;
}

// --- overlay drawing. The overlay draw callback runs on the render thread (overlay.cpp), so Lua must not run
// there. Instead the on_draw functions run on the main thread each tick and fill a command list; the render
// thread replays the last finished list. Two buffers with a lock, swapped when the main thread is done.
struct DrawCmd {
    int kind;  // 0 text, 1 rect
    float a, b, c, d;
    uint32_t rgba;
    std::string text;
};
bool PCall(lua_State* L, int nargs, int nres, std::string& err);  // defined below
std::vector<DrawCmd> g_drawBuild, g_drawShow;
std::mutex g_drawMx;
bool g_drawing = false;      // true while an on_draw function runs (so w3.draw.* is allowed)
bool g_drawHooked = false;   // the render-thread replay is registered once

void DrawReplay(void*) {  // render thread
    std::lock_guard<std::mutex> l(g_drawMx);
    for (const DrawCmd& c : g_drawShow) {
        if (c.kind == 0) OverlayApiText(c.a, c.b, c.rgba, c.text.c_str());
        else OverlayApiRect(c.a, c.b, c.c, c.d, c.rgba);
    }
}

int LDrawText(lua_State* L) {
    if (!g_drawing) return luaL_error(L, "w3.draw.text only inside an on_draw function");
    DrawCmd c{0, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)), 0, 0,
              static_cast<uint32_t>(luaL_checkinteger(L, 3)), luaL_checkstring(L, 4)};
    g_drawBuild.push_back(std::move(c));
    return 0;
}

int LDrawSize(lua_State* L) {  // w3.draw.size(text) -> w, h of w3.draw.text in pixels (0, 0 before the first frame)
    float w = 0, h = 0;
    OverlayTextSize(luaL_checkstring(L, 1), &w, &h);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}

int LDrawRect(lua_State* L) {
    if (!g_drawing) return luaL_error(L, "w3.draw.rect only inside an on_draw function");
    g_drawBuild.push_back({1, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                           static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
                           static_cast<uint32_t>(luaL_checkinteger(L, 5)), ""});
    return 0;
}

int LScreenSize(lua_State* L) {
    float w = 0, h = 0;
    OverlayApiScreenSize(&w, &h);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}

// --- function lists with owners (I5: a Lua mod reloads when its files change). on_tick, on_draw, on_ui and
// on_reload keep their functions in registry tables ("tw3se.ticks" ...). Each entry remembers the mod that added it
// ("tw3se.owner.<table>": index -> mod key), so a mod's functions can be taken out before it runs again. A function
// added while a mod loads, or while one of its functions runs, belongs to that mod.


void OwnerTable(lua_State* L, const char* list) {  // pushes "tw3se.owner.<list>" (made on first use)
    std::string key = std::string("tw3se.owner.") + list;
    lua_getfield(L, LUA_REGISTRYINDEX, key.c_str());
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, key.c_str());
}

// Adds the function at fnIdx to the list, owned by the current mod. Returns its index (a handle).
lua_Integer AddFn(lua_State* L, const char* list, int fnIdx) {
    fnIdx = lua_absindex(L, fnIdx);
    lua_getfield(L, LUA_REGISTRYINDEX, list);
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1)) + 1;  // a border: n is free
    lua_pushvalue(L, fnIdx);
    lua_rawseti(L, -2, n);
    lua_pop(L, 1);
    if (!g_curMod.empty()) {
        OwnerTable(L, list);
        lua_pushstring(L, g_curMod.c_str());
        lua_rawseti(L, -2, n);
        lua_pop(L, 1);
    }
    return n;
}

void RemoveFn(lua_State* L, const char* list, lua_Integer i) {
    lua_getfield(L, LUA_REGISTRYINDEX, list);
    lua_pushnil(L);
    lua_rawseti(L, -2, i);
    lua_pop(L, 1);
    OwnerTable(L, list);
    lua_pushnil(L);
    lua_rawseti(L, -2, i);
    lua_pop(L, 1);
}

// Calls every function of the list in index order (holes from removed ones are fine), each as its owner. A function
// that raises an error is removed and logged. `before` runs before each call (ImGui's error recovery for on_ui).
template <class Before, class After>
void RunFns(lua_State* L, const char* list, const char* what, Before before, After after) {
    int top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, list);
    int t = lua_gettop(L);
    OwnerTable(L, list);
    int o = lua_gettop(L);
    std::vector<lua_Integer> keys;
    lua_pushnil(L);
    while (lua_next(L, t)) {
        if (lua_isinteger(L, -2)) keys.push_back(lua_tointeger(L, -2));
        lua_pop(L, 1);
    }
    std::sort(keys.begin(), keys.end());
    for (lua_Integer i : keys) {
        lua_rawgeti(L, t, i);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_rawgeti(L, o, i);
        std::string owner = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        ModScope scope(owner);
        before();
        std::string err;
        bool ok;
        {
            ProfScope prof(owner, what);
            ok = PCall(L, 0, 0, err);
        }
        if (!ok) {
            after(false);
            Log("lua: %s function %lld%s%s removed: %s", what, static_cast<long long>(i), owner.empty() ? "" : " of ",
                owner.c_str(), err.c_str());
            lua_pushnil(L);
            lua_rawseti(L, t, i);
            lua_pushnil(L);
            lua_rawseti(L, o, i);
        } else {
            after(true);
        }
    }
    lua_settop(L, top);
}
template <class Before> void RunFns(lua_State* L, const char* list, const char* what, Before before) {
    RunFns(L, list, what, before, [](bool) {});
}
void RunFns(lua_State* L, const char* list, const char* what) { RunFns(L, list, what, [] {}); }

// Takes every function of the mod out of the list. Returns how many.
int RemoveModFns(lua_State* L, const char* list, const std::string& mod) {
    OwnerTable(L, list);
    int o = lua_gettop(L);
    std::vector<lua_Integer> mine;
    lua_pushnil(L);
    while (lua_next(L, o)) {
        if (lua_isinteger(L, -2) && lua_isstring(L, -1) && mod == lua_tostring(L, -1)) mine.push_back(lua_tointeger(L, -2));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    for (lua_Integer i : mine) RemoveFn(L, list, i);
    return static_cast<int>(mine.size());
}

int LOnDraw(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!g_drawHooked) {  // turn the overlay on and register the render-thread replay, once
        g_drawHooked = true;
        InstallOverlay(true);
        RegisterOverlayDraw(&DrawReplay, nullptr);
    }
    lua_pushinteger(L, AddFn(L, "tw3se.draws", 1));
    return 1;
}

void DrawNotice();  // below, with the errors a player sees

// Runs the on_draw functions on the main thread and swaps the finished command list in for the render thread.
void BuildDraws(lua_State* L) {
    g_drawBuild.clear();
    g_drawing = true;
    RunFns(L, "tw3se.draws", "on_draw");
    g_drawing = false;
    DrawNotice();
    std::lock_guard<std::mutex> l(g_drawMx);
    g_drawShow.swap(g_drawBuild);
}


// w3.new(className [, parent]): a new object of a scripted class, as `new <className> in <parent>` makes it.
// parent defaults to theGame. The compiler writes `new` as 0x26, the CClass* (8 bytes), then the parent expression
// (measured with tests/modTw3seNewProbe: thePlayer = 0x66, theGame = 0x65). Here the parent comes from Lua
// through a nested TW3SE_LuaArg call, and the whole expression is the argument of TW3SE_LuaSink, which writes the
// new object's handle into the result. The object has its fields' default values; it lives while something holds
// it (a Lua variable, a field it is stored in).
int LNew(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* cls = FindClassByName(name);
    if (!cls || RttiKind(cls) != 2) return luaL_error(L, "w3.new: no class %s", name);
    if (lua_isnoneornil(L, 2)) {
        lua_settop(L, 1);
        PushGlobal(L, ScriptGlobalOffsetOf("theGame"));
    }
    void* handleType = RttiFindType("handle:CObject");
    if (!handleType) return luaL_error(L, "w3.new: the type handle:CObject is not registered");
    std::vector<uint8_t> bytes;
    std::string e = EncodeArg(L, 2, handleType, bytes);
    void* argFn = e.empty() ? PrepareArgFn(0, handleType, e) : nullptr;
    if (!argFn) {
        ReleasePendingArgRefs();
        RestoreArgFns();
        return luaL_error(L, "w3.new: the parent: %s", e.c_str());
    }
    void* sink = OurNative("TW3SE_LuaSink");
    if (!sink) {
        ReleasePendingArgRefs();
        RestoreArgFns();
        return luaL_error(L, "w3.new needs TW3SE_LuaSink (natives off?)");
    }
    g_slots[0] = bytes;
    std::vector<uint8_t> bc;
    auto put = [&](const void* p, size_t n) { bc.insert(bc.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    bc.push_back(0x26);  // new
    put(&cls, 8);
    bc.push_back(0x1c);  // the parent: TW3SE_LuaArg0(0)
    uint16_t follow = 2 + 8 + 5 + 1, line = 0;
    put(&follow, 2);
    put(&line, 2);
    put(&argFn, 8);
    bc.push_back(0x04);
    int32_t slot = 0;
    put(&slot, 4);
    bc.push_back(0x20);
    bc.push_back(0x20);  // end of the sink's arguments
    alignas(16) uint8_t result[64] = {};
    bool faulted = false;
    bool called = InvokeNative(sink, nullptr, bc.data(), result, &faulted, g_localsBase);
    if (called || faulted) g_argRefs.clear();  // the parent's ref went to the call
    ReleasePendingArgRefs();
    RestoreArgFns();
    if (faulted) return luaL_error(L, "w3.new %s faulted inside the game (caught)", name);
    void* block = Rd<void*>(result, 0);
    if (!called || !Plausible(block) || !Rd<void*>(block, 0x08))
        return luaL_error(L, "w3.new %s: the game made no object (an abstract or native-only class?)", name);
    PushObject(L, Rd<void*>(block, 0x08), nullptr, false, block);
    ReleaseRef(block);           // the result slot's ref: the Lua value holds its own
    g_newBlocks.insert(block);   // once nothing holds it any more, Lua lets the game discard it
    return 1;
}

int LOnUi(lua_State* L) {  // w3.on_ui(fn): fn runs every frame while the in-game UI is open
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushinteger(L, AddFn(L, "tw3se.ui", 1));
    return 1;
}
int LOffUi(lua_State* L) {  // w3.off_ui(handle): the function from w3.on_ui stops
    RemoveFn(L, "tw3se.ui", luaL_checkinteger(L, 1));
    return 0;
}

int LOnTick(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushinteger(L, AddFn(L, "tw3se.ticks", 1));
    return 1;
}

int LDlcScans(lua_State* L) {  // w3.dlc_scans() -> post-mount DLC registrations done, one still pending (a live DLC)
    lua_pushinteger(L, DLCScansDone());
    lua_pushboolean(L, DLCScanPending());
    return 2;
}
int LTime(lua_State* L) {  // w3.time(): seconds since the start of Windows, for timers that span frames
    lua_pushnumber(L, static_cast<double>(GetTickCount64()) / 1000.0);
    return 1;
}
int LModsList(lua_State* L);     // F2's mod manager, below the Lua mods
int LPluginsList(lua_State* L);
int LVarGet(lua_State* L);
int LVarSet(lua_State* L);
int LListDir(lua_State* L);
int LTw3seInfo(lua_State* L);
int LMenuKey(lua_State* L);
int LKeyCapture(lua_State* L);

int LCurMod(lua_State* L) {  // w3._mod(): the mod loading or running now ("" = none)
    lua_pushstring(L, g_curMod.c_str());
    return 1;
}

int LMessageTake(lua_State* L) {  // w3._message_take() -> {{channel, text}, ...} since the last call (messages.cpp)
    auto list = MessageTakeForLua();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    int i = 0;
    for (const auto& m : list) {
        lua_createtable(L, 2, 0);
        lua_pushlstring(L, m.first.data(), m.first.size());
        lua_rawseti(L, -2, 1);
        lua_pushlstring(L, m.second.data(), m.second.size());
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}
int LTw3seConfig(lua_State* L) {  // w3._tw3se_config(key, default): a value from tw3se.config
    lua_pushstring(L, ConfigGet(luaL_checkstring(L, 1), luaL_optstring(L, 2, "")).c_str());
    return 1;
}
int LBackgroundFps(lua_State* L) {  // w3.background_fps([fps]) -> the frame cap behind another window (0: none)
    if (!lua_isnoneornil(L, 1)) SetBackgroundFps(static_cast<int>(luaL_checkinteger(L, 1)));
    lua_pushinteger(L, GetBackgroundFps());
    return 1;
}
int LMessageSend(lua_State* L) {  // w3._message_send(channel, text)
    size_t n = 0;
    const char* text = luaL_optlstring(L, 2, "", &n);
    MessageSend(luaL_checkstring(L, 1), std::string(text, n).c_str());
    return 0;
}
int LOffTick(lua_State* L) {  // w3.off_tick(handle): the function from w3.on_tick stops
    RemoveFn(L, "tw3se.ticks", luaL_checkinteger(L, 1));
    return 0;
}
int LOffDraw(lua_State* L) {  // w3.off_draw(handle): the function from w3.on_draw stops
    RemoveFn(L, "tw3se.draws", luaL_checkinteger(L, 1));
    return 0;
}

// --- keys: a key by name ("F5", "Ctrl+Shift+K", "Mouse4", "0x70"), read from Windows each tick. A key counts
// only while the game has the focus and the in-game console is closed.
constexpr int kCtrl = 1, kShift = 2, kAlt = 4;

constexpr int kPad = 0x100;  // a controller button: kPad + its bit (pad.cpp), as a modifier its bit << 8 in mods

int KeyCode(std::string n) {
    for (char& ch : n) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (int b = PadBit(n); b >= 0) return kPad + b;
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return 'A' + (n[0] - 'a');
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return n[0];
    if (n.size() >= 2 && n[0] == 'f' && isdigit(static_cast<unsigned char>(n[1]))) {
        int f = atoi(n.c_str() + 1);
        return (f >= 1 && f <= 24) ? VK_F1 + f - 1 : 0;
    }
    if (n.rfind("num", 0) == 0 && n.size() == 4 && isdigit(static_cast<unsigned char>(n[3]))) return VK_NUMPAD0 + (n[3] - '0');
    if (n.rfind("0x", 0) == 0) {
        long v = strtol(n.c_str(), nullptr, 16);
        return (v > 0 && v < 256) ? static_cast<int>(v) : 0;
    }
    static const std::pair<const char*, int> names[] = {
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
        {"tab", VK_TAB}, {"backspace", VK_BACK}, {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"control", VK_CONTROL},
        {"alt", VK_MENU}, {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT}, {"lctrl", VK_LCONTROL}, {"rctrl", VK_RCONTROL},
        {"lalt", VK_LMENU}, {"ralt", VK_RMENU}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT},
        {"right", VK_RIGHT}, {"insert", VK_INSERT}, {"ins", VK_INSERT}, {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR}, {"pagedown", VK_NEXT},
        {"pgdn", VK_NEXT}, {"capslock", VK_CAPITAL}, {"pause", VK_PAUSE}, {"numplus", VK_ADD},
        {"numminus", VK_SUBTRACT}, {"nummultiply", VK_MULTIPLY}, {"numdivide", VK_DIVIDE}, {"numdecimal", VK_DECIMAL},
        {"minus", VK_OEM_MINUS}, {"plus", VK_OEM_PLUS}, {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD},
        {"mouse1", VK_LBUTTON}, {"mouse2", VK_RBUTTON}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1},
        {"mouse5", VK_XBUTTON2}};
    for (const auto& [name, vk] : names)
        if (n == name) return vk;
    return 0;
}

// "Ctrl+Shift+K" -> the key and the modifiers it needs. Returns false for an unknown name.
bool ParseKey(const std::string& spec, int& vk, int& mods) {
    vk = 0;
    mods = 0;
    size_t start = 0;
    while (true) {
        size_t plus = spec.find('+', start);
        std::string part = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (plus == std::string::npos) {
            vk = KeyCode(part);
            return vk != 0;
        }
        std::string p = part;
        for (char& ch : p) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (p == "ctrl" || p == "control") mods |= kCtrl;
        else if (p == "shift") mods |= kShift;
        else if (p == "alt") mods |= kAlt;
        else if (int b = PadBit(p); b >= 0) mods |= 1 << (8 + b);  // "Pad_LB+Pad_A": LB held with A
        else return false;
        start = plus + 1;
    }
}

bool KeysAllowed() {  // the game has the focus and the console is closed
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && !UiIsOpen();
}

bool Down(int vk) {
    if (vk >= kPad) return (PadMask() >> (vk - kPad)) & 1;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// exact: no other keyboard modifier may be held (Ctrl+F5 does not fire "F5"); a modifier key itself is not counted
// against itself ("Shift" alone). Controller buttons as modifiers have to be held, others may be too.
bool KeyDown(int vk, int mods, bool exact) {
    if (!KeysAllowed() || !Down(vk)) return false;
    // a text field has the keyboard: the letters are typed there, no mod's key fires and no held key counts
    // (a mod's free camera turned on at the U and flew with W A S D while a line was typed in another mod's
    // field); F1-F24 type nothing, the mouse and the controller are no typing - they stay
    bool typingKey = vk > VK_XBUTTON2 && vk < kPad && !(vk >= VK_F1 && vk <= VK_F24);
    if (typingKey && UiTextHasKeys()) return false;
    uint32_t pad = static_cast<uint32_t>(mods) >> 8;
    if (pad && (PadMask() & pad) != pad) return false;
    mods &= 0xff;
    int held = (Down(VK_CONTROL) ? kCtrl : 0) | (Down(VK_SHIFT) ? kShift : 0) | (Down(VK_MENU) ? kAlt : 0);
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) held &= ~kCtrl;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) held &= ~kShift;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) held &= ~kAlt;
    return exact ? held == mods : (held & mods) == mods;
}

int CheckKey(lua_State* L, int idx, int& mods) {
    const char* spec = luaL_checkstring(L, idx);
    int vk = 0;
    if (!ParseKey(spec, vk, mods))
        luaL_error(L, "unknown key '%s' (F1-F24, A-Z, 0-9, Num0-9, Space, Ctrl+..., Mouse1-5, 0x.., Pad_A, Pad_LB+Pad_Y ...)", spec);
    return vk;
}

int LKeyDown(lua_State* L) {  // w3.key_down(spec): the key is held now (more modifiers held are fine)
    int mods = 0;
    int vk = CheckKey(L, 1, mods);
    lua_pushboolean(L, KeyDown(vk, mods, false));
    return 1;
}

int LKeyCode(lua_State* L) {  // w3._key(spec): its key code and modifiers (an error for an unknown name)
    int mods = 0;
    int vk = CheckKey(L, 1, mods);
    lua_pushinteger(L, vk);
    lua_pushinteger(L, mods);
    return 2;
}

int LKeyExact(lua_State* L) {  // w3._key_exact(vk, mods): held now with exactly these modifiers
    lua_pushboolean(L, KeyDown(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)), true));
    return 1;
}

int LOnKeyFn(lua_State* L) {  // w3._on_key(fn): a function checked every tick (the prelude's w3.on_key)
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushinteger(L, AddFn(L, "tw3se.keys", 1));
    return 1;
}
int LOffKey(lua_State* L) {  // w3.off_key(handle): the key from w3.on_key stops
    RemoveFn(L, "tw3se.keys", luaL_checkinteger(L, 1));
    return 0;
}

// w3._bundle_paths(file): the depot paths in a .bundle (its table only: header 0x20 'POTATO70', the table's size at
// +0x10, 0x130 per file with the path in its first 0x100 bytes; loosefiles.cpp writes the same), or nil and why
int LBundlePaths(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, file, -1, w, MAX_PATH * 2)) return luaL_argerror(L, 1, "a path");
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot open it");
        return 2;
    }
    uint8_t head[0x20] = {};
    DWORD got = 0;
    std::vector<uint8_t> toc;
    const char* why = nullptr;
    if (!ReadFile(h, head, sizeof head, &got, nullptr) || got != sizeof head || memcmp(head, "POTATO70", 8) != 0) {
        why = "not a bundle";
    } else {
        uint32_t size = 0;
        memcpy(&size, head + 0x10, 4);
        if (size % 0x130 || size > (64u << 20)) {
            why = "a table of another size";
        } else {
            toc.resize(size);
            if (size && (!ReadFile(h, toc.data(), size, &got, nullptr) || got != size)) why = "the table is cut off";
        }
    }
    CloseHandle(h);
    if (why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    lua_newtable(L);
    int n = 0;
    for (size_t off = 0; off + 0x130 <= toc.size(); off += 0x130) {
        const char* p = reinterpret_cast<const char*>(toc.data() + off);
        size_t len = strnlen(p, 0x100);
        if (!len) continue;
        lua_pushlstring(L, p, len);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// w3._lockey(key, text): GetLocStringByKeyExt answers the key with the text, in C (lockeys.cpp); false when that
// handler could not go on (the Lua hook of lua_lib.inc answers then)
int LLocKey(lua_State* L) {
    lua_pushboolean(L, LocKeySet(luaL_checkstring(L, 1), luaL_checkstring(L, 2)));
    return 1;
}

// w3.script_profile(["on" | "off" | "reset"]): the WitcherScript profiler (scriptstack.cpp). Returns the functions by
// their own time, {fn, file, mod, calls, own, whole} (ms over the whole count), the seconds it counted, and on.
int LScriptProfile(lua_State* L) {
    if (lua_isstring(L, 1)) {
        std::string a = lua_tostring(L, 1);
        if (a != "on" && a != "off" && a != "reset") return luaL_argerror(L, 1, "\"on\", \"off\" or \"reset\"");
        ScriptProfileSet(a != "off", a != "off");
    }
    double seconds = 0;
    std::vector<ScriptProfRow> rows = ScriptProfile(&seconds);
    lua_createtable(L, static_cast<int>(rows.size()), 0);
    int i = 0;
    for (const ScriptProfRow& r : rows) {
        lua_createtable(L, 0, 6);
        lua_pushstring(L, r.fn.c_str());
        lua_setfield(L, -2, "fn");
        lua_pushstring(L, r.file.c_str());
        lua_setfield(L, -2, "file");
        lua_pushstring(L, r.mod.c_str());
        lua_setfield(L, -2, "mod");
        lua_pushinteger(L, static_cast<lua_Integer>(r.calls));
        lua_setfield(L, -2, "calls");
        lua_pushnumber(L, r.ownMs);
        lua_setfield(L, -2, "own");
        lua_pushnumber(L, r.wholeMs);
        lua_setfield(L, -2, "whole");
        lua_rawseti(L, -2, ++i);
    }
    lua_pushnumber(L, seconds);
    lua_pushboolean(L, ScriptProfileOn());
    return 3;
}

// w3._ui_flag(name, on): the console's Inspector / Profiler box follows the window's X (lib.inspector, lib.profiler)
int LUiFlag(lua_State* L) {
    UiSetWindowFlag(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}

// w3._call_as(mod, fn, ...): fn(...) as that mod, guarded: true, or false and the error (logged, noted for F2 and the
// line over the game as that mod's). The game events (w3.on) run each mod's function this way.
int LCallAs(lua_State* L) {
    std::string mod = luaL_optstring(L, 1, "");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int n = lua_gettop(L) - 2;
    std::string err;
    bool ok;
    {
        ModScope scope(mod);
        ProfScope prof(mod, "event");
        ok = PCall(L, n, 0, err);
    }
    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    Log("lua: an event function of %s: %s", mod.empty() ? "the console" : mod.c_str(), err.c_str());
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
}

int LAs(lua_State* L) {  // w3._as(mod, fn, ...): fn(...) as that mod (a w3.run sequence resumes as its mod)
    std::string mod = luaL_optstring(L, 1, "");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    ModScope scope(mod);
    ProfScope prof(mod, "run");
    lua_call(L, lua_gettop(L) - 2, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

// w3._state(): what TW3SE holds for Lua right now, for tools/state.py (a test has to leave all of it as it found it):
// hooked functions and their handlers, the entries of each function list, Lua's memory, the watched figures.
int LState(lua_State* L) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_newtable(L);
    int hooks = 0, handlers = 0;
    for (const HookRec& h : g_hooks) {  // TW3SE's own handlers (owner "tw3se", the w3.saved hook) left out
        int mine = 0;
        for (const HookHandler& hh : h.handlers) mine += hh.owner != "tw3se";
        if (h.used && mine) ++hooks, handlers += mine;
    }
    lua_pushinteger(L, hooks);
    lua_setfield(L, -2, "hooks");
    lua_pushinteger(L, handlers);
    lua_setfield(L, -2, "hook_handlers");
    for (const char* list : {"ticks", "draws", "ui", "keys", "reloads", "loads", "saves"}) {
        lua_getfield(L, LUA_REGISTRYINDEX, (std::string("tw3se.") + list).c_str());
        lua_Integer n = 0;
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) ++n, lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_pushinteger(L, n);
        lua_setfield(L, -2, list);
    }
    lua_pushinteger(L, lua_gc(L, LUA_GCCOUNT, 0));
    lua_setfield(L, -2, "lua_kb");
    int pose = 0, mimic = 0;
    PoseWatchCounts(&pose, &mimic);
    lua_pushinteger(L, pose);
    lua_setfield(L, -2, "pose_watch");
    lua_pushinteger(L, mimic);
    lua_setfield(L, -2, "mimic_watch");
    return 1;
}

std::vector<std::pair<std::pair<std::string, std::string>, ProfStat>> ProfSorted() {
    std::vector<std::pair<std::pair<std::string, std::string>, ProfStat>> v(g_prof.begin(), g_prof.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    return v;
}

// w3.profile([reset]): a list of {mod, kind, calls, ms, max, per_tick}, the most time first, and the ticks counted.
int LProfile(lua_State* L) {
    bool reset = lua_toboolean(L, 1);
    auto v = ProfSorted();
    lua_createtable(L, static_cast<int>(v.size()), 0);
    int i = 0;
    for (const auto& [key, s] : v) {
        lua_createtable(L, 0, 6);
        lua_pushstring(L, key.first.c_str());
        lua_setfield(L, -2, "mod");
        lua_pushstring(L, key.second.c_str());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, static_cast<lua_Integer>(s.calls));
        lua_setfield(L, -2, "calls");
        lua_pushnumber(L, s.ms);
        lua_setfield(L, -2, "ms");
        lua_pushnumber(L, s.maxMs);
        lua_setfield(L, -2, "max");
        lua_pushnumber(L, g_profTicks ? s.ms / static_cast<double>(g_profTicks) : 0.0);
        lua_setfield(L, -2, "per_tick");
        lua_rawseti(L, -2, ++i);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(g_profTicks));
    if (reset) {
        g_prof.clear();
        g_profTicks = 0;
        g_profSince = QpcMs();
    }
    return 2;
}

int LPluginReload(lua_State* L) {  // w3.plugin_reload(name): unload a plugin and load its DLL again (a few ticks)
    std::string msg;
    bool ok = PluginReload(luaL_checkstring(L, 1), msg);
    if (!ok) return luaL_error(L, "w3.plugin_reload: %s", msg.c_str());
    lua_pushstring(L, msg.c_str());
    return 1;
}

// --- sounds (sound.cpp): w3.sound.play(file [, {entity =, bone =, volume =, loop =}]) -> a handle
int LSoundPlay(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    void* ent = nullptr;
    std::string bone;
    float volume = 1.0f;
    bool loop = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "entity");
        if (!lua_isnil(L, -1)) ent = CheckLive(L, -1)->ptr;
        lua_getfield(L, 2, "bone");
        if (lua_isstring(L, -1)) bone = lua_tostring(L, -1);
        lua_getfield(L, 2, "volume");
        if (lua_isnumber(L, -1)) volume = static_cast<float>(lua_tonumber(L, -1));
        lua_getfield(L, 2, "loop");
        loop = lua_toboolean(L, -1);
        lua_pop(L, 4);
    }
    std::string err;
    int32_t id = SoundPlay(file, ent, bone.empty() ? nullptr : bone.c_str(), volume, loop ? 1 : 0, g_curMod.c_str(), &err);
    if (!id) return luaL_error(L, "w3.sound.play: %s", err.c_str());
    lua_pushinteger(L, id);
    return 1;
}
int LSoundStop(lua_State* L) {
    lua_pushboolean(L, SoundStop(static_cast<int32_t>(luaL_checkinteger(L, 1))));
    return 1;
}
int LSoundVolume(lua_State* L) {
    lua_pushboolean(L, SoundVolume(static_cast<int32_t>(luaL_checkinteger(L, 1)), static_cast<float>(luaL_checknumber(L, 2))));
    return 1;
}
int LSoundPlaying(lua_State* L) {
    lua_pushboolean(L, SoundPlaying(static_cast<int32_t>(luaL_checkinteger(L, 1))));
    return 1;
}
int LSoundPreload(lua_State* L) {
    std::string err;
    if (!SoundPreload(luaL_checkstring(L, 1), &err)) return luaL_error(L, "w3.sound.preload: %s", err.c_str());
    return 0;
}

// --- data per save: the prelude's w3.saved tables go into tw3se\saves\<id>.lua when the game saves; the id
// is a fact, so it is part of the save and tells after a load which file belongs to it.
std::atomic<bool> g_gameLoaded{false};  // lifecycle.cpp: a game started or a save loaded; the tick fills the tables

std::wstring SavePath(lua_State* L) {
    return Dir() + L"\\saves\\" + std::to_wstring(luaL_checkinteger(L, 1)) + L".lua";
}

int LSaveWrite(lua_State* L) {  // w3._save_write(id, text): true when written (a temp file, then renamed)
    std::wstring path = SavePath(L);
    size_t n = 0;
    const char* text = luaL_checklstring(L, 2, &n);
    CreateDirectoryW((Dir() + L"\\saves").c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = h != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD w = 0;
        ok = WriteFile(h, text, static_cast<DWORD>(n), &w, nullptr) && w == n;
        CloseHandle(h);
    }
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    lua_pushboolean(L, ok);
    return 1;
}

int LSaveRead(lua_State* L) {  // w3._save_read(id): the file's text, or nil
    std::wstring path = SavePath(L);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        lua_pushnil(L);
        return 1;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string text(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    ReadFile(h, text.empty() ? nullptr : &text[0], static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(h);
    lua_pushlstring(L, text.data(), got);
    return 1;
}

int LOnLoad(lua_State* L) {  // w3.on_load(fn): fn runs after a save loaded or a new game started
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushinteger(L, AddFn(L, "tw3se.loads", 1));
    return 1;
}
int LOnSave(lua_State* L) {  // w3.on_save(fn): fn runs right before the w3.saved tables are written
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushinteger(L, AddFn(L, "tw3se.saves", 1));
    return 1;
}
int LOffLoad(lua_State* L) {  // w3.off_load(handle): the function from w3.on_load stops
    RemoveFn(L, "tw3se.loads", luaL_checkinteger(L, 1));
    return 0;
}
int LOffSave(lua_State* L) {  // w3.off_save(handle): the function from w3.on_save stops
    RemoveFn(L, "tw3se.saves", luaL_checkinteger(L, 1));
    return 0;
}
int LRunList(lua_State* L) {  // w3._run_list(list, what): every function of an owned list (on_load, on_save)
    std::string list = luaL_checkstring(L, 1);
    std::string what = luaL_checkstring(L, 2);
    RunFns(L, list.c_str(), what.c_str());
    return 0;
}

int LOnReload(lua_State* L) {  // w3.on_reload(fn): fn runs after every live reload of WitcherScript
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushinteger(L, AddFn(L, "tw3se.reloads", 1));
    return 1;
}
int LOffReload(lua_State* L) {  // w3.off_reload(handle): the function from w3.on_reload stops
    RemoveFn(L, "tw3se.reloads", luaL_checkinteger(L, 1));
    return 0;
}

std::string Joined(lua_State* L, int from) {
    std::string s;
    for (int i = from, n = lua_gettop(L); i <= n; ++i) {
        if (i > from) s += '\t';
        s += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    return s;
}

// w3._fault([kind]): a deliberate fault inside a w3 function, for the test that it stops that one call and Lua goes
// on (ldo.c, callC_guarded). "read" (the default) reads address 0x10, "div" divides by zero. The string here is
// destroyed on the way out (/EHa).
// w3._scene_stop(player): CStoryScenePlayer::Stop, the way the engine ends every running scene when a world unloads
// (docs/engine/scenes.md: the controller despawns the scene's actors, the scene system frees the player at its next
// update; a second call returns at once). Main thread, outside the scene's own update: w3.scene.stop defers it to the
// next tick. StopAllScenes and Destroy left a quest's scene running.
bool SceneStopGuarded(void (*stop)(void*), void* player) {
    __try {
        stop(player);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int LSceneStop(lua_State* L) {
    Obj* o = CheckLive(L, 1);
    if (RttiName(o->cls) != "CStoryScenePlayer") return luaL_error(L, "w3._scene_stop: a CStoryScenePlayer is needed");
    auto stop = reinterpret_cast<void (*)(void*)>(Address("CStoryScenePlayer::Stop"));
    if (!stop) return luaL_error(L, "w3.scene.stop: the scene system's stop is not known for this game build");
    if (!SceneStopGuarded(stop, o->ptr)) return luaL_error(L, "w3.scene.stop: the stop faulted inside the game (caught)");
    lua_pushboolean(L, 1);
    return 1;
}

// w3._anim_add_set(component, set) / w3._anim_remove_set(component, set) (docs/engine/animation.md): one
// more CSkeletalAnimationSet in a running CAnimatedComponent's container, the way a cutscene pushes its set onto its
// actors; a lookup by name walks the list from the end, so the added set wins. Main thread. The set has to stay loaded
// (w3.anim.add_set keeps it); a rebuild of the component's sets drops it.
bool AnimAddGuarded(void* cont, void* hb, uint32_t (*grow)(void*, uint32_t)) {
    __try {
        auto list = static_cast<char*>(cont) + 0x58;
        uint32_t n = 0;
        memcpy(&n, list + 8, 4);
        void** data = *reinterpret_cast<void***>(list);
        for (uint32_t k = 0; k < n && data; ++k)
            if (data[k] == hb) return true;  // there already
        _InterlockedIncrement(static_cast<volatile long*>(hb));  // the list holds a reference
        uint32_t i = grow(list, 1);                               // the old count: the new slot
        (*reinterpret_cast<void***>(list))[i] = hb;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool AnimRemoveGuarded(void (*remove)(void*, void*), void* cont, void* set) {
    __try {
        remove(cont, set);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* AnimContainer(lua_State* L, Obj* comp) {
    bool animated = false;
    for (void* c = comp->cls; Plausible(c) && !animated; c = Rd<void*>(c, 0x10)) animated = RttiName(c) == "CAnimatedComponent";
    if (!animated) luaL_error(L, "w3.anim: a CAnimatedComponent is needed, this is a %s", RttiName(comp->cls).c_str());
    void* cont = Rd<void*>(comp->ptr, FieldOr("CAnimatedComponent.animationContainer", 0xe80));
    if (!Plausible(cont)) luaL_error(L, "w3.anim: the component has no animation container (not in the world?)");
    return cont;
}
Obj* AnimSet(lua_State* L, int idx) {
    Obj* s = CheckLive(L, idx);
    if (RttiName(s->cls) != "CSkeletalAnimationSet")
        luaL_error(L, "w3.anim: a CSkeletalAnimationSet is needed, this is a %s", RttiName(s->cls).c_str());
    return s;
}
int LAnimAddSet(lua_State* L) {
    void* cont = AnimContainer(L, CheckLive(L, 1));
    Obj* set = AnimSet(L, 2);
    auto grow = reinterpret_cast<uint32_t (*)(void*, uint32_t)>(Address("TDynArray::GrowBy8"));
    if (!grow) return luaL_error(L, "w3.anim.add_set: not known for this game build");
    void* hb = BlockOf(set->ptr);
    if (!hb) return luaL_error(L, "w3.anim.add_set: the set has no handle");
    if (!AnimAddGuarded(cont, hb, grow)) return luaL_error(L, "w3.anim.add_set: faulted inside the game (caught)");
    lua_pushboolean(L, 1);
    return 1;
}
int LAnimRemoveSet(lua_State* L) {
    void* cont = AnimContainer(L, CheckLive(L, 1));
    Obj* set = AnimSet(L, 2);
    auto remove = reinterpret_cast<void (*)(void*, void*)>(Address("CSkeletalAnimationContainer::RemoveAnimationSet"));
    if (!remove) return luaL_error(L, "w3.anim.remove_set: not known for this game build");
    if (!AnimRemoveGuarded(remove, cont, set->ptr)) return luaL_error(L, "w3.anim.remove_set: faulted inside the game (caught)");
    lua_pushboolean(L, 1);
    return 1;
}

int LFault(lua_State* L) {
    std::string kind = luaL_optstring(L, 1, "read");
    if (kind == "div") {
        volatile int zero = 0;
        lua_pushinteger(L, 1 / zero);
        return 1;
    }
    volatile int v = *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10));
    lua_pushinteger(L, v);
    return 1;
}

int LPrint(lua_State* L) {
    std::string s = Joined(L, 1);
    if (g_out) *g_out += s + "\n";
    Log("lua: %s", s.c_str());
    return 0;
}

int Traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// Runs the function on top of the stack with the traceback handler; leaves its results on the stack.
// the last Lua errors for the report: time, mod, message with traceback
std::deque<std::string> g_recentErrors;

// --- errors a player sees: each mod's last error for F2's Mods tab (w3._mods), and once per mod a line over the
// game for 8 s ("TW3SE: mods/x has an error (F2 > Mods)"; error_notice = off in tw3se.config leaves it out). A mod
// that loads again without an error starts clean.
std::map<std::string, std::string> g_modErrors;  // mod key -> its last error
std::set<std::string> g_noticed;                 // the mods announced over the game this session
std::string g_notice;
ULONGLONG g_noticeUntil = 0;

void NoteModError(const std::string& mod, const std::string& err) {
    if (mod.empty() || mod == "tw3se") return;  // the console, WitcherScript, TW3SE's own library
    g_modErrors[mod] = err;
    if (!g_noticed.insert(mod).second || !ConfigBool("error_notice", true)) return;
    g_notice = "TW3SE: " + mod + " has an error (F2 > Mods)";
    g_noticeUntil = 0;  // the 8 s start at the first tick that draws it (an error in natives\ comes before the game)
}

void ShowNotice_(const std::string& text) {  // a line over the game for 8 s (core.h ShowNotice: safe mode)
    g_notice = text;
    g_noticeUntil = 0;
}

void DrawNotice() {  // inside BuildDraws, on the main thread's tick: the line goes into the same command list
    if (g_notice.empty() || UiIsOpen()) return;
    if (!g_noticeUntil) {
        g_noticeUntil = GetTickCount64() + 8000;
        if (!g_drawHooked) {  // the overlay, as w3.on_draw turns it on
            g_drawHooked = true;
            InstallOverlay(true);
            RegisterOverlayDraw(&DrawReplay, nullptr);
        }
    }
    if (GetTickCount64() >= g_noticeUntil) {
        g_notice.clear();
        return;
    }
    float w = 0, h = 0;
    OverlayTextSize(g_notice.c_str(), &w, &h);
    if (w <= 0) return;  // the overlay's font is not made yet
    g_drawBuild.push_back({1, 16, 16, w + 24, h + 12, 0x1E1E20E0u, ""});
    g_drawBuild.push_back({0, 28, 22, 0, 0, 0xFF8A80FFu, g_notice});
}

// --- a Lua function that does not come back (an endless loop) froze the game for good. Every entry into Lua from
// TW3SE starts a clock (the outermost one: a hook inside an on_tick function counts for the on_tick function), and a
// count hook stops the Lua code with an error once it ran lua_time_limit seconds (tw3se.config, default 5, 0 = off;
// under the watchdog's 8 s, so a stopped loop leaves no hang report). The console's lua, luafile and luatest run as
// long as they need. The VS Code debugger sets a hook of its own and
// replaces this one while it is attached; the tick puts it back when no hook is left.
thread_local int t_luaDepth = 0;
thread_local ULONGLONG t_luaSince = 0;
thread_local int t_luaNoLimit = 0;
ULONGLONG g_luaLimitMs = 5000;
constexpr int kLimitCount = 200000;  // Lua instructions between two looks at the clock (well under 1 ms)

struct LuaEntry {
    LuaEntry() {
        if (t_luaDepth++ == 0) t_luaSince = GetTickCount64();
    }
    ~LuaEntry() { --t_luaDepth; }
};
struct NoLimit {  // a console command: no time limit inside it
    NoLimit() { ++t_luaNoLimit; }
    ~NoLimit() { --t_luaNoLimit; }
};

void LimitHook(lua_State* L, lua_Debug*) {
    if (!t_luaDepth || t_luaNoLimit || !g_luaLimitMs) return;
    ULONGLONG ran = GetTickCount64() - t_luaSince;
    if (ran < g_luaLimitMs) return;
    luaL_error(L, "ran %d s without coming back and was stopped (an endless loop?); lua_time_limit in tw3se.config "
                  "sets the seconds", static_cast<int>(ran / 1000));
}

void ResetLuaClock() { t_luaSince = GetTickCount64(); }

void DropRunaways(lua_State* L) {  // see IsRunaway
    for (int id : g_dropHooks)
        if (UnhookId(L, id)) Log("lua: hook handler %d taken off (it ran into a limit)", id);
    g_dropHooks.clear();
    for (int i : g_dropNatives) {
        LuaNative& n = g_luaNatives[i];
        if (n.ref == LUA_NOREF) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, n.ref);
        n.ref = LUA_NOREF;
        Log("lua: native %s taken off (it ran into a limit); scripts get its default value", n.name.c_str());
    }
    g_dropNatives.clear();
}

// --- Lua's memory has a ceiling (tw3se.config lua_memory_limit in MB, default 1024, 0 = off). A mod whose tables grow
// without end took the game's memory with it until the game ended; at the ceiling Lua refuses the block, collects
// once and raises "not enough memory" in the code that asked. The allocator is Lua's own (realloc and free), so the
// blocks made before it was set are freed by it too.
size_t g_luaBytes = 0, g_luaMaxBytes = 0, g_luaPeak = 0;
bool g_luaFullLogged = false;
void* LimitedAlloc(void*, void* ptr, size_t osize, size_t nsize) {
    size_t old = ptr ? osize : 0;  // without a block, osize is the kind of object Lua makes
    if (nsize == 0) {
        free(ptr);
        g_luaBytes -= (std::min)(old, g_luaBytes);
        return nullptr;
    }
    if (g_luaMaxBytes && nsize > old && g_luaBytes + (nsize - old) > g_luaMaxBytes) {
        if (!g_luaFullLogged) {
            g_luaFullLogged = true;
            Log("lua: the memory limit of %zu MB is reached (lua_memory_limit in tw3se.config); a request of %zu bytes "
                "was refused", g_luaMaxBytes >> 20, nsize);
        }
        return nullptr;
    }
    void* p = realloc(ptr, nsize);
    if (p) {
        g_luaBytes = g_luaBytes - (std::min)(old, g_luaBytes) + nsize;
        if (g_luaBytes > g_luaPeak) g_luaPeak = g_luaBytes;
    }
    return p;
}

void ArmLimit(lua_State* L) {  // the state, and again whenever no hook is set (the debugger took its own off)
    if (L && !lua_gethook(L) && g_luaLimitMs) lua_sethook(L, &LimitHook, LUA_MASKCOUNT, kLimitCount);
}

bool PCall(lua_State* L, int nargs, int nres, std::string& err) {
    LuaEntry entry;
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, &Traceback);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
    if (rc == LUA_OK) return true;
    err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
    lua_pop(L, 1);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[32];
    snprintf(when, sizeof when, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    g_recentErrors.push_front(std::string(when) + " [" + (g_curMod.empty() ? "console" : g_curMod) + "] " + err);
    if (g_recentErrors.size() > 20) g_recentErrors.pop_back();
    NoteModError(g_curMod, err);
    return false;
}

bool RunFile(const std::wstring& path, std::string& err, int nres = 0) {  // nres 1: its return value stays on the stack
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "file not found";
        return false;
    }
    LARGE_INTEGER sz{};
    // a file on disk only: `luafile con` opened the console device ("con.lua" is CON as well) and read from it
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileSizeEx(h, &sz) || sz.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        err = "not a file";
        return false;
    }
    std::string code(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    ReadFile(h, code.empty() ? nullptr : &code[0], static_cast<DWORD>(code.size()), &got, nullptr);
    CloseHandle(h);
    char chunk[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, chunk, sizeof chunk, nullptr, nullptr);
    if (luaL_loadbuffer(g_L, code.data(), got, (std::string("@") + chunk).c_str()) != LUA_OK) {
        err = lua_tostring(g_L, -1);
        lua_pop(g_L, 1);
        return false;
    }
    return PCall(g_L, 0, nres, err);
}

// require() inside a mod finds the mod's own modules first: require("ui.panel") is <mod folder>\ui\panel.lua or
// ui\panel\init.lua. They are kept per mod (package.loaded["mods/<name>:ui.panel"]), so two mods can each have a
// module "util". Every other name goes on to Lua's require (tw3se\lua\?.lua). Upvalue 1: Lua's require.
std::string Narrow8(const std::wstring& w);  // the mod loader below
int LModRequire(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto it = g_modDirs.find(g_curMod);
    if (it != g_modDirs.end() && !strstr(name, "..") && !strpbrk(name, "/\\:")) {
        wchar_t wname[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_PATH)) wname[0] = 0;
        std::wstring rel = wname;
        std::replace(rel.begin(), rel.end(), L'.', L'\\');
        for (const wchar_t* suffix : {L".lua", L"\\init.lua"}) {
            std::wstring path = it->second + L"\\" + rel + suffix;
            DWORD a = GetFileAttributesW(path.c_str());
            if (rel.empty() || a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::string key = g_curMod + ":" + name;
            lua_getglobal(L, "package");
            lua_getfield(L, -1, "loaded");
            int loaded = lua_gettop(L);
            lua_getfield(L, loaded, key.c_str());
            if (!lua_isnil(L, -1)) return 1;
            lua_pop(L, 1);
            std::string code;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            LARGE_INTEGER sz{};
            if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz) || sz.QuadPart > (64LL << 20)) {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                return luaL_error(L, "module '%s': its file could not be read", name);
            }
            code.resize(static_cast<size_t>(sz.QuadPart));
            DWORD got = 0;
            ReadFile(h, code.empty() ? nullptr : &code[0], static_cast<DWORD>(code.size()), &got, nullptr);
            CloseHandle(h);
            std::string chunk = "@" + Narrow8(path);
            if (luaL_loadbuffer(L, code.data(), got, chunk.c_str()) != LUA_OK) return lua_error(L);
            lua_pushstring(L, name);
            lua_pushstring(L, chunk.c_str() + 1);
            lua_call(L, 2, 1);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushboolean(L, 1);
            }
            lua_pushvalue(L, -1);
            lua_setfield(L, loaded, key.c_str());
            return 1;
        }
    }
    int base = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    lua_call(L, 1, LUA_MULTRET);
    return lua_gettop(L) - base;
}

int LUse(lua_State* L);  // the mod loader below

lua_State* State() {
    if (g_L) return g_L;
    lua_State* L = luaL_newstate();
    if (!L) return nullptr;
    g_luaMaxBytes = static_cast<size_t>((std::max)(0, atoi(ConfigGet("lua_memory_limit", "1024").c_str()))) << 20;
    lua_setallocf(L, &LimitedAlloc, nullptr);  // before anything else is allocated (the state itself is not counted)
    luaL_openlibs(L);
    g_luaLimitMs = static_cast<ULONGLONG>((std::max)(0, atoi(ConfigGet("lua_time_limit", "5").c_str()))) * 1000;
    ArmLimit(L);
    luaL_newmetatable(L, kObj);
    const luaL_Reg meta[] = {{"__index", &LIndex}, {"__newindex", &LNewIndex}, {"__tostring", &LToString},
                             {"__eq", &LEq}, {"__gc", &LGc}, {nullptr, nullptr}};
    luaL_setfuncs(L, meta, 0);
    lua_pop(L, 1);
    luaL_newmetatable(L, kArr);
    const luaL_Reg arrMeta[] = {{"__index", &LArrIndex}, {"__newindex", &LArrNewIndex}, {"__len", &LArrLen},
                                {"__tostring", &LArrToStr}, {nullptr, nullptr}};
    luaL_setfuncs(L, arrMeta, 0);
    lua_pop(L, 1);
    luaL_newmetatable(L, kMap);  // a live view on a map field
    const luaL_Reg mapMeta[] = {{"__index", &LMapIndex}, {"__newindex", &LMapNewIndex}, {"__len", &LMapLen},
                                {"__pairs", &LMapPairs}, {"__tostring", &LMapToStr}, {nullptr, nullptr}};
    luaL_setfuncs(L, mapMeta, 0);
    lua_pop(L, 1);
    luaL_newmetatable(L, kLatent);  // what a latent call returns
    const luaL_Reg latentMeta[] = {{"__index", &LLatentIndex}, {"__tostring", &LLatentToStr}, {"__gc", &LLatentGc},
                                  {nullptr, nullptr}};
    luaL_setfuncs(L, latentMeta, 0);
    lua_pop(L, 1);
    const luaL_Reg w3[] = {{"global", &LGlobal}, {"player", &LPlayer}, {"call", &LCall}, {"enum", &LEnum},
                           {"class", &LClass}, {"fields", &LFields}, {"classof", &LClassOf}, {"find", &LFind},
                           {"on_tick", &LOnTick}, {"off_tick", &LOffTick}, {"_message_take", &LMessageTake}, {"_message_send", &LMessageSend}, {"_tw3se_config", &LTw3seConfig}, {"background_fps", &LBackgroundFps}, {"time", &LTime}, {"_mod", &LCurMod}, {"_mods", &LModsList}, {"use", &LUse}, {"_plugins", &LPluginsList}, {"_var_get", &LVarGet}, {"_var_set", &LVarSet}, {"_list", &LListDir}, {"_tw3se", &LTw3seInfo}, {"_menu_key", &LMenuKey}, {"_key_capture", &LKeyCapture}, {"off_draw", &LOffDraw}, {"valid", &LValid}, {"on_draw", &LOnDraw},
                           {"screen_size", &LScreenSize}, {"entities", &LEntities},
                           {"by_guid", &LByGuid}, {"guid", &LGuid},
                           {"addr", &LAddr}, {"peek", &LPeek}, {"_type", &LTypeAddr}, {"_cname", &LCNameText}, {"hwwatch", &LHwWatch}, {"rva", &LRva}, {"poke", &LPoke}, {"_fault", &LFault}, {"declare", &LDeclare}, {"discard", &LDiscard}, {"engine_hook", &LEngineHook}, {"engine_unhook", &LEngineUnhook}, {"engine_hooks", &LEngineHooks}, {"engine_call", &LEngineCall}, {"_mem_alloc", &LMemAlloc}, {"_mem_read", &LMemRead}, {"_mem_write", &LMemWrite}, {"_mem_free", &LMemFree}, {"engine_addr", &LEngineAddr}, {"find_code", &LFindCode}, {"patch", &LPatch}, {"unpatch", &LUnpatch}, {"_scene_stop", &LSceneStop}, {"watch_latent", &LWatchLatent}, {"unwatch_latent", &LUnwatchLatent}, {"new_enum", &LNewEnum}, {"_anim_add_set", &LAnimAddSet}, {"_anim_remove_set", &LAnimRemoveSet}, {"script_stack", &LScriptStack}, {"vhook", &LVHook}, {"vunhook", &LVUnhook},
                           {"vslot", &LVSlot}, {"object", &LObjectAt}, {"on_line", &LOnLine}, {"off_line", &LOffLine}, {"dir", &LDir},
                           {"_mat_params", &LMatParams}, {"_mat_set", &LMatSet},
                           {"_files_reload", &LFilesReload}, {"_files_set", &LFilesSet}, {"_files_clear", &LFilesClear}, {"_diskfile", &LDiskFile},
                           {"_files_alias", &LFilesAlias}, {"_set_mesh", &LSetMesh}, {"_refresh", &LRefresh},
                           {"_dbg_on_step", &LDbgOnStep}, {"_dbg_step", &LDbgStep}, {"_dbg_stack", &LDbgStack},
                           {"_dbg_frame", &LDbgFrame}, {"_dbg_setvar", &LDbgSetVar}, {"_dbg_listen", &LDbgListen},
                           {"_dbg_recv", &LDbgRecv}, {"_dbg_send", &LDbgSend}, {"_dbg_connected", &LDbgConnected},
                           {"_dbg_alive", &LDbgAlive}, {"_typename", &LTypeName}, {"_px_scene", &LPxScene},
                           {"_px_gravity", &LPxGravity}, {"_config", &LConfig}, {"_config_list", &LConfigList},
                           {"_sector", &LSectorList}, {"_sector_meshes", &LSectorMeshes}, {"_sector_world", &LSectorWorld}, {"_mesh_path", &LMeshPath}, {"_sector_pos", &LSectorSetPos}, {"_sector_proxies", &LSectorProxies}, {"_sector_unstream", &LSectorUnstream}, {"_proxy_lc", &LProxyLc}, {"_focus_color", &LFocusColor}, {"_focus_extra", &LFocusExtra}, {"_focus_probe", &LFocusProbe}, {"_focus_census", &LFocusCensus},
                           {"_terrain_overlay", &LTerrainOverlay}, {"_terrain_probe", &LTerrainProbe}, {"_rest_bones", &LRestBones}, {"_terrain_raise", &LTerrainRaise}, {"_scene_time", &LSceneTime}, {"_anim_sample", &LAnimSample}, {"_view_rect", &LViewRect}, {"_anim_pose", &LAnimPose}, {"_anim_create", &LAnimCreate}, {"_anim_publish", &LAnimPublish}, {"_anim_record", &LAnimRecord}, {"_anim_shape", &LAnimShape}, {"_anim_reach", &LAnimReach}, {"_anim_bones", &LAnimBones}, {"_anim_record_stop", &LAnimRecordStop}, {"_scene_pause", &LScenePause}, {"_scene_elements", &LSceneElements}, {"_scene_seek", &LSceneSeek}, {"_decal_spawn", &LDecalSpawn}, {"_decal_remove", &LDecalRemove}, {"_decal_clear", &LDecalClear}, {"_decal_count", &LDecalCount}, {"_decal_per_mesh", &LDecalPerMesh}, {"_decal_color", &LDecalColor}, {"_net_http", &LNetHttp}, {"_net_connect", &LNetConnect},
                           {"_net_udp", &LNetUdp}, {"_net_send", &LNetSend}, {"_net_close", &LNetClose}, {"_net_poll", &LNetPoll},
                           {"report", &LReport},
                           {"_deform_probe", &LDeformProbe}, {"_deform_stats", &LDeformStats}, {"_memfind", &LMemFind}, {"_bs_load", &LBlendLoad}, {"_bs_set", &LBlendSet},
                           {"_bs_names", &LBlendNames}, {"_bs_state", &LBlendState}, {"_bs_unload", &LBlendUnload}, {"_hide_open", &LHideOpen}, {"_hide_bits", &LHideBits}, {"_hide_close", &LHideClose}, {"_hide_state", &LHideState}, {"_hide_forget", &LHideForget},
                           {"_bs_dump", &LBlendDump}, {"_bs_stats", &LBlendStats}, {"_shot_burst", &LShotBurst}, {"_bs_forget", &LBlendForget}, {"_bs_reset_all", &LBlendResetAll}, {"_render_rebuild", &LRenderRebuild},
                           {"_lm_new", &LLiveNew}, {"_lm_attach", &LLiveAttach}, {"_lm_fail", &LLiveFail}, {"_lm_close", &LLiveClose},
                           {"_lm_info", &LLiveInfo}, {"_lm_write", &LLiveWrite}, {"_lm_bounds", &LLiveBounds}, {"_tex_probe", &LTexProbe}, {"_tex_test", &LTexTest}, {"_tex_pin", &LTexPin}, {"_tex_unpin", &LTexUnpin}, {"_tex_stamp", &LTexStamp}, {"_tex_eye", &LTexEye},{"_tex_stats", &LTexStats}, {"_tex_readback", &LTexReadback}, {"_bs_index_dump", &LIndexDump}, {"_skin_capture", &LSkinCapture}, {"_skin_mush", &LSkinMush}, {"_skin_dqs", &LSkinDqs}, {"_pso_trace", &LPsoTrace}, {"_skin_mush_mask", &LSkinMushMask}, {"_skin_mush_dump", &LMushDump}, {"_skin_overlay_test", &LSkinOverlayTest}, {"_skin_material_test", &LSkinMaterialTest}, {"_skin_overlay_raw", &LSkinOverlayRaw}, {"_bs_normals", &LBlendNormals}, {"_skin_caps_ex", &LSkinCapsulesEx},
                           {"_skin_add", &LSkinAdd}, {"_skin_remove", &LSkinRemove}, {"_skin_caps", &LSkinCapsules},
                           {"_skin_stats", &LSkinStats}, {"_skin_state", &LSkinState},
                           {"_skin_weights", &LSkinWeights}, {"_skin_chunks", &LSkinChunks},
                           {"_shot_burst_done", &LShotBurstDone},
                           {"pose_selftest", &LPoseSelftest}, {"pose_mapdump", &LPoseMapdump},
                           {"pose_meshlist", &LPoseMeshlist}, {"pose_selftest_sig", &LPoseSelftestSig},
                           {"pose_wave", &LPoseWave}, {"pose_restore", &LPoseRestore},
                           {"pose_watch", &LPoseWatch}, {"pose", &LPose}, {"new", &LNew}, {"native", &LNative},
                           {"hook", &LHook}, {"unhook", &LUnhook}, {"key_down", &LKeyDown}, {"_key", &LKeyCode},
                           {"_key_exact", &LKeyExact}, {"_on_key", &LOnKeyFn}, {"off_key", &LOffKey}, {"_as", &LAs}, {"_call_as", &LCallAs}, {"_bundle_paths", &LBundlePaths}, {"_lockey", &LLocKey}, {"script_profile", &LScriptProfile}, {"_ui_flag", &LUiFlag},
                           {"profile", &LProfile}, {"_save_write", &LSaveWrite}, {"_save_read", &LSaveRead},
                           {"on_load", &LOnLoad}, {"on_save", &LOnSave}, {"_run_list", &LRunList},
                           {"plugin_reload", &LPluginReload}, {"dlc_scans", &LDlcScans}, {"_state", &LState}, {"off_load", &LOffLoad},
                           {"off_save", &LOffSave}, {"pose_stop", &LPoseStop}, {"_params", &LParams},
                           {"_member", &LMember}, {"all", &LAll}, {"_text_set", &LTextSet},
                           {"_quest_blocks", &LQuestBlocks}, {"_quest_children", &LQuestChildren},
                           {"_quest_activate", &LQuestActivate}, {nullptr, nullptr}};
    luaL_newlib(L, w3);
    lua_setglobal(L, "w3");
    lua_pushcfunction(L, &LPrint);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.ticks");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.draws");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.keys");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.loads");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.saves");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.ui");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.reloads");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.modmodules");
    lua_getglobal(L, "w3");  // w3.ui = { window = ..., text = ... }, w3.on_ui, w3.off_ui
    LuaUiOpen(L);  // w3.ui (lua_ui.cpp)
    LuaProcOpen(L);  // w3.proc (lua_proc.cpp)
    LuaLinkOpen(L);  // w3.exec, w3.link (lua_link.cpp)
    LuaFsOpen(L);    // w3.fs (lua_fs.cpp)
    LuaInputOpen(L); // w3.input (lua_input.cpp)
    lua_pushcfunction(L, &LOnUi);
    lua_setfield(L, -2, "on_ui");
    lua_pushcfunction(L, &LOffUi);
    lua_setfield(L, -2, "off_ui");
    lua_pushcfunction(L, &LOnReload);
    lua_setfield(L, -2, "on_reload");
    lua_pushcfunction(L, &LOffReload);
    lua_setfield(L, -2, "off_reload");
    lua_pop(L, 1);
    lua_getglobal(L, "w3");  // w3.face = { set = ..., clear = ..., tracks = ... }
    const luaL_Reg face[] = {{"set", &LFaceSet}, {"clear", &LFaceClear}, {"tracks", &LFaceTracks}, {nullptr, nullptr}};
    luaL_newlib(L, face);
    lua_setfield(L, -2, "face");
    lua_pop(L, 1);
    lua_getglobal(L, "w3");  // w3.sound = { play = ..., stop = ..., volume = ..., playing = ..., preload = ... }
    const luaL_Reg sound[] = {{"play", &LSoundPlay}, {"stop", &LSoundStop}, {"volume", &LSoundVolume},
                              {"playing", &LSoundPlaying}, {"preload", &LSoundPreload}, {nullptr, nullptr}};
    luaL_newlib(L, sound);
    lua_setfield(L, -2, "sound");
    lua_pop(L, 1);
    lua_getglobal(L, "w3");  // w3.draw = { text = ..., rect = ... }
    lua_newtable(L);
    lua_pushcfunction(L, &LDrawText);
    lua_setfield(L, -2, "text");
    lua_pushcfunction(L, &LDrawRect);
    lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, &LDrawSize);
    lua_setfield(L, -2, "size");
    lua_setfield(L, -2, "draw");
    lua_pop(L, 1);
    char dir[MAX_PATH];  // require() finds modules in tw3se\lua
    WideCharToMultiByte(CP_UTF8, 0, Dir().c_str(), -1, dir, sizeof dir, nullptr, nullptr);
    lua_getglobal(L, "package");
    lua_pushstring(L, (std::string(dir) + "\\lua\\?.lua").c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
    lua_getglobal(L, "require");  // a mod's own modules first (LModRequire)
    lua_pushcclosure(L, &LModRequire, 1);
    lua_setglobal(L, "require");
    // A small prelude in Lua, for mod authors: watch a field, run something on an interval. Built on on_tick and
    // w3.valid, so it stays safe when an object is destroyed.
    static const char* kPrelude = R"LUA(
    -- Arguments are checked here (tools/fuzz_lua.py: w3.every(0.5) without a function, w3.watch({}, {}) and
    -- w3.on_key('x', nil) each added a tick or key that did nothing and never went away). Each returns the handle of its
    -- tick for w3.off_tick; w3.after and w3.watch take their tick out themselves when they are done.
    local function need_fn(fn, who) if type(fn) ~= "function" then error(who .. " needs a function", 3) end end
    local function need_seconds(s, who)
      if type(s) ~= "number" or s ~= s or s < 0 or s == math.huge then error(who .. " needs seconds (a number, 0 or more)", 3) end
    end
    function w3.watch(obj, field, fn)
      if type(obj) ~= "userdata" then error("w3.watch needs an object", 2) end
      if type(field) ~= "string" then error("w3.watch needs a field name", 2) end
      need_fn(fn, "w3.watch")
      local last = obj[field]
      local h
      h = w3.on_tick(function()
        if not w3.valid(obj) then w3.off_tick(h) return end  -- the object is gone: stop looking
        local v = obj[field]
        if v ~= last then local old = last; last = v; fn(v, old) end
      end)
      return h
    end
    local function now()
      local g = w3.global("theGame")
      return g and g:GetEngineTimeAsSeconds() or 0
    end
    function w3.every(seconds, fn)
      need_seconds(seconds, "w3.every")
      need_fn(fn, "w3.every")
      local next_at = now() + seconds
      return w3.on_tick(function()
        local t = now()
        -- loading a save sets the engine time back: a due time further off than one period is from before the load
        if t >= next_at or next_at - t > seconds then next_at = t + seconds; fn() end
      end)
    end
    function w3.after(seconds, fn)
      need_seconds(seconds, "w3.after")
      need_fn(fn, "w3.after")
      local at = now() + seconds
      local h
      h = w3.on_tick(function()
        if now() >= at then w3.off_tick(h) fn() end
      end)
      return h
    end
    -- The method names of an object's class (optionally only those containing `filter`, case-insensitive).
    function w3.methods(obj, filter)
      local c = w3.class(w3.classof(obj))
      local lf = filter and filter:lower()
      local out = {}
      for _, m in ipairs(c.methods) do
        if not lf or m:lower():find(lf, 1, true) then out[#out + 1] = m end
      end
      table.sort(out)
      return out
    end
    -- Dump an object's fields and their current values as text (optionally only names containing `filter`). Handy
    -- in the console: lua print(w3.dump(w3.player(), "health"))
    function w3.dump(obj, filter)
      local out = {}
      local lf = filter and filter:lower()
      for name, info in pairs(w3.fields(obj)) do
        if not lf or name:lower():find(lf, 1, true) then
          local ok, v = pcall(function() return obj[name] end)
          out[#out + 1] = string.format("%-44s %-24s = %s", name, info.type, ok and tostring(v) or "?")
        end
      end
      table.sort(out)
      return table.concat(out, "\n")
    end
    -- Entities with a tag (pre-placed or spawned), a list. Built on the engine's GetEntitiesByTag.
    function w3.by_tag(tag)
      local g = w3.global("theGame"); if not g then return {} end
      local ok, ents = pcall(function() return g:GetEntitiesByTag(tag, {}) end)
      return (ok and type(ents) == "table") and ents or {}
    end
    -- After a live reload (`reloadscripts force`): the game runs nothing again by itself, so a mod's init code
    -- (RegisterListener for new keys ...) runs here. w3.on_reload(fn) for Lua; TW3SE_OnScriptsReloaded(obj, 'OnX')
    -- from WitcherScript puts an object's event into w3._reload_objs. w3._reloaded() runs both, returns a summary.
    w3._reload_objs = {}
    function w3._reloaded()
      local events, errs = 0, {}
      for k, v in pairs(w3._reload_objs) do
        if not w3.valid(v.obj) then
          w3._reload_objs[k] = nil   -- the object is gone: its entry too
        else
          local ok, err = pcall(function() return v.obj[v.ev](v.obj) end)
          if ok then events = events + 1 else errs[#errs + 1] = v.ev .. ": " .. tostring(err) end
        end
      end
      return string.format("%d event(s)%s", events, #errs > 0 and (" | " .. table.concat(errs, " | ")) or "")
    end
    -- A resource by its depot path (an entity template, a mesh ...), loaded now: the game's LoadResource.
    function w3.load(path) return w3.call("LoadResource", path, true) end
    -- Sequences over several frames (B7 for Lua): w3.run(fn, ...) runs fn as a coroutine; inside it, w3.wait(s),
    -- w3.wait_frames(n) and w3.wait_until(test [, timeout]) pause it, the tick goes on and resumes it later. A latent
    -- action of the game is started with its ...Async twin and waited for with wait_until. A sequence belongs to
    -- the mod that started it and ends with it (w3._end_runs, called when the mod is taken out).
    do
      local runs = {}
      local function step()
        local now = w3.time()
        for i = #runs, 1, -1 do
          local r = runs[i]
          if r.frames then r.frames = r.frames - 1 end  -- wait_frames(1): on the next tick
          local ready = (r.frames and r.frames <= 0) or (r.at and now >= r.at) or
                        (r.test and (r.test() or (r.deadline and now >= r.deadline)))
          if ready then
            local ok, err = w3._as(r.mod, coroutine.resume, r.co, r.timedout)
            if not ok then print("w3.run: " .. tostring(err)) end
            if coroutine.status(r.co) == "dead" then table.remove(runs, i) end
          end
        end
      end
      w3._step = step  -- the C tick runs it (owned by no mod)
      function w3.run(fn, ...)
        local co = coroutine.create(fn)
        local r = {co = co, mod = w3._mod()}
        runs[#runs + 1] = r  -- listed before it runs, so its first wait finds it
        local ok, err = coroutine.resume(co, ...)
        if not ok or coroutine.status(co) == "dead" then
          for i = #runs, 1, -1 do if runs[i] == r then table.remove(runs, i) break end end
        end
        if not ok then error("w3.run: " .. tostring(err), 2) end
        return co
      end
      local function find(co) for _, r in ipairs(runs) do if r.co == co then return r end end end
      local function pause(set)
        local co, main = coroutine.running()
        if main then error("w3.wait works inside w3.run", 3) end
        local r = find(co) or {co = co}
        r.frames, r.at, r.test, r.deadline, r.timedout = nil, nil, nil, nil, nil
        set(r)
        return coroutine.yield()
      end
      function w3.wait(seconds) pause(function(r) r.at = w3.time() + (seconds or 0) end) end
      function w3.wait_frames(n) pause(function(r) r.frames = n or 1 end) end
      function w3.wait_until(test, timeout)  -- true when test() came true, false after the timeout
        local finished = false
        pause(function(r)
          r.test = function() local ok, v = pcall(test) if ok and v then finished = true end return ok and v end
          r.deadline = timeout and (w3.time() + timeout) or nil
        end)
        return finished
      end
      -- th:wait() on what a latent call returned (B7): until it is done; timeout in seconds, optional
      function w3._latent_wait(th, timeout) return w3.wait_until(function() return th.done end, timeout) end
      function w3._end_runs(mod)
        local n = 0
        for i = #runs, 1, -1 do if runs[i].mod == mod then table.remove(runs, i) n = n + 1 end end
        return n
      end
    end
    -- A key press: fn() runs once each time the key goes down (held keys do not repeat). spec is a name like
    -- "F5", "K", "Ctrl+Shift+K" or "Mouse4"; the modifiers must match exactly, so Ctrl+F5 does not fire "F5".
    function w3.on_key(spec, fn)
      need_fn(fn, "w3.on_key")
      local vk, mods = w3._key(spec)
      if not vk or vk == 0 then error("w3.on_key: no key " .. tostring(spec), 2) end
      local was = w3._key_exact(vk, mods)  -- a key held while it is bound fires at its next press
      return w3._on_key(function()
        local down = w3._key_exact(vk, mods)
        local pressed = down and not was
        was = down
        if pressed then fn() end
      end)
    end
    -- Data per save (H17): w3.saved([name]) is a table that goes with the save. When the game saves, every such table
    -- is written to tw3se\saves\<id>.lua; after a load they are filled again, in place. The id is the fact
    -- tw3se_save_id, so it is part of the save. Every save gets its own id: a new one after each save and each load.
    -- The moment of saving comes from CR4Game.OnSaveStarted, which logs LogChannel('Savegame', "OnSaveStarted ...").
    do
      -- A fact's value goes into the save as 16 bits (measured: 100000 came back as -31072), so the id is two
      -- facts of 15 bits each. A save from before has only FACT, with its id cut to 16 bits - its file has that name.
      local FACT, FACT_HI = "tw3se_save_id", "tw3se_save_id_hi"
      local tables = {}   -- name -> table
      local loaded = {}   -- what the last load brought (for tables made later)
      local hooked = false
      local mine = nil    -- the id this session set last (after a save or a load)
      local function query(name)
        local ok, v = pcall(w3.call, "FactsQueryLatestValue", name)
        return ok and math.tointeger(v) or 0
      end
      local function fact()
        local lo, hi = query(FACT), query(FACT_HI)
        if hi == 0 then return lo end
        return hi * 0x8000 + lo
      end
      local function set(name, v)
        pcall(w3.call, "FactsRemove", name)
        w3.call("FactsAdd", name, v, -1)  -- -1: for good (0 would not last, see FactsSet)
      end
      local function new_id()
        local old, id = fact()
        repeat id = math.random(0x8000, 0x3FFFFFFF) until id ~= old  -- the high part is never 0
        set(FACT, id & 0x7FFF)
        set(FACT_HI, id >> 15)
        mine = id
        return id
      end
      local function ser(v, out, depth)
        local t = type(v)
        if t == "number" then
          if v ~= v then out[#out + 1] = "0/0"
          elseif v == math.huge then out[#out + 1] = "1/0"
          elseif v == -math.huge then out[#out + 1] = "-1/0"
          elseif math.type(v) == "integer" then out[#out + 1] = string.format("%d", v)
          else out[#out + 1] = string.format("%.17g", v) end
        elseif t == "string" then out[#out + 1] = string.format("%q", v)
        elseif t == "boolean" then out[#out + 1] = tostring(v)
        elseif t == "table" then
          if depth > 64 then error("tables nest deeper than 64 (a table inside itself?)", 0) end
          out[#out + 1] = "{"
          for k, x in pairs(v) do
            local kt, xt = type(k), type(x)
            if (kt == "string" or kt == "number" or kt == "boolean") and
               (xt == "string" or xt == "number" or xt == "boolean" or xt == "table") then
              out[#out + 1] = "["; ser(k, out, depth + 1); out[#out + 1] = "]="; ser(x, out, depth + 1)
              out[#out + 1] = ","
            end
          end
          out[#out + 1] = "}"
        end
      end
      w3._serialize = function(v) local out = {} ser(v, out, 0) return table.concat(out) end
      local function fill(t, src)
        for k in pairs(t) do t[k] = nil end
        if type(src) == "table" then for k, v in pairs(src) do t[k] = v end end
      end
      local function save_started()
        w3._run_list("tw3se.saves", "on_save")
        local id = fact()
        if id == 0 or query(FACT_HI) == 0 then id = new_id() end  -- none yet, or one from before the split
        local out = {"return {"}
        for name, t in pairs(tables) do
          local part = {}
          local ok, err = pcall(ser, t, part, 0)
          if ok then out[#out + 1] = string.format("[%q]=%s,", name, table.concat(part))
          else print("w3.saved: " .. name .. " left out: " .. tostring(err)) end
        end
        out[#out + 1] = "}"
        if not w3._save_write(id, table.concat(out, "\n")) then print("w3.saved: could not write saves\\" .. id .. ".lua") end
        w3._last_save = id
      end
      local function ensure_hook()
        if hooked then return end
        hooked = true
        w3._as("tw3se", function()  -- TW3SE's own: w3._state leaves it out
          w3.hook("LogChannel", function(self, args)
            if args[1] ~= "Savegame" then return end
            local msg = tostring(args[2] or "")
            if msg:find("^OnSaveStarted") then save_started()
            elseif msg:find("^OnSaveCompleted") then new_id() end
          end, 1000)
        end)
        if w3.player() and fact() == 0 then new_id() end  -- a mod that starts using it in a running game
      end
      function w3._loaded()  -- the C tick, after a load or a new game: false until the player is there
        if not w3.player() then return false end
        local id = fact()
        -- the game reports one load twice (OnGameStart, RestoreSession 9 s later): when the facts still hold
        -- the id this session set, nothing new was loaded - the second call emptied the tables and ran on_load again
        if hooked and id ~= 0 and id == mine then return true end
        local data = {}
        local text = id ~= 0 and w3._save_read(id)
        if text then
          local f, err = load(text, "=saves/" .. id, "t", {})
          local ok, v = false, err
          if f then ok, v = pcall(f) end
          if ok and type(v) == "table" then data = v else print("w3.saved: saves\\" .. id .. ".lua: " .. tostring(v)) end
        end
        loaded = data
        for name, t in pairs(tables) do fill(t, data[name]) end
        w3._last_load = id
        if hooked then new_id() end  -- the next save gets its own file
        w3._run_list("tw3se.loads", "on_load")
        return true
      end
      function w3.saved(name)
        name = name or w3._mod()
        if name == "" then error("w3.saved needs a name outside a mod", 2) end
        ensure_hook()
        local t = tables[name]
        if not t then t = {} fill(t, loaded[name]) tables[name] = t end
        return t
      end
      local c_on_load, c_on_save = w3.on_load, w3.on_save
      function w3.on_load(fn) ensure_hook() return c_on_load(fn) end
      function w3.on_save(fn) ensure_hook() return c_on_save(fn) end
    end
    -- Entities near a point, a list. Two forms: w3.near(radius [, class]) around the player, or
    -- w3.near(pos, radius [, class]) around a position (pos is a struct or a table with X, Y, Z). Built on
    -- w3.entities + VecDistance.
    function w3.near(a, b, c)
      local center, radius, className
      if type(a) == "number" then
        local p = w3.player(); if not p then return {} end
        center, radius, className = p:GetWorldPosition(), a, b
      else
        center, radius, className = a, b, c
      end
      if not center or type(radius) ~= "number" then return {} end
      local out = {}
      for _, e in ipairs(w3.entities(className)) do
        local ok, d = pcall(function() return w3.call("VecDistance", center, e:GetWorldPosition()) end)
        if ok and d <= radius then out[#out + 1] = e end
      end
      return out
    end
    )LUA";
    std::string e;
    if (luaL_dostring(L, kPrelude) != LUA_OK) {
        Log("lua: prelude error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    static const char* const kGameLib =  // w3.time, w3.weather, w3.camera, w3.stats, w3.inv ...
#include "lua_lib.inc"
        ;
    if (luaL_loadbuffer(L, kGameLib, strlen(kGameLib), "=tw3se game library") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        Log("lua: game library error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    g_profSince = QpcMs();
    g_L = L;
    InstallPluginFns(L);  // functions plugins registered before Lua existed
    if (ConfigBool("debugger", false)) {  // the script debugger (lua\lib\debugger.lua), for VS Code
        std::string start = "local ok, err = require('lib.debugger').start(" +
                            std::to_string(atoi(ConfigGet("debugger_port", "29170").c_str())) + ") return ok, err";
        if (luaL_dostring(L, start.c_str()) != LUA_OK) {
            Log("lua: debugger: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            if (!lua_toboolean(L, -2)) Log("lua: debugger: %s", lua_tostring(L, -1));
            lua_pop(L, 2);
        }
    }
    Log("lua: %s ready", LUA_RELEASE);
    return L;
}

// --- Lua mods: load, take out, load again. A mod is a folder tw3se\mods\<name>\ with init.lua (key
// "mods/<name>") or a file tw3se\lua\autorun\<file>.lua (key "autorun/<file>"). Loading runs it as its own mod
// (everything it adds belongs to it) and notes the modules its require() brought in. Taking it out removes its
// on_tick/on_draw/on_ui/on_reload functions and hooks and forgets those modules. With lua_autoreload on (default), a
// changed .lua file of a mod loads it again within a second, and a new mod folder loads by itself.
struct ModFile {
    std::wstring init;   // the file that runs
    std::wstring dir;    // the folder whose .lua files are watched (a mod folder), or empty (an autorun file)
    uint64_t stamp = 0;  // newest write time of its files, plus their count
    bool loaded = false;
    bool loading = false;  // its init.lua runs right now (w3.use: two mods that use each other)
    std::string err;     // why it failed to load (F2's mod list)
};
std::map<std::string, ModFile> g_mods;
std::map<std::string, std::set<std::string>> g_modUsers;  // a mod -> the mods that w3.use it (they load again with it)
bool g_autoreload = true;

uint64_t FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

uint64_t DirStamp(const std::wstring& dir, int depth = 0) {  // newest .lua write time in the folder tree + file count
    uint64_t newest = 0, count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 4) newest = (std::max)(newest, DirStamp(dir + L"\\" + n, depth + 1));
            continue;
        }
        if (n.size() < 4 || _wcsicmp(n.c_str() + n.size() - 4, L".lua") != 0) continue;
        ++count;
        uint64_t t = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        newest = (std::max)(newest, t);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return newest + count;
}

std::string Narrow8(const std::wstring& w) {
    char b[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof b, nullptr, nullptr);
    return b;
}

// lua_mods_off = mymod, recorder: a mod folder's name or an autorun file (with or without .lua), any case
bool g_luaLeftOut = false;
bool LuaModOff(std::string name) {
    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) name.resize(name.size() - 4);
    std::string list = ConfigGet("lua_mods_off", ""), item;
    for (size_t i = 0; i <= list.size(); ++i) {
        char c = i < list.size() ? list[i] : ',';
        if (c == ',' || c == ' ' || c == ';') {
            if (item.size() > 4 && item.compare(item.size() - 4, 4, ".lua") == 0) item.resize(item.size() - 4);
            if (!item.empty() && item == name) return g_luaLeftOut = true;
            item.clear();
        } else {
            item += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
    }
    return false;
}

std::map<std::string, ModFile> ScanMods() {
    std::map<std::string, ModFile> found;
    std::wstring auto_ = Dir() + L"\\lua\\autorun\\";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((auto_ + L"*.lua").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            ModFile m;
            m.init = auto_ + fd.cFileName;
            m.stamp = FileStamp(m.init);
            found["autorun/" + Narrow8(fd.cFileName)] = m;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::map<std::string, std::wstring> dirs;
    for (const ModFolder& f : ModFolders()) {  // tw3se\mods\<name>\ and <game>\Mods\<modName>\tw3se\ (modfolders.cpp)
        dirs["mods/" + f.name] = f.dir;
        ModFile m;
        m.dir = f.dir;
        m.init = m.dir + L"\\init.lua";
        if (GetFileAttributesW(m.init.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        m.stamp = DirStamp(m.dir);
        found["mods/" + f.name] = m;
    }
    g_modDirs = dirs;
    for (auto it = found.begin(); it != found.end();) {  // lua_mods_off
        if (LuaModOff(it->first.substr(it->first.find('/') + 1))) {
            static std::set<std::string> told;
            if (told.insert(it->first).second) Log("lua: %s left out (lua_mods_off)", it->first.c_str());
            it = found.erase(it);
        } else {
            ++it;
        }
    }
    return found;
}

bool UnhookId(lua_State* L, int id);  // the hook API below

void UnloadMod(lua_State* L, const std::string& key) {
    int fns = 0;
    for (const char* list : {"tw3se.ticks", "tw3se.draws", "tw3se.ui", "tw3se.reloads", "tw3se.keys", "tw3se.loads",
                             "tw3se.saves"}) fns += RemoveModFns(L, list, key);
    fns += LocTextDropOwner(key);  // its replaced texts
    fns += VUnhookOwner(L, key);   // its virtual hooks
    fns += OffLineOwner(L, key);   // its watched script lines
    fns += FaceClearOwner(key);    // its faces (w3.face) go back to the game
    fns += EngUnhookOwner(L, key);  // its engine hooks (w3.engine_hook)
    fns += PatchRestoreOwner(key);  // its byte patches (w3.patch)
    fns += LatentUnwatchOwner(L, key);  // its latent watches (w3.watch_latent)
    OwnerTable(L, "hooks");
    int o = lua_gettop(L);
    std::vector<int> ids;
    lua_pushnil(L);
    while (lua_next(L, o)) {
        if (lua_isinteger(L, -2) && lua_isstring(L, -1) && key == lua_tostring(L, -1))
            ids.push_back(static_cast<int>(lua_tointeger(L, -2)));
        lua_pop(L, 1);
    }
    for (int id : ids) {
        UnhookId(L, id);
        lua_pushnil(L);
        lua_rawseti(L, o, id);
    }
    lua_pop(L, 1);
    int top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "tw3se.modmodules");  // the modules its require() brought in
    int mm = lua_gettop(L);
    lua_getfield(L, mm, key.c_str());
    int list = lua_gettop(L);
    int mods = 0;
    if (lua_istable(L, list)) {
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "loaded");
        int loaded = lua_gettop(L);
        for (lua_Integer i = 1, n = static_cast<lua_Integer>(lua_rawlen(L, list)); i <= n; ++i) {
            lua_rawgeti(L, list, i);
            lua_pushnil(L);
            lua_settable(L, loaded);  // package.loaded[name] = nil
            ++mods;
        }
    }
    lua_pushnil(L);
    lua_setfield(L, mm, key.c_str());
    lua_settop(L, top);
    {
        // and the modules it required later, inside a function after its init.lua ran (the list above has only
        // what init.lua brought in): named after its folder (a new panel.lua and an old dock.lua stay apart)
        std::string folder = key.substr(key.find_last_of('/') + 1);
        std::string pre = folder + ".";
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "loaded");
        int loaded = lua_gettop(L);
        std::vector<std::string> later;
        lua_pushnil(L);
        while (lua_next(L, loaded)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char* n = lua_tostring(L, -2);
                if (folder == n || strncmp(n, pre.c_str(), pre.size()) == 0) later.push_back(n);
            }
            lua_pop(L, 1);
        }
        for (const std::string& n : later) {
            lua_pushnil(L);
            lua_setfield(L, loaded, n.c_str());
            ++mods;
        }
        lua_settop(L, top);
    }
    int runs = 0;  // its w3.run sequences end with it
    lua_getglobal(L, "w3");
    lua_getfield(L, -1, "_end_runs");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, key.c_str());
        if (lua_pcall(L, 1, 1, 0) == LUA_OK) runs = static_cast<int>(lua_tointeger(L, -1));
    }
    lua_settop(L, top);
    lua_getglobal(L, "w3");  // its entry in F2's mod list (w3.mod); its saved settings stay
    lua_getfield(L, -1, "_mod_drop");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, key.c_str());
        lua_pcall(L, 1, 0, 0);
    }
    lua_settop(L, top);
    lua_getglobal(L, "w3");  // its w3.on functions (the game events)
    lua_getfield(L, -1, "_events_drop");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, key.c_str());
        if (lua_pcall(L, 1, 1, 0) == LUA_OK) fns += static_cast<int>(lua_tointeger(L, -1));
    }
    lua_settop(L, top);
    lua_getfield(L, LUA_REGISTRYINDEX, "tw3se.modexports");  // what its init.lua returned (w3.use)
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, key.c_str());
    }
    lua_settop(L, top);
    for (auto& [used, users] : g_modUsers) users.erase(key);  // it asks again when it loads again
    int sounds = SoundStopOwner(key);
    Log("lua: %s taken out (%d function(s), %zu hook(s), %d module(s), %d sequence(s), %d sound(s))", key.c_str(), fns,
        ids.size(), mods, runs, sounds);
}

bool LoadMod(lua_State* L, const std::string& key, ModFile& m, std::string& err) {
    int top = lua_gettop(L);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    int loaded = lua_gettop(L);
    std::vector<std::string> before;
    lua_pushnil(L);
    while (lua_next(L, loaded)) {
        if (lua_type(L, -2) == LUA_TSTRING) before.emplace_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    bool ok;
    {
        ModScope scope(key);
        m.loading = true;
        ok = RunFile(m.init, err, 1);
        m.loading = false;
    }
    if (ok) {  // loaded without an error: it starts clean (F2, the line over the game)
        g_modErrors.erase(key);
        g_noticed.erase(key);
    } else {
        NoteModError(key, err);  // also a syntax error, which never reaches PCall
    }
    if (ok) {  // what init.lua returned, for w3.use (true when it returned nothing)
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_getfield(L, LUA_REGISTRYINDEX, "tw3se.modexports");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.modexports");
        }
        lua_insert(L, -2);
        lua_setfield(L, -2, key.c_str());
        lua_pop(L, 1);
    }
    lua_newtable(L);  // the modules that came in while it loaded
    int added = lua_gettop(L);
    int n = 0;
    lua_pushnil(L);
    std::string own = key + ":";
    while (lua_next(L, loaded)) {
        // another mod's own modules (a mod it w3.use'd, loaded inside this window) stay that mod's
        const char* name = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        bool other = name && strchr(name, ':') && strncmp(name, own.c_str(), own.size()) != 0;
        if (name && !other && std::find(before.begin(), before.end(), name) == before.end()) {
            lua_pushvalue(L, -2);
            lua_rawseti(L, added, ++n);
        }
        lua_pop(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "tw3se.modmodules");
    lua_pushvalue(L, added);
    lua_setfield(L, -2, key.c_str());
    lua_settop(L, top);
    return ok;
}

void LoadAll(lua_State* L) {
    g_mods = ScanMods();
    for (auto& [key, m] : g_mods) {
        if (m.loaded || !m.err.empty()) continue;  // a mod before it loaded it already (w3.use)
        std::string err;
        m.loaded = LoadMod(L, key, m, err);
        m.err = err;
        if (m.loaded) Log("lua: %s loaded", key.c_str());
        else Log("lua: %s: %s", key.c_str(), err.c_str());
    }
}

// The mods that w3.use a mod load again after it (it loaded again or went away), and theirs after them.
void ReloadUsers(lua_State* L, const std::string& key, int depth = 0) {
    auto u = g_modUsers.find(key);
    if (u == g_modUsers.end() || depth > 8) return;
    std::set<std::string> users = u->second;
    g_modUsers.erase(u);  // each asks again while it loads
    for (const std::string& user : users) {
        auto it = g_mods.find(user);
        if (it == g_mods.end()) continue;
        UnloadMod(L, user);
        std::string err;
        it->second.loaded = LoadMod(L, user, it->second, err);
        it->second.err = err;
        if (it->second.loaded) Log("lua: %s loaded again (it uses %s)", user.c_str(), key.c_str());
        else Log("lua: %s: %s", user.c_str(), err.c_str());
        ReloadUsers(L, user, depth + 1);
    }
}

// w3.use(name): another mod, loaded first if it has not loaded yet, and what its init.lua returned (true when it
// returned nothing). The mod that asks loads again after that one loads again. name: the folder ("modOther") or
// "mods/<folder>".
int LUse(lua_State* L) {
    std::string key = luaL_checkstring(L, 1);
    if (key.find('/') == std::string::npos) key = "mods/" + key;
    std::string me = g_curMod.empty() ? "the console" : g_curMod;
    auto it = g_mods.find(key);
    if (it == g_mods.end())
        return luaL_error(L, "%s needs the mod %s (it is not installed)", me.c_str(), key.c_str() + 5);
    if (key == g_curMod) return luaL_error(L, "w3.use: a mod cannot use itself");
    ModFile& m = it->second;
    if (m.loading) return luaL_error(L, "w3.use: %s and %s use each other", me.c_str(), key.c_str());
    if (!m.loaded) {
        if (!m.err.empty()) return luaL_error(L, "%s needs the mod %s, which failed: %s", me.c_str(), key.c_str() + 5, m.err.c_str());
        std::string err;
        m.loaded = LoadMod(L, key, m, err);
        m.err = err;
        if (!m.loaded) {
            Log("lua: %s: %s", key.c_str(), err.c_str());
            return luaL_error(L, "%s needs the mod %s, which failed: %s", me.c_str(), key.c_str() + 5, err.c_str());
        }
        Log("lua: %s loaded (%s uses it)", key.c_str(), me.c_str());
    }
    if (!g_curMod.empty()) g_modUsers[key].insert(g_curMod);
    lua_getfield(L, LUA_REGISTRYINDEX, "tw3se.modexports");
    if (lua_istable(L, -1)) lua_getfield(L, -1, key.c_str());
    else lua_pushnil(L);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    return 1;
}

// Once a second: a mod whose files changed loads again, a new one loads, a removed one is taken out.
void WatchMods(lua_State* L) {
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (!g_autoreload || now < next) return;
    next = now + 1000;
    std::map<std::string, ModFile> found = ScanMods();
    for (auto& [key, m] : found) {
        auto it = g_mods.find(key);
        if (it != g_mods.end() && it->second.stamp == m.stamp) continue;
        bool again = it != g_mods.end();
        if (again) UnloadMod(L, key);
        ModFile& cur = g_mods[key] = m;
        std::string err;
        cur.loaded = LoadMod(L, key, cur, err);
        cur.err = err;
        if (cur.loaded) Log("lua: %s %s", key.c_str(), again ? "loaded again (a file changed)" : "loaded (new)");
        else Log("lua: %s: %s", key.c_str(), err.c_str());
        if (again) ReloadUsers(L, key);
    }
    std::vector<std::string> gone;
    for (auto it = g_mods.begin(); it != g_mods.end();) {
        if (found.count(it->first)) {
            ++it;
            continue;
        }
        UnloadMod(L, it->first);
        gone.push_back(it->first);
        it = g_mods.erase(it);
    }
    for (const std::string& key : gone) ReloadUsers(L, key);  // they fail now with "needs the mod ..."
}

void CmdLuaMods(const char* arg, char* reply, uint32_t size) {
    std::string a = arg ? arg : "";
    lua_State* L = State();
    if (!L) {
        strncpy_s(reply, size, "error Lua is off", _TRUNCATE);
        return;
    }
    if (a.rfind("reload ", 0) == 0) {
        std::string key = a.substr(7);
        if (key.find('/') == std::string::npos) key = "mods/" + key;
        auto it = g_mods.find(key);
        if (it == g_mods.end()) {
            snprintf(reply, size, "error no mod %s (luamods lists them)", key.c_str());
            return;
        }
        UnloadMod(L, key);
        std::string err;
        it->second.loaded = LoadMod(L, key, it->second, err);
        it->second.err = err;
        ReloadUsers(L, key);
        snprintf(reply, size, "%s %s", key.c_str(), it->second.loaded ? "loaded again" : ("failed: " + err).c_str());
        return;
    }
    std::string s = "lua mods (autoreload " + std::string(g_autoreload ? "on" : "off") + "):";
    for (auto& [key, m] : g_mods) s += " " + key + (m.loaded ? "" : " (failed)");
    strncpy_s(reply, size, s.c_str(), _TRUNCATE);
}

// --- F2's mod manager (lua\lib\modmanager.lua): the lists it shows and the store its settings go to
int LModsList(lua_State* L) {  // w3._mods(): {{key, loaded, error, dir}, ...} - the Lua mods
    lua_newtable(L);
    int i = 0;
    for (auto& [key, m] : g_mods) {
        lua_newtable(L);
        lua_pushstring(L, key.c_str());
        lua_setfield(L, -2, "key");
        lua_pushboolean(L, m.loaded);
        lua_setfield(L, -2, "loaded");
        lua_pushstring(L, m.err.c_str());
        lua_setfield(L, -2, "error");
        auto e = g_modErrors.find(key);  // its last error while it ran (a tick function, a hook ...)
        lua_pushstring(L, e != g_modErrors.end() && m.loaded ? e->second.c_str() : "");
        lua_setfield(L, -2, "last_error");
        lua_pushstring(L, Narrow8(m.dir.empty() ? m.init : m.dir).c_str());
        lua_setfield(L, -2, "path");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}
int LPluginsList(lua_State* L) {  // w3._plugins(): {{name, author, version, file, state}, ...}
    lua_newtable(L);
    int i = 0;
    for (const PluginSummary& p : PluginList()) {
        lua_newtable(L);
        lua_pushstring(L, p.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, p.author.c_str());
        lua_setfield(L, -2, "author");
        char v[32];
        snprintf(v, sizeof v, "%u.%u.%u", p.version >> 16, (p.version >> 8) & 0xff, p.version & 0xff);
        lua_pushstring(L, v);
        lua_setfield(L, -2, "version");
        lua_pushstring(L, p.file.c_str());
        lua_setfield(L, -2, "file");
        lua_pushstring(L, p.state.c_str());
        lua_setfield(L, -2, "state");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}
// w3._tw3se(): TW3SE's version, statement lines on in this session (F2's entry), safe mode (lua_lib.inc then leaves
// the files of the mods' items, option pages and key bindings in place)
int LTw3seInfo(lua_State* L) {
    char v[32];
    snprintf(v, sizeof v, "%d.%d.%d", TW3SE_VERSION_MAJOR, TW3SE_VERSION_MINOR, TW3SE_VERSION_PATCH);
    lua_pushstring(L, v);
    lua_pushboolean(L, ScriptLinesOn());
    lua_pushboolean(L, SafeMode());
    return 3;
}
// w3._menu_key([name]): the TW3SE Menu's key and its default; with a name: set it first (an error for a name
// that is no key)
int LMenuKey(lua_State* L) {
    if (!lua_isnoneornil(L, 1) && !UiSetMenuKey(luaL_checkstring(L, 1)))
        return luaL_error(L, "not a key for the menu: %s", lua_tostring(L, 1));
    lua_pushstring(L, UiMenuKey().c_str());
    lua_pushstring(L, ConfigGet("console_key", "F2").c_str());  // the default: tw3se.config's, else F2
    return 2;
}
int LKeyCapture(lua_State* L) {  // w3._key_capture(on): a key field waits, the menu key leaves the menu open
    UiCaptureKeys(lua_toboolean(L, 1) != 0);
    return 0;
}
int LVarGet(lua_State* L) {  // w3._var_get(key): the stored text, nil when unset
    std::string v;
    if (VarGet(luaL_checkstring(L, 1), v)) lua_pushlstring(L, v.data(), v.size());
    else lua_pushnil(L);
    return 1;
}
int LVarSet(lua_State* L) {  // w3._var_set(key, text|nil) -> true; false over the store's limits
    size_t len = 0;
    const char* s = lua_isnil(L, 2) ? nullptr : luaL_checklstring(L, 2, &len);
    std::string v = s ? std::string(s, len) : std::string();
    lua_pushboolean(L, VarSet(luaL_checkstring(L, 1), s ? &v : nullptr));
    return 1;
}
// w3._list(folder, folders_only): the names in a folder (UTF-8 path), sorted; nil when the folder is missing
int LListDir(lua_State* L) {
    const char* dir = luaL_checkstring(L, 1);
    bool dirsOnly = lua_toboolean(L, 2) != 0;
    wchar_t wdir[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH * 2)) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(wdir) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::string> names;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dirsOnly != isDir) continue;
        names.push_back(Narrow8(n));
    } while (FindNextFileW(h, &fd) && names.size() < 10000);
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

void Reply(char* reply, uint32_t size, const std::string& s);  // below

// luaprof [reset]: where Lua spends its time, per mod and kind, the most first.
void CmdLuaProf(const char* arg, char* reply, uint32_t size) {
    std::string a = arg ? arg : "";
    double secs = (QpcMs() - g_profSince) / 1000.0;
    char head[160];
    snprintf(head, sizeof head, "lua time over %llu tick(s), %.0f s (own time, ms per tick / calls / slowest call):",
             static_cast<unsigned long long>(g_profTicks), secs);
    std::string s = head;
    int shown = 0;
    for (const auto& [key, st] : ProfSorted()) {
        if (++shown > 30) break;
        char line[320];
        snprintf(line, sizeof line, "\n  %-24s %-36s %8.3f %9llu %8.3f", key.first.empty() ? "(none)" : key.first.c_str(),
                 key.second.c_str(), g_profTicks ? st.ms / static_cast<double>(g_profTicks) : 0.0,
                 static_cast<unsigned long long>(st.calls), st.maxMs);
        s += line;
    }
    if (g_prof.empty()) s += " nothing ran yet";
    if (a == "reset") {
        g_prof.clear();
        g_profTicks = 0;
        g_profSince = QpcMs();
        s += "\n(reset)";
    }
    Reply(reply, size, s);
}

void OnTick(float, void*) {
    if (!g_autorunDone) {
        g_autorunDone = true;
        g_autoreload = ConfigGet("lua_autoreload", "on") != "off";
        bool hasAutorun = GetFileAttributesW((Dir() + L"\\lua\\autorun").c_str()) != INVALID_FILE_ATTRIBUTES;
        // a mod folder in tw3se\mods or in <game>\Mods\<mod>\tw3se (the player's zip has neither tw3se\mods nor autorun)
        bool hasMods = GetFileAttributesW((Dir() + L"\\mods").c_str()) != INVALID_FILE_ATTRIBUTES || !ModFolders().empty();
        if (NoLuaMods()) {
            g_autoreload = false;  // the mod watch loads nothing either
            if (State()) ShowNotice("TW3SE safe mode: Lua mods and plugins are off (Shift held at the start)");
            Log("lua: safe mode - the Lua mods stay out");
        } else if ((hasAutorun || hasMods) && State()) {
            LoadAll(g_L);
        }
    }
    if (!g_L) return;
    ArmLimit(g_L);
    DropRunaways(g_L);
    DiscardPending();  // objects w3.new made that nothing holds any more (no Lua running right now)
    ExpirePoseWatches();
    FaceExpire();
    ReviveHooks();
    WatchMods(g_L);
    ++g_profTicks;
    if (g_gameLoaded) {  // a save loaded or a game started: the w3.saved tables fill once the player is there
        lua_getglobal(g_L, "w3");
        lua_getfield(g_L, -1, "_loaded");
        std::string err;
        if (lua_isfunction(g_L, -1)) {
            if (!PCall(g_L, 0, 1, err)) {
                Log("lua: w3.saved: %s", err.c_str());
                g_gameLoaded = false;
            } else {
                if (lua_toboolean(g_L, -1)) g_gameLoaded = false;
                lua_pop(g_L, 1);
            }
        } else {
            lua_pop(g_L, 1);
        }
        lua_pop(g_L, 1);
    }
    RunFns(g_L, "tw3se.keys", "on_key");
    RunFns(g_L, "tw3se.ticks", "on_tick");
    lua_getglobal(g_L, "w3");  // the w3.run sequences
    lua_getfield(g_L, -1, "_step");
    std::string stepErr;
    if (lua_isfunction(g_L, -1)) {
        if (!PCall(g_L, 0, 0, stepErr)) Log("lua: w3.run: %s", stepErr.c_str());
    } else {
        lua_pop(g_L, 1);
    }
    lua_pop(g_L, 1);
    BuildDraws(g_L);  // run on_draw functions on the main thread, hand the command list to the render thread
}

void Reply(char* reply, uint32_t size, const std::string& s) {
    std::string r = s;
    while (!r.empty() && r.back() == '\n') r.pop_back();
    for (char& c : r)
        if (c == '\n') c = '|';  // the pipe protocol is one line
    strncpy_s(reply, size, r.empty() ? "ok" : r.c_str(), _TRUNCATE);
}

// Runs `arg` as Lua: first as an expression (so a value is returned), else as a statement. Returns the printed
// output plus any returned values, or "error: ..." on failure. Shared by the console command and the native.
std::string RunLuaChunk(const char* arg) {
    Crumb("lua: chunk", reinterpret_cast<uintptr_t>(arg));
    DoingScope doing("a Lua chunk (console, WitcherScript or a plugin)");
    lua_State* L = State();
    if (!L) return "lua: no state";
    std::string out, err;
    std::string* prev = g_out;
    g_out = &out;
    int top = lua_gettop(L);
    std::string code = std::string("return ") + arg;
    if (luaL_loadbuffer(L, code.c_str(), code.size(), "=chunk") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, arg, strlen(arg), "=chunk") != LUA_OK) {
            err = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
    }
    if (err.empty() && PCall(L, 0, LUA_MULTRET, err) && lua_gettop(L) > top) out += Joined(L, top + 1);
    lua_settop(L, top);
    g_out = prev;
    return err.empty() ? out : out + "error: " + err;
}

void CmdLua(const char* arg, char* reply, uint32_t size) {
    NoLimit unlimited;
    if (!State() || !arg || !*arg) {
        strncpy_s(reply, size, State() ? "usage: lua <code>" : "lua: no state", _TRUNCATE);
        return;
    }
    Reply(reply, size, RunLuaChunk(arg));
}

// import function TW3SE_Lua(code : string) : string;  -> run Lua from WitcherScript, get its output/result back
void Tw3Lua(void* /*self*/, void* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadScriptParam(frame, buf);
    EndScriptParams(frame);
    const char* code = *reinterpret_cast<char**>(buf);
    std::string out = !g_luaOn ? "error: Lua is off (REDkit editor: not available yet)"
                      : code    ? RunLuaChunk(code)
                                : "error: no code";
    SetScriptResultString(result, out.c_str(), out.size());
}

// import function TW3SE_OnScriptsReloaded(obj : IScriptable, eventName : name);
// After every live reload TW3SE calls obj.eventName() (an event or function without arguments). The same object and
// name again replace the entry, so registering at every load is fine. A destroyed object drops out.
void Tw3OnScriptsReloaded(void* /*self*/, void* frame, void* /*result*/) {
    uint8_t objBuf[16] = {0}, nameBuf[16] = {0};
    ReadScriptParam(frame, objBuf);
    ReadScriptParam(frame, nameBuf);
    EndScriptParams(frame);
    void* block = *reinterpret_cast<void**>(objBuf);  // an object parameter arrives as its handle block, with a ref
    std::string ev = NameOfId(*reinterpret_cast<uint32_t*>(nameBuf));
    lua_State* L = g_luaOn ? State() : nullptr;
    void* obj = Plausible(block) ? Rd<void*>(block, 0x08) : nullptr;
    if (L && obj && !ev.empty()) {
        int top = lua_gettop(L);
        lua_getglobal(L, "w3");
        lua_getfield(L, -1, "_reload_objs");
        if (lua_istable(L, -1)) {
            char key[96];
            sprintf_s(key, "%p|%s", obj, ev.c_str());
            lua_newtable(L);
            PushObject(L, obj, nullptr, false, block);
            lua_setfield(L, -2, "obj");
            lua_pushstring(L, ev.c_str());
            lua_setfield(L, -2, "ev");
            lua_setfield(L, -2, key);
        }
        lua_settop(L, top);
    } else if (!L) {
        Log("TW3SE_OnScriptsReloaded: Lua is off, the event will not run after a reload");
    }
    if (Plausible(block)) ReleaseRef(block);  // the parameter's ref (the Lua entry holds its own)
}

void CmdLuaFile(const char* arg, char* reply, uint32_t size) {
    NoLimit unlimited;
    if (!arg || !*arg || strpbrk(arg, ":/\\") || !State()) {
        strncpy_s(reply, size, "usage: luafile <name>  (runs tw3se\\lua\\<name>.lua)", _TRUNCATE);
        return;
    }
    std::wstring path = Dir() + L"\\lua\\";
    for (const char* c = arg; *c; ++c) path += static_cast<wchar_t>(*c);
    if (path.size() < 4 || path.compare(path.size() - 4, 4, L".lua") != 0) path += L".lua";
    std::string out, err;
    g_out = &out;
    bool ok = RunFile(path, err);
    g_out = nullptr;
    Reply(reply, size, ok ? out : out + "error: " + err);
}

// The test harness, run once before the test files: `test(name, fn)` collects, `eq`/`check`/`near`/`is_error`
// assert. A test file only calls `test(...)`. `luatest [filter]` runs them and reports pass/fail per test.
const char* const kHarness = R"LUA(
_G.T = { tests = {}, order = {} }
function test(name, fn) T.tests[name] = fn; T.order[#T.order + 1] = name end
local function fail(msg) error(msg, 2) end
function check(cond, msg) if not cond then fail(msg or "check failed") end end
function eq(a, b, msg)
  if a ~= b then fail((msg or "eq") .. ": expected " .. tostring(b) .. ", got " .. tostring(a)) end
end
function near(a, b, msg)
  if type(a) ~= "number" or math.abs(a - b) > 1e-3 then fail((msg or "near") .. ": " .. tostring(a) .. " vs " .. tostring(b)) end
end
function is_error(fn, want)  -- the call must raise, and the message contain `want`
  local ok, err = pcall(fn)
  if ok then fail("expected an error") end
  if want and not tostring(err):find(want, 1, true) then fail("error '" .. tostring(err) .. "' lacks '" .. want .. "'") end
end
local function wanted(name, filter)  -- "=name" is one test by its exact name, anything else a part of the name
  if not filter then return true end
  if filter:sub(1, 1) == "=" then return name == filter:sub(2) end
  return name:find(filter, 1, true) ~= nil
end
function T.run(filter)
  local pass, total, lines = 0, 0, {}
  for _, name in ipairs(T.order) do
    if wanted(name, filter) then
      total = total + 1
      local ok, err = pcall(T.tests[name])
      if ok then pass = pass + 1 else lines[#lines + 1] = "FAIL " .. name .. ": " .. tostring(err) end
    end
  end
  return pass, total, table.concat(lines, "\n")
end
)LUA";

void CmdLuaTest(const char* arg, char* reply, uint32_t size) {
    NoLimit unlimited;
    lua_State* L = State();
    if (!L) {
        strncpy_s(reply, size, "luatest: no state", _TRUNCATE);
        return;
    }
    std::string out, err;
    g_out = &out;
    // Fresh harness each run, so a renamed or removed test does not linger.
    if (luaL_dostring(L, kHarness) != LUA_OK) {
        err = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    int files = 0;
    std::wstring dir = Dir() + L"\\lua\\tests\\";
    WIN32_FIND_DATAW fd;
    HANDLE f = err.empty() ? FindFirstFileW((dir + L"*.lua").c_str(), &fd) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::string e;
            if (!RunFile(dir + fd.cFileName, e)) out += "error in a test file: " + e + "\n";
            ++files;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::string summary;
    if (err.empty()) {
        lua_getglobal(L, "T");
        lua_getfield(L, -1, "run");
        if (arg && *arg) lua_pushstring(L, arg);
        else lua_pushnil(L);
        if (PCall(L, 1, 3, err)) {
            lua_Integer pass = lua_tointeger(L, -3), total = lua_tointeger(L, -2);
            const char* fails = lua_tostring(L, -1);
            if (fails && *fails) out += std::string(fails) + "\n";
            summary = "luatest: " + std::to_string(pass) + "/" + std::to_string(total) + " passed (" +
                      std::to_string(files) + " file(s))";
            lua_pop(L, 3);
        }
        lua_pop(L, 1);
    }
    g_out = nullptr;
    Reply(reply, size, err.empty() ? out + summary : out + "error: " + err);
}

}  // namespace

bool ParseKeyName(const std::string& spec, int& vk, int& mods) { return ParseKey(spec, vk, mods); }
void ShowNotice(const std::string& text) { ShowNotice_(text); }
const char* CurrentLuaMod() { return g_curMod.c_str(); }  // crashreport.cpp: the mod that ran when the game faulted

// A script function one of the hook slots has on it (ScriptCallDirect asks for every script call the engine makes
// with the native bit set, which only hooked script functions have).
bool IsHookedScriptFunction(void* fn) {
    for (const HookRec& h : g_hooks)
        if (h.used && !h.parked && h.isScript && h.fn == fn) return true;
    return false;
}

// --- plugins and Lua (interface 16): w3.plugin.<name>(...) calls a plugin's TW3SE_LuaFunction with the arguments as
// text; RunLua runs a chunk for a plugin. Plugins register before Lua exists (they load before the engine starts),
// so the functions wait in g_pluginFns and go into w3.plugin when the state is made.
namespace {
struct PluginFn {
    std::string name;
    TW3SE_LuaFunction fn;
    void* ctx;
};
std::vector<PluginFn> g_pluginFns;
// the reply in bytes a plugin gave during its call (LuaReplyBytes), else none
thread_local std::string* t_replyBytes = nullptr;
thread_local bool t_replyBytesSet = false;

int PluginFnGuarded(TW3SE_LuaFunction fn, int argc, const char* const* argv, char* reply, uint32_t size, void* ctx,
                    bool* faulted) {
    __try {
        return fn(argc, argv, reply, size, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return 0;
    }
}

int LPluginCall(lua_State* L) {  // upvalue 1: the index in g_pluginFns
    size_t i = static_cast<size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    if (i >= g_pluginFns.size() || !g_pluginFns[i].fn) return luaL_error(L, "w3.plugin: the function is not there (its plugin is unloaded)");
    int n = lua_gettop(L);
    std::vector<std::string> args;
    for (int k = 1; k <= n; ++k) {
        args.emplace_back(luaL_tolstring(L, k, nullptr));
        lua_pop(L, 1);
    }
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    char reply[4096] = {};
    bool faulted = false;
    const PluginFn& p = g_pluginFns[i];
    std::string bytes;
    std::string* outer = t_replyBytes;  // (a plugin function may call Lua that calls another)
    bool outerSet = t_replyBytesSet;
    t_replyBytes = &bytes;
    t_replyBytesSet = false;
    int ok = PluginFnGuarded(p.fn, n, argv.data(), reply, sizeof reply, p.ctx, &faulted);
    bool set = t_replyBytesSet;
    t_replyBytes = outer;
    t_replyBytesSet = outerSet;
    if (faulted) return luaL_error(L, "w3.plugin.%s faulted inside the plugin (caught)", p.name.c_str());
    if (!ok) return luaL_error(L, "%s", reply[0] ? reply : "the plugin function failed");
    if (set) lua_pushlstring(L, bytes.data(), bytes.size());
    else lua_pushstring(L, reply);
    return 1;
}

void InstallPluginFn(lua_State* L, size_t i) {  // w3.plugin[name] = the closure
    lua_getglobal(L, "w3");
    lua_getfield(L, -1, "plugin");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "plugin");
    }
    lua_pushinteger(L, static_cast<lua_Integer>(i));
    lua_pushcclosure(L, &LPluginCall, 1);
    lua_setfield(L, -2, g_pluginFns[i].name.c_str());
    lua_pop(L, 2);
}
}  // namespace

int LuaReplyBytes(const void* data, size_t size) {
    if (!t_replyBytes || (size && !data)) return 0;
    t_replyBytes->assign(static_cast<const char*>(data), size);
    t_replyBytesSet = true;
    return 1;
}

int LuaRegisterPluginFunction(const char* name, TW3SE_LuaFunction fn, void* ctx) {
    if (!name || !*name || !fn) return 0;
    size_t i = 0;
    for (; i < g_pluginFns.size() && g_pluginFns[i].name != name; ++i) {}
    if (i == g_pluginFns.size()) g_pluginFns.push_back({name, fn, ctx});
    else g_pluginFns[i] = {name, fn, ctx};  // the same name again replaces it
    if (g_L) InstallPluginFn(g_L, i);
    return 1;
}

void InstallPluginFns(lua_State* L) {
    for (size_t i = 0; i < g_pluginFns.size(); ++i) InstallPluginFn(L, i);
}

// A plugin is unloaded: its Lua functions go (w3.plugin.<name> raises "not there" until it registers again).
int LuaDropPluginFunctionsIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (size_t i = 0; i < g_pluginFns.size(); ++i) {
        uintptr_t f = reinterpret_cast<uintptr_t>(g_pluginFns[i].fn);
        if (!g_pluginFns[i].fn || f < lo || f >= hi) continue;
        g_pluginFns[i].fn = nullptr;
        ++n;
        if (!g_L) continue;
        lua_getglobal(g_L, "w3");
        lua_getfield(g_L, -1, "plugin");
        if (lua_istable(g_L, -1)) {
            lua_pushnil(g_L);
            lua_setfield(g_L, -2, g_pluginFns[i].name.c_str());
        }
        lua_pop(g_L, 2);
    }
    return n;
}

int LuaRunForPlugin(const char* code, char* reply, uint32_t size) {
    std::string r;
    int ok = 0;
    if (!code) r = "error: no code";
    else if (!OnMainThread()) r = "error: RunLua runs on the main thread (RunOnMainThread)";
    else {
        r = g_luaOn ? RunLuaChunk(code) : "error: Lua is off";
        ok = r.rfind("error", 0) == 0 ? 0 : 1;
    }
    if (reply && size) strncpy_s(reply, size, r.c_str(), _TRUNCATE);
    return ok;
}

// --- typed values for plugins (interface 18): Call, GetField/SetField, HookScript and the catalog. A
// TW3SE_Value goes onto the Lua stack and the call runs through the same code as Lua's (argument encoding, out values,
// latent calls, hook chains), so a plugin reaches what Lua reaches. What comes back is copied into a scratch store the
// values point into.
namespace {
struct ApiScratch {  // owns the memory returned values point at
    std::deque<std::string> strings;
    std::deque<std::vector<uint8_t>> bytes;
    std::deque<std::vector<TW3SE_Value>> lists;
    void Clear() { strings.clear(); bytes.clear(); lists.clear(); }
    const char* Str(const char* s, size_t n) { return strings.emplace_back(s, n).c_str(); }
};
ApiScratch g_apiScratch;  // main thread only: the values of the last call
std::string g_apiErr;

void ToValue(lua_State* L, int idx, TW3SE_Value& out, ApiScratch& sc, int depth = 0) {
    idx = lua_absindex(L, idx);
    out = TW3SE_Value{};
    switch (lua_type(L, idx)) {
    case LUA_TNIL: return;
    case LUA_TBOOLEAN: out.type = TW3SE_BOOL; out.v.i = lua_toboolean(L, idx); return;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) { out.type = TW3SE_INT; out.v.i = lua_tointeger(L, idx); }
        else { out.type = TW3SE_FLOAT; out.v.f = lua_tonumber(L, idx); }
        return;
    case LUA_TSTRING: {
        size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        out.type = TW3SE_STRING;
        out.v.s = sc.Str(s, n);
        return;
    }
    case LUA_TTABLE: {
        int n = static_cast<int>(lua_rawlen(L, idx));
        if (depth > 8 || n > 100000) break;
        auto& list = sc.lists.emplace_back(static_cast<size_t>(n));
        for (int k = 1; k <= n; ++k) {
            lua_rawgeti(L, idx, k);
            ToValue(L, -1, list[k - 1], sc, depth + 1);
            lua_pop(L, 1);
        }
        out.type = TW3SE_ARRAY;
        out.count = static_cast<uint32_t>(n);
        out.v.items = list.data();
        return;
    }
    case LUA_TUSERDATA:
        if (auto o = static_cast<Obj*>(luaL_testudata(L, idx, kObj))) {
            if (!o->isStruct) {
                out.type = TW3SE_OBJECT;
                out.v.object = o->ptr;
                return;
            }
            uint32_t size = RttiSize(o->cls);
            auto& b = sc.bytes.emplace_back(size);
            if (size && !SafeRead(o->ptr, b.data(), size)) memset(b.data(), 0, size);
            std::string tn = RttiName(o->cls);
            out.type = TW3SE_STRUCT;
            out.count = size;
            out.typeName = sc.Str(tn.c_str(), tn.size());
            out.v.data = b.data();
            return;
        }
        if (luaL_testudata(L, idx, kArr) && depth <= 8) {  // a live array view: its items, as a list
            lua_getfield(L, idx, "to_table");
            lua_pushvalue(L, idx);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                ToValue(L, -1, out, sc, depth + 1);
                lua_pop(L, 1);
                return;
            }
            lua_pop(L, 1);
        }
        break;
    default: break;
    }
    size_t n = 0;
    const char* s = luaL_tolstring(L, idx, &n);  // a function, a latent call, ...: described
    out.type = TW3SE_OTHER;
    out.v.s = sc.Str(s, n);
    lua_pop(L, 1);
}

// Pushes a plugin's value; false (and g_apiErr) when it cannot be one.
bool PushApiValueRaw(lua_State* L, const TW3SE_Value& v, int depth) {
    switch (v.type) {
    case TW3SE_NIL: lua_pushnil(L); return true;
    case TW3SE_BOOL: lua_pushboolean(L, v.v.i != 0); return true;
    case TW3SE_INT: lua_pushinteger(L, v.v.i); return true;
    case TW3SE_FLOAT: lua_pushnumber(L, v.v.f); return true;
    case TW3SE_STRING: lua_pushstring(L, v.v.s ? v.v.s : ""); return true;
    case TW3SE_OBJECT:
        if (!v.v.object) { lua_pushnil(L); return true; }
        PushObject(L, v.v.object, nullptr, false);
        if (lua_isnil(L, -1)) { g_apiErr = "an OBJECT value is not an object of the game"; return false; }
        return true;
    case TW3SE_STRUCT: {
        void* type = v.typeName ? RttiFindType(v.typeName) : nullptr;
        if (RttiKind(type) != 2) { g_apiErr = std::string("no struct type ") + (v.typeName ? v.typeName : "(typeName NULL)"); return false; }
        if (!v.v.data || v.count != RttiSize(type)) {
            g_apiErr = std::string("a ") + v.typeName + " has " + std::to_string(RttiSize(type)) + " bytes, count says " + std::to_string(v.count);
            return false;
        }
        PushStructCopy(L, v.v.data, type);
        return true;
    }
    case TW3SE_ARRAY:
        if (depth > 8 || (v.count && !v.v.items)) { g_apiErr = "an ARRAY without items, or nested too deep"; return false; }
        lua_createtable(L, static_cast<int>(v.count < 4096 ? v.count : 4096), 0);
        for (uint32_t k = 0; k < v.count; ++k) {
            if (!PushApiValueRaw(L, v.v.items[k], depth + 1)) return false;
            lua_rawseti(L, -2, k + 1);
        }
        return true;
    default:
        g_apiErr = "value type " + std::to_string(v.type) + " cannot be passed";
        return false;
    }
}

bool PushApiValue(lua_State* L, const TW3SE_Value& v) {  // one value more on the stack, or none and false
    int top = lua_gettop(L);
    if (PushApiValueRaw(L, v, 0)) return true;
    lua_settop(L, top);
    return false;
}

bool ApiReady() {
    if (!OnMainThread()) g_apiErr = "runs on the main thread (RunOnMainThread)";
    else if (!g_L || !g_luaOn) g_apiErr = "Lua is off (lua = off in tw3se.config)";
    else return true;
    return false;
}

int ApiGetK(lua_State* L) {  // (obj, key) -> obj[key]
    lua_gettable(L, 1);
    return 1;
}
int ApiSetK(lua_State* L) {  // (obj, key, value): obj[key] = value
    lua_settable(L, 1);
    return 0;
}

// The plugin's script hooks: a C closure in the Lua hook chain per handler (upvalue: the record).
struct PluginHook {
    TW3SE_ScriptHook fn;
    void* ctx;
    int id;
};
std::vector<std::unique_ptr<PluginHook>> g_pluginHooks;
}  // namespace

struct HookCallImpl {  // what a TW3SE_HookCall* points at
    lua_State* L;
    int argsIdx, origIdx;
    std::vector<TW3SE_Value>* args;
    ApiScratch* sc;
};

namespace {
int HookCbGuarded(PluginHook* h, void* self, TW3SE_Value* args, int n, HookCallImpl* call, TW3SE_Value* res, bool* faulted) {
    __try {
        return h->fn(self, args, n, reinterpret_cast<TW3SE_HookCall*>(call), res, h->ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return 0;
    }
}

void WriteBackArgs(HookCallImpl* c) {  // the plugin's changes to args go into the chain's args table
    for (size_t k = 0; k < c->args->size(); ++k)
        if (PushApiValue(c->L, (*c->args)[k])) lua_rawseti(c->L, c->argsIdx, static_cast<lua_Integer>(k + 1));
}

int LPluginHook(lua_State* L) {  // fn(self, args, orig) of the chain; upvalue 1: the PluginHook
    auto h = static_cast<PluginHook*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!h || !h->fn) return 0;
    void* self = nullptr;
    if (auto o = static_cast<Obj*>(luaL_testudata(L, 1, kObj))) self = o->ptr;
    ApiScratch sc;
    std::vector<TW3SE_Value> args(lua_istable(L, 2) ? lua_rawlen(L, 2) : 0);
    for (size_t k = 0; k < args.size(); ++k) {
        lua_rawgeti(L, 2, static_cast<lua_Integer>(k + 1));
        ToValue(L, -1, args[k], sc);
        lua_pop(L, 1);
    }
    HookCallImpl call{L, 2, 3, &args, &sc};
    TW3SE_Value res{};
    bool faulted = false;
    int r = HookCbGuarded(h, self, args.data(), static_cast<int>(args.size()), &call, &res, &faulted);
    if (faulted) {
        Log("plugin hook %d faulted inside the plugin (caught); the chain goes on", h->id);
        return 0;
    }
    WriteBackArgs(&call);
    if (r != 1) return 0;
    if (!PushApiValue(L, res)) return luaL_error(L, "plugin hook %d: result: %s", h->id, g_apiErr.c_str());
    return 1;
}
}  // namespace

int ApiValueCall(void* object, const char* name, const TW3SE_Value* args, int argc, TW3SE_Value* results, int maxResults) {
    g_apiErr.clear();
    if (!name || !*name || argc < 0 || (argc && !args)) { g_apiErr = "no name, or args NULL with argCount > 0"; return -1; }
    if (!ApiReady()) return -1;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    g_apiScratch.Clear();
    ModScope scope("");
    if (object) {
        PushObject(L, object, nullptr, false);
        if (lua_isnil(L, -1)) { lua_settop(L, top); g_apiErr = "not an object of the game"; return -1; }
        lua_pushcfunction(L, &ApiGetK);
        lua_pushvalue(L, -2);
        lua_pushstring(L, name);
        std::string err;
        if (!PCall(L, 2, 1, err)) { lua_settop(L, top); g_apiErr = err; return -1; }
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            g_apiErr = std::string("the object has no method ") + name;
            return -1;
        }
        lua_insert(L, -2);  // fn, obj
    } else {
        lua_getglobal(L, "w3");
        lua_getfield(L, -1, "call");
        lua_remove(L, -2);
        lua_pushstring(L, name);
    }
    int base = lua_gettop(L) - 1;  // the function's slot
    for (int k = 0; k < argc; ++k)
        if (!PushApiValue(L, args[k])) {
            lua_settop(L, top);
            g_apiErr = "argument " + std::to_string(k + 1) + ": " + g_apiErr;
            return -1;
        }
    std::string err;
    if (!PCall(L, lua_gettop(L) - base, LUA_MULTRET, err)) {
        lua_settop(L, top);
        g_apiErr = err;
        return -1;
    }
    int n = lua_gettop(L) - base + 1;
    for (int k = 0; k < n && k < maxResults && results; ++k) ToValue(L, base + k, results[k], g_apiScratch);
    lua_settop(L, top);
    return n;
}

int ApiValueGet(void* object, const char* field, TW3SE_Value* out) {
    g_apiErr.clear();
    if (!object || !field || !out) { g_apiErr = "object, field or out NULL"; return 0; }
    if (!ApiReady()) return 0;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    g_apiScratch.Clear();
    lua_pushcfunction(L, &ApiGetK);
    PushObject(L, object, nullptr, false);
    if (lua_isnil(L, -1)) { lua_settop(L, top); g_apiErr = "not an object of the game"; return 0; }
    lua_pushstring(L, field);
    std::string err;
    bool ok = PCall(L, 2, 1, err);
    if (ok && lua_isnil(L, -1) && !FindProp(ClassOfGuarded(object), field)) {
        ok = false;
        err = std::string("no field ") + field;
    }
    if (ok) ToValue(L, -1, *out, g_apiScratch);
    else g_apiErr = err;
    lua_settop(L, top);
    return ok ? 1 : 0;
}

int ApiValueSet(void* object, const char* field, const TW3SE_Value* value) {
    g_apiErr.clear();
    if (!object || !field || !value) { g_apiErr = "object, field or value NULL"; return 0; }
    if (!ApiReady()) return 0;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    lua_pushcfunction(L, &ApiSetK);
    PushObject(L, object, nullptr, false);
    if (lua_isnil(L, -1)) { lua_settop(L, top); g_apiErr = "not an object of the game"; return 0; }
    lua_pushstring(L, field);
    if (!PushApiValue(L, *value)) { lua_settop(L, top); return 0; }
    std::string err;
    bool ok = PCall(L, 3, 0, err);
    if (!ok) g_apiErr = err;
    lua_settop(L, top);
    return ok ? 1 : 0;
}

void ApiSetError(const char* e) { g_apiErr = e ? e : ""; }  // decal.cpp's reasons for LastError

const char* ApiValueLastError() {
    size_t tb = g_apiErr.find("\nstack traceback:");  // Lua's traceback names only C frames for a plugin's call
    if (tb != std::string::npos) g_apiErr.resize(tb);
    return g_apiErr.c_str();
}

int ApiValueHook(const char* cls, const char* name, TW3SE_ScriptHook fn, void* ctx, int priority) {
    g_apiErr.clear();
    if (!name || !*name || !fn) { g_apiErr = "no name or no hook function"; return 0; }
    if (!ApiReady()) return 0;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    ModScope scope("");  // a plugin's handler belongs to no Lua mod
    auto rec = std::make_unique<PluginHook>(PluginHook{fn, ctx, 0});
    lua_pushcfunction(L, &LHook);
    if (cls && *cls) lua_pushstring(L, cls);
    else lua_pushnil(L);
    lua_pushstring(L, name);
    lua_pushlightuserdata(L, rec.get());
    lua_pushcclosure(L, &LPluginHook, 1);
    lua_pushinteger(L, priority);
    std::string err;
    if (!PCall(L, 4, 1, err)) {
        lua_settop(L, top);
        g_apiErr = err;
        return 0;
    }
    rec->id = static_cast<int>(lua_tointeger(L, -1));
    lua_settop(L, top);
    int id = rec->id;
    g_pluginHooks.push_back(std::move(rec));
    return id;
}

int ApiValueUnhook(int id) {
    if (!OnMainThread() || !g_L) return 0;
    auto it = std::find_if(g_pluginHooks.begin(), g_pluginHooks.end(), [&](const auto& h) { return h->id == id; });
    if (it == g_pluginHooks.end()) return 0;
    UnhookId(g_L, id);
    (*it)->fn = nullptr;  // a chain running right now holds the record; it stays (a few bytes) and does nothing
    g_pluginHooks.erase(it);
    return 1;
}

int ApiValueCallOriginal(TW3SE_HookCall* hc, TW3SE_Value* result) {
    g_apiErr.clear();
    auto c = reinterpret_cast<HookCallImpl*>(hc);
    if (!c || !c->L) { g_apiErr = "no hook call"; return 0; }
    lua_State* L = c->L;
    int top = lua_gettop(L);
    WriteBackArgs(c);
    lua_pushvalue(L, c->origIdx);
    std::string err;
    if (!PCall(L, 0, 1, err)) {
        lua_settop(L, top);
        g_apiErr = err;
        return 0;
    }
    if (result) ToValue(L, -1, *result, *c->sc);
    lua_settop(L, top);
    return 1;
}

// A plugin is unloaded: its script hooks leave their chains.
int LuaDropPluginHooksIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (size_t k = 0; k < g_pluginHooks.size();) {
        uintptr_t f = reinterpret_cast<uintptr_t>(g_pluginHooks[k]->fn);
        if (f >= lo && f < hi) {
            if (g_L) UnhookId(g_L, g_pluginHooks[k]->id);
            g_pluginHooks[k]->fn = nullptr;
            g_pluginHooks.erase(g_pluginHooks.begin() + static_cast<std::ptrdiff_t>(k));
            ++n;
        } else {
            ++k;
        }
    }
    return n;
}

const char* ApiClassName(void* cls) {
    if (!Plausible(cls)) return nullptr;
    std::string n = RttiName(cls);
    if (n.empty()) return nullptr;
    static thread_local std::string s;
    s = n;
    return s.c_str();
}

void* ApiClassParent(void* cls) {
    if (!Plausible(cls)) return nullptr;
    void* p = Rd<void*>(cls, 0x10);
    return Plausible(p) ? p : nullptr;
}

int ItemCbGuarded(TW3SE_ItemCallback cb, const TW3SE_ItemInfo* info, void* ctx) {
    __try {
        return cb(info, ctx) == 1 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int ApiForEachItem(void* cls, bool methods, TW3SE_ItemCallback cb, void* ctx) {
    if (!Plausible(cls) || !cb || RttiKind(cls) != 2) return 0;
    int n = 0;
    ForEachClassItem(cls, methods ? 0x48 : 0x30, [&](void* item, void* owner) {
        std::string name = NameOfId(Rd<uint32_t>(item, 0x10)), type, ownerName = RttiName(owner);
        TW3SE_ItemInfo info{};
        if (methods) {
            void* ret = Rd<void*>(item, 0x20);
            if (Plausible(ret)) type = RttiName(Rd<void*>(ret, 0x08));
            info.flags = Rd<uint32_t>(item, 0x18);
        } else {
            type = RttiName(Rd<void*>(item, 0x08));
            info.offset = Rd<uint32_t>(item, 0x20);
            info.flags = Rd<uint32_t>(item, 0x24);
        }
        info.name = name.c_str();
        info.typeName = type.c_str();
        info.ownerClass = ownerName.c_str();
        info.item = item;
        ++n;
        return ItemCbGuarded(cb, &info, ctx) != 0;  // 1 stops, a fault too
    });
    return n;
}

int ObjectCbGuarded(TW3SE_ObjectCallback cb, void* o, void* ctx) {
    __try {
        return cb(o, ctx) == 1 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int ApiForEachObject(const char* className, TW3SE_ObjectCallback cb, void* ctx) {
    if (!className || !cb || !OnMainThread()) return 0;
    void* cls = RttiFindType(className);
    if (RttiKind(cls) != 2) return 0;
    std::vector<void*> objs = ObjectsOf(cls, SIZE_MAX);  // a snapshot: the callback may make or destroy objects
    int n = 0;
    for (void* o : objs) {
        ++n;
        if (ObjectCbGuarded(cb, o, ctx)) break;
    }
    return n;
}

int ApiEnumValue(const char* enumName, const char* valueName, int64_t* out) {
    if (!enumName || !valueName || !out) return 0;
    void* t = RttiFindType(enumName);
    if (RttiKind(t) != 1 && RttiKind(t) != 9) return 0;
    int32_t v = 0;
    if (!RttiEnumValue(t, valueName, &v)) return 0;
    *out = v;
    return 1;
}

// For ui.cpp: a Lua chunk from the in-game console, and the w3.on_ui functions inside ImGui's frame.
std::string LuaRun(const char* code) { return g_luaOn ? RunLuaChunk(code) : "error: Lua is off"; }

void LuaRunUi() {
    if (!g_L) return;
    g_uiFrame = true;
    g_uiTables = 0;
    ImGuiErrorRecoveryState state;
    RunFns(g_L, "tw3se.ui", "on_ui", [&] { ImGui::ErrorRecoveryStoreState(&state); },
           [&](bool ok) { if (!ok) ImGui::ErrorRecoveryTryToRecoverState(&state); });  // a failing window: closed
    g_uiFrame = false;
}

// F2's mod manager (lua\lib\modmanager.lua) inside the main window's tab bar: require('lib.modmanager')[fn](). An
// error shows in the tab and goes to the log once per message; ImGui's state is put back as it was.
void LuaUiLib(const char* fn) {
    if (!g_L) {
        ImGui::TextDisabled("Lua is off (lua = off in tw3se.config)");
        return;
    }
    lua_State* L = g_L;
    int top = lua_gettop(L);
    g_uiFrame = true;
    g_uiTables = 0;
    ImGuiErrorRecoveryState state;
    ImGui::ErrorRecoveryStoreState(&state);
    std::string err;
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib.modmanager");
    bool ok = PCall(L, 1, 1, err);
    if (ok) {
        lua_getfield(L, -1, fn);
        ok = lua_isfunction(L, -1) ? PCall(L, 0, 0, err) : (err = std::string("lib.modmanager has no ") + fn, false);
    }
    g_uiFrame = false;
    lua_settop(L, top);
    if (!ok) {
        ImGui::ErrorRecoveryTryToRecoverState(&state);
        static std::string last;
        if (err != last) Log("ui: mod manager (%s): %s", fn, err.c_str());
        last = err;
        ImGui::TextColored(ImVec4(1.0f, 0.47f, 0.43f, 1.0f), "%s", err.c_str());
    }
}

void QueueLuaNatives() {
    for (int i = 0; i < kArgSlots; ++i) {
        char name[24];
        sprintf_s(name, "TW3SE_LuaArg%d", i);
        QueueNative(name, kLuaArgNatives[i], 1);
    }
    QueueNative("TW3SE_LuaSink", &LuaSink, 1);
    QueueNative("TW3SE_Lua", &Tw3Lua, 1);  // WitcherScript -> Lua: import function TW3SE_Lua(code:string):string;
    QueueNative("TW3SE_OnScriptsReloaded", &Tw3OnScriptsReloaded, 2);  // an object's event after each live reload
}

// The scripts are compiled again (a live reload, or the editor's Reload scripts). Main thread, the old functions
// still exist. Everything Lua holds that points into them goes: the lookup caches, the LuaArg originals, the hooks
// (parked, and put back on the new functions on the next tick or the next w3.hook).
void LuaScriptsRecompiling() {
    ++g_scriptGen;
    g_propCache.clear();
    g_methodCache.clear();
    g_argFnOrig.clear();
    ParkHooks();
    g_hooksParked = true;
}

// After a reload pass: the hooks go back on now (the next tick would do it too).
void LuaScriptsReloaded() { ReviveHooks(); }

void LuaGameLoaded() { g_gameLoaded = true; }

// The live reload is over (hotreload.cpp, main thread): the registered events and w3.on_reload functions run.
void LuaScriptsReloadDone() {
    if (!g_L) return;
    lua_State* L = g_L;
    int top = lua_gettop(L);
    lua_getglobal(L, "w3");
    lua_getfield(L, -1, "_reloaded");
    std::string err;
    if (lua_isfunction(L, -1) && PCall(L, 0, 1, err)) Log("reload: after the reload ran %s", lua_tostring(L, -1));
    else if (!err.empty()) Log("reload: after-reload events failed: %s", err.c_str());
    lua_settop(L, top);
    RunFns(L, "tw3se.reloads", "on_reload");
}

// The files in tw3se\lua\natives\ run now, at the game start: their w3.native calls queue natives the scripts find
// when they load. The engine is not up yet, so these files only declare natives.
void RunEarlyNativesUnguarded();

bool RunEarlyNativesGuarded() {  // a fault this early (an engine part not up yet) must not end the game start
    __try {
        RunEarlyNativesUnguarded();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RunEarlyNatives() {
    if (!RunEarlyNativesGuarded()) Log("lua: a fault while running tw3se\\lua\\natives at the start - those natives are off");
}

const char* const kScriptCore =
#include "script_core.inc"
    ;

// Safe mode: plugins and the natives\*.lua of Lua mods do not run, but a WitcherScript mod that imports one of their
// natives must still compile (otherwise the game ends itself).
// Each such native becomes one that does nothing (the script gets its default value): the ones of the last start
// without safe mode (scripting.cpp keeps them) and the names in w3.native("Name", ...) of the mods' natives files.
bool StubNative(const std::string& name) {
    lua_State* L = State();
    if (!L || name.empty() || NativeKnown(name)) return false;
    int top = lua_gettop(L);
    lua_pushcfunction(L, &LNative);
    lua_pushstring(L, name.c_str());
    luaL_loadstring(L, "return nil");
    bool ok = lua_pcall(L, 2, 0, 0) == LUA_OK;
    lua_settop(L, top);
    return ok;
}

int StubNativesIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"*.lua").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static const std::regex kNative(R"(w3\.native\s*\(\s*["']([A-Za-z_][A-Za-z0-9_]*)["'])");
    int n = 0;
    do {
        std::ifstream in(dir + fd.cFileName, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (std::sregex_iterator it(text.begin(), text.end(), kNative), end; it != end; ++it)
            n += StubNative((*it)[1].str()) ? 1 : 0;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

void StubModNatives(const std::vector<std::wstring>& natDirs, bool pluginsOut) {
    ModScope scope("safe mode");
    int n = 0;
    // the last full start's list holds the plugins' natives too: only when the plugins stay out (a plugin that loads
    // registers its own afterwards); the Lua mods' that run are known by now and skipped
    if (pluginsOut)
        for (const std::string& name : ModNativesLastTime()) n += StubNative(name) ? 1 : 0;
    for (const std::wstring& d : natDirs) n += StubNativesIn(d);
    if (n) Log("lua: safe mode - %d native(s) of plugins and Lua mods answer nothing, so the scripts that import them compile", n);
}

bool LuaModsLeftOut() { return g_luaLeftOut; }

void RunEarlyNativesUnguarded() {
    if (lua_State* L = State()) {  // TW3SE's own script library first (J3)
        ModScope scope("tw3se");
        int top = lua_gettop(L);
        std::string err;
        if (luaL_loadbuffer(L, kScriptCore, strlen(kScriptCore), "=tw3se script library") != LUA_OK) {
            Log("lua: the script library does not load: %s", lua_tostring(L, -1));
        } else if (!PCall(L, 0, 0, err)) {
            Log("lua: the script library failed: %s", err.c_str());
        }
        lua_settop(L, top);
    }
    // tw3se\lua\natives\*.lua, then the natives\*.lua of each mod folder (as that mod: its require finds its modules)
    std::vector<std::pair<std::string, std::wstring>> dirs = {{"", Dir() + L"\\lua\\natives\\"}};
    std::vector<std::wstring> safeDirs;
    for (const ModFolder& m : ModFolders()) {
        g_modDirs["mods/" + m.name] = m.dir;
        if (!NoLuaMods() && !LuaModOff(m.name)) dirs.push_back({"mods/" + m.name, m.dir + L"\\natives\\"});  // a Lua mod's part: out in safe mode
        else safeDirs.push_back(m.dir + L"\\natives\\");
    }
    for (const auto& [mod, dir] : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"*.lua").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        if (State()) {
            do {
                std::string err;
                ModScope scope(mod.empty() ? "natives/" + Narrow8(fd.cFileName) : mod);
                const char* sep = mod.empty() ? "" : "/";
                if (!RunFile(dir + fd.cFileName, err))
                    Log("lua: natives %s%s%ls: %s", mod.c_str(), sep, fd.cFileName, err.c_str());
                else
                    Log("lua: natives %s%s%ls", mod.c_str(), sep, fd.cFileName);
            } while (FindNextFileW(f, &fd));
        }
        FindClose(f);
    }
    if (NoLuaMods() || NoPlugins() || !safeDirs.empty()) StubModNatives(safeDirs, NoPlugins());
}

void InstallLua() {
    QueueLuaNatives();
    RunEarlyNatives();  // tw3se\lua\natives\*.lua: w3.native before the scripts load
    g_luaOn = true;
    RegisterTick(&OnTick, nullptr);
    RegisterCommand("lua", &CmdLua, "lua <code>: run Lua against the game (w3.player(), w3.call, w3.enum ...)");
    RegisterCommand("luafile", &CmdLuaFile, "luafile <name>: run tw3se\\lua\\<name>.lua");
    RegisterCommand("luamods", &CmdLuaMods, "luamods [reload <name>]: the Lua mods; reload takes one out and loads it again");
    RegisterCommand("luaprof", &CmdLuaProf, "luaprof [reset]: Lua time per mod and kind (on_tick, hooks, natives ...)");
    RegisterCommand("luatest", &CmdLuaTest, "luatest [filter|=name]: run the Lua tests in tw3se\\lua\\tests\\ (=name: one test)");
}

std::string LuaRecentErrors() {
    std::string out;
    for (const std::string& e : g_recentErrors) out += e + "\r\n\r\n";
    return out.empty() ? "(none)\r\n" : out;
}

}  // namespace tw3se

// --- a fault inside a C function Lua calls (deps\lua\src\ldo.c, callC_guarded). The filter takes what a guard may
// take (GuardableFault: the faults the game's crash handler lets through to TW3SE's guards) and notes where it was;
// the error then ends that one call, and the Lua code above it sees an ordinary error with the place in it. Unguarded,
// the fault would leave through every Lua frame (a Lua error is a C++ exception here, a fault is not), and the tick
// would drop Lua for the rest of the session.
namespace {
struct LuaFault {
    DWORD code;
    uintptr_t at, access, kind;
};
thread_local LuaFault t_luaFault;
std::atomic<int> g_luaFaults{0};
}  // namespace

int tw3se_fault_filter(struct _EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep ? ep->ExceptionRecord : nullptr;
    if (!r || !tw3se::GuardableFault(r->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;  // a Lua error, a yield
    t_luaFault = {r->ExceptionCode, reinterpret_cast<uintptr_t>(r->ExceptionAddress), 0, 0};
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        t_luaFault.kind = r->ExceptionInformation[0];  // 0 read, 1 write, 8 execute
        t_luaFault.access = r->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void tw3se_fault_error(lua_State* L) {
    const LuaFault f = t_luaFault;
    lua_Debug ar{};
    std::string fn = "a TW3SE function";
    if (lua_getstack(L, 0, &ar) && lua_getinfo(L, "n", &ar) && ar.name) fn = std::string("'") + ar.name + "'";
    char what[96];
    if (f.code == EXCEPTION_ACCESS_VIOLATION)
        snprintf(what, sizeof what, "a bad %s at 0x%llx", f.kind == 1 ? "write" : f.kind == 8 ? "jump" : "read",
                 static_cast<unsigned long long>(f.access));
    else
        snprintf(what, sizeof what, "exception 0x%08lx", f.code);
    std::string msg = fn + " stopped on " + what + " in " + tw3se::AddressPlace(f.at) + "; the game goes on";
    int n = g_luaFaults.fetch_add(1) + 1;
    if (n <= 20 || n % 100 == 0)
        tw3se::Log("lua: fault #%d%s%s: %s", n, tw3se::g_curMod.empty() ? "" : " in ",
                   tw3se::g_curMod.c_str(), msg.c_str());
    lua_pushstring(L, msg.c_str());
    lua_error(L);
}
