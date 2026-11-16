// Hooks on virtual methods: one slot of a vtable points at a stub of TW3SE's.
//
// The types of a virtual method's arguments are unknown, so the stub keeps all four argument registers, integer
// (rcx, rdx, r8, r9) and float (xmm0-xmm3), in a block on its stack and hands that block to the handler, with a pointer
// to the caller's stack arguments. The original runs through a second stub that loads the block back into the
// registers (and copies eight stack arguments) before the call. Its xmm0 result goes back into the block; the handler's
// value goes back in rax and xmm0 both, so a method that returns a float gets it too. Both stubs have unwind info
// (RtlAddFunctionTable), so a fault in the original unwinds through them into a TW3SE guard.
#include "core.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace tw3se {
namespace {

struct VHook {
    void** slot = nullptr;   // the vtable entry
    void* original = nullptr;
    VHandler handler = nullptr;
    void* user = nullptr;
    uint8_t* stub = nullptr;
    bool used = false;
};
constexpr int kMaxVHooks = 64;
VHook g_vh[kMaxVHooks];
std::mutex g_vhMx;

uint8_t* g_block = nullptr;  // one executable block: the call stub, then a stub per hook; unwind info at its end
constexpr size_t kBlock = 0x4000, kStubSize = 0x60, kCallStubOff = 0, kFirstStubOff = 0x80, kUnwindOff = 0x3000;
using CallOrigFn = uint64_t (*)(VRegs*, void*, uint64_t*);
CallOrigFn g_callOrig = nullptr;

uint64_t Entry(VRegs* regs, int id, uint64_t* stackArgs) {
    VHook& h = g_vh[id];
    if (!h.used || !h.handler) return VHookCallOriginal(id, regs, stackArgs);
    return h.handler(id, regs, stackArgs, h.user);
}

void Put(std::vector<uint8_t>& b, std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes); }
void Put64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

// uint64 CallOrig(VRegs* regs /*rcx*/, void* target /*rdx*/, uint64* stackArgs /*r8*/)
std::vector<uint8_t> CallStub() {
    std::vector<uint8_t> b;
    Put(b, {0x53, 0x56, 0x57, 0x55});                    // push rbx, rsi, rdi, rbp
    Put(b, {0x48, 0x89, 0xE5});                          // mov rbp, rsp           (end of prologue part: 7)
    Put(b, {0x48, 0x83, 0xEC, 0x68});                    // sub rsp, 0x68          (11)
    Put(b, {0x48, 0x89, 0xCB});                          // mov rbx, rcx
    Put(b, {0x48, 0x89, 0xD0});                          // mov rax, rdx
    Put(b, {0x4C, 0x89, 0xC6});                          // mov rsi, r8
    Put(b, {0x48, 0x8D, 0x7C, 0x24, 0x20});              // lea rdi, [rsp+0x20]
    Put(b, {0xB9, 0x08, 0x00, 0x00, 0x00});              // mov ecx, 8
    Put(b, {0xF3, 0x48, 0xA5});                          // rep movsq: eight stack arguments
    Put(b, {0x48, 0x8B, 0x0B});                          // mov rcx, [rbx]
    Put(b, {0x48, 0x8B, 0x53, 0x08});                    // mov rdx, [rbx+8]
    Put(b, {0x4C, 0x8B, 0x43, 0x10});                    // mov r8, [rbx+0x10]
    Put(b, {0x4C, 0x8B, 0x4B, 0x18});                    // mov r9, [rbx+0x18]
    Put(b, {0xF3, 0x0F, 0x7E, 0x43, 0x20});              // movq xmm0, [rbx+0x20]
    Put(b, {0xF3, 0x0F, 0x7E, 0x4B, 0x28});              // movq xmm1, [rbx+0x28]
    Put(b, {0xF3, 0x0F, 0x7E, 0x53, 0x30});              // movq xmm2, [rbx+0x30]
    Put(b, {0xF3, 0x0F, 0x7E, 0x5B, 0x38});              // movq xmm3, [rbx+0x38]
    Put(b, {0xFF, 0xD0});                                // call rax
    Put(b, {0x66, 0x0F, 0xD6, 0x43, 0x20});              // movq [rbx+0x20], xmm0: a float result
    Put(b, {0x48, 0x89, 0xEC});                          // mov rsp, rbp
    Put(b, {0x5D, 0x5F, 0x5E, 0x5B});                    // pop rbp, rdi, rsi, rbx
    Put(b, {0xC3});                                      // ret
    return b;
}

// The entry stub of hook `id`: the registers into a VRegs on the stack, Entry(regs, id, stackArgs), rax into xmm0.
std::vector<uint8_t> EntryStub(int id) {
    std::vector<uint8_t> b;
    Put(b, {0x48, 0x83, 0xEC, 0x78});                    // sub rsp, 0x78          (prologue: 4)
    Put(b, {0x48, 0x89, 0x4C, 0x24, 0x28});              // mov [rsp+0x28], rcx
    Put(b, {0x48, 0x89, 0x54, 0x24, 0x30});              // mov [rsp+0x30], rdx
    Put(b, {0x4C, 0x89, 0x44, 0x24, 0x38});              // mov [rsp+0x38], r8
    Put(b, {0x4C, 0x89, 0x4C, 0x24, 0x40});              // mov [rsp+0x40], r9
    Put(b, {0x66, 0x0F, 0xD6, 0x44, 0x24, 0x48});        // movq [rsp+0x48], xmm0
    Put(b, {0x66, 0x0F, 0xD6, 0x4C, 0x24, 0x50});        // movq [rsp+0x50], xmm1
    Put(b, {0x66, 0x0F, 0xD6, 0x54, 0x24, 0x58});        // movq [rsp+0x58], xmm2
    Put(b, {0x66, 0x0F, 0xD6, 0x5C, 0x24, 0x60});        // movq [rsp+0x60], xmm3
    Put(b, {0x48, 0x8D, 0x4C, 0x24, 0x28});              // lea rcx, [rsp+0x28]: the VRegs
    Put(b, {0xBA});                                      // mov edx, id
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(id >> (i * 8)));
    Put(b, {0x4C, 0x8D, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00});  // lea r8, [rsp+0xa0]: the caller's fifth argument on
    Put(b, {0x48, 0xB8});                                // mov rax, Entry
    Put64(b, reinterpret_cast<uint64_t>(&Entry));
    Put(b, {0xFF, 0xD0});                                // call rax
    Put(b, {0x66, 0x48, 0x0F, 0x6E, 0xC0});              // movq xmm0, rax
    Put(b, {0x48, 0x83, 0xC4, 0x78});                    // add rsp, 0x78
    Put(b, {0xC3});                                      // ret
    return b;
}

