// The PhysX scene under the game's physics. PhysX 3 is linked into witcher3.exe with its RTTI,
// so the scene is found by type: the world holds a CPhysicsWorldPhysXImpl, which holds the physx::NpScene. The
// scene's methods are PxScene's virtual functions; their slot numbers come from the code, not from a list: the slot
// that names "PxScene::setCCDMaxPasses()" in its warning is setCCDMaxPasses, and PxScene declares setGravity,
// getGravity, setBounceThresholdVelocity, getBounceThresholdVelocity right before it (read in 5.01: slots 196
// to 200; setGravity takes const PxVec3&, getGravity returns PxVec3 through a hidden pointer).
#include "core.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace tw3se {
namespace {

bool Plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7fffffffffffULL && (v & 7) == 0;
}

std::string TypeOf(void* obj) {  // MSVC RTTI type name of a polymorphic object, "" when it has none
    char name[96] = {};
    uintptr_t vt = 0, col = 0;
    uint32_t tdRva = 0;
    if (!obj || !SafeRead(obj, &vt, 8) || vt < ImageBase() || !SafeRead(reinterpret_cast<void*>(vt - 8), &col, 8) ||
        !SafeRead(reinterpret_cast<void*>(col + 12), &tdRva, 4) ||
        !SafeRead(reinterpret_cast<void*>(ImageBase() + tdRva + 0x10), name, sizeof name - 1))
        return "";
    return name;
}

// the first pointer field in [obj, obj + span) whose object has the RTTI type `want`
void* FieldOfType(void* obj, size_t span, const char* want) {
    for (size_t off = 0; off < span; off += 8) {
        void* p = nullptr;
        if (!SafeRead(static_cast<char*>(obj) + off, &p, 8) || !Plausible(p)) continue;
        if (TypeOf(p) == want) return p;
    }
    return nullptr;
}

size_t ImageSize() {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(ImageBase());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(ImageBase() + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

const char* FindText(const char* text) {  // a string in the game's image
    const char* base = reinterpret_cast<const char*>(ImageBase());
    size_t n = ImageSize(), len = strlen(text);
    for (size_t i = 0; i + len < n; ++i) {
        MEMORY_BASIC_INFORMATION mi;
        if ((i & 0xfff) == 0 && (!VirtualQuery(base + i, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
                                 (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) {
            i += 0xfff;
            continue;
        }
        if (base[i] == text[0] && memcmp(base + i, text, len) == 0) return base + i;
    }
    return nullptr;
}

// setCCDMaxPasses: the function that loads the warning text (a lea to it in the code)
const uint8_t* CcdFunction() {
    static const uint8_t* fn = nullptr;
    static bool done = false;
    if (done) return fn;
    done = true;
    const char* text = FindText("PxScene::setCCDMaxPasses() not allowed");
    if (!text) return nullptr;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(ImageBase());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(ImageBase() + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        const uint8_t* lo = reinterpret_cast<const uint8_t*>(ImageBase() + sec->VirtualAddress);
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 7 <= n; ++i) {
            if ((lo[i] != 0x48 && lo[i] != 0x4c) || lo[i + 1] != 0x8d || (lo[i + 2] & 0xc7) != 0x05) continue;
            int32_t disp;
            memcpy(&disp, lo + i + 3, 4);
            if (reinterpret_cast<const char*>(lo + i + 7) + disp != text) continue;
            // back to the function's start: the int3 padding before it (the lea sits 0x20 in, 5.01)
            size_t k = i;
            while (k > 0 && i - k < 0x200 && !(lo[k - 1] == 0xcc && (k < 2 || lo[k - 2] == 0xcc))) --k;
            fn = lo + k;
            return fn;
        }
    }
    return nullptr;
}

int g_ccdSlot = -1;  // setCCDMaxPasses in NpScene's vtable

int CcdSlot(void** vt) {  // the slot of setCCDMaxPasses in this vtable, -1 when it is not NpScene's
    const uint8_t* fn = CcdFunction();
    if (!fn) return -1;
    if (g_ccdSlot >= 0) {
        void* f = nullptr;
        return SafeRead(&vt[g_ccdSlot], &f, 8) && f == fn ? g_ccdSlot : -1;
    }
    for (int s = 8; s < 400; ++s) {
        void* f = nullptr;
        if (!SafeRead(&vt[s], &f, 8)) break;
        if (f == fn) return g_ccdSlot = s;
    }
    return -1;
}

// the first pointer field in [obj, obj + span) to an NpScene (PhysX has no RTTI locators: known by its vtable)
void* SceneField(void* obj, size_t span) {
    for (size_t off = 0; off < span; off += 8) {
        void* p = nullptr;
        void** vt = nullptr;
        if (!SafeRead(static_cast<char*>(obj) + off, &p, 8) || !Plausible(p) || !SafeRead(p, &vt, 8)) continue;
        if (reinterpret_cast<uintptr_t>(vt) < ImageBase() || reinterpret_cast<uintptr_t>(vt) >= ImageBase() + ImageSize())
            continue;
        if (CcdSlot(vt) >= 0) return p;
    }
    return nullptr;
}

}  // namespace

std::string ObjectTypeName(void* obj) { return TypeOf(obj); }
void* ObjectFieldOfType(void* obj, size_t span, const char* type) { return FieldOfType(obj, span, type); }

// The world's PhysX scene: CWorld -> CPhysicsWorldPhysXImpl -> NpScene (fields within the first bytes of each).
// A world has more than one physics world (Kaer Morhen: +0x1f0 and +0x1f8, the scene in the second): each
// is tried.
void* PhysxScene(void* world) {
    if (!Plausible(world)) return nullptr;
    for (size_t at = 0; at < 0x2000; at += 8) {
        void* pw = nullptr;
        if (!SafeRead(static_cast<char*>(world) + at, &pw, 8) || !Plausible(pw)) continue;
        if (TypeOf(pw) != ".?AVCPhysicsWorldPhysXImpl@@") continue;
        if (void* s = SceneField(pw, 0x4000)) return s;
        for (size_t off = 0; off < 0x800; off += 8) {  // one step further in
            void* p = nullptr;
            if (SafeRead(static_cast<char*>(pw) + off, &p, 8) && Plausible(p))
                if (void* s = SceneField(p, 0x400)) return s;
        }
    }
    return nullptr;
}

// gravity of the scene (set: the new one first); "" or the reason
std::string PhysxGravity(void* scene, float* xyz, bool set) {
    void** vt = nullptr;
    if (!SafeRead(scene, &vt, 8) || !vt) return "no scene";
    int s = CcdSlot(vt);
    if (s < 4) return "PxScene::setGravity was not found in this build";
    using Set_t = void (*)(void*, const float*);
    using Get_t = float* (*)(void*, float*);
    if (set) reinterpret_cast<Set_t>(vt[s - 4])(scene, xyz);
    float out[3] = {};
    reinterpret_cast<Get_t>(vt[s - 3])(scene, out);
    memcpy(xyz, out, sizeof out);
    return "";
}

}  // namespace tw3se
