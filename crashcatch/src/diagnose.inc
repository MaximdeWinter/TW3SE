struct Change {
    std::string file;
    std::string section;
    std::string key, value;
};

struct Variant {
    std::string name;
    std::vector<Change> changes;
    int starts = 0, crashes = 0;
    double minutes = 0;
};

struct Diagnose {
    bool on = false;
    std::vector<Variant> vs;
    std::wstring gameExe;
    std::wstring mo2;
    int applied = -1;
    FILETIME appliedAt{};
    std::vector<std::string> restores;
} g_diag;

std::wstring g_diagFile, g_diagState;

const char* kDiagDefault =
    "# crashcatch --diagnose: each start of the game runs with one line below, the one played least so far.\r\n"
    "# A line: a name, a colon, then the changes, separated by ;\r\n"
    "#   tw3se.config key = value                      (bin\\x64_dx12\\tw3se\\tw3se.config)\r\n"
    "#   dx12user.settings [Section] key = value       (Documents\\The Witcher 3\\dx12user.settings)\r\n"
    "# crashcatch puts every change back when the game ends.\r\n"
    "as installed:\r\n"
    "no plugins and Lua mods: tw3se.config safe_mode = on\r\n"
    "no pose hook: tw3se.config posehook = off\r\n"
    "no script hooks: tw3se.config scriptstack = off; tw3se.config script_lines = off\r\n"
    "no path tracing: dx12user.settings [Rendering/RT/PathTracer] PTEnable = false\r\n";

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::vector<std::string> Split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = s.find(c, a)) != std::string::npos; a = b + 1) out.push_back(s.substr(a, b - a));
    out.push_back(s.substr(a));
    return out;
}

bool ParseChange(const std::string& text, Change& c, std::string& err) {
    std::string t = Trim(text);
    size_t sp = t.find(' ');
    if (sp == std::string::npos) return err = "no file in \"" + t + "\"", false;
    c.file = t.substr(0, sp);
    t = Trim(t.substr(sp + 1));
    if (c.file == "dx12user.settings") {
        size_t close = t.find(']');
        if (t.empty() || t[0] != '[' || close == std::string::npos) return err = "no [Section] in \"" + text + "\"", false;
        c.section = t.substr(1, close - 1);
        t = Trim(t.substr(close + 1));
    } else if (c.file != "tw3se.config") {
        return err = "unknown file " + c.file + " (tw3se.config or dx12user.settings)", false;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) return err = "no key = value in \"" + text + "\"", false;
    c.key = Trim(t.substr(0, eq));
    c.value = Trim(t.substr(eq + 1));
    return c.key.empty() ? (err = "no key in \"" + text + "\"", false) : true;
}

bool LoadVariants(std::string& err) {
    if (!Exists(g_diagFile)) WriteFileText(g_diagFile, kDiagDefault);
    std::string text = ReadFileText(g_diagFile);
    g_diag.vs.clear();
    for (const std::string& raw : Split(text, '\n')) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) return err = "diagnose.txt: no colon in \"" + line + "\"", false;
        Variant v;
        v.name = Trim(line.substr(0, colon));
        std::string rest = Trim(line.substr(colon + 1));
        if (!rest.empty())
            for (const std::string& part : Split(rest, ';')) {
                Change c;
                if (!ParseChange(part, c, err)) return err = "diagnose.txt: " + err, false;
                v.changes.push_back(c);
            }
        g_diag.vs.push_back(v);
    }
    return g_diag.vs.empty() ? (err = "diagnose.txt has no variant", false) : true;
}

void SaveDiag() {
    std::string s = "game|" + Utf8(g_diag.gameExe) + "\r\nmo2|" + Utf8(g_diag.mo2) + "\r\n";
    for (const auto& v : g_diag.vs) s += Fmt("v|%s|%d|%d|%.2f\r\n", v.name.c_str(), v.starts, v.crashes, v.minutes);
    for (const auto& r : g_diag.restores) s += "restore|" + r + "\r\n";
    WriteFileText(g_diagState, s);
}

void LoadDiagState() {
    for (const std::string& raw : Split(ReadFileText(g_diagState), '\n')) {
        auto f = Split(Trim(raw), '|');
        if (f[0] == "game" && f.size() > 1) g_diag.gameExe = Wide(f[1]);
        else if (f[0] == "mo2" && f.size() > 1) g_diag.mo2 = Wide(f[1]);
        else if (f[0] == "v" && f.size() > 4) {
            for (auto& v : g_diag.vs)
                if (v.name == f[1]) v.starts = atoi(f[2].c_str()), v.crashes = atoi(f[3].c_str()), v.minutes = atof(f[4].c_str());
        } else if (f[0] == "restore" && f.size() > 5) {
            g_diag.restores.push_back(f[1] + "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" + f[5]);
        }
    }
}

std::wstring ChangeFile(const std::string& file) {
    if (file == "dx12user.settings") return Wide(KnownFolder(FOLDERID_Documents)) + L"\\The Witcher 3\\dx12user.settings";
    std::wstring bin = DirName(g_diag.gameExe);
    std::wstring real = bin + L"\\tw3se\\tw3se.config";
    if (!g_diag.mo2.empty()) {
        std::wstring root = DirName(DirName(bin));
        std::wstring over = g_diag.mo2 + L"\\overwrite" + bin.substr(root.size()) + L"\\tw3se\\tw3se.config";
        if (Exists(over)) return over;
    }
    return real;
}

