std::string Where(const Session& s, uint64_t a);
std::wstring Newest(const Session& s, const std::wstring& rel);

struct RFunc {
    uint32_t begin, end, unwind;
};

struct ModNames {
    bool built = false;
    std::vector<RFunc> funcs;
    std::map<uint32_t, std::string> virt;
    std::map<uint32_t, uint32_t> virtCount;
    std::map<uint32_t, std::string> given;
};

std::map<uint64_t, ModNames> g_names;
std::wstring g_selfDir;
std::wstring g_namesFile;

struct MemSource {
    virtual ~MemSource() {}
    virtual size_t Read(uint64_t at, void* out, size_t n) = 0;
    virtual int Query(uint64_t at, MEMORY_BASIC_INFORMATION* mb) = 0;
};
MemSource* g_mem = nullptr;

size_t ReadSome(HANDLE proc, uint64_t at, void* out, size_t n) {
    if (g_mem) return g_mem->Read(at, out, n);
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<void*>(at), out, n, &got)) {
        MEMORY_BASIC_INFORMATION mb{};
        if (got == 0 && VirtualQueryEx(proc, reinterpret_cast<void*>(at), &mb, sizeof mb) && mb.State == MEM_COMMIT) {
            uint64_t end = reinterpret_cast<uint64_t>(mb.BaseAddress) + mb.RegionSize;
            if (end > at && end - at < n) ReadProcessMemory(proc, reinterpret_cast<void*>(at), out, end - at, &got);
        }
    }
    return got;
}
bool Rpm(HANDLE proc, uint64_t at, void* out, size_t n) { return ReadSome(proc, at, out, n) == n; }

std::string Undecorate(const std::string& d) {
    std::string s = d;
    if (s.rfind(".?AV", 0) == 0 || s.rfind(".?AU", 0) == 0) s = s.substr(4);
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, "@@") == 0) s.resize(s.size() - 2);
    if (s.find('?') != std::string::npos || s.find('$') != std::string::npos) return s;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('@', i);
        if (e == std::string::npos) e = s.size();
        if (e > i) parts.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    std::string r;
    for (size_t k = parts.size(); k-- > 0;) r += parts[k] + (k ? "::" : "");
    return r.empty() ? d : r;
}

struct Section {
    uint32_t rva, size, flags;
};

bool Sections(HANDLE proc, uint64_t base, std::vector<Section>& out, IMAGE_DATA_DIRECTORY* exc) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!Rpm(proc, base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !Rpm(proc, base + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return false;
    if (exc) *exc = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    uint64_t at = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections && i < 96; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!Rpm(proc, at + i * sizeof sh, &sh, sizeof sh)) return false;
        out.push_back({sh.VirtualAddress, std::max(sh.Misc.VirtualSize, sh.SizeOfRawData), sh.Characteristics});
    }
    return true;
}

std::string ClassOfCol(HANDLE proc, const Module& m, uint64_t col) {
    if (col < m.base || col + 24 > m.base + m.size) return {};
    uint32_t c[6];
    if (!Rpm(proc, col, c, sizeof c) || c[0] != 1 || c[5] != col - m.base || !c[3] || c[3] >= m.size) return {};
    char name[256] = {};
    ReadSome(proc, m.base + c[3] + 0x10, name, sizeof name - 1);
    name[sizeof name - 1] = 0;
    if (name[0] != '.' || name[1] != '?') return {};
    return Undecorate(name);
}

