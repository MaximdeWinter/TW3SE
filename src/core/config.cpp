// TW3SE core - a tiny config file: <game>\bin\x64_dx12\tw3se\tw3se.config, "key = value" per line, '#' comments.
#include "core.h"
#include <fstream>
#include <map>
#include <string>

namespace tw3se {
namespace {

std::map<std::string, std::string> g_config;

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

void LoadConfig() {
    g_config.clear();
    std::ifstream f((Dir() + L"\\tw3se.config").c_str());
    if (!f) return;
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(t.substr(0, eq));
        std::string val = Trim(t.substr(eq + 1));
        if (!key.empty()) {
            g_config[key] = val;
            ++n;
        }
    }
    Log("config: %d setting(s) from tw3se.config", n);
}

std::string ConfigGet(const char* key, const char* def) {
    auto it = g_config.find(key ? key : "");
    return it == g_config.end() ? std::string(def ? def : "") : it->second;
}

bool ConfigBool(const char* key, bool def) {
    std::string v = ConfigGet(key, def ? "true" : "false");
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

}  // namespace tw3se