bool SetKey(const std::wstring& path, const std::string& section, const std::string& key, const std::string* value,
            std::string* old, bool* existed, bool readOnly = false) {
    std::string text = ReadFileText(path);
    std::string nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    std::vector<std::string> lines = Split(text, '\n');
    for (auto& l : lines)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    bool inSection = section.empty();
    int found = -1, sectionEnd = -1, header = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string t = Trim(lines[i]);
        if (!section.empty() && !t.empty() && t[0] == '[') {
            if (inSection && sectionEnd < 0) sectionEnd = i;
            inSection = t == "[" + section + "]";
            if (inSection) header = i, sectionEnd = -1;
            continue;
        }
        if (!inSection || t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) found = i;
    }
    if (inSection && sectionEnd < 0) sectionEnd = static_cast<int>(lines.size());
    if (existed) *existed = found >= 0;
    if (old) *old = found >= 0 ? Trim(lines[found].substr(lines[found].find('=') + 1)) : "";
    if (readOnly) return true;
    std::string sep = section.empty() ? " = " : "=";
    if (!value) {
        if (found >= 0) lines.erase(lines.begin() + found);
    } else if (found >= 0) {
        lines[found] = key + sep + *value;
    } else if (!section.empty() && header < 0) {
        lines.push_back("[" + section + "]");
        lines.push_back(key + sep + *value);
    } else {
        lines.insert(lines.begin() + (section.empty() ? static_cast<int>(lines.size()) : sectionEnd), key + sep + *value);
    }
    std::string out;
    for (const auto& l : lines) out += l + nl;
    std::wstring tmp = path + L".crashcatch_tmp";
    CreateDirectoryW(DirName(path).c_str(), nullptr);
    return WriteFileText(tmp, out) && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::mutex g_diagMx;

void RestoreAll() {
    std::lock_guard<std::mutex> lk(g_diagMx);
    if (g_diag.restores.empty()) return;
    for (auto it = g_diag.restores.rbegin(); it != g_diag.restores.rend(); ++it) {
        auto f = Split(*it, '|');
        if (f.size() < 5) continue;
        bool existed = f[4] == "1";
        SetKey(Wide(f[0]), f[1], f[2], existed ? &f[3] : nullptr, nullptr, nullptr);
    }
    g_diag.restores.clear();
    g_diag.applied = -1;
    SaveDiag();
}

int NextVariant() {
    int best = 0;
    for (int i = 1; i < static_cast<int>(g_diag.vs.size()); ++i) {
        const auto &a = g_diag.vs[i], &b = g_diag.vs[best];
        if (a.minutes < b.minutes - 0.01 || (fabs(a.minutes - b.minutes) <= 0.01 && a.starts < b.starts)) best = i;
    }
    return best;
}

void ApplyNext() {
    RestoreAll();
    if (g_diag.gameExe.empty()) {
        Say("diagnose: the first start runs as installed (crashcatch learns where the game is)");
        return;
    }
    int i = NextVariant();
    Variant& v = g_diag.vs[i];
    for (const auto& c : v.changes) {
        std::wstring path = ChangeFile(c.file);
        std::string old;
        bool existed = false;
        SetKey(path, c.section, c.key, nullptr, &old, &existed, true);
        g_diag.restores.push_back(Utf8(path) + "|" + c.section + "|" + c.key + "|" + old + "|" + (existed ? "1" : "0"));
        SaveDiag();
        if (!SetKey(path, c.section, c.key, &c.value, nullptr, nullptr))
            Say("diagnose: could not write %s", Utf8(path).c_str());
    }
    g_diag.applied = i;
    GetSystemTimeAsFileTime(&g_diag.appliedAt);
    SaveDiag();
    Say("diagnose: the next start of the game runs \"%s\"", v.name.c_str());
}

std::string DiagTable() {
    std::string r;
    for (int i = 0; i < static_cast<int>(g_diag.vs.size()); ++i) {
        const auto& v = g_diag.vs[i];
        double perHour = v.minutes > 0.5 ? v.crashes * 60.0 / v.minutes : 0;
        r += Fmt("  %-34s %3d start%s %6.0f min %3d crash%s  %s%s\r\n", v.name.c_str(), v.starts, v.starts == 1 ? " " : "s",
                 v.minutes, v.crashes, v.crashes == 1 ? "  " : "es", v.minutes > 0.5 ? Fmt("%.1f per hour", perHour).c_str() : "",
                 i == g_diag.applied ? "  <- this start" : "");
    }
    return r;
}

void PrintTable() {
    std::string tab = "diagnose results:\n" + DiagTable();
    tab.erase(std::remove(tab.begin(), tab.end(), '\r'), tab.end());
    printf("\n%s\n", tab.c_str());
    fflush(stdout);
}

void RecordEnd(const std::wstring& exePath, const std::wstring& mo2, ULONGLONG created, double minutes, bool crashed, bool ended) {
    bool learned = g_diag.gameExe.empty();
    g_diag.gameExe = exePath;
    if (!mo2.empty()) g_diag.mo2 = mo2;
    ULONGLONG applied = (static_cast<ULONGLONG>(g_diag.appliedAt.dwHighDateTime) << 32) | g_diag.appliedAt.dwLowDateTime;
    int idx = g_diag.applied;
    if (idx < 0 && learned && g_diag.vs[0].changes.empty()) idx = 0;
    if (ended && idx >= 0 && (learned || created >= applied)) {
        auto& v = g_diag.vs[idx];
        v.starts++;
        v.minutes += minutes;
        if (crashed) v.crashes++;
    } else if (ended) {
        Say("diagnose: this start is not counted (the game ran before crashcatch changed anything)");
    }
    PrintTable();
    SaveDiag();
}