void BuildNames(HANDLE proc, const Module& m, ModNames& n) {
    n.built = true;
    std::vector<Section> secs;
    IMAGE_DATA_DIRECTORY exc{};
    if (!Sections(proc, m.base, secs, &exc)) return;
    if (exc.VirtualAddress && exc.Size >= 12 && exc.Size < (64u << 20)) {
        n.funcs.resize(exc.Size / 12);
        if (!Rpm(proc, m.base + exc.VirtualAddress, n.funcs.data(), n.funcs.size() * 12)) n.funcs.clear();
        std::sort(n.funcs.begin(), n.funcs.end(), [](const RFunc& a, const RFunc& b) { return a.begin < b.begin; });
    }
    auto isCode = [&](uint64_t va) {
        if (va < m.base || va >= m.base + m.size) return false;
        uint32_t r = static_cast<uint32_t>(va - m.base);
        for (const auto& s : secs)
            if ((s.flags & IMAGE_SCN_MEM_EXECUTE) && r >= s.rva && r < s.rva + s.size) return true;
        return false;
    };
    for (const auto& s : secs) {
        if ((s.flags & IMAGE_SCN_MEM_EXECUTE) || (s.flags & IMAGE_SCN_MEM_WRITE) || !(s.flags & IMAGE_SCN_MEM_READ)) continue;
        if (s.size > (256u << 20)) continue;
        std::vector<uint64_t> q(s.size / 8);
        if (q.empty() || !Rpm(proc, m.base + s.rva, q.data(), q.size() * 8)) continue;
        std::map<uint64_t, std::string> cols;
        for (size_t i = 0; i + 1 < q.size(); ++i) {
            uint64_t col = q[i];
            if (col < m.base + s.rva || col >= m.base + s.rva + s.size || (col & 3) || !isCode(q[i + 1])) continue;
            auto it = cols.find(col);
            if (it == cols.end()) it = cols.emplace(col, ClassOfCol(proc, m, col)).first;
            if (it->second.empty()) continue;
            for (size_t k = i + 1, slot = 0; k < q.size() && slot < 2048 && isCode(q[k]); ++k, ++slot) {
                uint32_t f = static_cast<uint32_t>(q[k] - m.base);
                if (!n.virt.count(f)) n.virt[f] = it->second + "::vf" + std::to_string(slot);
                ++n.virtCount[f];
            }
        }
    }
    if (!g_namesFile.empty() && Lower(BaseName(m.path)).find(L".exe") != std::wstring::npos) {
        std::string t = ReadFileText(g_namesFile);
        size_t i = 0;
        while (i < t.size()) {
            size_t e = t.find('\n', i);
            std::string line = t.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? t.size() : e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            char* end = nullptr;
            unsigned long long rva = strtoull(line.c_str(), &end, 16);
            if (!end || *end != ' ' || !rva || rva >= m.size) continue;
            n.given[static_cast<uint32_t>(rva)] = end + 1;
        }
    }
}

ModNames& NamesOf(const Session& s, const Module& m) {
    ModNames& n = g_names[m.base];
    if (!n.built && (s.proc || g_mem)) {
        if (g_namesFile.empty()) g_namesFile = Newest(s, L"\\tw3se\\out\\names.txt");
        BuildNames(s.proc, m, n);
    }
    return n;
}

uint32_t FuncStart(const Session& s, const Module& m, uint32_t rva) {
    ModNames& n = NamesOf(s, m);
    auto it = std::upper_bound(n.funcs.begin(), n.funcs.end(), rva, [](uint32_t v, const RFunc& f) { return v < f.begin; });
    if (it == n.funcs.begin()) return 0;
    --it;
    if (rva >= it->end) return 0;
    RFunc f = *it;
    for (int depth = 0; depth < 8; ++depth) {
        unsigned char ui[4];
        if (!Rpm(s.proc, m.base + (f.unwind & ~1u), ui, 4)) break;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        uint32_t codes = (ui[2] + 1u) & ~1u;
        RFunc parent{};
        if (!Rpm(s.proc, m.base + (f.unwind & ~1u) + 4 + codes * 2, &parent, sizeof parent) || parent.begin >= f.begin) break;
        f = parent;
    }
    return f.begin;
}

