struct DumpRange {
    uint64_t start, size;
    const unsigned char* data;
};

struct DumpImage {
    uint64_t base = 0, size = 0;
    std::vector<unsigned char> bytes;
};

struct DumpMem : MemSource {
    std::vector<DumpRange> ranges;
    std::vector<MINIDUMP_MEMORY_INFO> info;
    std::vector<DumpImage> images;
    size_t Read(uint64_t at, void* out, size_t n) override {
        auto* o = static_cast<unsigned char*>(out);
        size_t done = 0;
        while (done < n) {
            uint64_t a = at + done;
            size_t step = 0;
            auto it = std::upper_bound(ranges.begin(), ranges.end(), a, [](uint64_t v, const DumpRange& r) { return v < r.start; });
            if (it != ranges.begin() && a < std::prev(it)->start + std::prev(it)->size) {
                const DumpRange& r = *std::prev(it);
                step = static_cast<size_t>(std::min<uint64_t>(n - done, r.start + r.size - a));
                memcpy(o + done, r.data + (a - r.start), step);
            } else {
                for (const auto& im : images)
                    if (a >= im.base && a < im.base + im.bytes.size()) {
                        step = static_cast<size_t>(std::min<uint64_t>(n - done, im.base + im.bytes.size() - a));
                        memcpy(o + done, im.bytes.data() + (a - im.base), step);
                        break;
                    }
            }
            if (!step) break;
            done += step;
        }
        return done;
    }
    int Query(uint64_t at, MEMORY_BASIC_INFORMATION* mb) override {
        if (info.empty()) return 0;
        for (const auto& i : info)
            if (at >= i.BaseAddress && at < i.BaseAddress + i.RegionSize) {
                mb->BaseAddress = reinterpret_cast<void*>(i.BaseAddress);
                mb->RegionSize = static_cast<SIZE_T>(i.RegionSize);
                mb->State = i.State;
                mb->Protect = i.Protect;
                mb->Type = i.Type;
                return i.State == MEM_FREE ? -1 : 1;
            }
        return -1;
    }
};

bool LoadImage(const std::wstring& path, uint64_t base, uint32_t size, uint32_t stamp, uint32_t sum, DumpImage& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fs{};
    GetFileSizeEx(f, &fs);
    std::vector<unsigned char> file(static_cast<size_t>(fs.QuadPart));
    DWORD got = 0;
    size_t at = 0;
    while (at < file.size() && ReadFile(f, file.data() + at, static_cast<DWORD>(std::min<size_t>(file.size() - at, 1u << 30)), &got, nullptr) && got)
        at += got;
    CloseHandle(f);
    if (at != file.size() || file.size() < 0x400) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file.size()) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    if (nt->OptionalHeader.SizeOfImage != size || (stamp && nt->FileHeader.TimeDateStamp != stamp) ||
        (sum && nt->OptionalHeader.CheckSum != sum))
        return false;
    out.base = base;
    out.size = size;
    out.bytes.assign(size, 0);
    memcpy(out.bytes.data(), file.data(), std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, size));
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uint64_t n = std::min<uint64_t>(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        if (sec[i].PointerToRawData + n > file.size() || sec[i].VirtualAddress + n > size) n = 0;
        if (n) memcpy(out.bytes.data() + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, static_cast<size_t>(n));
    }
    int64_t delta = static_cast<int64_t>(base - nt->OptionalHeader.ImageBase);
    IMAGE_DATA_DIRECTORY rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && rd.VirtualAddress && rd.Size && rd.VirtualAddress + rd.Size <= size) {
        uint32_t p = rd.VirtualAddress, end = rd.VirtualAddress + rd.Size;
        while (p + 8 <= end) {
            auto* b = reinterpret_cast<IMAGE_BASE_RELOCATION*>(out.bytes.data() + p);
            if (b->SizeOfBlock < 8 || p + b->SizeOfBlock > end) break;
            auto* e = reinterpret_cast<WORD*>(b + 1);
            for (uint32_t k = 0; k < (b->SizeOfBlock - 8) / 2; ++k) {
                uint32_t off = b->VirtualAddress + (e[k] & 0xfff);
                if ((e[k] >> 12) == IMAGE_REL_BASED_DIR64 && off + 8 <= size) {
                    uint64_t v;
                    memcpy(&v, out.bytes.data() + off, 8);
                    v += delta;
                    memcpy(out.bytes.data() + off, &v, 8);
                }
            }
            p += b->SizeOfBlock;
        }
    }
    return true;
}

