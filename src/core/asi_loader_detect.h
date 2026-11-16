// Is this file the Ultimate ASI Loader? (Under the name dinput8_chain.dll it stops the game: it decides
// by its own file name what to be, and an unknown one opens a dialog. Its file carries its name as text.)
// Shared by the loader (dinput8.cpp) and the core (asi.cpp).
#pragma once
#include <windows.h>
#include <string>

inline bool IsAsiLoaderFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string buf;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (16 << 20)) {
        buf.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, &buf[0], static_cast<DWORD>(buf.size()), &got, nullptr)) got = 0;
        buf.resize(got);
    }
    CloseHandle(h);
    // TW3SE's own dinput8.dll carries these texts too (its own check, src\loader\dinput8.cpp) - and the UTF-16 name of
    // the dinput8_chain.dll it loads, which the ASI loader never does (so with UAL as winmm.dll TW3SE's loader is not
    // counted as a second one)
    static const char kChain[] = "d\0i\0n\0p\0u\0t\0" "8\0_\0c\0h\0a\0i\0n\0";
    if (buf.find(std::string(kChain, sizeof kChain - 1)) != std::string::npos) return false;
    return buf.find("Ultimate-ASI-Loader") != std::string::npos || buf.find("Ultimate ASI Loader") != std::string::npos;
}
