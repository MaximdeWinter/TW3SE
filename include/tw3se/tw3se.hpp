/* TW3SE (The Witcher 3 Script Extender) - optional C++ convenience layer over the stable C ABI (tw3se.h).
 *
 * The C interface (tw3se.h) is the ABI boundary. This header is for plugin authors who build with C++17: a
 * WitcherScript native is an ordinary typed C++ function. You register
 *
 *     int Add(int a, int b) { return a + b; }
 *     tw3se::register_native<&Add>("Add");            // in TW3SE_PluginLoad, after tw3se::init(api)
 *
 * and a script calls it as `import function Add(a : int, b : int) : int;`. The parameter reading, the
 * end-of-arguments marker and the return write come from Add's signature, and so does the parameter count
 * passed to RegisterNative.
 *
 * Supported parameter and return types: int, bool, float, std::string (a WitcherScript `string`, UTF-8) and
 * tw3se::name (a WitcherScript `name`). const char* works as a parameter. Strings and names as return values
 * need TW3SE 1.2 (interface 21).
 * A WitcherScript type keyword (name, string, int, bool, float, ...) as a parameter name in the
 * `import function` line is a compile error in WitcherScript.
 *
 * Header-only; it allocates nothing of its own and throws nothing across the ABI boundary.
 */
#ifndef TW3SE_HPP
#define TW3SE_HPP

#include "tw3se/tw3se.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace tw3se {

/* The interface pointer the convenience helpers use. Set it once at the top of TW3SE_PluginLoad:
 *     tw3se::init(api);
 * Returns the stored pointer elsewhere via tw3se::api(). */
inline const TW3SE_Interface*& _api_ref() {
    static const TW3SE_Interface* p = nullptr;
    return p;
}
inline void init(const TW3SE_Interface* api) { _api_ref() = api; }
inline const TW3SE_Interface* api() { return _api_ref(); }

/* A WitcherScript `name`: its id in the game's name pool. tw3se::name("Geralt") makes one from a text,
 * str() gives the text back. */
struct name {
    uint32_t id = 0;
    name() = default;
    explicit name(uint32_t i) : id(i) {}
    explicit name(const char* text) : id(text && api()->interfaceVersion >= 21 ? api()->NameFromString(text) : 0) {}
    const char* str() const { return api()->GetCNameString(id); }
    bool valid() const { return id != 0; }
};

/* --- reading one script parameter into a C++ value (declaration order) --- */
/* A WitcherScript `string` arrives as a pointer to char* at the front of the out buffer; int/bool/float use
 * their 4-byte representation. We always hand ReadParam a zeroed 16-byte buffer to be safe, then interpret. */
template <class T>
T _read_one(void* frame) {
    unsigned char buf[16] = {0};
    api()->ReadParam(frame, buf);
    if constexpr (std::is_same_v<T, int>) {
        int v; std::memcpy(&v, buf, sizeof v); return v;
    } else if constexpr (std::is_same_v<T, bool>) {
        int v; std::memcpy(&v, buf, sizeof v); return v != 0;
    } else if constexpr (std::is_same_v<T, float>) {
        float v; std::memcpy(&v, buf, sizeof v); return v;
    } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, const char*>) {
        char* s; std::memcpy(&s, buf, sizeof s); return T(s ? s : "");
    } else if constexpr (std::is_same_v<T, name>) {
        uint32_t v; std::memcpy(&v, buf, sizeof v); return name(v);
    } else {
        static_assert(sizeof(T) == 0, "tw3se: unsupported native parameter type");
        return T{};
    }
}

/* --- writing the return value into the result slot --- */
template <class R>
void _write_result(void* result, R v) {
    if (!result) return;
    if constexpr (std::is_same_v<R, bool>) {
        *static_cast<bool*>(result) = v;                 /* the engine's bool result slot is one byte */
    } else if constexpr (std::is_same_v<R, std::string>) {
        if (api()->interfaceVersion >= 21) api()->ReturnString(result, v.data(), static_cast<uint32_t>(v.size()));
    } else if constexpr (std::is_same_v<R, name>) {
        *static_cast<uint32_t*>(result) = v.id;
    } else {
        static_assert(std::is_same_v<R, int> || std::is_same_v<R, float> || std::is_same_v<R, bool>,
                      "tw3se: unsupported native return type");
        std::memcpy(result, &v, sizeof v);
    }
}

