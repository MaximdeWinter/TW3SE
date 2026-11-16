// The script debugger's connection: one client on 127.0.0.1, text lines both ways. The
// debugger itself is Lua (lua\lib\debugger.lua): it reads the lines with w3._dbg_recv, also while the main thread
// stands at a breakpoint, and answers with w3._dbg_send. tools\vscode-witcherscript connects VS Code to it.
#include <winsock2.h>  // before windows.h (core.h)
#include <ws2tcpip.h>

#include "core.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace tw3se {
namespace {

std::mutex g_mx;
std::condition_variable g_cv;
std::deque<std::string> g_in;
SOCKET g_listen = INVALID_SOCKET;
SOCKET g_client = INVALID_SOCKET;
std::mutex g_sendMx;
bool g_started = false;

DWORD WINAPI ServeThread(void*) {
    for (;;) {
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            Sleep(500);
            continue;
        }
        {
            std::lock_guard<std::mutex> l(g_mx);
            if (g_client != INVALID_SOCKET) {  // one client at a time: the new one takes over
                closesocket(g_client);
            }
            g_client = c;
            g_in.clear();
            g_in.push_back("connected");
        }
        g_cv.notify_all();
        Log("debugger: a client connected");
        std::string buf;
        char chunk[4096];
        for (;;) {
            int n = recv(c, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            buf.append(chunk, n);
            size_t at;
            bool any = false;
            while ((at = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, at);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, at + 1);
                std::lock_guard<std::mutex> l(g_mx);
                g_in.push_back(std::move(line));
                any = true;
            }
            if (any) g_cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> l(g_mx);
            if (g_client == c) {
                g_client = INVALID_SOCKET;
                g_in.push_back("disconnected");
            }
        }
        g_cv.notify_all();
        closesocket(c);
        Log("debugger: the client left");
    }
}

}  // namespace

bool DebugListen(int port) {
    if (g_started) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 1) != 0) {
        Log("debugger: port %d is taken (%d)", port, WSAGetLastError());
        closesocket(s);
        return false;
    }
    g_listen = s;
    g_started = true;
    HANDLE t = CreateThread(nullptr, 0, &ServeThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    Log("debugger: listening on 127.0.0.1:%d", port);
    return true;
}

bool DebugConnected() {
    std::lock_guard<std::mutex> l(g_mx);
    return g_client != INVALID_SOCKET;
}

bool DebugRecv(std::string& line, int timeoutMs) {
    std::unique_lock<std::mutex> l(g_mx);
    if (g_in.empty() && timeoutMs > 0)
        g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [] { return !g_in.empty(); });
    if (g_in.empty()) return false;
    line = std::move(g_in.front());
    g_in.pop_front();
    return true;
}

void DebugSend(const std::string& line) {
    SOCKET c;
    {
        std::lock_guard<std::mutex> l(g_mx);
        c = g_client;
    }
    if (c == INVALID_SOCKET) return;
    std::string out = line + "\n";
    std::lock_guard<std::mutex> l(g_sendMx);
    const char* p = out.data();
    int left = static_cast<int>(out.size());
    while (left > 0) {
        int n = send(c, p, left, 0);
        if (n <= 0) return;
        p += n;
        left -= n;
    }
}

}  // namespace tw3se
