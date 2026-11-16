// TW3SE core - log file.
#include "core.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace tw3se {
namespace {
FILE* g_file = nullptr;
std::mutex g_mx;
}  // namespace

void LogOpen(const std::wstring& path) {
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_file) g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);   // readable while the game runs
}

void LogV(const char* prefix, const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (prefix && *prefix) fprintf(g_file, "[%s] ", prefix);
    vfprintf(g_file, fmt, ap);
    fputc('\n', g_file);
    fflush(g_file);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(nullptr, fmt, ap);
    va_end(ap);
}

}  // namespace tw3se
