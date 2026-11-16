// TW3SE core - w3.fs (for an editor in the game: its projects, the config, the catalog's files): files and
// folders with UTF-8 paths. Lua's io.open takes the ANSI code page, so a path with an umlaut fails there. Shared with
// the UI lab.
//
//   w3.fs.list(dir)              -> {{name =, dir = bool, size =, mtime =}, ...} sorted by name, nil + error
//   w3.fs.exists(p), w3.fs.isdir(p), w3.fs.mtime(p) (seconds since 1970), w3.fs.size(p)
//   w3.fs.read(p)                -> the bytes, nil + error
//   w3.fs.write(p, bytes)        written to p.tmp, then put in p's place (the old file stays until then)
//   w3.fs.mkdir(p)               with every folder above it
//   w3.fs.remove(p)  w3.fs.rmdir(p)  w3.fs.rename(a, b)  w3.fs.copy(a, b)
//   w3.fs.folder("documents" | "appdata" | "localappdata" | "temp" | "game" | "tw3se")
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <string>
#include <vector>
#include "lua.h"
#include "lauxlib.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace tw3se {
namespace {

std::wstring Wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
std::string Utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
int Fail(lua_State* L) {
    DWORD e = GetLastError();
    wchar_t buf[512] = {0};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, 511, nullptr);
    std::string m = Utf8(buf);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    lua_pushnil(L);
    lua_pushstring(L, m.c_str());
    return 2;
}
double UnixTime(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (static_cast<double>(u.QuadPart) - 116444736000000000.0) / 1e7;
}

int FList(lua_State* L) {
    std::wstring dir = Wide(luaL_checkstring(L, 1));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return Fail(L);
    struct E { std::string name; bool dir; double size, mtime; };
    std::vector<E> out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({Utf8(fd.cFileName), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       static_cast<double>((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow),
                       UnixTime(fd.ftLastWriteTime)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const E& a, const E& b) { return a.name < b.name; });
    lua_createtable(L, static_cast<int>(out.size()), 0);
    for (size_t i = 0; i < out.size(); ++i) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, out[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, out[i].dir);
        lua_setfield(L, -2, "dir");
        lua_pushnumber(L, out[i].size);
        lua_setfield(L, -2, "size");
        lua_pushnumber(L, out[i].mtime);
        lua_setfield(L, -2, "mtime");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

bool Attr(const char* p, WIN32_FILE_ATTRIBUTE_DATA* a) {
    return GetFileAttributesExW(Wide(p).c_str(), GetFileExInfoStandard, a) != 0;
}
int FExists(lua_State* L) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    lua_pushboolean(L, Attr(luaL_checkstring(L, 1), &a));
    return 1;
}
int FIsDir(lua_State* L) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    lua_pushboolean(L, Attr(luaL_checkstring(L, 1), &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    return 1;
}
int FMtime(lua_State* L) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!Attr(luaL_checkstring(L, 1), &a)) return Fail(L);
    lua_pushnumber(L, UnixTime(a.ftLastWriteTime));
    return 1;
}
int FSize(lua_State* L) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!Attr(luaL_checkstring(L, 1), &a)) return Fail(L);
    lua_pushnumber(L, static_cast<double>((static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow));
    return 1;
}

int FRead(lua_State* L) {
    HANDLE h = CreateFileW(Wide(luaL_checkstring(L, 1)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Fail(L);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return Fail(L);
    }
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    size_t at = 0;
    while (at < data.size()) {
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - at, 1 << 30));
        if (!ReadFile(h, data.data() + at, want, &got, nullptr) || got == 0) break;
        at += got;
    }
    CloseHandle(h);
    lua_pushlstring(L, data.data(), at);
    return 1;
}

