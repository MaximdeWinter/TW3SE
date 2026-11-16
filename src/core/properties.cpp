// TW3SE core - generic RTTI property access (read/write any reflected field of an object).
//
// Based on docs/engine/scripting.md section 8 (confirmed on build
// 0x6abd8695). A CClass carries its own properties at +0x30 (CProperty* array) / +0x38 (count) and its parent
// class at +0x10; the full inherited set is the union walking the parent chain. A CProperty stores the field's
// byte offset inside an instance at +0x20 and its name's CName id at +0x10. So reading/writing a field is:
// find the CProperty by name (walk parents, scan by id), then object + offset. This needs no engine call - it
// is a plain memory walk against those offsets, which keeps it independent of CDPR's internal TDynArray format.
//
// What this does not resolve yet: how to obtain an object's CClass* at runtime (object -> class) or a class by
// name (CRTTISystem::FindClass final address). Until one of those lands, a plugin must already hold a CClass*
// (e.g. known from a hook). Tracked with 6a.
#include "core.h"

namespace tw3se {
namespace {
using NamePoolGet_t = void* (*)();
using NamePoolAdd_t = uint32_t (*)(void*, const char*);
using GetRtti_t = void* (*)();
using GetClass_t = void* (*)(void* object);         // vtable slot 1: IScriptable::GetClass
using FindType_t = void* (*)(void* rtti, const void* name);  // CRTTISystem::FindType(rtti, CName*)
}  // namespace

// The CClass* of an engine object, via the virtual GetClass() at vtable slot 1 (IScriptable::GetClass, shared
// by CObject and IScriptable, final-confirmed). The virtual call (rather than the hard-wired object+0x18 field)
// stays correct even if a subclass ever overrode it. Returns null for a null object.
void* GetObjectClass(void* object) {
    if (!object) return nullptr;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!vtable || !vtable[1]) return nullptr;
    return reinterpret_cast<GetClass_t>(vtable[1])(object);
}

// A CClass* looked up by class name, via CRTTISystem::FindType (the engine's generic RTTI-type name lookup).
// FindType also resolves enums and simple types ("Float"/"Vector"/...), so the result is validated to actually
// be a class via CClass::GetKind (vtable slot 4, returns 2 for a class); a non-class name yields null rather
// than a wrongly-typed pointer. Null if the name is unknown.
void* FindClassByName(const char* name) {
    if (!name) return nullptr;
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto findType = Fn<FindType_t>("CRTTISystem::FindType");
    if (!getRtti || !poolGet || !poolAdd || !findType) return nullptr;
    uint32_t id = poolAdd(poolGet(), name);  // a CName is just this id; its address serves as CName*
    void* type = findType(getRtti(), &id);
    if (!type) return nullptr;
    void** vtable = *reinterpret_cast<void***>(type);  // GetKind at slot 4 returns 2 for a class
    if (!vtable || !vtable[4] || reinterpret_cast<int (*)(void*)>(vtable[4])(type) != 2) return nullptr;
    return type;
}

// Finds a property (incl. inherited) of `cclass` by name. Returns the CProperty* or null. Walks the parent
// chain (CClass+0x10) and scans each class's own property array (CClass+0x30 ptr / +0x38 count), comparing
// CProperty+0x10 (name id) - the same lookup pattern as FindGlobalFunction, over the property list.
void* FindProperty(void* cclass, const char* name) {
    if (!cclass || !name) return nullptr;
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    if (!poolGet || !poolAdd) return nullptr;
    uint32_t id = poolAdd(poolGet(), name);
    for (char* c = reinterpret_cast<char*>(cclass); c; c = *reinterpret_cast<char**>(c + 0x10)) {
        void** props = *reinterpret_cast<void***>(c + 0x30);
        uint32_t n = *reinterpret_cast<uint32_t*>(c + 0x38);
        if (!props) continue;
        for (uint32_t i = 0; i < n; ++i) {
            char* p = reinterpret_cast<char*>(props[i]);
            if (p && *reinterpret_cast<uint32_t*>(p + 0x10) == id) return p;
        }
    }
    return nullptr;
}