bool MakeBlock() {
    if (g_block) return true;
    g_block = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_block) return false;
    memset(g_block, 0xCC, kBlock);
    std::vector<uint8_t> c = CallStub();
    memcpy(g_block + kCallStubOff, c.data(), c.size());
    g_callOrig = reinterpret_cast<CallOrigFn>(g_block + kCallStubOff);
    // unwind info: [0] the call stub (pushes rbx rsi rdi rbp, rbp = rsp, 0x68 more), [1] every entry stub (0x78)
    uint8_t* u = g_block + kUnwindOff;
    const uint8_t callInfo[] = {0x01, 11, 6, 0x05,                 // version 1, prologue 11 bytes, 6 codes, frame rbp+0
                                11, 0xC2, 7, 0x03, 4, 0x50, 3, 0x70, 2, 0x60, 1, 0x30};
    memcpy(u, callInfo, sizeof callInfo);
    const uint8_t entryInfo[] = {0x01, 4, 1, 0x00, 4, 0xE2, 0x00, 0x00};  // sub rsp, 0x78: UWOP_ALLOC_SMALL 14
    memcpy(u + 0x20, entryInfo, sizeof entryInfo);
    auto* rf = reinterpret_cast<RUNTIME_FUNCTION*>(u + 0x40);
    rf[0] = {static_cast<DWORD>(kCallStubOff), static_cast<DWORD>(kCallStubOff + c.size()), static_cast<DWORD>(kUnwindOff)};
    for (int i = 0; i < kMaxVHooks; ++i) {
        DWORD start = static_cast<DWORD>(kFirstStubOff + i * kStubSize);
        rf[1 + i] = {start, start + static_cast<DWORD>(kStubSize), static_cast<DWORD>(kUnwindOff + 0x20)};
    }
    if (!RtlAddFunctionTable(rf, 1 + kMaxVHooks, reinterpret_cast<DWORD64>(g_block)))
        Log("vhook: unwind info not registered - a fault in a hooked method ends the game");
    return true;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, 8);
    return true;
}

}  // namespace

int VHookInstall(void* vtable, int slotIndex, VHandler handler, void* user, std::string& err) {
    std::lock_guard<std::mutex> l(g_vhMx);
    if (!MakeBlock()) {
        err = "no memory for the stubs";
        return -1;
    }
    if (slotIndex < 0 || slotIndex > 1024) {
        err = "slot out of range";
        return -1;
    }
    void** slot = static_cast<void**>(vtable) + slotIndex;
    void* original = nullptr;
    if (!SafeRead(slot, &original, 8) || !original) {
        err = "the vtable slot cannot be read";
        return -1;
    }
    for (const VHook& h : g_vh)
        if (h.used && h.slot == slot) {
            err = "this slot is hooked already";
            return -1;
        }
    int id = -1;
    for (int i = 0; i < kMaxVHooks; ++i)
        if (!g_vh[i].used) {
            id = i;
            break;
        }
    if (id < 0) {
        err = "all 64 virtual hooks are in use";
        return -1;
    }
    VHook& h = g_vh[id];
    h.stub = g_block + kFirstStubOff + id * kStubSize;
    std::vector<uint8_t> s = EntryStub(id);
    if (s.size() > kStubSize) {
        err = "stub too big";
        return -1;
    }
    memcpy(h.stub, s.data(), s.size());
    FlushInstructionCache(GetCurrentProcess(), h.stub, kStubSize);
    h.slot = slot;
    h.original = original;
    h.handler = handler;
    h.user = user;
    h.used = true;
    if (!WriteSlot(slot, h.stub)) {
        h.used = false;
        err = "the vtable is not writable";
        return -1;
    }
    return id;
}

bool VHookRemove(int id) {
    std::lock_guard<std::mutex> l(g_vhMx);
    if (id < 0 || id >= kMaxVHooks || !g_vh[id].used) return false;
    VHook& h = g_vh[id];
    WriteSlot(h.slot, h.original);
    h.handler = nullptr;  // a call already inside the stub goes to the original (Entry checks)
    h.used = false;
    return true;
}

uint64_t VHookCallOriginal(int id, VRegs* regs, uint64_t* stackArgs) {
    void* target = (id >= 0 && id < kMaxVHooks) ? g_vh[id].original : nullptr;
    if (!target || !g_callOrig) return 0;
    return g_callOrig(regs, target, stackArgs);
}

void* VHookOriginal(int id) { return (id >= 0 && id < kMaxVHooks) ? g_vh[id].original : nullptr; }

}  // namespace tw3se
