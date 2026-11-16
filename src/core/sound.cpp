// TW3SE core - own sounds: a WAV or OGG file played through XAudio2, flat or at an entity (3D, heard from the
// camera), with a volume, once or as a loop, and a handle to stop it. For plugins (interface 17), WitcherScript
// (TW3SE_PlaySound ...) and Lua (w3.sound). XAudio2 2.9 and X3DAudio come from Windows (XAudio2_9.dll), loaded on
// the first sound; OGG is decoded by stb_vorbis (deps\stb). A file is decoded once and kept.
//
// The 3D part runs each tick on the main thread: the listener is the camera of the active world (its camera
// director), the emitter the entity (CNode+0x70, translation in the last row) or one of its bones
// (GetBoneWorldMatrixByIndex). The engine is Z-up and right-handed, X3DAudio Y-up and left-handed: (x, y, z) goes in
// as (x, z, y). An entity is held through its handle block (one reference), so a destroyed entity is seen as gone
// and its sound stops.
#include "core.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <xaudio2.h>
#include <x3daudio.h>

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                                        short** output);

namespace tw3se {
namespace {

using XAudio2Create_t = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
using XAudio2CreateV_t = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR, DWORD);
using X3DInit_t = HRESULT(WINAPI*)(UINT32, FLOAT32, X3DAUDIO_HANDLE);
using X3DCalc_t = void(WINAPI*)(const X3DAUDIO_HANDLE, const X3DAUDIO_LISTENER*, const X3DAUDIO_EMITTER*, UINT32,
                                X3DAUDIO_DSP_SETTINGS*);

struct Clip {
    WAVEFORMATEX fmt{};
    std::vector<uint8_t> data;
};

struct Voice {
    IXAudio2SourceVoice* voice = nullptr;
    std::shared_ptr<Clip> clip;
    void* block = nullptr;  // the entity's handle block (one reference held), null = a flat sound
    std::string bone;       // a bone of the entity, or empty
    int boneIndex = -2;     // -2 = not looked up yet, -1 = not found (the entity's position is used)
    float volume = 1.0f;
    std::string owner;      // the Lua mod that started it ("" = a plugin, WitcherScript, the console)
};

std::mutex g_mx;
std::map<std::wstring, std::shared_ptr<Clip>> g_clips;
std::map<int32_t, Voice> g_voices;
int32_t g_nextId = 1;
bool g_tried = false;
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
X3DAUDIO_HANDLE g_x3d = {};
X3DCalc_t g_calc = nullptr;
UINT32 g_outChannels = 2;
void* g_cameraWorld = nullptr;  // the world g_director belongs to
void* g_director = nullptr;     // its camera director

bool Start() {  // under g_mx
    if (g_tried) return g_xa != nullptr;
    g_tried = true;
    HMODULE m = LoadLibraryW(L"XAudio2_9.dll");
    if (!m) {
        Log("sound: XAudio2_9.dll not found, sounds stay off");
        return false;
    }
    auto createV = reinterpret_cast<XAudio2CreateV_t>(GetProcAddress(m, "XAudio2CreateWithVersionInfo"));
    auto create = reinterpret_cast<XAudio2Create_t>(GetProcAddress(m, "XAudio2Create"));
    HRESULT hr = createV ? createV(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR, NTDDI_VERSION)
                         : create ? create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_FAIL;
    if (FAILED(hr) || !g_xa) {
        Log("sound: XAudio2 did not start (0x%08lx)", hr);
        g_xa = nullptr;
        return false;
    }
    hr = g_xa->CreateMasteringVoice(&g_master);
    if (FAILED(hr)) {
        Log("sound: no output device (0x%08lx)", hr);
        g_xa->Release();
        g_xa = nullptr;
        return false;
    }
    DWORD mask = SPEAKER_STEREO;
    g_master->GetChannelMask(&mask);
    XAUDIO2_VOICE_DETAILS d{};
    g_master->GetVoiceDetails(&d);
    g_outChannels = d.InputChannels;
    auto init = reinterpret_cast<X3DInit_t>(GetProcAddress(m, "X3DAudioInitialize"));
    g_calc = reinterpret_cast<X3DCalc_t>(GetProcAddress(m, "X3DAudioCalculate"));
    if (!init || !g_calc || FAILED(init(mask, X3DAUDIO_SPEED_OF_SOUND, g_x3d))) g_calc = nullptr;
    Log("sound: XAudio2 ready (%u output channel(s), 3D %s)", g_outChannels, g_calc ? "on" : "off");
    return true;
}

std::wstring FullPath(const char* file) {  // absolute, or from the game folder (where bin, content and mods are)
    wchar_t w[MAX_PATH * 2] = {};
    MultiByteToWideChar(CP_UTF8, 0, file, -1, w, MAX_PATH * 2);
    std::wstring p = w;
    if (p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'))) return p;
    return Dir() + L"\\..\\..\\..\\" + p;
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // a file on disk only ("CON", "COM1" or a pipe open as devices and can hold the main thread in the read), and at
    // most 256 MiB (it is read on the main thread)
    LARGE_INTEGER sz{};
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (256LL << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    CloseHandle(h);
    return ok;
}

// A RIFF WAVE file: PCM 8/16/24/32 bit or 32-bit float, any channel count.
bool ParseWav(const std::vector<uint8_t>& f, Clip& c) {
    if (f.size() < 12 || memcmp(f.data(), "RIFF", 4) != 0 || memcmp(f.data() + 8, "WAVE", 4) != 0) return false;
    bool fmt = false;
    for (size_t p = 12; p + 8 <= f.size();) {
        uint32_t len = 0;
        memcpy(&len, f.data() + p + 4, 4);
        const uint8_t* body = f.data() + p + 8;
        if (p + 8 + len > f.size()) len = static_cast<uint32_t>(f.size() - p - 8);
        if (memcmp(f.data() + p, "fmt ", 4) == 0 && len >= 16) {
            memcpy(&c.fmt, body, 16);
            if (c.fmt.wFormatTag == 0xFFFE && len >= 40) {  // WAVE_FORMAT_EXTENSIBLE: the sub format decides
                uint16_t sub = 0;
                memcpy(&sub, body + 24, 2);
                c.fmt.wFormatTag = sub;
            }
            c.fmt.cbSize = 0;
            fmt = c.fmt.wFormatTag == WAVE_FORMAT_PCM || c.fmt.wFormatTag == 3 /* IEEE float */;
        } else if (memcmp(f.data() + p, "data", 4) == 0) {
            c.data.assign(body, body + len);
        }
        p += 8 + len + (len & 1);
    }
    return fmt && !c.data.empty();
}

bool ParseOgg(const std::vector<uint8_t>& f, Clip& c) {
    int channels = 0, rate = 0;
    short* pcm = nullptr;
    int frames = stb_vorbis_decode_memory(f.data(), static_cast<int>(f.size()), &channels, &rate, &pcm);
    if (frames <= 0 || !pcm || channels <= 0) {
        free(pcm);
        return false;
    }
    size_t bytes = static_cast<size_t>(frames) * channels * 2;
    c.data.assign(reinterpret_cast<uint8_t*>(pcm), reinterpret_cast<uint8_t*>(pcm) + bytes);
    free(pcm);
    c.fmt.wFormatTag = WAVE_FORMAT_PCM;
    c.fmt.nChannels = static_cast<WORD>(channels);
    c.fmt.nSamplesPerSec = static_cast<DWORD>(rate);
    c.fmt.wBitsPerSample = 16;
    c.fmt.nBlockAlign = static_cast<WORD>(channels * 2);
    c.fmt.nAvgBytesPerSec = c.fmt.nSamplesPerSec * c.fmt.nBlockAlign;
    return true;
}

std::shared_ptr<Clip> LoadClip(const char* file, std::string& err) {  // under g_mx
    std::wstring path = FullPath(file);
    auto it = g_clips.find(path);
    if (it != g_clips.end()) return it->second;
    std::vector<uint8_t> f;
    if (!ReadAll(path, f)) {
        err = std::string("file not found: ") + file;
        return nullptr;
    }
    auto c = std::make_shared<Clip>();
    if (!ParseWav(f, *c) && !ParseOgg(f, *c)) {
        err = std::string("not a WAV or OGG file this can play: ") + file;
        return nullptr;
    }
    g_clips[path] = c;
    return c;
}

void* BlockOfEntity(void* e) {  // the handle block of a scriptable (object+0x08, pointing back at it)
    void* b = nullptr;
    void* back = nullptr;
    if (!e || !SafeRead(static_cast<char*>(e) + 0x08, &b, 8) || !b) return nullptr;
    if (!SafeRead(static_cast<char*>(b) + 0x08, &back, 8) || back != e) return nullptr;
    return b;
}

void* BlockObject(void* block) {
    void* o = nullptr;
    return block && SafeRead(static_cast<char*>(block) + 0x08, &o, 8) ? o : nullptr;
}

// Gives the held reference back, the way Lua does for objects it did not make: never the engine's last steps.
void ReleaseBlock(void* block) {
    if (!block) return;
    auto refs = static_cast<volatile LONG*>(block);
    for (;;) {
        LONG r = 0;
        uint32_t fl = 0;
        if (!SafeRead(block, &r, 4) || !SafeRead(static_cast<char*>(block) + 4, &fl, 4)) return;
        if (!(r > 2 || (r == 2 && !(fl & 2)))) return;
        if (InterlockedCompareExchange(refs, r - 1, r) == r) return;
    }
}

void StopVoice(Voice& v) {  // under g_mx
    if (v.voice) {
        v.voice->Stop(0);
        v.voice->DestroyVoice();
        v.voice = nullptr;
    }
    ReleaseBlock(v.block);
    v.block = nullptr;
}

// --- 3D, each tick ---
bool Vec(void* obj, const char* method, float out[4]) {
    alignas(16) uint8_t res[32] = {};
    if (!obj || !CallMethodV(obj, method, "", res)) return false;
    memcpy(out, res, 16);
    return true;
}

void* ObjField(void* obj, const char* field) {  // a handle field's object
    void* cls = obj ? GetObjectClass(obj) : nullptr;
    void* prop = cls ? FindProperty(cls, field) : nullptr;
    void* p = prop ? PropertyPtr(obj, prop) : nullptr;
    void* block = nullptr;
    if (!p || !SafeRead(p, &block, 8)) return nullptr;
    return BlockObject(block);
}

bool Listener(X3DAUDIO_LISTENER& l) {
    void* world = ObjField(GetTheGame(), "activeWorld");
    if (!world) return false;
    if (world != g_cameraWorld) {  // once per world: its camera director (the call hands out one reference)
        alignas(16) uint8_t res[32] = {};
        g_director = CallMethodV(world, "GetCameraDirector", "", res) ? ResolveHandle(*reinterpret_cast<void**>(res)) : nullptr;
        g_cameraWorld = world;
    }
    float pos[4], fwd[4], up[4];
    if (!g_director || !Vec(g_director, "GetCameraPosition", pos) || !Vec(g_director, "GetCameraForward", fwd) ||
        !Vec(g_director, "GetCameraUp", up))
        return false;
    l = X3DAUDIO_LISTENER{};
    l.Position = {pos[0], pos[2], pos[1]};
    l.OrientFront = {fwd[0], fwd[2], fwd[1]};
    l.OrientTop = {up[0], up[2], up[1]};
    return true;
}

bool EmitterPos(Voice& v, void* ent, float out[3]) {
    if (!v.bone.empty() && v.boneIndex == -2) {
        int32_t idx = -1;
        std::string spec = "n:" + v.bone;
        alignas(16) uint8_t res[16] = {};
        if (CallMethodV(ent, "GetBoneIndex", spec.c_str(), res)) memcpy(&idx, res, 4);
        v.boneIndex = idx;
    }
    if (v.boneIndex >= 0) {
        alignas(16) float m[16] = {};
        std::string spec = "i:" + std::to_string(v.boneIndex);
        if (CallMethodV(ent, "GetBoneWorldMatrixByIndex", spec.c_str(), m)) {
            out[0] = m[12], out[1] = m[13], out[2] = m[14];
            return true;
        }
    }
    float m[16];
    if (!SafeRead(static_cast<char*>(ent) + 0x70, m, sizeof m) || m[15] != 1.0f) return false;
    out[0] = m[12], out[1] = m[13], out[2] = m[14];
    return true;
}

void Place(Voice& v, const X3DAUDIO_LISTENER& l, void* ent) {
    float p[3];
    if (!g_calc || !EmitterPos(v, ent, p)) return;
    UINT32 src = v.clip->fmt.nChannels;
    float az[8] = {};
    X3DAUDIO_EMITTER e{};
    e.Position = {p[0], p[2], p[1]};
    e.OrientFront = {0, 0, 1};
    e.OrientTop = {0, 1, 0};
    e.ChannelCount = src > 8 ? 8 : src;
    e.ChannelRadius = 0.1f;
    e.pChannelAzimuths = az;
    e.CurveDistanceScaler = 2.0f;  // full volume within 2 m, then falling off with the distance
    float matrix[8 * 8] = {};
    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.SrcChannelCount = e.ChannelCount;
    dsp.DstChannelCount = g_outChannels > 8 ? 8 : g_outChannels;
    dsp.pMatrixCoefficients = matrix;
    g_calc(g_x3d, &l, &e, X3DAUDIO_CALCULATE_MATRIX, &dsp);
    v.voice->SetOutputMatrix(g_master, dsp.SrcChannelCount, dsp.DstChannelCount, matrix);
}

void OnTick(float, void*) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_voices.empty()) return;
    X3DAUDIO_LISTENER l;
    bool haveListener = false, triedListener = false;
    for (auto it = g_voices.begin(); it != g_voices.end();) {
        Voice& v = it->second;
        XAUDIO2_VOICE_STATE st{};
        v.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        void* ent = v.block ? BlockObject(v.block) : nullptr;
        if (st.BuffersQueued == 0 || (v.block && !ent)) {  // played to the end, or the entity is gone
            StopVoice(v);
            it = g_voices.erase(it);
            continue;
        }
        if (ent) {
            if (!triedListener) {
                haveListener = Listener(l);
                triedListener = true;
            }
            if (haveListener) Place(v, l, ent);
        }
        ++it;
    }
}

}  // namespace