void IndexImages(const std::wstring& dir, int depth, std::multimap<std::wstring, std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && Lower(n) != L"content" && Lower(n) != L"dlc" && Lower(n) != L"mods") IndexImages(dir + L"\\" + n, depth - 1, out);
        } else {
            std::wstring l = Lower(n);
            if (l.size() > 4 && (l.compare(l.size() - 4, 4, L".exe") == 0 || l.compare(l.size() - 4, 4, L".dll") == 0 ||
                                 l.compare(l.size() - 4, 4, L".asi") == 0))
                out.emplace(l, dir + L"\\" + n);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string MinidumpString(const unsigned char* base, RVA rva) {
    if (!rva) return {};
    auto* s = reinterpret_cast<const MINIDUMP_STRING*>(base + rva);
    return Utf8(std::wstring(s->Buffer, s->Length / 2));
}

std::string AnalyzeDump(const std::wstring& path, const std::vector<std::wstring>& imageDirs, Session& s,
                        uint64_t commitFree) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "    cannot open the dump\r\n";
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    auto* base = map ? static_cast<const unsigned char*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    std::string r;
    if (!base) {
        if (map) CloseHandle(map);
        CloseHandle(f);
        return "    cannot read the dump\r\n";
    }
    DumpMem mem;
    void* p = nullptr;
    ULONG n = 0;
    if (MiniDumpReadDumpStream(const_cast<unsigned char*>(base), MemoryListStream, nullptr, &p, &n) && p) {
        auto* ml = static_cast<MINIDUMP_MEMORY_LIST*>(p);
        for (ULONG i = 0; i < ml->NumberOfMemoryRanges; ++i) {
            const auto& d = ml->MemoryRanges[i];
            mem.ranges.push_back({d.StartOfMemoryRange, d.Memory.DataSize, base + d.Memory.Rva});
        }
    }
    if (MiniDumpReadDumpStream(const_cast<unsigned char*>(base), Memory64ListStream, nullptr, &p, &n) && p) {
        auto* ml = static_cast<MINIDUMP_MEMORY64_LIST*>(p);
        uint64_t at = ml->BaseRva;
        for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; ++i) {
            const auto& d = ml->MemoryRanges[i];
            mem.ranges.push_back({d.StartOfMemoryRange, d.DataSize, base + at});
            at += d.DataSize;
        }
    }
    std::sort(mem.ranges.begin(), mem.ranges.end(), [](const DumpRange& a, const DumpRange& b) { return a.start < b.start; });
    if (MiniDumpReadDumpStream(const_cast<unsigned char*>(base), MemoryInfoListStream, nullptr, &p, &n) && p) {
        auto* il = static_cast<MINIDUMP_MEMORY_INFO_LIST*>(p);
        auto* e = reinterpret_cast<const unsigned char*>(il) + il->SizeOfHeader;
        for (ULONG64 i = 0; i < il->NumberOfEntries; ++i)
            mem.info.push_back(*reinterpret_cast<const MINIDUMP_MEMORY_INFO*>(e + i * il->SizeOfEntry));
    }
    std::multimap<std::wstring, std::wstring> index;
    for (const auto& d : imageDirs) IndexImages(d, 3, index);
    s.mods.clear();
    std::vector<std::string> missing;
    if (MiniDumpReadDumpStream(const_cast<unsigned char*>(base), ModuleListStream, nullptr, &p, &n) && p) {
        auto* ml = static_cast<MINIDUMP_MODULE_LIST*>(p);
        for (ULONG i = 0; i < ml->NumberOfModules; ++i) {
            const auto& d = ml->Modules[i];
            Module m;
            m.base = d.BaseOfImage;
            m.size = d.SizeOfImage;
            m.path = Wide(MinidumpString(base, d.ModuleNameRva));
            m.stamp = d.TimeDateStamp;
            m.sum = d.CheckSum;
            s.mods.push_back(m);
            std::vector<std::wstring> tries{m.path};
            auto range = index.equal_range(Lower(BaseName(m.path)));
            for (auto it = range.first; it != range.second; ++it) tries.push_back(it->second);
            DumpImage im;
            bool ok = false;
            for (const auto& t : tries)
                if (!t.empty() && LoadImage(t, m.base, d.SizeOfImage, d.TimeDateStamp, d.CheckSum, im)) {
                    ok = true;
                    break;
                }
            if (ok) mem.images.push_back(std::move(im));
            else if (m.size > 0x100000) missing.push_back(Utf8(BaseName(m.path)));
        }
    }
    g_mem = &mem;
    g_names.clear();
    if (MiniDumpReadDumpStream(const_cast<unsigned char*>(base), ExceptionStream, nullptr, &p, &n) && p) {
        auto* es = static_cast<MINIDUMP_EXCEPTION_STREAM*>(p);
        EXCEPTION_RECORD er{};
        er.ExceptionCode = es->ExceptionRecord.ExceptionCode;
        er.ExceptionAddress = reinterpret_cast<void*>(es->ExceptionRecord.ExceptionAddress);
        er.NumberParameters = std::min<DWORD>(es->ExceptionRecord.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
        for (DWORD i = 0; i < er.NumberParameters; ++i) er.ExceptionInformation[i] = static_cast<ULONG_PTR>(es->ExceptionRecord.ExceptionInformation[i]);
        CONTEXT c{};
        bool haveCtx = es->ThreadContext.DataSize >= sizeof(CONTEXT);
        if (haveCtx) memcpy(&c, base + es->ThreadContext.Rva, sizeof c);
        r += Fmt("    %s at %s, thread %lu", CodeLine(er.ExceptionCode).c_str(), Where(s, reinterpret_cast<uint64_t>(er.ExceptionAddress)).c_str(),
                 es->ThreadId);
        if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2)
            r += Fmt(", %s of 0x%llx", er.ExceptionInformation[0] == 0 ? "read" : er.ExceptionInformation[0] == 1 ? "write" : "execution",
                     static_cast<unsigned long long>(er.ExceptionInformation[1]));
        r += "\r\n  what it likely was:\r\n" + Verdict(s, er, c, haveCtx, commitFree);
        std::string known = KnownCrash(Where(s, reinterpret_cast<uint64_t>(er.ExceptionAddress)));
        if (!known.empty()) r += "  known (known_crashes.txt): " + known + "\r\n";
        if (haveCtx) {
            std::string o = RegObjects(s, c);
            if (!o.empty()) r += "  what the registers point at:\r\n" + o;
            uint64_t q[128];
            size_t got = mem.Read(c.Rsp, q, sizeof q);
            std::string st;
            int shown = 0;
            for (size_t i = 0; i < got / 8 && shown < 30; ++i) {
                const Module* m = ModuleAt(s, q[i]);
                if (!m || m->size <= 0x1000) continue;
                st += Fmt("    rsp+0x%03zx %s\r\n", i * 8, Where(s, q[i]).c_str());
                ++shown;
            }
            if (!st.empty()) r += "  code addresses on the stack:\r\n" + st;
            std::set<std::string> touched;
            auto touch = [&](uint64_t a) {
                if (const Module* m = ModuleAt(s, a)) touched.insert(Utf8(BaseName(m->path)));
            };
            touch(c.Rip);
            for (size_t i = 0; i < got / 8; ++i) touch(q[i]);
            std::vector<std::string> keep;
            for (const auto& m : missing)
                if (touched.count(m)) keep.push_back(m);
            missing.swap(keep);
        }
    } else {
        r += "    no fault in this dump (written by hand or for a hang)\r\n";
    }
    if (!missing.empty()) {
        r += "  modules of the stack whose file of the same build was not found here (no names in them):";
        for (const auto& m : missing) r += " " + m;
        r += "\r\n";
    }
    g_mem = nullptr;
    UnmapViewOfFile(base);
    CloseHandle(map);
    CloseHandle(f);
    return r;
}