std::string FuncName(const Session& s, uint64_t a) {
    const Module* m = ModuleAt(s, a);
    if (!m || !m->loaded || m->size <= 0x1000 || (!s.proc && !g_mem)) return {};
    uint32_t rva = static_cast<uint32_t>(a - m->base);
    uint32_t start = FuncStart(s, *m, rva);
    ModNames& n = NamesOf(s, *m);
    if (!start) {
        uint32_t best = 0;
        auto below = [&](const std::map<uint32_t, std::string>& names) {
            auto it = names.upper_bound(rva);
            if (it != names.begin() && std::prev(it)->first > best) best = std::prev(it)->first;
        };
        below(n.virt);
        below(n.given);
        if (!best || rva - best > 0x800) return {};
        auto f = std::upper_bound(n.funcs.begin(), n.funcs.end(), best, [](uint32_t v, const RFunc& x) { return v < x.begin; });
        if (f != n.funcs.end() && f->begin <= rva) return {};
        if (f != n.funcs.begin() && std::prev(f)->end > best) return {};
        std::vector<unsigned char> code(rva - best);
        if (!code.empty() && Rpm(s.proc, m->base + best, code.data(), code.size()))
            for (size_t i = 0; i + 1 < code.size(); ++i)
                if ((code[i] == 0xc3 && code[i + 1] == 0xcc) || (code[i] == 0xcc && code[i + 1] == 0xcc)) return {};
        start = best;
    }
    auto g = n.given.find(start);
    if (g != n.given.end()) return Fmt("%s+0x%x", g->second.c_str(), rva - start);
    auto v = n.virt.find(start);
    if (v != n.virt.end()) {
        uint32_t more = n.virtCount[start] - 1;
        return Fmt("%s+0x%x%s", v->second.c_str(), rva - start, more ? Fmt(", in %u more vtables", more).c_str() : "");
    }
    return Fmt("function at +0x%x, +0x%x into it", start, rva - start);
}

std::string ClassAt(const Session& s, uint64_t p) {
    uint64_t vt = 0, col = 0;
    if (!Rpm(s.proc, p, &vt, 8)) return {};
    const Module* m = ModuleAt(s, vt);
    if (!m || !m->loaded || !Rpm(s.proc, vt - 8, &col, 8)) return {};
    return ClassOfCol(s.proc, *m, col);
}

bool Canonical(uint64_t a) { return a < 0x0000800000000000ull || a >= 0xffff800000000000ull; }

std::string MemoryAt(const Session& s, uint64_t a) {
    MEMORY_BASIC_INFORMATION mb{};
    if (g_mem) {
        int q = g_mem->Query(a, &mb);
        if (q < 0) return "no memory there";
        if (q == 0) {
            uint64_t probe;
            return g_mem->Read(a, &probe, 8) == 8 ? std::string() : "not saved in the dump (unknown)";
        }
    } else if (!VirtualQueryEx(s.proc, reinterpret_cast<void*>(a), &mb, sizeof mb)) {
        return "no memory there";
    }
    if (mb.State == MEM_FREE) return "no memory there (never given out, or given back)";
    if (mb.State == MEM_RESERVE) return "reserved, not in use (memory given back, or not yet taken)";
    if (mb.Protect & PAGE_GUARD) return "a guard page";
    if (mb.Protect & PAGE_NOACCESS) return "in use, but locked (no access)";
    return {};
}

std::string Describe(const Session& s, uint64_t v) {
    if (v == 0) return "0";
    if (v < 0x10000) return "a small number (as a pointer: null plus an offset)";
    if (v == 0xffffffffull || v == 0xffffffffffffffffull) return "-1 (often: no index, an invalid id)";
    if (!Canonical(v)) {
        uint32_t hi = static_cast<uint32_t>(v >> 32);
        float f;
        memcpy(&f, &hi, 4);
        if (std::isfinite(f) && std::fabs(f) > 1e-6f && std::fabs(f) < 1e12f && (hi & 0xffff) == 0)
            return Fmt("no address (bytes of data, the upper half reads as the float %g)", f);
        return "no address (bytes of data in a pointer's place)";
    }
    if (const Module* m = ModuleAt(s, v)) {
        uint64_t col = 0;
        std::string cls = Rpm(s.proc, v - 8, &col, 8) ? ClassOfCol(s.proc, *m, col) : std::string();
        return cls.empty() ? Where(s, v) : Where(s, v) + ", the vtable of class " + cls;
    }
    std::string mem = MemoryAt(s, v);
    if (!mem.empty()) return mem;
    std::string cls = ClassAt(s, v);
    if (!cls.empty()) return "an object of class " + cls;
    uint64_t q = 0;
    if (Rpm(s.proc, v, &q, 8)) {
        std::string inner;
        if (q && Canonical(q) && q >= 0x10000 && !ModuleAt(s, q) && MemoryAt(s, q).empty()) {
            std::string c2 = ClassAt(s, q);
            if (!c2.empty()) inner = ", it holds a pointer to an object of class " + c2;
        }
        return Fmt("memory in use (first 8 bytes %016llx%s)", static_cast<unsigned long long>(q), inner.c_str());
    }
    return "memory in use";
}

