struct Bisect {
    bool on = false;
    std::wstring root;
    double minutes = 0;
    std::vector<std::vector<std::string>> groups;
    std::vector<int> suspects;
    std::vector<int> active;
    std::vector<std::string> history;
    std::vector<std::string> parked;
    std::string result;
} g_bis;

std::wstring g_bisFile, g_bisState;
std::mutex g_bisMx;

std::wstring ParkDir() { return g_bis.root + L"\\crashcatch_parked"; }

bool IsGameDlc(const std::wstring& name) {
    std::wstring n = Lower(name);
    if (n == L"bob" || n == L"ep1" || n == L"ep2") return true;
    if (n.size() > 3 && n.compare(0, 3, L"dlc") == 0) {
        for (size_t i = 3; i < n.size(); ++i)
            if (!iswdigit(n[i])) return false;
        return true;
    }
    return false;
}

std::string GroupName(int g) {
    std::string r;
    for (const auto& m : g_bis.groups[g]) r += (r.empty() ? "" : " + ") + m;
    return r;
}

void SaveBisect() {
    std::string s = "root|" + Utf8(g_bis.root) + Fmt("\r\nminutes|%g\r\n", g_bis.minutes);
    for (const auto& g : g_bis.groups) {
        s += "group";
        for (const auto& m : g) s += "|" + m;
        s += "\r\n";
    }
    s += "suspects";
    for (int i : g_bis.suspects) s += Fmt("|%d", i);
    s += "\r\nactive";
    for (int i : g_bis.active) s += Fmt("|%d", i);
    s += "\r\n";
    for (const auto& h : g_bis.history) s += "history|" + h + "\r\n";
    for (const auto& p : g_bis.parked) s += "parked|" + p + "\r\n";
    if (!g_bis.result.empty()) s += "result|" + g_bis.result + "\r\n";
    WriteFileText(g_bisState, s);
}

Bisect ReadBisectState() {
    Bisect b;
    for (const std::string& raw : Split(ReadFileText(g_bisState), '\n')) {
        auto f = Split(Trim(raw), '|');
        if (f[0] == "root" && f.size() > 1) b.root = Wide(f[1]);
        else if (f[0] == "minutes" && f.size() > 1) b.minutes = atof(f[1].c_str());
        else if (f[0] == "group") b.groups.emplace_back(f.begin() + 1, f.end());
        else if (f[0] == "suspects") for (size_t i = 1; i < f.size(); ++i) b.suspects.push_back(atoi(f[i].c_str()));
        else if (f[0] == "history" && f.size() > 1) b.history.push_back(f[1]);
        else if (f[0] == "parked" && f.size() > 1) b.parked.push_back(f[1]);
        else if (f[0] == "result" && f.size() > 1) b.result = f[1];
    }
    return b;
}

