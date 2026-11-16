// TW3SE core - messages between WitcherScript, Lua and plugins (TW3SE 1.2).
//
// A message is a channel name and a text. Whoever sends it (TW3SE_Send in a script, w3.send in Lua, SendToChannel in a
// plugin), every listener of the channel gets it on the main thread: plugins through RegisterOnMessage, Lua through
// w3.on_message, scripts by asking (TW3SE_Receive(channel) hands out the next text of the channel, "" when there is
// none). This takes the place of the queues mods built for themselves (a native that pops a command string, ~270
// branches that parse it: Wolven Trainer). A message sent from another thread is delivered at the next tick.
#include "core.h"

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace tw3se {
namespace {

struct Listener {
    std::string channel;  // "*": every channel
    TW3SE_MessageCallback cb;
    void* ctx;
};
std::mutex g_mx;
std::vector<Listener> g_listeners;
std::map<std::string, std::deque<std::string>> g_scriptInbox;  // per channel, for TW3SE_Receive
std::deque<std::pair<std::string, std::string>> g_luaInbox;    // for w3.on_message (lua_lib.inc drains it)
bool g_luaListens = false;
constexpr size_t kInboxMax = 1024;  // per channel / for Lua: the oldest go when nobody asks

bool CallGuarded(TW3SE_MessageCallback cb, const char* ch, const char* text, void* ctx) {
    __try {
        cb(ch, text, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Deliver(const std::string& channel, const std::string& text) {
    std::vector<Listener> list;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        list = g_listeners;
        auto& box = g_scriptInbox[channel];
        box.push_back(text);
        if (box.size() > kInboxMax) box.pop_front();
        if (g_luaListens) {
            g_luaInbox.emplace_back(channel, text);
            if (g_luaInbox.size() > kInboxMax) g_luaInbox.pop_front();
        }
    }
    for (const Listener& l : list) {
        if (l.channel != "*" && l.channel != channel) continue;
        if (!CallGuarded(l.cb, channel.c_str(), text.c_str(), l.ctx))
            Log("messages: a listener of '%s' faulted", channel.c_str());
    }
}

struct Pending {
    std::string channel, text;
};
void DeliverTask(void* p) {
    auto m = static_cast<Pending*>(p);
    Deliver(m->channel, m->text);
    delete m;
}

std::string ReadString(void* frame) {
    alignas(16) uint8_t buf[16] = {};
    ReadScriptParam(frame, buf);
    const char* s = *reinterpret_cast<char**>(buf);
    return s ? s : "";
}

// import function TW3SE_Send(channel : string, message : string);
void NatSend(void*, void* frame, void*) {
    std::string ch = ReadString(frame), text = ReadString(frame);
    EndScriptParams(frame);
    if (!ch.empty()) MessageSend(ch.c_str(), text.c_str());
}
// import function TW3SE_Receive(channel : string) : string;  -> the channel's next message, "" when none
void NatReceive(void*, void* frame, void* result) {
    std::string ch = ReadString(frame);
    EndScriptParams(frame);
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_scriptInbox.find(ch);
        if (it != g_scriptInbox.end() && !it->second.empty()) {
            text = std::move(it->second.front());
            it->second.pop_front();
        }
    }
    if (result) SetScriptResultString(result, text.data(), text.size());
}
// import function TW3SE_Pending(channel : string) : int;  -> how many messages of the channel wait
void NatPending(void*, void* frame, void* result) {
    std::string ch = ReadString(frame);
    EndScriptParams(frame);
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_scriptInbox.find(ch);
        if (it != g_scriptInbox.end()) n = static_cast<int>(it->second.size());
    }
    if (result) *static_cast<int32_t*>(result) = n;
}

}  // namespace

void MessageSend(const char* channel, const char* text) {
    if (!channel || !*channel) return;
    if (OnMainThread()) {
        Deliver(channel, text ? text : "");
        return;
    }
    RunOnMainThread(&DeliverTask, new Pending{channel, text ? text : ""});
}

int MessageListen(const char* channel, TW3SE_MessageCallback cb, void* ctx) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    g_listeners.push_back({channel && *channel ? channel : "*", cb, ctx});
    return 1;
}

int DropMessageListenersIn(uintptr_t lo, uintptr_t hi) {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0;
    for (size_t i = g_listeners.size(); i-- > 0;) {
        auto a = reinterpret_cast<uintptr_t>(g_listeners[i].cb);
        if (a >= lo && a < hi) {
            g_listeners.erase(g_listeners.begin() + static_cast<long>(i));
            ++n;
        }
    }
    return n;
}

// Lua (lua.cpp w3._message_take): the messages since the last call; the first call starts the Lua inbox
std::vector<std::pair<std::string, std::string>> MessageTakeForLua() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_luaListens = true;
    std::vector<std::pair<std::string, std::string>> out(g_luaInbox.begin(), g_luaInbox.end());
    g_luaInbox.clear();
    return out;
}

void QueueMessageNatives() {
    QueueNative("TW3SE_Send", &NatSend, 2);
    QueueNative("TW3SE_Receive", &NatReceive, 1);
    QueueNative("TW3SE_Pending", &NatPending, 1);
}

}  // namespace tw3se
