// Network from Lua: HTTP(S) requests through WinHTTP, TCP clients and UDP sockets. Every
// request and socket works on a thread of its own; what arrives goes into one queue, which Lua empties each tick
// (w3._net_poll) and hands to the callbacks on the main thread. Lua never waits for the network.
#include <winsock2.h>  // before windows.h (core.h)
#include <ws2tcpip.h>
#include <winhttp.h>

#include "core.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace tw3se {

namespace {

std::mutex g_mx;
std::deque<NetEvent> g_events;
std::atomic<int> g_nextId{1};
std::map<int, SOCKET> g_sockets;  // TCP and UDP sockets by id
bool g_wsa = false;

void Push(NetEvent e) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_events.size() < 10000) g_events.push_back(std::move(e));
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

bool StartWsa() {
    if (g_wsa) return true;
    WSADATA wsa;
    g_wsa = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    return g_wsa;
}

void HttpThread(int id, std::string method, std::string url, std::string headers, std::string body, int timeoutMs) {
    NetEvent e;
    e.id = id;
    e.kind = "http";
    std::wstring wurl = Wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        e.error = "not a URL: " + url;
        Push(std::move(e));
        return;
    }
    HINTERNET s = WinHttpOpen(L"TW3SE", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    std::wstring object = std::wstring(path) + extra;
    HINTERNET r = c ? WinHttpOpenRequest(c, Wide(method).c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (r) WinHttpSetTimeouts(r, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    std::wstring wh = Wide(headers);
    bool ok = r && WinHttpSendRequest(r, wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(),
                                      wh.empty() ? 0 : static_cast<DWORD>(-1L),
                                      body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data(),
                                      static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
              WinHttpReceiveResponse(r, nullptr);
    if (ok) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX);
        e.status = static_cast<int>(status);
        DWORD hlen = 0;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hlen,
                            WINHTTP_NO_HEADER_INDEX);
        if (hlen) {
            std::wstring hw(hlen / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &hw[0], &hlen,
                                    WINHTTP_NO_HEADER_INDEX)) {
                int n = WideCharToMultiByte(CP_UTF8, 0, hw.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string hs(n > 0 ? n - 1 : 0, '\0');
                if (n > 0) WideCharToMultiByte(CP_UTF8, 0, hw.c_str(), -1, &hs[0], n, nullptr, nullptr);
                e.headers = hs;
            }
        }
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
            if (e.body.size() + avail > (64u << 20)) {
                e.error = "the answer is larger than 64 MB";
                break;
            }
            size_t at = e.body.size();
            e.body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(r, &e.body[at], avail, &got)) break;
            e.body.resize(at + got);
        }
    } else {
        e.error = "request failed (" + std::to_string(GetLastError()) + ")";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    Push(std::move(e));
}

void ReadThread(int id, SOCKET sock, bool udp) {
    char buf[65536];
    for (;;) {
        sockaddr_in from{};
        int fromLen = sizeof from;
        int n = udp ? recvfrom(sock, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen)
                    : recv(sock, buf, sizeof buf, 0);
        if (n <= 0) break;
        NetEvent e;
        e.id = id;
        e.kind = udp ? "udp" : "data";
        e.body.assign(buf, n);
        if (udp) {
            char ip[64] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            e.host = ip;
            e.port = ntohs(from.sin_port);
        }
        Push(std::move(e));
    }
    NetEvent e;
    e.id = id;
    e.kind = "close";
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_sockets.erase(id)) closesocket(sock);
        else e.error = "closed";
    }
    Push(std::move(e));
}

void ConnectThread(int id, std::string host, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    NetEvent e;
    e.id = id;
    e.kind = "connect";
    SOCKET s = INVALID_SOCKET;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s != INVALID_SOCKET && connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
        freeaddrinfo(res);
    }
    if (s == INVALID_SOCKET) {
        e.error = "no connection to " + host + ":" + std::to_string(port);
        Push(std::move(e));
        return;
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_sockets[id] = s;
    }
    Push(std::move(e));
    ReadThread(id, s, false);
}

}  // namespace

int NetHttp(const std::string& method, const std::string& url, const std::string& headers, const std::string& body,
            int timeoutMs) {
    int id = g_nextId++;
    std::thread(HttpThread, id, method, url, headers, body, timeoutMs).detach();
    return id;
}

int NetConnect(const std::string& host, int port) {
    if (!StartWsa()) return 0;
    int id = g_nextId++;
    std::thread(ConnectThread, id, host, port).detach();
    return id;
}

// a UDP socket on 127.0.0.1 (this PC only: a socket open to the network would bring up the firewall's question)
int NetUdp(int port, std::string& err) {
    if (!StartWsa()) return err = "no network", 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return err = "no socket", 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return err = "port " + std::to_string(port) + " is taken", 0;
    }
    int id = g_nextId++;
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_sockets[id] = s;
    }
    std::thread(ReadThread, id, s, true).detach();
    return id;
}

std::string NetSend(int id, const std::string& data, const char* host, int port) {
    SOCKET s;
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_sockets.find(id);
        if (it == g_sockets.end()) return "the socket is closed";
        s = it->second;
    }
    if (host) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, std::to_string(port).c_str(), &hints, &res) != 0 || !res) return "unknown host";
        int n = sendto(s, data.data(), static_cast<int>(data.size()), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);
        return n == static_cast<int>(data.size()) ? "" : "send failed";
    }
    size_t at = 0;
    while (at < data.size()) {
        int n = send(s, data.data() + at, static_cast<int>(data.size() - at), 0);
        if (n <= 0) return "send failed";
        at += n;
    }
    return "";
}

void NetClose(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sockets.find(id);
    if (it == g_sockets.end()) return;
    shutdown(it->second, SD_BOTH);
    closesocket(it->second);  // its reading thread ends and reports "close"
    g_sockets.erase(it);
}

std::vector<NetEvent> NetPoll() {
    std::lock_guard<std::mutex> l(g_mx);
    std::vector<NetEvent> out(g_events.begin(), g_events.end());
    g_events.clear();
    return out;
}

}  // namespace tw3se
