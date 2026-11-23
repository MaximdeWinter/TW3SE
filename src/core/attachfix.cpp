// The attachment list fix (20.11., config `attach_fix = on`, off by default: the crash it was made for came all the
// same in the test with it on - kept for the next search): a node keeps its attachments in a singly linked
// list of 16-byte cells {value, next}. A worker thread breaks a skinning attachment (CMeshSkinningAttachment, vtable
// slot 44 -> the node's slot 55 -> the list's remove) while the main thread makes a hard attachment on the same node
// (CHardAttachment slot 43 -> slot 54 -> the list's append), and neither takes a lock. The remove frees its cell first
// and unlinks it after; an append walking the list meanwhile hangs its new cell onto the freed one. That cell stays in
// the list, the next append writes its `next` into freed memory: the game's 16-byte heap gets a free list 0xe off,
// and seconds or minutes later an allocation hands out a crooked pointer (PhysX's allocator stops at int 3,
// witcher3.exe+0x1e88295) or a walk reads garbage (witcher3.exe+0x4be2d5). It shows after a change of upscaler or
// frame generation, when the rebuild re-attaches many meshes at once. Found with `quarantine = small` (a reader of
// a poisoned cell, the free 141 s before on a worker thread).
// The fix: the two list functions run under one lock - one thread at a time changes such a list.
#include "core.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace tw3se {
namespace {

using ListFn = uintptr_t (*)(void* list, void* value);
ListFn oRemove = nullptr, oAppend = nullptr;
CRITICAL_SECTION g_lock;
std::atomic<uint64_t> g_calls{0}, g_waited{0};

void Enter() {
    if (!TryEnterCriticalSection(&g_lock)) {  // another thread is in a list right now: this is the case it fixes
        ++g_waited;
        EnterCriticalSection(&g_lock);
    }
    ++g_calls;
}

uintptr_t RemoveHook(void* list, void* value) {
    Enter();
    uintptr_t r = oRemove(list, value);
    LeaveCriticalSection(&g_lock);
    return r;
}

uintptr_t AppendHook(void* list, void* value) {
    Enter();
    uintptr_t r = oAppend(list, value);
    LeaveCriticalSection(&g_lock);
    return r;
}

// once a minute in the log while it happens: how often two threads met at such a list
void WaitedTick(float, void*) {
    static ULONGLONG last = 0;
    static uint64_t logged = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 60000) return;
    last = now;
    uint64_t w = g_waited.load();
    if (w == logged) return;
    Log("attach fix: %llu time(s) a thread waited for another at an attachment list (%llu calls)",
        static_cast<unsigned long long>(w - logged), static_cast<unsigned long long>(g_calls.load()));
    logged = w;
}

}  // namespace

void InstallAttachFix() {
    if (!ConfigBool("attach_fix", false)) return;
    void* rm = reinterpret_cast<void*>(Address("AttachList::Remove"));
    void* ap = reinterpret_cast<void*>(Address("AttachList::Append"));
    if (!rm || !ap) {
        Log("attach fix: the attachment list's functions not found in this build - off");
        return;
    }
    InitializeCriticalSectionAndSpinCount(&g_lock, 4000);
    if (!Hook(rm, reinterpret_cast<void*>(&RemoveHook), reinterpret_cast<void**>(&oRemove)) ||
        !Hook(ap, reinterpret_cast<void*>(&AppendHook), reinterpret_cast<void**>(&oAppend))) {
        Log("attach fix: not hooked - off");
        return;
    }
    RegisterTick(&WaitedTick, nullptr);
    Log("attach fix: on (a node's attachment list changed by one thread at a time)");
}

}  // namespace tw3se