// --- the API (plugins, natives, Lua) ---
int32_t SoundPlay(const char* file, void* entity, const char* bone, float volume, int loop, const char* owner,
                  std::string* errOut) {
    std::string err;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!file || !*file) err = "no file";
    else if (!Start()) err = "sound is off (XAudio2 did not start)";
    std::shared_ptr<Clip> clip = err.empty() ? LoadClip(file, err) : nullptr;
    Voice v;
    if (clip) {
        HRESULT hr = g_xa->CreateSourceVoice(&v.voice, &clip->fmt);
        if (FAILED(hr)) err = "XAudio2 refused the format of " + std::string(file);
    }
    if (!err.empty()) {
        Log("sound: %s", err.c_str());
        if (errOut) *errOut = err;
        return 0;
    }
    XAUDIO2_BUFFER b{};
    b.AudioBytes = static_cast<UINT32>(clip->data.size());
    b.pAudioData = clip->data.data();
    b.Flags = XAUDIO2_END_OF_STREAM;
    b.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    v.voice->SubmitSourceBuffer(&b);
    v.clip = clip;
    v.volume = volume > 0 ? volume : 1.0f;
    v.voice->SetVolume(v.volume);
    if (entity) {
        v.block = BlockOfEntity(entity);
        if (v.block) InterlockedIncrement(static_cast<volatile LONG*>(v.block));
        if (bone) v.bone = bone;
    }
    v.owner = owner ? owner : "";
    if (v.block) {  // a first position before it starts, so it does not begin in the middle of the head
        X3DAUDIO_LISTENER l;
        if (OnMainThread() && Listener(l)) Place(v, l, entity);
    }
    v.voice->Start(0);
    int32_t id = g_nextId++;
    g_voices[id] = std::move(v);
    return id;
}

