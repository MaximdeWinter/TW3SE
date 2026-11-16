// TW3SE core - mount DLC folders into the running game.
//
// The engine builds a content pack from <dlc>\content\ and attaches it to the depot, the way it does for mod.io mods
// (CContentManager::LoadModioMods): ScanContentPack -> CreatePackage -> AttachPackage. The DLC's resources are
// loadable right away; the DLC manager notices the new content (isNewContentAvailable) and mounts its mounters
// (quests, items, ...) on the next game start or save load.
#include "core.h"
#include <atomic>

namespace tw3se {
namespace {

// Engine string { char* data; uint32 size incl. NUL }, packed to 4. Read-only inputs may live in our memory.
#pragma pack(push, 4)
struct RedString {
    const char* data;
    uint32_t size;
};
struct RedArray {
    void* data;
    uint32_t size;
};
#pragma pack(pop)

using ScanContentPack_t = bool (*)(void* cm, const RedString* path, const uint32_t* name, const RedArray* exclude,
                                   void* info, bool a6, bool a7);
using CreatePackage_t = void* (*)(void* cm, void* info, uint64_t, uint64_t, bool);
using AttachPackage_t = void (*)(void* cm, void* pkg);
using NamesPoolGet_t = void* (*)();
using NamesPoolAdd_t = uint32_t (*)(void* pool, const char* name);
using DLCScan_t = void (*)(void* dlcManager, uint32_t scanType, bool flag);

constexpr size_t kPackageIdOffset = 0x40;  // CContentPackage::m_packageID (5.0)

int g_pendingScan = 0;  // ticks until a post-mount DLC-manager scan (spans ticks so the attach is processed first)
std::atomic<int> g_scansDone{0};  // post-mount scans run so far: a reload waits for the one it asked for (DLCScansDone)

void Reply(char* reply, uint32_t size, const std::string& s) { strncpy_s(reply, size, s.c_str(), _TRUNCATE); }

char* Game() {
    auto g = reinterpret_cast<char**>(Address("GGame"));
    return g ? *g : nullptr;
}

char* DLCManager() {
    char* game = Game();
    size_t off = Address("CCommonGame.dlcManager");
    return game && off ? *reinterpret_cast<char**>(game + off) : nullptr;
}

void CmdMount(const char* args, char* reply, uint32_t size) { MountDLC(args, reply, size); }

void CmdState(const char*, char* reply, uint32_t size) {
    char* game = Game();
    char* dlc = DLCManager();
    size_t flag = Address("CDLCManager.isNewContentAvailable");
    char buf[200];
    snprintf(buf, sizeof buf, "game %p active %d dlcmgr %p newContent %d scans %d pending %d", game,
             game ? game[0x110] : -1, dlc, dlc && flag ? dlc[flag] : -1, g_scansDone.load(), g_pendingScan);
    Reply(reply, size, buf);
}

void CmdDLCScan(const char* args, char* reply, uint32_t size) {
    char* dlc = DLCManager();
    auto scan = Fn<DLCScan_t>("CDLCManager::Scan");
    if (!dlc || !scan) return Reply(reply, size, "error no DLC manager");
    uint32_t type = args && *args ? strtoul(args, nullptr, 10) : 0;
    scan(dlc, type, type != 0);
    Reply(reply, size, "ok scanned (type " + std::to_string(type) + ")");
}

void CmdLiveDLC(const char* args, char* reply, uint32_t size) { LiveLoadDLC(args, reply, size); }

}  // namespace

bool MountDLC(const char* dlcFolder, char* reply, uint32_t replySize) {
    if (!OnMainThread()) {
        Reply(reply, replySize, "error MountDLC must run on the main thread");
        return false;
    }
    auto cmPtr = reinterpret_cast<void**>(Address("GContentManager"));
    void* cm = cmPtr ? *cmPtr : nullptr;
    auto scanPack = Fn<ScanContentPack_t>("CContentManager::ScanContentPack");
    auto create = Fn<CreatePackage_t>("CContentManager::CreatePackage");
    auto attach = Fn<AttachPackage_t>("CContentManager::AttachPackage");
    auto poolGet = Fn<NamesPoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamesPoolAdd_t>("CNamesPool::AddEntry");
    if (!cm || !scanPack || !create || !attach || !poolGet || !poolAdd) {
        Reply(reply, replySize, "error content manager not available in this build");
        return false;
    }
    std::string d = dlcFolder ? dlcFolder : "";
    while (!d.empty() && (d.back() == '\\' || d.back() == '/' || d.back() == ' ')) d.pop_back();
    size_t slash = d.find_last_of("\\/");
    std::string folder = slash == std::string::npos ? d : d.substr(slash + 1);
    std::string content = d + "\\content\\";
    if (folder.empty() || GetFileAttributesA(content.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Reply(reply, replySize, "error no content folder: " + content);
        return false;
    }
    RedString path{content.c_str(), static_cast<uint32_t>(content.size() + 1)};
    uint32_t name = poolAdd(poolGet(), folder.c_str());
    RedArray exclude{nullptr, 0};
    // Pack info (0x34 bytes, zeroed), filled by ScanContentPack, read by CreatePackage. Kept for the session: the
    // package may point into what the engine allocated for it.
    void* info = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!scanPack(cm, &path, &name, &exclude, info, false, true)) {
        Reply(reply, replySize, "error the engine found no content in " + content);
        return false;
    }
    void* pkg = create(cm, info, 0, 0, false);
    if (!pkg) {
        Reply(reply, replySize, "error CreatePackage failed");
        return false;
    }
    uint32_t id = *reinterpret_cast<uint32_t*>(static_cast<char*>(pkg) + kPackageIdOffset);
    attach(cm, pkg);
    Log("mounted %s as package %u", folder.c_str(), id);
    Reply(reply, replySize, "ok mounted " + folder + " package " + std::to_string(id));
    return true;
}

bool LiveLoadDLC(const char* dlcFolder, char* reply, uint32_t replySize) {
    char r[256] = {0};
    if (!MountDLC(dlcFolder, r, sizeof r)) {
        Reply(reply, replySize, r);  // MountDLC already wrote "error ..."
        return false;
    }
    // Registering the DLC with the manager (Scan reads the .reddlc from the attached bundle) must happen a few
    // ticks after the mount - the content manager processes the attach over ticks, and a console command/native
    // runs within a single main-thread tick. So schedule the scan; TickContent runs it later. Then a save reload
    // (CDLCManager::OnGameStarting) mounts the DLC and starts its quests. Proven order: mount -> (ticks) -> scan -> reload.
    if (!DLCManager() || !Fn<DLCScan_t>("CDLCManager::Scan")) {
        Reply(reply, replySize, "error DLC manager not available in this build");
        return false;
    }
    g_pendingScan = 120;  // ~2 s at 60 fps (more at the throttled background frame rate)
    Reply(reply, replySize, std::string(r) + "; registering (~2s), then reload a save to activate quests");
    return true;
}

// Called once per tick from the main-thread hook: runs the deferred post-mount DLC scan.
void TickContent() {
    if (g_pendingScan > 0 && --g_pendingScan == 0) {
        char* dlc = DLCManager();
        auto scan = Fn<DLCScan_t>("CDLCManager::Scan");
        if (dlc && scan) {
            scan(dlc, 1, true);   // look for new content
            scan(dlc, 0, false);
            Log("content: post-mount DLC registration scan done");
        }
        ++g_scansDone;  // (a reload before it would start the save without the new quest; at a low frame rate the
                        // scan can come 30 s late)
    }
}
int DLCScansDone() { return g_scansDone.load(); }
bool DLCScanPending() { return g_pendingScan > 0; }

void RegisterContentCommands() {
    RegisterCommand("mount", &CmdMount, "mount <dlc folder>: attach a DLC installed while the game runs");
    RegisterCommand("state", &CmdState, "game and DLC manager flags");
    RegisterCommand("dlcscan", &CmdDLCScan, "dlcscan [type]: let the DLC manager look for new .reddlc files");
    RegisterCommand("livedlc", &CmdLiveDLC,
                    "livedlc <dlc folder>: mount + register a DLC for live loading (then reload a save)");
}

}  // namespace tw3se
