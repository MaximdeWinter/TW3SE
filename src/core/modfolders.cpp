// TW3SE core - the folders of the mods: one mod, one folder.
//
// A mod's TW3SE part is a folder: bin\x64_dx12\tw3se\mods\<name>\, or <game>\Mods\<modName>\tw3se\ beside the mod's
// own content\ (where Vortex and a player unpacking by hand put a mod anyway). The folder holds init.lua (a Lua mod),
// natives\*.lua (w3.native before the scripts load), files\ (loose files) and the modules its require() loads. A mod
// in Mods that the game's mods.settings switches off (Enabled=0 or false) is off for TW3SE too. Two folders with the
// same name: the one in tw3se\mods counts, the other is named in the log once.
#include "core.h"

#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

namespace tw3se {
namespace {

std::wstring Root() {  // Dir() is <game>\bin\x64_dx12\tw3se
    std::wstring d = Dir();
    for (int i = 0; i < 3; ++i) {
        size_t s = d.find_last_of(L"\\/");
        if (s == std::wstring::npos) return L"";
        d.resize(s);
    }
    return d;
}

std::wstring ModsSettings() {  // Documents\The Witcher 3\mods.settings
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) return L"";
    std::wstring s = std::wstring(p) + L"\\The Witcher 3\\mods.settings";
    CoTaskMemFree(p);
    return s;
}

std::string Utf8(const std::wstring& w) {
    char b[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof b, nullptr, nullptr);
    return b;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> SubDirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') out.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

std::mutex g_mx;
std::set<std::string> g_told;  // the doubles and switched-off mods already named in the log

// mods.settings, read again only when its time or size changed (ModFolders runs once a second): the sections whose
// Enabled is 0, lower case
std::set<std::wstring> g_off;
uint64_t g_offStamp = ~0ull;

const std::set<std::wstring>& SwitchedOff(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    uint64_t stamp = 0;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        stamp = ((static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) ^
                fa.nFileSizeLow;
    if (stamp == g_offStamp) return g_off;
    g_offStamp = stamp;
    g_off.clear();
    HANDLE h = stamp ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                   nullptr)
                     : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) return g_off;
    std::string text(fa.nFileSizeLow, '\0');
    DWORD got = 0;
    if (!text.empty()) ReadFile(h, &text[0], static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(h);
    text.resize(got);
    std::wstring section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            std::string name = line.substr(1, line.size() - 2);
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w, MAX_PATH);
            section = w;
            std::transform(section.begin(), section.end(), section.begin(), towlower);
        } else if (!section.empty()) {
            std::string l = line;
            l.erase(std::remove(l.begin(), l.end(), ' '), l.end());
            // (5.02 writes Enabled=false / true where 5.01 wrote 0 / 1 - atoi read "true" as 0, a mod switched on again
            // in the game's Mods menu stayed off for TW3SE: off is 0 or false, anything else on)
            if (_strnicmp(l.c_str(), "Enabled=", 8) == 0) {
                const char* v = l.c_str() + 8;
                if (_stricmp(v, "false") == 0 || (isdigit(static_cast<unsigned char>(*v)) && atoi(v) == 0))
                    g_off.insert(section);
            }
        }
    }
    return g_off;
}

void TellOnce(const std::string& what) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_told.insert(what).second) Log("mods: %s", what.c_str());
}

}  // namespace

std::vector<ModFolder> ModFolders() {
    std::vector<ModFolder> out;
    std::set<std::string> names;  // lower case
    auto add = [&](const std::wstring& name, const std::wstring& dir, bool inGameMods) {
        std::string n = Utf8(name), low = n;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if (!names.insert(low).second) {
            TellOnce("two mods named " + n + " - " + Utf8(dir) + " is left out (the one in tw3se\\mods counts)");
            return;
        }
        out.push_back({n, dir, inGameMods});
    };
    for (const std::wstring& n : SubDirs(Dir() + L"\\mods")) add(n, Dir() + L"\\mods\\" + n, false);
    std::wstring root = Root();
    if (!root.empty()) {
        static const std::wstring settings = ModsSettings();
        const std::set<std::wstring>& off = SwitchedOff(settings);
        for (const std::wstring& n : SubDirs(root + L"\\Mods")) {
            std::wstring dir = root + L"\\Mods\\" + n + L"\\tw3se";
            if (!IsDir(dir)) continue;
            std::wstring low = n;
            std::transform(low.begin(), low.end(), low.begin(), towlower);
            if (off.count(low)) {
                TellOnce(Utf8(n) + " is switched off in mods.settings - its tw3se folder stays out too");
                continue;
            }
            add(n, dir, true);
        }
    }
    return out;
}

// A mod folder of <game>\Mods that mods.settings switches off (Enabled=0 or false): the game leaves its scripts out too
bool GameModSwitchedOff(const std::wstring& name) {
    std::wstring n = name;
    std::transform(n.begin(), n.end(), n.begin(), towlower);
    std::lock_guard<std::mutex> l(g_mx);
    return SwitchedOff(ModsSettings()).count(n) > 0;
}

}  // namespace tw3se