// A predefined WitcherScript global (thePlayer, theGame, ...) by its field offset in GScriptGlobals (6a,
// docs/engine/world.md). Two-stage: the field points to a Handle wrapper, the live object sits at wrapper+0x08.
// Returns null when the global is not populated yet (e.g. thePlayer in the main menu / before a spawn).
void* GetScriptGlobal(uint32_t fieldOffset) {
    uintptr_t addr = Address("GScriptGlobals");
    if (!addr) return nullptr;
    // Address() gives the location of the pointer variable (like GGame), so deref once to reach the struct
    // base, then the field -> Handle wrapper, then the live object at wrapper+0x08 (6a, world.md; the missing
    // first deref was the crash). Three dereferences total.
    void* base = *reinterpret_cast<void**>(addr);
    if (!base) return nullptr;
    void* wrapper = *reinterpret_cast<void**>(reinterpret_cast<char*>(base) + fieldOffset);
    if (!wrapper) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(wrapper) + 0x08);
}
void* GetThePlayer() { return GetScriptGlobal(0x28); }
void* GetTheGame() { return GetScriptGlobal(0x20); }

// The real object inside a Handle { refCount@0x00, flags@0x04, object@0x08 } return value (6a). An object-
// or component-returning game function hands back such a handle, not a raw pointer; call this before using
// the result as an object. Null for a null handle.
void* ResolveHandle(void* handle) {
    return handle ? *reinterpret_cast<void**>(reinterpret_cast<char*>(handle) + 0x08) : nullptr;
}

// The string behind a CName id (6a, scripting.md section 5): CName ids are a running counter, and the pool
// keeps a flat, insertion-indexed array (CNamesPool.entries) whose node at +0x00 holds the char*. So id -> name
// is a direct array index, no hashing. Bounds-checked against CNamesPool.entryCount. Returns null if out of
// range or unavailable. Lets us dump real property names (property name id is at CProperty+0x10).
const char* GetCNameString(uint32_t id) {
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    if (!poolGet) return nullptr;
    void* pool = poolGet();
    if (!pool) return nullptr;
    uintptr_t entriesOff = Address("CNamesPool.entries");
    uintptr_t countOff = Address("CNamesPool.entryCount");
    if (!entriesOff) return nullptr;
    if (countOff) {
        uint32_t count = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(pool) + countOff);
        if (id >= count) return nullptr;
    }
    void** entries = *reinterpret_cast<void***>(reinterpret_cast<char*>(pool) + entriesOff);
    if (!entries) return nullptr;
    void* node = entries[id];
    return node ? *reinterpret_cast<const char**>(node) : nullptr;
}

// Total number of properties of `cclass` including inherited ones (walks the parent chain, sums each class's
// CClass+0x38 count). A diagnostic to confirm, live, that a class and its property list resolved sensibly.
uint32_t CountProperties(void* cclass) {
    uint32_t total = 0;
    for (char* c = reinterpret_cast<char*>(cclass); c; c = *reinterpret_cast<char**>(c + 0x10))
        total += *reinterpret_cast<uint32_t*>(c + 0x38);
    return total;
}

// Address of a property's value inside `object`. For a C++-imported field the value sits at object + offset
// (CProperty+0x20). For a WitcherScript-declared `var` (CProperty+0x24 bit 0x20 set) the script instance data
// lives in a separate block the object points to at +0x20, so the value is *(object+0x20) + offset
// (CScriptedClass::ForEachPropertyPair; docs/engine/scripting.md section 8). The caller reads/writes it as the
// field's known C++ type (int/float 4 bytes, bool 1, handle 8, string pointer first). Null for null inputs.
void* PropertyPtr(void* object, void* cproperty) {
    if (!object || !cproperty) return nullptr;
    char* prop = reinterpret_cast<char*>(cproperty);
    uint32_t off = *reinterpret_cast<uint32_t*>(prop + 0x20);
    uint8_t flags = *reinterpret_cast<uint8_t*>(prop + 0x24);
    char* base = reinterpret_cast<char*>(object);
    if (flags & 0x20) {  // script var: redirect through the script-instance data block at object+0x20
        base = *reinterpret_cast<char**>(base + 0x20);
        if (!base) return nullptr;
    }
    return base + off;
}

}  // namespace tw3se