bool SoundStop(int32_t id) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_voices.find(id);
    if (it == g_voices.end()) return false;
    StopVoice(it->second);
    g_voices.erase(it);
    return true;
}

bool SoundVolume(int32_t id, float volume) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_voices.find(id);
    if (it == g_voices.end()) return false;
    it->second.volume = volume < 0 ? 0 : volume;
    it->second.voice->SetVolume(it->second.volume);
    return true;
}

bool SoundPlaying(int32_t id) {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_voices.count(id) != 0;
}

bool SoundPreload(const char* file, std::string* errOut) {
    std::string err;
    std::lock_guard<std::mutex> lk(g_mx);
    bool ok = file && Start() && LoadClip(file, err) != nullptr;
    if (!ok && errOut) *errOut = err.empty() ? "sound is off" : err;
    return ok;
}

int SoundStopOwner(const std::string& owner) {  // a Lua mod taken out: its sounds end with it
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0;
    for (auto it = g_voices.begin(); it != g_voices.end();) {
        if (it->second.owner == owner) {
            StopVoice(it->second);
            it = g_voices.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

namespace {
// import function TW3SE_PlaySound(file : string, optional entity : CEntity, optional bone : name,
//                                 optional volume : float, optional loop : bool) : int;
void Tw3PlaySound(void*, void* frame, void* result) {
    alignas(16) uint8_t fileBuf[16] = {}, entBuf[16] = {}, boneBuf[16] = {}, volBuf[16] = {}, loopBuf[16] = {};
    ReadScriptParam(frame, fileBuf);
    ReadScriptParam(frame, entBuf);
    ReadScriptParam(frame, boneBuf);
    ReadScriptParam(frame, volBuf);
    ReadScriptParam(frame, loopBuf);
    EndScriptParams(frame);
    const char* file = *reinterpret_cast<char**>(fileBuf);
    void* handle = *reinterpret_cast<void**>(entBuf);
    void* ent = handle ? ResolveHandle(handle) : nullptr;
    uint32_t boneId = 0;
    memcpy(&boneId, boneBuf, 4);
    const char* bone = boneId ? GetCNameString(boneId) : nullptr;
    float vol = 0;
    memcpy(&vol, volBuf, 4);
    int32_t id = SoundPlay(file, ent, bone, vol, loopBuf[0] != 0, "", nullptr);
    if (result) *static_cast<int32_t*>(result) = id;
}
void Tw3StopSound(void*, void* frame, void* result) {  // import function TW3SE_StopSound(sound : int) : bool;
    int32_t id = 0;
    ReadScriptParam(frame, &id);
    EndScriptParams(frame);
    bool ok = SoundStop(id);
    if (result) *static_cast<bool*>(result) = ok;
}
// import function TW3SE_SetSoundVolume(sound : int, volume : float) : bool;
void Tw3SetSoundVolume(void*, void* frame, void* result) {
    int32_t id = 0;
    float v = 0;
    ReadScriptParam(frame, &id);
    ReadScriptParam(frame, &v);
    EndScriptParams(frame);
    bool ok = SoundVolume(id, v);
    if (result) *static_cast<bool*>(result) = ok;
}
void Tw3SoundPlaying(void*, void* frame, void* result) {  // import function TW3SE_SoundPlaying(sound : int) : bool;
    int32_t id = 0;
    ReadScriptParam(frame, &id);
    EndScriptParams(frame);
    bool ok = SoundPlaying(id);
    if (result) *static_cast<bool*>(result) = ok;
}

void CmdSound(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    if (a.rfind("play ", 0) == 0) {
        std::string err;
        int32_t id = SoundPlay(a.substr(5).c_str(), nullptr, nullptr, 1.0f, 0, "", &err);
        if (id) snprintf(reply, size, "sound %d", id);
        else snprintf(reply, size, "error %s", err.c_str());
        return;
    }
    if (a.rfind("stop ", 0) == 0) {
        snprintf(reply, size, "%s", SoundStop(atoi(a.c_str() + 5)) ? "stopped" : "no such sound");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mx);
    snprintf(reply, size, "sound %s, %zu playing, %zu file(s) loaded, %u output channel(s)",
             g_xa ? "on" : (g_tried ? "off" : "not started yet"), g_voices.size(), g_clips.size(), g_outChannels);
}
}  // namespace

void InstallSound() {
    RegisterTick(&OnTick, nullptr);
    RegisterCommand("sound", &CmdSound, "sound [play <file> | stop <id>]: own sounds (XAudio2)");
    QueueNative("TW3SE_PlaySound", &Tw3PlaySound, 5);
    QueueNative("TW3SE_StopSound", &Tw3StopSound, 1);
    QueueNative("TW3SE_SetSoundVolume", &Tw3SetSoundVolume, 2);
    QueueNative("TW3SE_SoundPlaying", &Tw3SoundPlaying, 1);
}

}  // namespace tw3se