int FWrite(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    std::wstring path = Wide(luaL_checkstring(L, 1));
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Fail(L);
    size_t at = 0;
    DWORD put = 0;
    bool ok = true;
    while (at < len) {
        DWORD want = static_cast<DWORD>(std::min<size_t>(len - at, 1 << 30));
        if (!WriteFile(h, data + at, want, &put, nullptr)) {
            ok = false;
            break;
        }
        at += put;
    }
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        int r = Fail(L);
        DeleteFileW(tmp.c_str());
        return r;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int FMkdir(lua_State* L) {
    int r = SHCreateDirectoryExW(nullptr, Wide(luaL_checkstring(L, 1)).c_str(), nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) {
        SetLastError(r);
        return Fail(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}
int FRemove(lua_State* L) {
    if (!DeleteFileW(Wide(luaL_checkstring(L, 1)).c_str())) return Fail(L);
    lua_pushboolean(L, 1);
    return 1;
}
int FRmdir(lua_State* L) {
    if (!RemoveDirectoryW(Wide(luaL_checkstring(L, 1)).c_str())) return Fail(L);
    lua_pushboolean(L, 1);
    return 1;
}
int FRename(lua_State* L) {
    if (!MoveFileExW(Wide(luaL_checkstring(L, 1)).c_str(), Wide(luaL_checkstring(L, 2)).c_str(), 0)) return Fail(L);
    lua_pushboolean(L, 1);
    return 1;
}
int FCopy(lua_State* L) {
    if (!CopyFileW(Wide(luaL_checkstring(L, 1)).c_str(), Wide(luaL_checkstring(L, 2)).c_str(), FALSE)) return Fail(L);
    lua_pushboolean(L, 1);
    return 1;
}
int FFolder(lua_State* L) {
    std::string which = luaL_checkstring(L, 1);
    if (which == "temp") {
        wchar_t buf[MAX_PATH + 1] = {0};
        DWORD n = GetTempPathW(MAX_PATH, buf);
        if (n && buf[n - 1] == L'\\') buf[n - 1] = 0;
        lua_pushstring(L, Utf8(buf).c_str());
        return 1;
    }
    if (which == "tw3se") {  // bin\x64_dx12\tw3se (1.2): the folder of the DLL this code is in (no core call: the lab)
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH] = {0};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&FFolder), &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring d(path);
        d = d.substr(0, d.find_last_of(L"\\/"));
        lua_pushstring(L, Utf8(d.c_str()).c_str());
        return 1;
    }
    if (which == "game") {  // the game's folder, the one with bin, content and Mods (1.2)
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring d(exe);
        for (int up = 0; up < 3 && d.find_last_of(L"\\/") != std::wstring::npos; ++up) d = d.substr(0, d.find_last_of(L"\\/"));
        lua_pushstring(L, Utf8(d.c_str()).c_str());
        return 1;
    }
    const KNOWNFOLDERID* id = which == "documents"      ? &FOLDERID_Documents
                              : which == "appdata"      ? &FOLDERID_RoamingAppData
                              : which == "localappdata" ? &FOLDERID_LocalAppData
                                                        : nullptr;
    if (!id) return luaL_argerror(L, 1, "documents, appdata, localappdata, temp, game or tw3se");
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(*id, 0, nullptr, &p))) return Fail(L);
    lua_pushstring(L, Utf8(p).c_str());
    CoTaskMemFree(p);
    return 1;
}

}  // namespace

// w3.fs into the table on top of the stack (w3)
void LuaFsOpen(lua_State* L) {
    static const luaL_Reg fns[] = {{"list", &FList},     {"exists", &FExists}, {"isdir", &FIsDir},   {"mtime", &FMtime},
                                   {"size", &FSize},     {"read", &FRead},     {"write", &FWrite},   {"mkdir", &FMkdir},
                                   {"remove", &FRemove}, {"rmdir", &FRmdir},   {"rename", &FRename}, {"copy", &FCopy},
                                   {"folder", &FFolder}, {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_setfield(L, -2, "fs");
}

}  // namespace tw3se