std::string NameReportStacks(const std::string& report, Session& s) {
    std::string r;
    std::set<std::string> seen;
    size_t i = 0;
    while (i < report.size()) {
        size_t e = report.find('\n', i);
        std::string line = report.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? report.size() : e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t plus = line.find("+0x");
        if (line.find("#") == std::string::npos || plus == std::string::npos || line.find('(') != std::string::npos) continue;
        size_t ms = line.rfind(' ', plus);
        std::string mod = line.substr(ms + 1, plus - ms - 1);
        uint64_t rva = strtoull(line.c_str() + plus + 3, nullptr, 16);
        for (const auto& m : s.mods)
            if (Lower(BaseName(m.path)) == Lower(Wide(mod))) {
                std::string fn = FuncName(s, m.base + rva);
                size_t lead = line.find_first_not_of(' ');
                std::string key = line.substr(lead == std::string::npos ? 0 : lead);
                if (!fn.empty() && seen.insert(key).second) r += line + "  (" + fn + ")\r\n";
                break;
            }
    }
    return r;
}

int Analyze(const std::wstring& target, const std::vector<std::wstring>& imageDirs) {
    std::wstring dir = target;
    std::vector<std::wstring> dumps;
    std::wstring low = Lower(target);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, L".zip") == 0) {
        dir = target.substr(0, target.size() - 4);
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring cmd = L"tar.exe -x -f \"" + target + L"\" -C \"" + dir + L"\"";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 300000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } else if (low.size() > 4 && low.compare(low.size() - 4, 4, L".dmp") == 0) {
        dir = DirName(target);
        dumps.push_back(target);
    }
    if (dumps.empty()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do dumps.push_back(dir + L"\\" + fd.cFileName);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (dumps.empty()) {
        printf("no dump in %s\n", Utf8(dir).c_str());
        return 1;
    }
    Session s;
    std::string out = "crashcatch analysis\r\n\r\n";
    uint64_t commitFree = UINT64_MAX;
    {
        std::string rep = ReadFileText(dir + L"\\report.txt");
        size_t at = rep.find("the lowest while the game ran: ");
        if (at != std::string::npos) commitFree = static_cast<uint64_t>(atof(rep.c_str() + at + 31) * 1073741824.0);
    }
    for (const auto& d : dumps) {
        printf("analysing %s ...\n", Utf8(BaseName(d)).c_str());
        out += "[" + Utf8(BaseName(d)) + "]\r\n" + AnalyzeDump(d, imageDirs, s, commitFree) + "\r\n";
    }
    std::string rep = ReadFileText(dir + L"\\report.txt");
    std::string tw = ReadFileText(dir + L"\\crash_report.txt");
    if (!rep.empty() || !tw.empty()) {
        DumpMem mem;
        std::multimap<std::wstring, std::wstring> index;
        for (const auto& d : imageDirs) IndexImages(d, 3, index);
        for (const auto& m : s.mods) {
            std::vector<std::wstring> tries{m.path};
            auto range = index.equal_range(Lower(BaseName(m.path)));
            for (auto it = range.first; it != range.second; ++it) tries.push_back(it->second);
            DumpImage im;
            for (const auto& t : tries)
                if (!t.empty() && LoadImage(t, m.base, static_cast<uint32_t>(m.size), m.stamp, m.sum, im)) {
                    mem.images.push_back(std::move(im));
                    break;
                }
        }
        g_mem = &mem;
        g_names.clear();
        std::string named = NameReportStacks(rep, s);
        std::string namedTw = NameReportStacks(tw, s);
        g_mem = nullptr;
        if (!named.empty()) out += "[the stacks of report.txt, named]\r\n" + named + "\r\n";
        if (!namedTw.empty()) out += "[the stacks of crash_report.txt, named]\r\n" + namedTw + "\r\n";
    }
    std::wstring file = dir + L"\\analysis.txt";
    WriteFileText(file, Anonymous(out));
    printf("%s\nwritten: %s\n", out.c_str(), Utf8(file).c_str());
    return 0;
}