bool BuildGroups(std::string& err) {
    std::vector<std::string> mods;
    for (const wchar_t* sub : {L"Mods", L"dlc"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_bis.root + L"\\" + sub + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (std::wstring(sub) == L"dlc" && IsGameDlc(n)) continue;
            mods.push_back(Utf8(std::wstring(sub) + L"\\" + n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (mods.size() < 2) return err = Fmt("%zu mod folder(s) in Mods\\ and dlc\\: nothing to halve", mods.size()), false;
    std::set<std::string> taken;
    auto find = [&](const std::string& name) -> std::string {
        for (const auto& m : mods) {
            std::string low = m, n = name;
            for (auto& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            for (auto& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (low == n || low.substr(low.find('\\') + 1) == n) return m;
        }
        return {};
    };
    if (!Exists(g_bisFile))
        WriteFileText(g_bisFile, "# crashcatch --bisect: mods that need each other, on one line (separated by spaces); they are\r\n"
                                 "# switched off and on together. Every other mod folder is a group of its own.\r\n"
                                 "# Example:  modMyArmor modMyArmorPatch dlcmyarmor\r\n");
    for (const std::string& raw : Split(ReadFileText(g_bisFile), '\n')) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> g;
        for (const std::string& w : Split(line, ' ')) {
            std::string name = Trim(w);
            if (name.empty()) continue;
            std::string m = find(name);
            if (m.empty()) Say("bisect: %s from bisect.txt is not in Mods\\ or dlc\\ (left out)", name.c_str());
            else if (taken.insert(m).second) g.push_back(m);
        }
        if (!g.empty()) g_bis.groups.push_back(g);
    }
    for (const auto& m : mods)
        if (!taken.count(m)) g_bis.groups.push_back({m});
    g_bis.suspects.clear();
    for (int i = 0; i < static_cast<int>(g_bis.groups.size()); ++i) g_bis.suspects.push_back(i);
    return true;
}

bool MoveMod(const std::string& rel, bool park) {
    std::wstring from = g_bis.root + L"\\" + Wide(rel), to = ParkDir() + L"\\" + Wide(rel);
    if (!park) std::swap(from, to);
    CreateDirectoryW(ParkDir().c_str(), nullptr);
    CreateDirectoryW(DirName(to).c_str(), nullptr);
    if (MoveFileExW(from.c_str(), to.c_str(), 0)) return true;
    Say("bisect: could not move %s %s (error %lu; is the game still running?)", rel.c_str(), park ? "away" : "back", GetLastError());
    return false;
}

void BisectRestoreAll() {
    std::lock_guard<std::mutex> lk(g_bisMx);
    if (g_bis.parked.empty()) return;
    std::vector<std::string> left;
    for (const auto& p : g_bis.parked)
        if (!MoveMod(p, false)) left.push_back(p);
    g_bis.parked = left;
    if (left.empty()) {
        RemoveDirectoryW((ParkDir() + L"\\Mods").c_str());
        RemoveDirectoryW((ParkDir() + L"\\dlc").c_str());
        RemoveDirectoryW(ParkDir().c_str());
    }
    SaveBisect();
}

void PrintBisect() {
    std::string r = Fmt("\nbisect: %zu groups, %zu still suspected\n", g_bis.groups.size(), g_bis.suspects.size());
    for (const auto& h : g_bis.history) r += "  " + h + "\n";
    printf("%s\n", r.c_str());
    fflush(stdout);
}

void ApplyRound() {
    BisectRestoreAll();
    if (!g_bis.result.empty()) return;
    if (g_bis.suspects.size() == 1) {
        g_bis.result = GroupName(g_bis.suspects[0]);
        SaveBisect();
        Say("bisect: done. The crash comes with: %s", g_bis.result.c_str());
        WriteFileText(DirName(g_bisState) + L"\\bisect_result.txt",
                      "The crash comes with: " + g_bis.result + "\r\n\r\n" + [&] {
                          std::string h;
                          for (const auto& x : g_bis.history) h += x + "\r\n";
                          return h;
                      }());
        return;
    }
    if (g_bis.suspects.empty()) {
        g_bis.result = "none of the mods (the crash came with each half: it needs two mods together, or it is no mod)";
        SaveBisect();
        Say("bisect: %s", g_bis.result.c_str());
        return;
    }
    g_bis.active.assign(g_bis.suspects.begin(), g_bis.suspects.begin() + (g_bis.suspects.size() + 1) / 2);
    std::set<int> on(g_bis.active.begin(), g_bis.active.end());
    std::lock_guard<std::mutex> lk(g_bisMx);
    for (int g = 0; g < static_cast<int>(g_bis.groups.size()); ++g) {
        if (on.count(g)) continue;
        for (const auto& m : g_bis.groups[g]) {
            g_bis.parked.push_back(m);
            SaveBisect();
            if (!MoveMod(m, true)) g_bis.parked.pop_back();
        }
    }
    SaveBisect();
    std::string names;
    for (int g : g_bis.active) names += "\n    " + GroupName(g);
    Say("bisect: the next start runs with %zu of %zu suspected groups on (all other mods are parked):%s", g_bis.active.size(),
        g_bis.suspects.size(), names.c_str());
    Say("bisect: play until it crashes, or at least %g minutes; then end the game as usual", g_bis.minutes);
}

void RecordBisect(double minutes, bool crashed, bool ended, bool mo2) {
    if (!ended || !g_bis.result.empty()) return;
    if (mo2) {
        Say("bisect: the game runs under Mod Organizer 2, its mods are not in the game folder - halve them in MO2 instead");
        g_bis.result = "stopped (Mod Organizer 2)";
        BisectRestoreAll();
        return;
    }
    std::set<int> on(g_bis.active.begin(), g_bis.active.end());
    if (crashed) {
        g_bis.history.push_back(Fmt("%zu groups on: a crash after %.1f min -> the culprit is among them", g_bis.active.size(), minutes));
        g_bis.suspects = g_bis.active;
    } else if (minutes >= g_bis.minutes) {
        g_bis.history.push_back(Fmt("%zu groups on: no crash in %.1f min -> the culprit is among the others", g_bis.active.size(), minutes));
        std::vector<int> rest;
        for (int g : g_bis.suspects)
            if (!on.count(g)) rest.push_back(g);
        g_bis.suspects = rest;
    } else {
        Say("bisect: not counted - the game ended after %.0f min without a crash (at least %g min tell)", minutes, g_bis.minutes);
    }
    SaveBisect();
    PrintBisect();
}

bool StartBisect(std::string& err) {
    std::wstring root = g_bis.root;
    Bisect b = ReadBisectState();
    if (!b.parked.empty()) {
        g_bis.root = b.root;
        g_bis.parked = b.parked;
        BisectRestoreAll();
        if (!g_bis.parked.empty()) return err = "mods from an earlier run could not be moved back (is the game running?)", false;
    }
    g_bis.root = root;
    bool goOn = !b.groups.empty() && b.result.empty() && Lower(b.root) == Lower(root) && b.suspects.size() > 1;
    if (goOn) {
        g_bis.groups = b.groups, g_bis.suspects = b.suspects, g_bis.history = b.history;
        if (g_bis.minutes <= 0) g_bis.minutes = b.minutes;
        Say("bisect: going on with the search from before (%zu of %zu groups suspected)", g_bis.suspects.size(), g_bis.groups.size());
    } else if (!BuildGroups(err)) {
        return false;
    }
    if (g_bis.minutes <= 0) g_bis.minutes = 20;
    SaveBisect();
    return true;
}
