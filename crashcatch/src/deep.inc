struct Deep {
    bool on = false;
    std::wstring config;
    std::vector<std::string> lines;
} g_deep;
std::wstring g_deepState;

std::wstring FindW3Folder() {
    if (const wchar_t* t = _wgetenv(L"CRASHCATCH_W3_FOLDER")) return t;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (Lower(pe.szExeFile) == L"witcher3.exe")
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t p[MAX_PATH * 2];
                DWORD n = MAX_PATH * 2;
                bool got = QueryFullProcessImageNameW(h, 0, p, &n) != 0;
                CloseHandle(h);
                if (got) {
                    CloseHandle(snap);
                    return DirName(DirName(DirName(p)));
                }
            }
    CloseHandle(snap);
    wchar_t steam[MAX_PATH] = L"";
    DWORD sz = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &sz) == ERROR_SUCCESS) {
        std::string vdf = ReadFileText(std::wstring(steam) + L"\\steamapps\\libraryfolders.vdf");
        std::vector<std::wstring> libs{steam};
        for (size_t at = vdf.find("\"path\""); at != std::string::npos; at = vdf.find("\"path\"", at + 6)) {
            size_t a = vdf.find('"', at + 6), b = a == std::string::npos ? a : vdf.find('"', a + 1);
            if (b == std::string::npos) break;
            std::string p = vdf.substr(a + 1, b - a - 1);
            std::string clean;
            for (size_t i = 0; i < p.size(); ++i) clean += (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ? p[i++] : p[i];
            libs.push_back(Wide(clean));
        }
        for (const auto& l : libs) {
            std::wstring w3 = l + L"\\steamapps\\common\\The Witcher 3";
            if (Exists(w3 + L"\\bin\\x64_dx12\\witcher3.exe")) return w3;
        }
    }
    for (const wchar_t* key : {L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207664663", L"SOFTWARE\\GOG.com\\Games\\1207664663",
                               L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1495134320"}) {
        wchar_t p[MAX_PATH] = L"";
        DWORD n = sizeof p;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"path", RRF_RT_REG_SZ, nullptr, p, &n) == ERROR_SUCCESS &&
            Exists(std::wstring(p) + L"\\bin\\x64_dx12\\witcher3.exe"))
            return p;
    }
    return {};
}

void SaveDeep() {
    std::string s = "config|" + Utf8(g_deep.config) + "\r\n";
    for (const auto& l : g_deep.lines) s += "restore|" + l + "\r\n";
    WriteFileText(g_deepState, s);
}

void DeepRestore() {
    std::string t = ReadFileText(g_deepState);
    if (t.empty()) return;
    std::wstring cfg;
    std::vector<std::string> lines;
    for (const std::string& raw : Split(t, '\n')) {
        auto f = Split(Trim(raw), '|');
        if (f[0] == "config" && f.size() > 1) cfg = Wide(f[1]);
        else if (f[0] == "restore" && f.size() > 3) lines.push_back(f[1] + "|" + f[2] + "|" + f[3]);
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        auto f = Split(*it, '|');
        SetKey(cfg, "", f[0], f[2] == "1" ? &f[1] : nullptr, nullptr, nullptr);
    }
    DeleteFileW(g_deepState.c_str());
    g_deep.lines.clear();
}

bool DeepApply(std::string& err) {
    DeepRestore();
    std::wstring root = FindW3Folder();
    if (root.empty()) return err = "The Witcher 3 was not found (start the game once, then deep.bat again)", false;
    std::wstring dir = root + L"\\bin\\x64_dx12\\tw3se";
    if (!Exists(dir + L"\\tw3se.dll")) return err = "TW3SE is not installed in " + Utf8(root), false;
    g_deep.config = dir + L"\\tw3se.config";
    const char* keys[][2] = {{"quarantine", "poison"}, {"dred", "on"}};
    for (const auto& kv : keys) {
        std::string old;
        bool existed = false;
        SetKey(g_deep.config, "", kv[0], nullptr, &old, &existed, true);
        g_deep.lines.push_back(std::string(kv[0]) + "|" + old + "|" + (existed ? "1" : "0"));
        SaveDeep();
        std::string v = kv[1];
        if (!SetKey(g_deep.config, "", kv[0], &v, nullptr, nullptr)) return err = "could not write " + Utf8(g_deep.config), false;
    }
    return true;
}