/* --- function-pointer traits --- */
template <class F> struct _fn_traits;
template <class R, class... A> struct _fn_traits<R (*)(A...)> {
    using ret = R;
    using args = std::tuple<std::decay_t<A>...>;
    static constexpr int arity = static_cast<int>(sizeof...(A));
};

/* Read a tuple of arguments in declaration order. The braced-init-list is evaluated left-to-right, so the
 * parameters are read from the script stack in the correct sequence. */
template <class Tuple, std::size_t... I>
Tuple _read_args_impl(void* frame, std::index_sequence<I...>) {
    return Tuple{_read_one<std::tuple_element_t<I, Tuple>>(frame)...};
}
template <class Tuple>
Tuple _read_args(void* frame) {
    return _read_args_impl<Tuple>(frame, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

/* The non-capturing thunk the engine calls. Reads the arguments left-to-right (braced-init guarantees the
 * order), skips the end-of-arguments marker, invokes the C++ function and writes its return value. */
template <auto Fn>
void _native_thunk(void* self, void* frame, void* result) {
    (void)self;
    using Tr = _fn_traits<decltype(Fn)>;
    typename Tr::args a = _read_args<typename Tr::args>(frame);
    api()->EndParams(frame);
    if constexpr (std::is_void_v<typename Tr::ret>) {
        std::apply(Fn, a);
        (void)result;
    } else {
        _write_result<typename Tr::ret>(result, std::apply(Fn, a));
    }
}

/* Register a WitcherScript native from a plain C++ function pointer known at compile time. The parameter
 * count given to the engine is the function's arity, so it always matches the handler. Returns true on
 * success. Call after tw3se::init(api). */
template <auto Fn>
bool register_native(const char* name) {
    using Tr = _fn_traits<decltype(Fn)>;
    return api()->RegisterNative(name, &_native_thunk<Fn>, Tr::arity) != 0;
}

/* Small helpers mirroring the C interface, for symmetry. */
inline bool register_command(const char* name, TW3SE_Command handler, const char* help) {
    return api()->RegisterCommand(name, handler, help) != 0;
}

/* Register a per-tick callback (needs interface version 3; set minInterface accordingly). */
inline void on_tick(TW3SE_TickCallback callback, void* context = nullptr) {
    api()->RegisterOnTick(callback, context);
}

/* Register a per-present callback for overlay drawing (needs interface version 8). */
inline void on_present(TW3SE_PresentCallback callback, void* context = nullptr) {
    api()->RegisterOnPresent(callback, context);
}

/* Call a global function / a method with typed arguments (needs interface version 9). argspec is a
 * space-separated typespec: "i:N" int, "f:N" float, "s:text" string, "b:1" bool true. e.g.
 *   int n = 0; tw3se::call_function_v("StrLen", "s:witcher", nullptr, &n);   // n == 7 */
inline bool call_function_v(const char* name, const char* argspec, void* self, void* result) {
    return api()->CallFunctionV(name, argspec, self, result) != 0;
}
inline bool call_method_v(void* object, const char* name, const char* argspec, void* result) {
    return api()->CallMethodV(object, name, argspec, result) != 0;
}

/* Overlay drawing (needs interface version 10). Register a per-frame callback and draw inside it; the draw
 * calls only take effect while the callback runs. Pixels from the top-left, colour 0xRRGGBBAA. e.g.
 *   tw3se::on_overlay_draw([](void*){ tw3se::overlay_rect(10,10,200,40, 0x000000A0);
 *                                     tw3se::overlay_text(16,18, 0xFFFFFFFF, "HP 100/100"); }); */
inline void on_overlay_draw(TW3SE_OverlayDrawCallback callback, void* context = nullptr) {
    api()->RegisterOnOverlayDraw(callback, context);
}
inline void overlay_text(float x, float y, uint32_t rgba, const char* text) {
    api()->OverlayDrawText(x, y, rgba, text);
}
inline void overlay_rect(float x, float y, float w, float h, uint32_t rgba) {
    api()->OverlayDrawRect(x, y, w, h, rgba);
}
inline void overlay_screen_size(float* w, float* h) { api()->OverlayGetScreenSize(w, h); }

/* Fired after a new game starts or a save loads (needs interface version 11). Re-apply persistent changes here
 * by reading them back from disk and applying them. */
inline void on_game_loaded(TW3SE_GameLoadedCallback callback, void* context = nullptr) {
    api()->RegisterOnGameLoaded(callback, context);
}

/* Before and after the game rebuilds its render resources (interface version 25; false on an older TW3SE):
 * callback(TW3SE_REBUILD_BEFORE or _AFTER, context) on the main thread. Let go of the game's render data before,
 * take it again after. */
inline bool on_render_rebuild(TW3SE_RenderRebuildCallback callback, void* context = nullptr) {
    return api()->interfaceVersion >= 25 && api()->RegisterOnRenderRebuild(callback, context) != 0;
}

/* True inside the REDkit editor, false in the game (interface version 15; false on an older TW3SE). */
inline bool is_editor() { return api()->interfaceVersion >= 15 && api()->IsEditor() != 0; }

/* Lua (interface version 16; both return false on an older TW3SE). run_lua on the main thread only. */
inline bool run_lua(const char* code, char* reply, uint32_t size) {
    return api()->interfaceVersion >= 16 && api()->RunLua(code, reply, size) != 0;
}
inline bool register_lua_function(const char* name, TW3SE_LuaFunction fn, void* context = nullptr) {
    return api()->interfaceVersion >= 16 && api()->RegisterLuaFunction(name, fn, context) != 0;
}

/* Sounds (interface version 17; 0 / false on an older TW3SE). */
inline int32_t play_sound(const char* file, void* entity = nullptr, const char* bone = nullptr, float volume = 1.0f,
                          bool loop = false) {
    return api()->interfaceVersion >= 17 ? api()->PlaySound(file, entity, bone, volume, loop ? 1 : 0) : 0;
}
inline bool stop_sound(int32_t sound) { return api()->interfaceVersion >= 17 && api()->StopSound(sound) != 0; }
inline bool set_sound_volume(int32_t sound, float volume) {
    return api()->interfaceVersion >= 17 && api()->SetSoundVolume(sound, volume) != 0;
}
inline bool sound_playing(int32_t sound) { return api()->interfaceVersion >= 17 && api()->SoundPlaying(sound) != 0; }
inline bool preload_sound(const char* file) { return api()->interfaceVersion >= 17 && api()->PreloadSound(file) != 0; }

/* Typed values (interface version 18). tw3se::value(7), value(2.5), value(true), value("text"), object(ptr),
 * structure("Vector", &vec, sizeof vec) make a TW3SE_Value; call() returns the return value and the out arguments
 * (empty on an error, tw3se::last_error() says why). Main thread only.
 *   auto r = tw3se::call(nullptr, "StrLen", {tw3se::value("witcher")});   // r[0].v.i == 7
 *   auto pos = tw3se::call(tw3se::the_player(), "GetWorldPosition");      // r[0]: a Vector struct */
inline TW3SE_Value value(int64_t i) { TW3SE_Value v{}; v.type = TW3SE_INT; v.v.i = i; return v; }
inline TW3SE_Value value(int i) { return value(static_cast<int64_t>(i)); }
inline TW3SE_Value value(double f) { TW3SE_Value v{}; v.type = TW3SE_FLOAT; v.v.f = f; return v; }
inline TW3SE_Value value(bool b) { TW3SE_Value v{}; v.type = TW3SE_BOOL; v.v.i = b ? 1 : 0; return v; }
inline TW3SE_Value value(const char* s) { TW3SE_Value v{}; v.type = TW3SE_STRING; v.v.s = s; return v; }
inline TW3SE_Value object(void* o) { TW3SE_Value v{}; v.type = TW3SE_OBJECT; v.v.object = o; return v; }
inline TW3SE_Value structure(const char* typeName, const void* data, uint32_t size) {
    TW3SE_Value v{}; v.type = TW3SE_STRUCT; v.typeName = typeName; v.v.data = data; v.count = size; return v;
}
inline std::vector<TW3SE_Value> call(void* object, const char* name, std::initializer_list<TW3SE_Value> args = {},
                                     int maxResults = 8) {
    std::vector<TW3SE_Value> a(args), r(static_cast<size_t>(maxResults));
    if (api()->interfaceVersion < 18) return {};
    int n = api()->Call(object, name, a.data(), static_cast<int>(a.size()), r.data(), maxResults);
    r.resize(n < 0 ? 0 : (n < maxResults ? n : maxResults));
    return r;
}
inline bool get_field(void* object, const char* field, TW3SE_Value& out) {
    return api()->interfaceVersion >= 18 && api()->GetField(object, field, &out) != 0;
}
inline bool set_field(void* object, const char* field, const TW3SE_Value& v) {
    return api()->interfaceVersion >= 18 && api()->SetField(object, field, &v) != 0;
}
inline const char* last_error() { return api()->interfaceVersion >= 18 ? api()->LastError() : "TW3SE too old (interface 18)"; }

/* --- generic RTTI property access (needs interface version 4) --- */
/* Find a property descriptor by name on an object's CClass (walks inheritance). Returns null if absent.
 * Cache the result: the same CProperty* works for every object of that class. */
inline void* find_property(void* cclass, const char* name) { return api()->FindProperty(cclass, name); }

/* Typed pointer to a field inside `object`, given its CProperty*. Null if either input is null. Use the
 * field's known type (vitality: float, money: int, a flag: bool, a position: a 3-float Vector struct). */
template <class T>
T* property_ptr(void* object, void* cproperty) {
    return static_cast<T*>(api()->PropertyPtr(object, cproperty));
}

/* Read / write a field in one call, looking the property up by name each time (cache the CProperty* yourself
 * in a hot path). Returns a default / does nothing if the property or object is not found. */
template <class T>
T read_property(void* object, void* cclass, const char* name, T fallback = T{}) {
    void* p = api()->FindProperty(cclass, name);
    T* ptr = p ? static_cast<T*>(api()->PropertyPtr(object, p)) : nullptr;
    return ptr ? *ptr : fallback;
}
template <class T>
bool write_property(void* object, void* cclass, const char* name, const T& value) {
    void* p = api()->FindProperty(cclass, name);
    T* ptr = p ? static_cast<T*>(api()->PropertyPtr(object, p)) : nullptr;
    if (ptr) *ptr = value;
    return ptr != nullptr;
}

/* An object's CClass* (virtual GetClass) and a class by name (needs interface version 5). */
inline void* get_object_class(void* object) { return api()->GetObjectClass(object); }
inline void* find_class(const char* name) { return api()->FindClassByName(name); }

/* Call a global WitcherScript native from C++ with string arguments (needs interface version 6). `self` is the
 * context (nullptr for plain globals); `result` receives int/bool. Returns true if found and invoked.
 * e.g.  int n = 0; tw3se::call_function("StrLen", {"hello"}, nullptr, &n);  // n == 5 */
inline bool call_function(const char* name, std::initializer_list<const char*> args, void* self, void* result) {
    std::vector<const char*> v(args);
    return api()->CallFunction(name, v.data(), static_cast<int>(v.size()), self, result) != 0;
}

/* --- player/game, method calls, handles, names (needs interface version 7) --- */
inline void* the_player() { return api()->GetThePlayer(); }
inline void* the_game() { return api()->GetTheGame(); }
inline void* resolve_handle(void* handle) { return api()->ResolveHandle(handle); }
inline const char* cname_string(uint32_t id) { return api()->GetCNameString(id); }

/* Call a method on `object` with string arguments. For a method that returns an object/component, pass the
 * result through resolve_handle() before using it. e.g.:
 *   void* handle = nullptr; tw3se::call_method(player, "GetMovingAgentComponent", {}, &handle);
 *   void* comp = tw3se::resolve_handle(handle); */
inline bool call_method(void* object, const char* name, std::initializer_list<const char*> args, void* result) {
    std::vector<const char*> v(args);
    return api()->CallMethod(object, name, v.data(), static_cast<int>(v.size()), result) != 0;
}

/* The ergonomic form: read/write a field by name, with the object's class derived automatically. This is the
 * everyday call - e.g. tw3se::write_property_of<float>(player, "vitality", 1000.f). */
template <class T>
T read_property_of(void* object, const char* name, T fallback = T{}) {
    void* cls = api()->GetObjectClass(object);
    return cls ? read_property<T>(object, cls, name, fallback) : fallback;
}
template <class T>
bool write_property_of(void* object, const char* name, const T& value) {
    void* cls = api()->GetObjectClass(object);
    return cls ? write_property<T>(object, cls, name, value) : false;
}

/* --- objects of the game and world edits (needs interface version 12) --- */
/* The loaded object with this GUID, or nullptr when it is not loaded. Keep the GUID between ticks, not the
 * pointer. A GUID as text ("bd5160b6 45e69f5e cf66a598 084ca441", what TW3SE_EntityGuid gives) works too. */
inline void* entity_by_guid(const uint32_t guid[4]) { return api()->EntityByGuid(guid); }
inline void* entity_by_guid(const char* text) {
    unsigned g[4] = {0};
    if (!text || sscanf_s(text, "%x %x %x %x", &g[0], &g[1], &g[2], &g[3]) != 4) return nullptr;
    uint32_t guid[4] = {g[0], g[1], g[2], g[3]};
    return api()->EntityByGuid(guid);
}
/* An entity's GUID; false for one the game made at run time (a counter that does not come back). */
inline bool entity_guid(void* entity, uint32_t out[4]) { return api()->EntityGuid(entity, out) != 0; }
/* One world.txt line for this session, applied at once if the object is loaded (docs/manual/world-edit.md). */
inline bool world_edit(const char* line) { return api()->WorldEdit(line) != 0; }

/* Messages, the game's state and the controller (interface 21, TW3SE 1.2).
 *   tw3se::send("mymod.ui", "opened");                         // to scripts (TW3SE_Receive), Lua and plugins
 *   tw3se::on_message("mymod.cmd", [](const char*, const char* text, void*) { ... });
 *   if (tw3se::game_state() & TW3SE_STATE_IN_GAME) ...
 *   uint32_t b; float ax[6]; if (tw3se::pad(&b, ax) && (b & XINPUT_GAMEPAD_A)) ...                */
inline void send(const char* channel, const char* message) {
    if (api()->interfaceVersion >= 21) api()->SendToChannel(channel, message ? message : "");
}
inline bool on_message(const char* channel, TW3SE_MessageCallback fn, void* context = nullptr) {
    return api()->interfaceVersion >= 21 && api()->RegisterOnMessage(channel, fn, context) != 0;
}
inline uint32_t game_state() { return api()->interfaceVersion >= 21 ? api()->GameState() : 0; }
inline bool pad(uint32_t* buttons, float* axes = nullptr) {
    return api()->interfaceVersion >= 21 && api()->PadRead(buttons, axes) != 0;
}

/* Values that go with the save, and web requests (TW3SE 1.2; through TW3SE's Lua, main thread).
 *   tw3se::saved_set("MyMod", "kills", "12");   std::string k = tw3se::saved_get("MyMod", "kills");
 *   tw3se::http_request("GET", "https://example.com/x.json", "", "mymod.web");
 *   tw3se::on_message("mymod.web", ...)  -> "200\n<body>" (status 0 and the reason when no answer came) */
inline std::string _lua_quote(const std::string& s) {  // a Lua string literal, every byte kept
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c == 127) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\%03u", c);
            out += esc;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}
inline std::string _lua(const std::string& code) {
    char reply[4096] = {0};
    if (api()->interfaceVersion < 21 || !api()->RunLua(code.c_str(), reply, sizeof reply)) return {};
    return reply;
}
inline bool saved_set(const char* mod, const char* key, const char* value) {
    return _lua("w3.saved('plugin.' .. " + _lua_quote(mod) + ")[ " + _lua_quote(key) + " ] = " + _lua_quote(value) +
                " return 'ok'") == "ok";
}
inline std::string saved_get(const char* mod, const char* key) {
    return _lua("return w3.saved('plugin.' .. " + _lua_quote(mod) + ")[ " + _lua_quote(key) + " ] or ''");
}
inline bool http_request(const char* method, const char* url, const char* body, const char* channel) {
    std::string c = _lua_quote(channel);
    return _lua("w3.http.request({method = " + _lua_quote(method && *method ? method : "GET") + ", url = " +
                 _lua_quote(url) + ", body = " + (body && *body ? _lua_quote(body) : std::string("nil")) +
                 "}, function(s, b, h, e) w3.send(" + c + ", tostring(s) .. '\\n' .. (b or e or '')) end) return 'ok'") ==
           "ok";
}

}  // namespace tw3se

/* Defines TW3SE_PluginQuery from literals, so a plugin only writes TW3SE_PluginLoad. `minIface` is the lowest
 * interface version the plugin needs (2 for RegisterNative). */
#define TW3SE_PLUGIN(NAME, AUTHOR, VERSION_U32, MIN_IFACE)                 \
    TW3SE_EXPORT int TW3SE_PluginQuery(TW3SE_PluginInfo* info) {           \
        info->name = (NAME);                                              \
        info->author = (AUTHOR);                                          \
        info->version = (VERSION_U32);                                    \
        info->minInterface = (MIN_IFACE);                                 \
        return 1;                                                         \
    }

#endif /* TW3SE_HPP */
