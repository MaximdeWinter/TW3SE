// TW3SE core - main-loop stack sampler (diagnostics, off unless `stacksample = on` in tw3se.config).
//
// Finds the per-frame call chain of a build TW3SE has no tick entry for yet (the REDkit editor): a thread
// suspends the main thread at intervals, unwinds its stack and counts the return addresses inside the exe. A frame
// that is on the stack in most samples belongs to the main loop. Every 5 s a block goes to out\mainloop_stacks.txt:
// the exe's frames seen in at least 10 % of the samples, ordered from the bottom of the stack up, and a few whole
// stacks as examples.
//
// While the main thread is suspended this thread calls nothing that could take a lock the main thread might hold:
// the unwind tables (.pdata) of all modules are read beforehand, the lookup is a binary search over that copy, and
// RtlVirtualUnwind itself takes no locks. Reads of the stack are guarded.
#include "core.h"
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace tw3se {
namespace {

struct Module {
    uintptr_t base = 0, end = 0;
    const RUNTIME_FUNCTION* pdata = nullptr;
    size_t count = 0;
};

std::vector<Module> g_mods;
DWORD g_mainId = 0;

void LoadModules() {
    g_mods.clear();
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        auto base = reinterpret_cast<uintptr_t>(mods[i]);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        Module m;
        m.base = base;
        m.end = base + nt->OptionalHeader.SizeOfImage;
        m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
        m.count = dir.VirtualAddress ? dir.Size / sizeof(RUNTIME_FUNCTION) : 0;
        g_mods.push_back(m);
    }
}

const Module* ModuleOf(uintptr_t pc) {
    for (const Module& m : g_mods)
        if (pc >= m.base && pc < m.end) return &m;
    return nullptr;
}

PRUNTIME_FUNCTION Lookup(const Module& m, uintptr_t pc) {
    auto rva = static_cast<DWORD>(pc - m.base);
    size_t lo = 0, hi = m.count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION& f = m.pdata[mid];
        if (rva < f.BeginAddress) hi = mid;
        else if (rva >= f.EndAddress) lo = mid + 1;
        else return const_cast<PRUNTIME_FUNCTION>(&f);
    }
    return nullptr;
}

// Unwinds a suspended thread's context into pcs (innermost first). No C++ objects here (SEH).
int Unwind(CONTEXT* ctx, uintptr_t* pcs, int max) {
    int n = 0;
    __try {
        while (n < max && ctx->Rip) {
            pcs[n++] = ctx->Rip;
            const Module* m = ModuleOf(ctx->Rip);
            if (!m) break;
            PRUNTIME_FUNCTION f = Lookup(*m, ctx->Rip);
            if (!f) {  // a leaf function: the return address is on top of the stack
                if (!MemReadable(reinterpret_cast<void*>(ctx->Rsp), 8)) break;  // VirtualQuery: a syscall, no locks
                ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
                ctx->Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, ctx->Rip, f, ctx, &handlerData, &establisher, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

int Sample(HANDLE thread, uintptr_t* pcs, int max) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    int n = GetThreadContext(thread, &ctx) ? Unwind(&ctx, pcs, max) : 0;
    ResumeThread(thread);
    return n;
}

DWORD WINAPI SamplerThread(void*) {
    Sleep(15000);  // past the start-up
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainId);
    if (!thread) {
        Log("stacksample: main thread %lu not opened (%lu)", g_mainId, GetLastError());
        return 0;
    }
    const uintptr_t exeBase = ImageBase();
    std::wstring path = Dir() + L"\\out\\mainloop_stacks.txt";
    CreateDirectoryW((Dir() + L"\\out").c_str(), nullptr);
    // stacksample_min: the share of samples a frame needs to be listed (percent; 1 finds calls that are short but
    // frequent, like the entity attaches while a world loads)
    const int minPct = (std::max)(1, std::atoi(ConfigGet("stacksample_min", "10").c_str()));
    Log("stacksample: sampling the main thread (frames from %d %%), blocks go to out\\mainloop_stacks.txt", minPct);
    for (int window = 0; window < 120; ++window) {  // 10 minutes
        LoadModules();
        const Module* exe = ModuleOf(exeBase);
        uintptr_t exeEnd = exe ? exe->end : exeBase;
        constexpr int kSamples = 200;
        struct Stat {
            int count = 0;
            long long depthSum = 0;  // distance from the bottom of the stack, summed
        };
        std::map<uint32_t, Stat> stats;
        std::vector<std::vector<uint32_t>> examples;
        int taken = 0;
        for (int s = 0; s < kSamples; ++s) {
            uintptr_t pcs[96];
            int n = Sample(thread, pcs, 96);
            if (n > 0) {
                ++taken;
                std::vector<uint32_t> mine;
                std::map<uint32_t, bool> seen;
                for (int i = n - 1; i >= 0; --i) {  // bottom first
                    if (pcs[i] < exeBase || pcs[i] >= exeEnd) continue;
                    auto rva = static_cast<uint32_t>(pcs[i] - exeBase);
                    mine.push_back(rva);
                    if (seen[rva]) continue;
                    seen[rva] = true;
                    Stat& st = stats[rva];
                    ++st.count;
                    st.depthSum += n - 1 - i;
                }
                if (s % (minPct < 10 ? 10 : 40) == 0) examples.push_back(mine);
            }
            Sleep(25);
        }
        std::vector<std::pair<uint32_t, Stat>> rows;
        for (auto& [rva, st] : stats)
            if (st.count * 100 >= taken * minPct) rows.push_back({rva, st});
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
            return a.second.depthSum * b.second.count < b.second.depthSum * a.second.count;  // mean depth
        });
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"a") == 0 && f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(f, "== %02d:%02d:%02d window %d, %d samples\n", t.wHour, t.wMinute, t.wSecond, window, taken);
            for (auto& [rva, st] : rows)
                fprintf(f, "  rva %08x  in %3d%%  depth %.1f\n", rva, taken ? st.count * 100 / taken : 0,
                        static_cast<double>(st.depthSum) / st.count);
            for (auto& ex : examples) {
                fprintf(f, "  stack:");
                for (uint32_t rva : ex) fprintf(f, " %x", rva);
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }
    CloseHandle(thread);
    Log("stacksample: done");
    return 0;
}

}  // namespace

void StartStackSampler() {
    if (!ConfigBool("stacksample", false)) return;
    g_mainId = GetCurrentThreadId();  // called from the start-up, which runs on the main thread
    if (HANDLE t = CreateThread(nullptr, 0, &SamplerThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace tw3se
