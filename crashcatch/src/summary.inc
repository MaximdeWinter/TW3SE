struct ReportInfo {
    std::wstring path;
    std::string when, fingerprint, verdict, known, gpu, windows, ram, tw3se, endedHow;
    std::set<std::string> mods;
    bool crash = false;
};

std::string LineAfter(const std::string& t, const std::string& key) {
    size_t a = t.find(key);
    if (a == std::string::npos) return {};
    a += key.size();
    size_t e = t.find('\n', a);
    std::string r = t.substr(a, e == std::string::npos ? std::string::npos : e - a);
    while (!r.empty() && (r.back() == '\r' || r.back() == ' ')) r.pop_back();
    return r;
}

std::string PlaceIn(const std::string& line) {
    for (size_t at = line.find(" at "); at != std::string::npos; at = line.find(" at ", at + 1)) {
        std::string r = line.substr(at + 4);
        size_t e = r.find_first_of(" ,");
        r = e == std::string::npos ? r : r.substr(0, e);
        if (r.find("+0x") != std::string::npos || r.rfind("0x", 0) == 0) return r;
    }
    return {};
}

bool ReadReport(const std::wstring& dir, ReportInfo& ri) {
    std::string rep = ReadFileText(dir + L"\\report.txt");
    if (rep.empty()) return false;
    std::string tw = ReadFileText(dir + L"\\crash_report.txt");
    ri.path = dir;
    ri.when = LineAfter(rep, "watched from ").substr(0, 16);
    ri.fingerprint = LineAfter(rep, "Fingerprint: ");
    if (!ri.fingerprint.empty()) ri.fingerprint = ri.fingerprint.substr(0, ri.fingerprint.find(' '));
    else ri.fingerprint = PlaceIn(LineAfter(rep, "The last fault before the end: "));
    ri.known = LineAfter(rep, "Known (known_crashes.txt): ");
    if (ri.known.empty() && !ri.fingerprint.empty()) ri.known = KnownCrash(ri.fingerprint);
    size_t v = rep.find("What it likely was:\r\n");
    if (v != std::string::npos) {
        std::string first = LineAfter(rep.substr(v), "What it likely was:\r\n");
        std::string second = LineAfter(rep.substr(v + 21), "\r\n");
        ri.verdict = Trim(first.rfind("    It happened at", 0) == 0 ? second : first);
    }
    ri.endedHow = rep.find("Written by hand") != std::string::npos ? "by hand (D)"
                : LineAfter(rep, "The game ended with exit code ").substr(0, 60);
    ri.crash = rep.find("a normal end") == std::string::npos && rep.find("Written by hand") == std::string::npos;
    ri.windows = LineAfter(rep, "Windows 10.0 build ");
    ri.ram = LineAfter(rep, "RAM ").substr(0, 7);
    for (const std::string& raw : Split(rep, '\n')) {
        if (raw.rfind("GPU ", 0) != 0) continue;
        std::string g = Trim(raw.substr(4));
        if (g.find("Virtual") != std::string::npos || g.find("HUB") != std::string::npos) continue;
        ri.gpu = g.substr(0, g.find(','));
        break;
    }
    std::string s = LineAfter(rep, "started ");
    size_t tv = s.find("TW3SE ");
    if (tv != std::string::npos) ri.tw3se = s.substr(tv + 6, s.find(',', tv) - tv - 6);
    if (ri.tw3se.empty()) {
        std::string m = ReadFileText(dir + L"\\session.txt");
        size_t a = m.find("TW3SE ");
        if (a != std::string::npos) ri.tw3se = m.substr(a + 6, m.find(',', a) - a - 6);
    }
    size_t mf = rep.find("[mod folders of the game]");
    if (mf != std::string::npos) {
        std::vector<std::string> lines = Split(rep.substr(mf), '\n');
        for (size_t i = 1; i < lines.size(); ++i) {
            std::string l = Trim(lines[i]);
            if (l.empty() || l[0] == '[') break;
            ri.mods.insert(l.substr(0, l.find(':')));
        }
    }
    if (ri.mods.empty()) {
        for (const char* k : {"[Mods] ", "[dlc] "}) {
            std::string l = LineAfter(tw, k);
            for (const std::string& m : Split(l, ',')) {
                std::string n = Trim(m);
                if (!n.empty() && n != "(none)") ri.mods.insert(std::string(k[1] == 'M' ? "Mods\\" : "dlc\\") + n);
            }
        }
    }
    return true;
}