std::string SourceOf(const Session& s, const uint64_t* regs, const char* const* rn, int i) {
    for (int j = 0; j < 16; ++j) {
        if (j == i || j == 7 || regs[j] < 0x10000 || !Canonical(regs[j]) || ModuleAt(s, regs[j])) continue;
        uint64_t q[33];
        size_t got = ReadSome(s.proc, regs[j], q, sizeof q);
        for (size_t k = 0; k < got / 8; ++k)
            if (q[k] == regs[i]) {
                std::string cls = ClassAt(s, regs[j]);
                return Fmt(", read from [%s+0x%zx]%s", rn[j], k * 8,
                           cls.empty() ? "" : (" (an object of class " + cls + ")").c_str());
            }
    }
    return {};
}

std::string Verdict(const Session& s, const EXCEPTION_RECORD& er, const CONTEXT& c, bool haveCtx, uint64_t commitFree) {
    std::vector<std::string> out;
    DWORD code = er.ExceptionCode;
    uint64_t rip = reinterpret_cast<uint64_t>(er.ExceptionAddress);
    const uint64_t regs[16] = {c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp,
                               c.R8,  c.R9,  c.R10, c.R11, c.R12, c.R13, c.R14, c.R15};
    static const char* rn[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                 "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    if (!FuncName(s, rip).empty()) out.push_back("It happened at " + Where(s, rip) + ".");
    if (code == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
        uint64_t kind = er.ExceptionInformation[0], a = er.ExceptionInformation[1];
        const char* verb = kind == 0 ? "read" : kind == 1 ? "write" : "run code";
        if (kind == 8) {
            out.push_back(Fmt("The game tried to run code at 0x%llx, where there is none: a call through a broken or "
                              "freed function pointer or object.", static_cast<unsigned long long>(a)));
        } else if (a == ~0ull) {
            std::string held;
            for (int i = 0; i < 16 && held.empty(); ++i) {
                if (Canonical(regs[i])) continue;
                held = Fmt(" %s holds %016llx", rn[i], static_cast<unsigned long long>(regs[i])) + SourceOf(s, regs, rn, i) + ".";
            }
            out.push_back("A pointer held data, no address (the CPU refused it):" + (held.empty() ? std::string(" a register held it.") : held) +
                          " The object it was read from was freed and its memory used again, or it was overwritten.");
        } else if (a < 0x10000) {
            out.push_back(Fmt("A %s through a null pointer (at offset 0x%llx): an object that was not there.", verb,
                              static_cast<unsigned long long>(a)));
            if (kind == 1 && commitFree < (4ull << 30))
                out.push_back(Fmt("Windows had only %.1f GB left to give (the least while the game ran): a request for "
                                  "memory that failed and was used anyway looks like this (out of memory).",
                                  commitFree / 1073741824.0));
        } else if (!Canonical(a)) {
            out.push_back(Fmt("A %s at 0x%llx, which is no address: a pointer held other data (overwritten, or the "
                              "object it was read from was freed and its memory used again).", verb,
                              static_cast<unsigned long long>(a)));
        } else {
            std::string mem = MemoryAt(s, a);
            out.push_back(Fmt("A %s at 0x%llx: %s.", verb, static_cast<unsigned long long>(a),
                              mem.empty() ? "memory that is there (a protection fault)" : mem.c_str()));
            bool gone = !mem.empty() && mem.find("unknown") == std::string::npos;
            for (int i = 0; haveCtx && gone && i < 16; ++i)
                if (i != 7 && regs[i] >= 0x10000 && a >= regs[i] && a < regs[i] + 0x1000) {
                    out.push_back(Fmt("%s points there (+0x%llx): an object used after its memory was given back%s.", rn[i],
                                      static_cast<unsigned long long>(a - regs[i]), SourceOf(s, regs, rn, i).c_str()));
                    break;
                }
        }
        if (haveCtx) {
            for (int i = 0; i < 16; ++i)
                if (regs[i] == 0xffffffffull && a > 0x10000) {
                    out.push_back(Fmt("%s holds 0xffffffff: an id of 0 or an index of -1 used to reach an element "
                                      "(something given back twice, or an id that was never handed out).", rn[i]));
                    break;
                }
            for (int i = 0; i < 16; ++i) {
                uint64_t q = 0;
                if (i == 7 || regs[i] < 0x10000 || !Canonical(regs[i]) || ModuleAt(s, regs[i])) continue;
                if (!Rpm(s.proc, regs[i], &q, 8)) continue;
                if (q && a >= q && a < q + 0x1000 && ClassAt(s, regs[i]).empty()) {
                    out.push_back(Fmt("%s points at an object whose first 8 bytes (%016llx) lead to the bad address: "
                                      "its vtable is gone, the object was freed or overwritten.", rn[i],
                                      static_cast<unsigned long long>(q)));
                    break;
                }
            }
        }
    } else if (code == EXCEPTION_STACK_OVERFLOW) {
        out.push_back("The thread's stack ran full: a function that calls itself without end, or a very deep chain.");
    } else if (code == 0xC0000409) {
        uint64_t ff = er.NumberParameters ? er.ExceptionInformation[0] : ~0ull;
        const char* why = ff == 2 ? "a stack buffer overrun (the stack cookie was overwritten)"
                        : ff == 5 ? "an invalid argument to a C runtime function"
                        : ff == 7 ? "the program ended itself on purpose (abort or a fatal error it found)"
                        : ff == 10 ? "an invalid call through a guarded function pointer"
                        : ff == 14 ? "a corrupted list in memory"
                        : "a check in the program failed";
        out.push_back(Fmt("The program stopped itself at once (fail-fast %llu): %s.", static_cast<unsigned long long>(ff), why));
    } else if (code == 0xC0000374) {
        out.push_back("The heap is damaged: something wrote past its memory or freed it twice, earlier than this.");
    }
    if (commitFree < (2ull << 30) && code != EXCEPTION_ACCESS_VIOLATION)
        out.push_back(Fmt("Windows had only %.1f GB left to give (RAM and page file).", commitFree / 1073741824.0));
    std::string r;
    for (const auto& l : out) r += "    " + l + "\r\n";
    return r;
}

std::string KnownCrash(const std::string& fault) {
    std::string t = ReadFileText(g_selfDir + L"\\known_crashes.txt");
    size_t i = 0;
    while (i < t.size()) {
        size_t e = t.find('\n', i);
        std::string line = t.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? t.size() : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t bar = line.find(" | ");
        if (line.empty() || line[0] == '#' || bar == std::string::npos || bar == 0) continue;
        if (fault.find(line.substr(0, bar)) != std::string::npos) return line.substr(bar + 3);
    }
    return {};
}

std::string RegObjects(const Session& s, const CONTEXT& c) {
    const uint64_t regs[16] = {c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp,
                               c.R8,  c.R9,  c.R10, c.R11, c.R12, c.R13, c.R14, c.R15};
    static const char* rn[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                 "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    std::string r;
    for (int i = 0; i < 16; ++i) {
        if (i == 7 || regs[i] < 0x10000) continue;
        std::string d = Describe(s, regs[i]);
        if (d.rfind("memory in use (first 8 bytes", 0) == 0 && d.find("object") == std::string::npos) continue;
        if (d.rfind("not saved in the dump", 0) == 0) continue;
        r += Fmt("    %-3s %s\r\n", rn[i], d.c_str());
    }
    return r;
}