int Summary(const std::wstring& folder) {
    std::vector<ReportInfo> all;
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName, p = folder + L"\\" + n;
            if (n == L"." || n == L"..") continue;
            std::wstring low = Lower(n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                dirs.push_back(p);
            } else if (low.size() > 4 && low.compare(low.size() - 4, 4, L".zip") == 0) {
                std::wstring to = p.substr(0, p.size() - 4);
                if (!Exists(to + L"\\report.txt")) {
                    CreateDirectoryW(to.c_str(), nullptr);
                    std::wstring cmd = L"tar.exe -x -f \"" + p + L"\" -C \"" + to + L"\"";
                    STARTUPINFOW si{sizeof si};
                    PROCESS_INFORMATION pi{};
                    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                        WaitForSingleObject(pi.hProcess, 300000);
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    dirs.clear();
    h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                dirs.push_back(folder + L"\\" + fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const auto& d : dirs) {
        ReportInfo ri;
        if (ReadReport(d, ri)) {
            all.push_back(ri);
            continue;
        }
        HANDLE h2 = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            ReportInfo sub;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..") &&
                ReadReport(d + L"\\" + fd.cFileName, sub))
                all.push_back(sub);
        } while (FindNextFileW(h2, &fd));
        FindClose(h2);
    }
    if (all.empty()) {
        printf("no report in %s\n", Utf8(folder).c_str());
        return 1;
    }
    std::map<std::string, std::vector<const ReportInfo*>> groups;
    std::set<std::string> everyMod;
    for (const auto& r : all) {
        std::string key = r.endedHow == "by hand (D)" ? "(reports by hand with the key D: no crash)"
                        : r.fingerprint.empty() ? "(no fault: the game ended without an exception)" : r.fingerprint;
        groups[key].push_back(&r);
        everyMod.insert(r.mods.begin(), r.mods.end());
    }
    std::vector<std::pair<std::string, std::vector<const ReportInfo*>>> order(groups.begin(), groups.end());
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
    std::string out = Fmt("crashcatch summary: %zu reports, %zu different places\r\n\r\n", all.size(), order.size());
    for (const auto& [fp, list] : order) {
        std::set<std::string> pcs, gpus, tw, verdicts, knowns;
        std::map<std::string, int> modCount;
        for (const auto* r : list) {
            pcs.insert(r->gpu + "|" + r->windows + "|" + r->ram);
            if (!r->gpu.empty()) gpus.insert(r->gpu);
            if (!r->tw3se.empty()) tw.insert(r->tw3se);
            if (!r->verdict.empty()) verdicts.insert(r->verdict);
            if (!r->known.empty()) knowns.insert(r->known);
            for (const auto& m : r->mods) modCount[m]++;
        }
        out += Fmt("[%s] %zu report%s from %zu PC%s\r\n", fp.c_str(), list.size(), list.size() == 1 ? "" : "s", pcs.size(),
                   pcs.size() == 1 ? "" : "s");
        for (const auto& k : knowns) out += "  known: " + k + "\r\n";
        for (const auto& v : verdicts) out += "  likely: " + v + "\r\n";
        std::string gl, tl;
        for (const auto& g : gpus) gl += (gl.empty() ? "" : ", ") + g;
        for (const auto& t : tw) tl += (tl.empty() ? "" : ", ") + t;
        if (!gl.empty()) out += "  graphics cards: " + gl + "\r\n";
        if (!tl.empty()) out += "  TW3SE: " + tl + "\r\n";
        std::string common, only;
        for (const auto& [m, n] : modCount) {
            if (n != static_cast<int>(list.size())) continue;
            common += (common.empty() ? "" : ", ") + m;
            bool elsewhere = false;
            for (const auto& [fp2, list2] : order)
                if (fp2 != fp)
                    for (const auto* r2 : list2) elsewhere |= r2->mods.count(m) != 0;
            if (!elsewhere && order.size() > 1) only += (only.empty() ? "" : ", ") + m;
        }
        if (!common.empty()) out += "  mods in every report of it: " + common + "\r\n";
        if (!only.empty()) out += "  of those, in no report of another place: " + only + "\r\n";
        for (const auto* r : list)
            out += Fmt("    %s  %s  %s\r\n", r->when.c_str(), Utf8(BaseName(r->path)).c_str(), r->endedHow.c_str());
        out += "\r\n";
    }
    std::wstring file = folder + L"\\summary.txt";
    WriteFileText(file, Anonymous(out));
    printf("%s\nwritten: %s\n", out.c_str(), Utf8(file).c_str());
    return 0;
}
