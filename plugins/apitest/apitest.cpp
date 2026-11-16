// apitest - a test plugin for TW3SE's plugin interface: the console command "apifuzz" calls every function of the
// interface with what a careless plugin passes (NULL, "", negative counts, NaN, pointers to nothing) and logs each
// call before it makes it, so after a crash the last "apifuzz #n" line in tw3se.log names the call. The game has to
// go on; every refused call returns 0 or does nothing. Not part of a release (build\tw3se\plugins\apitest.dll).
//
// Left out: RegisterOnPose(NULL) and RegisterOnMimic(NULL) disarm the one pose / mimic callback, also another
// plugin's; MountDLC and WorldEdit with a real text would mount or edit.
#include <d3d12.h>  // (before tw3se.h: windows.h renames PlaySound by a macro, in both places then)
#include "tw3se/tw3se.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const TW3SE_Interface* g_api = nullptr;
static int g_n = 0;
static TW3SE_Pose g_pose;

#define STEP(desc, call)                                         \
    do {                                                         \
        g_api->Log("ApiTest", "apifuzz #%d %s", ++g_n, desc);    \
        call;                                                    \
    } while (0)

static void Detour() {}

static void Fuzz(const char*, char* reply, uint32_t size) {
    g_n = 0;
    const float nan = NAN, inf = INFINITY;
    void* junk = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10));
    alignas(16) unsigned char zeros[256] = {};  // an "object" whose vtable is NULL
    void* fake = zeros;
    alignas(16) unsigned char res[512] = {};
    char buf[64] = {};
    uint32_t guid[4] = {};
    void* player = g_api->GetThePlayer();
    void* cls = g_api->FindClassByName("CEntity");
    const char* argsWithNull[2] = {"1", nullptr};
    void* orig = nullptr;

    STEP("Log(NULL, NULL)", g_api->Log(nullptr, nullptr));
    STEP("GetAddress(NULL)", g_api->GetAddress(nullptr));
    STEP("GetAddress(\"\")", g_api->GetAddress(""));
    STEP("Hook(NULL, NULL, NULL)", g_api->Hook(nullptr, nullptr, nullptr));
    STEP("Hook(0x10, Detour, &orig)", g_api->Hook(junk, reinterpret_cast<void*>(&Detour), &orig));
    STEP("RunOnMainThread(NULL)", g_api->RunOnMainThread(nullptr, nullptr));
    STEP("RegisterCommand(NULL, NULL, NULL)", g_api->RegisterCommand(nullptr, nullptr, nullptr));
    STEP("RegisterCommand(\"\", Fuzz, NULL)", g_api->RegisterCommand("", &Fuzz, nullptr));
    STEP("RegisterCommand(\"apitest_x\", NULL, NULL)", g_api->RegisterCommand("apitest_x", nullptr, nullptr));
    STEP("MountDLC(NULL, NULL, 0)", g_api->MountDLC(nullptr, nullptr, 0));
    STEP("MountDLC(\"\", NULL, 99)", g_api->MountDLC("", nullptr, 99));
    STEP("RegisterNative(NULL, NULL, -1)", g_api->RegisterNative(nullptr, nullptr, -1));
    STEP("ReadParam(NULL, NULL)", g_api->ReadParam(nullptr, nullptr));
    STEP("EndParams(NULL)", g_api->EndParams(nullptr));
    STEP("RegisterOnTick(NULL)", g_api->RegisterOnTick(nullptr, nullptr));
    STEP("FindProperty(NULL, \"x\")", g_api->FindProperty(nullptr, "x"));
    STEP("FindProperty(CEntity, NULL)", g_api->FindProperty(cls, nullptr));
    STEP("FindProperty(0x10, \"x\")", g_api->FindProperty(junk, "x"));
    STEP("PropertyPtr(NULL, NULL)", g_api->PropertyPtr(nullptr, nullptr));
    STEP("PropertyPtr(player, 0x10)", g_api->PropertyPtr(player, junk));
    STEP("GetObjectClass(NULL)", g_api->GetObjectClass(nullptr));
    STEP("GetObjectClass(0x10)", g_api->GetObjectClass(junk));
    STEP("GetObjectClass(fake)", g_api->GetObjectClass(fake));
    STEP("FindClassByName(NULL)", g_api->FindClassByName(nullptr));
    STEP("FindClassByName(\"NoSuchClass\")", g_api->FindClassByName("NoSuchClass"));
    STEP("CallFunction(NULL, NULL, 0)", g_api->CallFunction(nullptr, nullptr, 0, nullptr, res));
    STEP("CallFunction(\"x\", NULL, 3)", g_api->CallFunction("x", nullptr, 3, nullptr, res));
    STEP("CallFunction(\"x\", {\"1\", NULL}, 2)", g_api->CallFunction("x", argsWithNull, 2, nullptr, res));
    STEP("CallFunction(\"x\", NULL, -1)", g_api->CallFunction("x", nullptr, -1, nullptr, res));
    STEP("CallFunction(\"GetLocalTime\", NULL, 0, NULL, NULL)", g_api->CallFunction("GetLocalTime", nullptr, 0, nullptr, nullptr));
    STEP("CallMethod(NULL, \"GetName\")", g_api->CallMethod(nullptr, "GetName", nullptr, 0, res));
    STEP("CallMethod(player, NULL)", g_api->CallMethod(player, nullptr, nullptr, 0, res));
    STEP("CallMethod(0x10, \"GetName\")", g_api->CallMethod(junk, "GetName", nullptr, 0, res));
    STEP("CallMethod(fake, \"GetName\")", g_api->CallMethod(fake, "GetName", nullptr, 0, res));
    STEP("ResolveHandle(NULL)", g_api->ResolveHandle(nullptr));
    STEP("ResolveHandle(0x10)", g_api->ResolveHandle(junk));
    STEP("ResolveHandle(fake)", g_api->ResolveHandle(fake));
    STEP("GetCNameString(0)", g_api->GetCNameString(0));
    STEP("GetCNameString(0xffffffff)", g_api->GetCNameString(0xffffffffu));
    STEP("GetCNameString(123456789)", g_api->GetCNameString(123456789u));
    STEP("RegisterOnPresent(NULL)", g_api->RegisterOnPresent(nullptr, nullptr));
    STEP("CallFunctionV(NULL, NULL)", g_api->CallFunctionV(nullptr, nullptr, nullptr, res));
    STEP("CallFunctionV(\"x\", NULL)", g_api->CallFunctionV("x", nullptr, nullptr, res));
    STEP("CallFunctionV(\"x\", \"%%s%%n\")", g_api->CallFunctionV("x", "%s%n", nullptr, res));
    STEP("CallMethodV(NULL, \"x\")", g_api->CallMethodV(nullptr, "x", "", res));
    STEP("CallMethodV(0x10, \"GetName\")", g_api->CallMethodV(junk, "GetName", "", res));
    STEP("CallMethodV(fake, \"GetName\")", g_api->CallMethodV(fake, "GetName", "", res));
    STEP("CallMethodV(player, \"GetName\", \"q:\")", g_api->CallMethodV(player, "GetName", "q:", res));
    STEP("RegisterOnOverlayDraw(NULL)", g_api->RegisterOnOverlayDraw(nullptr, nullptr));
    STEP("OverlayDrawText(nan, inf, 0, NULL)", g_api->OverlayDrawText(nan, inf, 0, nullptr));
    STEP("OverlayDrawText outside a draw", g_api->OverlayDrawText(1, 1, 0xffffffff, "x"));
    STEP("OverlayDrawRect(nan ...)", g_api->OverlayDrawRect(nan, nan, -inf, inf, 0));
    STEP("OverlayGetScreenSize(NULL, NULL)", g_api->OverlayGetScreenSize(nullptr, nullptr));
    STEP("RegisterOnGameLoaded(NULL)", g_api->RegisterOnGameLoaded(nullptr, nullptr));
    STEP("EntityByGuid(NULL)", g_api->EntityByGuid(nullptr));
    STEP("EntityByGuid(zeros)", g_api->EntityByGuid(guid));
    STEP("EntityGuid(NULL, guid)", g_api->EntityGuid(nullptr, guid));
    STEP("EntityGuid(player, NULL)", g_api->EntityGuid(player, nullptr));
    STEP("EntityGuid(0x10, guid)", g_api->EntityGuid(junk, guid));
    STEP("WorldEdit(NULL)", g_api->WorldEdit(nullptr));
    STEP("WorldEdit(\"\")", g_api->WorldEdit(""));
    STEP("WatchPose(NULL, 1)", g_api->WatchPose(nullptr, 1));
    STEP("WatchPose(fake, 1)", g_api->WatchPose(fake, 1));
    STEP("WatchPose(fake, 0)", g_api->WatchPose(fake, 0));
    STEP("GetPose(NULL, 0, &pose)", g_api->GetPose(nullptr, 0, &g_pose));
    STEP("GetPose(player, 99, &pose)", g_api->GetPose(player, 99, &g_pose));
    STEP("GetPose(player, 0, NULL)", g_api->GetPose(player, 0, nullptr));
    STEP("WatchMimic(NULL, 1)", g_api->WatchMimic(nullptr, 1));
    STEP("IsEditor()", g_api->IsEditor());
    STEP("RunLua(NULL, NULL, 0)", g_api->RunLua(nullptr, nullptr, 0));
    STEP("RunLua(\"return 1\", NULL, 50)", g_api->RunLua("return 1", nullptr, 50));
    STEP("RunLua(long result, buf, 4)", g_api->RunLua("return string.rep('a', 10000)", buf, 4));
    STEP("RunLua(error, buf, 0)", g_api->RunLua("error('x')", buf, 0));
    STEP("RegisterLuaFunction(NULL, NULL)", g_api->RegisterLuaFunction(nullptr, nullptr, nullptr));
    STEP("RegisterLuaFunction(\"x\", NULL)", g_api->RegisterLuaFunction("x", nullptr, nullptr));
    STEP("PlaySound(NULL ...)", g_api->PlaySound(nullptr, nullptr, nullptr, 1, 0));
    STEP("PlaySound(\"CON\", NULL, NULL, nan, 1)", g_api->PlaySound("CON", nullptr, nullptr, nan, 1));
    STEP("PlaySound(\"x\", fake, \"x\", inf, 1)", g_api->PlaySound("x", fake, "x", inf, 1));
    STEP("StopSound(-1)", g_api->StopSound(-1));
    STEP("SetSoundVolume(0x7fffffff, nan)", g_api->SetSoundVolume(0x7fffffff, nan));
    STEP("SoundPlaying(0)", g_api->SoundPlaying(0));
    STEP("PreloadSound(NULL)", g_api->PreloadSound(nullptr));
    STEP("PreloadSound(\"NUL\")", g_api->PreloadSound("NUL"));
    if (g_api->interfaceVersion >= 18) {
        TW3SE_Value v[4] = {};
        TW3SE_Value bad[3] = {};
        bad[0].type = TW3SE_STRUCT;  // no type name, no bytes
        bad[1].type = TW3SE_ARRAY;
        bad[1].count = 5;            // items NULL
        bad[2].type = 99;
        STEP("Call(NULL, NULL, ...)", g_api->Call(nullptr, nullptr, nullptr, 0, v, 4));
        STEP("Call(NULL, \"StrLen\", NULL, 3)", g_api->Call(nullptr, "StrLen", nullptr, 3, v, 4));
        STEP("Call(NULL, \"StrLen\", bad struct)", g_api->Call(nullptr, "StrLen", &bad[0], 1, v, 4));
        STEP("Call(NULL, \"StrLen\", bad array)", g_api->Call(nullptr, "StrLen", &bad[1], 1, v, 4));
        STEP("Call(NULL, \"StrLen\", type 99)", g_api->Call(nullptr, "StrLen", &bad[2], 1, v, 4));
        STEP("Call(0x10, \"GetName\")", g_api->Call(junk, "GetName", nullptr, 0, v, 4));
        STEP("Call(fake, \"GetName\")", g_api->Call(fake, "GetName", nullptr, 0, v, 4));
        STEP("Call(player, \"NoSuchMethod\")", g_api->Call(player, "NoSuchMethod", nullptr, 0, v, 4));
        STEP("Call(NULL, \"StrLen\", results NULL)", g_api->Call(nullptr, "StrLen", nullptr, 0, nullptr, 4));
        STEP("GetField(NULL ...)", g_api->GetField(nullptr, nullptr, nullptr));
        STEP("GetField(fake, \"x\", v)", g_api->GetField(fake, "x", v));
        STEP("GetField(player, \"noSuchField\", v)", g_api->GetField(player, "noSuchField", v));
        STEP("SetField(player, \"equippedSign\", type 99)", g_api->SetField(player, "equippedSign", &bad[2]));
        STEP("SetField(0x10, \"x\", v)", g_api->SetField(junk, "x", v));
        STEP("LastError()", g_api->LastError());
        STEP("HookScript(NULL, NULL, NULL)", g_api->HookScript(nullptr, nullptr, nullptr, nullptr, 0));
        STEP("HookScript(\"NoClass\", \"x\", ...)", g_api->HookScript("NoClass", "x", reinterpret_cast<TW3SE_ScriptHook>(&Detour), nullptr, 0));
        STEP("Unhook(-5)", g_api->Unhook(-5));
        STEP("CallOriginal(NULL, NULL)", g_api->CallOriginal(nullptr, nullptr));
        STEP("ClassName(NULL)", g_api->ClassName(nullptr));
        STEP("ClassName(0x10)", g_api->ClassName(junk));
        STEP("ClassName(fake)", g_api->ClassName(fake));
        STEP("ClassParent(fake)", g_api->ClassParent(fake));
        STEP("ForEachField(fake, NULL)", g_api->ForEachField(fake, nullptr, nullptr));
        STEP("ForEachField(0x10, cb)", g_api->ForEachField(junk, reinterpret_cast<TW3SE_ItemCallback>(&Detour), nullptr));
        STEP("ForEachMethod(fake, cb)", g_api->ForEachMethod(fake, reinterpret_cast<TW3SE_ItemCallback>(&Detour), nullptr));
        STEP("EnumValue(NULL, NULL, NULL)", g_api->EnumValue(nullptr, nullptr, nullptr));
        STEP("ForEachObject(NULL, NULL)", g_api->ForEachObject(nullptr, nullptr, nullptr));
        STEP("ForEachObject(\"CEntity\", NULL)", g_api->ForEachObject("CEntity", nullptr, nullptr));
        STEP("ForEachObject(\"EDeathType\", cb)", g_api->ForEachObject("EDeathType", reinterpret_cast<TW3SE_ObjectCallback>(&Detour), nullptr));
        STEP("EnumValue(\"CEntity\", \"x\", &i)", [&] { int64_t i = 0; g_api->EnumValue("CEntity", "x", &i); }());
    }
    if (g_api->interfaceVersion >= 19) {
        STEP("DecalDefaults(NULL)", g_api->DecalDefaults(nullptr));
        STEP("SpawnDecals(NULL, 1, ...)", g_api->SpawnDecals(nullptr, 1, nullptr, 0, nullptr));
        STEP("SpawnDecals(d, 1, NULL, 2, NULL)", [&] {
            TW3SE_Decal d;
            g_api->DecalDefaults(&d);
            g_api->SpawnDecals(&d, 1, nullptr, 2, nullptr);
        }());
        STEP("SpawnDecals(no texture)", [&] {
            TW3SE_Decal d;
            g_api->DecalDefaults(&d);
            uint32_t id = 1;
            g_api->SpawnDecals(&d, 1, nullptr, 0, &id);
        }());
        STEP("SpawnDecals(entity 0x10)", [&] {
            TW3SE_Decal d;
            g_api->DecalDefaults(&d);
            d.diffuse = "fx\\textures\\blood\\blood_splash_spray_small_3.xbm";
            void* e = junk;
            g_api->SpawnDecals(&d, 1, &e, 1, nullptr);
        }());
        STEP("RemoveDecal(0)", g_api->RemoveDecal(0));
        STEP("SetDecalColor(0, ...)", g_api->SetDecalColor(0, 0, 0, 0, 0, 0));
        STEP("ClearDecals(0x10)", g_api->ClearDecals(junk));
        STEP("DecalCount()", g_api->DecalCount());
        STEP("SetDecalBudget(0, 0, 0)", g_api->SetDecalBudget(0, 0, 0));
    }
    if (g_api->interfaceVersion >= 21) {  // the result slot of ReturnString is the engine's: no junk there
        static std::string big(100000, 'x');
        STEP("ReturnString(NULL, NULL, 0)", g_api->ReturnString(nullptr, nullptr, 0));
        STEP("ReturnString(NULL, \"abc\", 999)", g_api->ReturnString(nullptr, "abc", 999));
        STEP("NameFromString(NULL)", g_api->NameFromString(nullptr));
        STEP("NameFromString(\"\")", g_api->NameFromString(""));
        STEP("NameFromString(100000 chars)", g_api->NameFromString(big.c_str()));
        STEP("RegisterOnImGui(NULL, ...)", g_api->RegisterOnImGui(nullptr, nullptr, 0, 0, 0));
        STEP("RegisterOnImGui(cb, version 1)", g_api->RegisterOnImGui(reinterpret_cast<TW3SE_ImGuiCallback>(&Detour), nullptr, 1, 1, 1));
        STEP("ImGuiAllocators(NULL, NULL, NULL)", g_api->ImGuiAllocators(nullptr, nullptr, nullptr));
        STEP("ImGuiOverlay(5)", g_api->ImGuiOverlay(5));
        STEP("ImGuiOverlay(0)", g_api->ImGuiOverlay(0));
        STEP("ImGuiCaptureInput(-1)", g_api->ImGuiCaptureInput(-1));
        STEP("ImGuiCaptureInput(0)", g_api->ImGuiCaptureInput(0));
        STEP("SendToChannel(NULL, NULL)", g_api->SendToChannel(nullptr, nullptr));
        STEP("SendToChannel(\"\", NULL)", g_api->SendToChannel("", nullptr));
        STEP("SendToChannel(\"apifuzz\", 100000 chars)", g_api->SendToChannel("apifuzz", big.c_str()));
        STEP("RegisterOnMessage(NULL, NULL, NULL)", g_api->RegisterOnMessage(nullptr, nullptr, nullptr));
        STEP("RegisterOnMessage(\"x\", NULL, NULL)", g_api->RegisterOnMessage("x", nullptr, nullptr));
        STEP("GameState()", g_api->GameState());
        STEP("PadRead(NULL, NULL)", g_api->PadRead(nullptr, nullptr));
        STEP("RegisterOnPipeline(NULL, NULL)", g_api->RegisterOnPipeline(nullptr, nullptr));
        STEP("ShaderReplace(0, NULL, 0)", g_api->ShaderReplace(0, nullptr, 0));
        STEP("ShaderReplace(1, NULL, 100)", g_api->ShaderReplace(1, nullptr, 100));
        STEP("ShaderReplace(1, 0x10, 100)", g_api->ShaderReplace(1, junk, 100));
        STEP("ShaderReplace(1, zeros, 1 << 40)", g_api->ShaderReplace(1, zeros, size_t(1) << 40));
        STEP("RootSignatureAddParam(0, NULL)", g_api->RootSignatureAddParam(0, nullptr));
        STEP("RootSignatureAddParam(1, 0x10)", g_api->RootSignatureAddParam(1, junk));
        STEP("RootSignatureAddParam(1, type 99)", [&] {
            unsigned char p[64] = {};
            p[0] = 99;
            g_api->RootSignatureAddParam(1, p);
        }());
        STEP("RootSignatureAddParam(1, table of 1000000 ranges at 0x10)", [&] {
            D3D12_ROOT_PARAMETER1 t = {};
            t.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            t.DescriptorTable.NumDescriptorRanges = 1000000;
            t.DescriptorTable.pDescriptorRanges = static_cast<const D3D12_DESCRIPTOR_RANGE1*>(junk);
            g_api->RootSignatureAddParam(1, &t);
        }());
        STEP("RootSignatureAddParam(1, table of 3 ranges at NULL)", [&] {
            D3D12_ROOT_PARAMETER1 t = {};
            t.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            t.DescriptorTable.NumDescriptorRanges = 3;
            g_api->RootSignatureAddParam(1, &t);
        }());
        STEP("RootParamIndex(NULL, 0)", g_api->RootParamIndex(nullptr, 0));
        STEP("RootParamIndex(0x10, 5)", g_api->RootParamIndex(junk, 5));
        STEP("MarkPipeline(NULL, NULL, NULL, NULL)", g_api->MarkPipeline(nullptr, nullptr, nullptr, nullptr));
        STEP("MarkPipeline(0x10, NULL, ...) (unmark nothing)", g_api->MarkPipeline(junk, nullptr, nullptr, nullptr));
        STEP("RegisterOnSubmit(NULL, NULL)", g_api->RegisterOnSubmit(nullptr, nullptr));
    }
    if (g_api->interfaceVersion >= 22) {
        float w[2] = {nan, inf};
        STEP("DeformOwn(NULL)", g_api->DeformOwn(nullptr));
        STEP("DeformOwn(0x10)", g_api->DeformOwn(junk));
        STEP("DeformOwn(fake)", g_api->DeformOwn(fake));
        STEP("DeformOwn(player) (not a mesh component)", g_api->DeformOwn(player));
        STEP("DeformLoaded(0x10)", g_api->DeformLoaded(junk));
        STEP("DeformRelease(player)", g_api->DeformRelease(player));
        STEP("DeformLoad(NULL, NULL, nan)", g_api->DeformLoad(nullptr, nullptr, nan));
        STEP("DeformLoad(player, \"x]==]y\", 1)", g_api->DeformLoad(player, "x]==]y", 1));
        STEP("DeformLoad(player, \"no_such.twdf\", -inf)", g_api->DeformLoad(player, "no_such.twdf", -inf));
        STEP("DeformWeights(0, NULL, 5)", g_api->DeformWeights(0, nullptr, 5));
        STEP("DeformWeights(-1, {nan, inf}, -3)", g_api->DeformWeights(-1, w, -3));
        STEP("DeformState(-5)", g_api->DeformState(-5));
        STEP("DeformUnload(123456)", g_api->DeformUnload(123456));
        STEP("DeformShapeIndex(1, NULL)", g_api->DeformShapeIndex(1, nullptr));
    }
    if (g_api->interfaceVersion >= 23) {
        float caps[16] = {nan, inf, -inf, 1e30f, 0, 0, 0, 0, 1, 2, 3, -1, 4, 5, 6, nan};
        STEP("SkinHookActive()", g_api->SkinHookActive());
        STEP("SkinHookAdd(NULL)", g_api->SkinHookAdd(nullptr));
        STEP("SkinHookAdd(0x10)", g_api->SkinHookAdd(junk));
        STEP("SkinHookAdd(player) (not a mesh component)", g_api->SkinHookAdd(player));
        STEP("SkinHookCapsules(0, NULL, 5)", g_api->SkinHookCapsules(0, nullptr, 5));
        STEP("SkinHookCapsules(1, NULL, 3)", g_api->SkinHookCapsules(1, nullptr, 3));
        STEP("SkinHookCapsules(99, caps, 2)", g_api->SkinHookCapsules(99, caps, 2));
        STEP("SkinHookCapsules(1, caps, -4)", g_api->SkinHookCapsules(1, caps, -4));
        STEP("SkinHookState(-1)", g_api->SkinHookState(-1));
        STEP("SkinHookRemove(1000)", g_api->SkinHookRemove(1000));
        STEP("SkinHookWeights(0, 0, caps, 4)", g_api->SkinHookWeights(0, 0, caps, 4));
        STEP("SkinHookWeights(1, -1, caps, 4)", g_api->SkinHookWeights(1, -1, caps, 4));
        STEP("SkinHookWeights(1, 0, NULL, 9)", g_api->SkinHookWeights(1, 0, nullptr, 9));
        STEP("SkinHookWeights(8, 999, caps, -2)", g_api->SkinHookWeights(8, 999, caps, -2));
        STEP("SkinHookWeights(3, 0, caps, 16) (no mesh)", g_api->SkinHookWeights(3, 0, caps, 16));
    }
    if (g_api->interfaceVersion >= 25) STEP("RegisterOnRenderRebuild(NULL)", g_api->RegisterOnRenderRebuild(nullptr, nullptr));
    snprintf(reply, size, "apifuzz: %d calls made, the game goes on", g_n);
}

// --- apiv18: interface 18 does what it says, against the running game (each check a line, then a count)
static char g_out[8192];
static int g_pass, g_fail;
static void Check(const char* name, bool ok, const char* detail = "") {
    ok ? ++g_pass : ++g_fail;
    size_t n = strlen(g_out);
    snprintf(g_out + n, sizeof g_out - n, "%s %s %s | ", ok ? "PASS" : "FAIL", name, detail);
}
static TW3SE_Value Str(const char* s) { TW3SE_Value v = {}; v.type = TW3SE_STRING; v.v.s = s; return v; }
static TW3SE_Value Int(int64_t i) { TW3SE_Value v = {}; v.type = TW3SE_INT; v.v.i = i; return v; }

static int OnStrLenPlusOne(void*, TW3SE_Value*, int, TW3SE_HookCall* call, TW3SE_Value* result, void*) {
    TW3SE_Value r = {};
    if (!g_api->CallOriginal(call, &r) || r.type != TW3SE_INT) return 0;
    *result = Int(r.v.i + 1);
    return 1;
}
static int OnStrLenArg(void*, TW3SE_Value* args, int argc, TW3SE_HookCall*, TW3SE_Value*, void*) {
    if (argc >= 1) args[0] = Str("ab");  // the next handler and the original see "ab"
    return 0;
}
struct FieldHunt {
    int count;
    bool sign;
    char signType[64];  // copied: the info's strings live during the callback only
};
static int OnField(const TW3SE_ItemInfo* info, void* ctx) {
    auto h = static_cast<FieldHunt*>(ctx);
    ++h->count;
    if (!strcmp(info->name, "equippedSign")) {
        h->sign = true;
        snprintf(h->signType, sizeof h->signType, "%s", info->typeName);
    }
    return 0;
}
struct ObjHunt {
    void* want;
    bool found;
    int count;
};
static int OnObject(void* o, void* ctx) {
    auto h = static_cast<ObjHunt*>(ctx);
    ++h->count;
    if (o == h->want) h->found = true;
    return 0;
}
static int OnMethod(const TW3SE_ItemInfo* info, void* ctx) {
    if (!strcmp(info->name, "GetName")) *static_cast<bool*>(ctx) = true;
    return 0;
}

static void ApiV18(const char*, char* reply, uint32_t size) {
    g_out[0] = 0;
    g_pass = g_fail = 0;
    char d[256];
    TW3SE_Value r[4] = {};
    void* player = g_api->GetThePlayer();
    if (g_api->interfaceVersion < 18 || !player) {
        snprintf(reply, size, "apiv18: needs interface 18 and a loaded game");
        return;
    }
    TW3SE_Value a = Str("witcher");
    int n = g_api->Call(nullptr, "StrLen", &a, 1, r, 4);
    Check("Call a global with a string", n == 1 && r[0].type == TW3SE_INT && r[0].v.i == 7, g_api->LastError());

    TW3SE_Value sp[4] = {Str("a.b.c"), Str("."), Str(""), Str("")};
    n = g_api->Call(nullptr, "StrSplitFirst", sp, 4, r, 4);
    snprintf(d, sizeof d, "n=%d %s %s", n, n >= 2 && r[1].type == TW3SE_STRING ? r[1].v.s : "?", n >= 3 && r[2].type == TW3SE_STRING ? r[2].v.s : "?");
    Check("out arguments come back after the return", n == 3 && r[0].type == TW3SE_BOOL && r[0].v.i == 1 &&
          !strcmp(r[1].v.s, "a") && !strcmp(r[2].v.s, "b.c"), d);

    n = g_api->Call(player, "GetWorldPosition", nullptr, 0, r, 4);
    bool vec = n == 1 && r[0].type == TW3SE_STRUCT && r[0].count == 16 && r[0].typeName && !strcmp(r[0].typeName, "Vector");
    Check("a method returns a struct (Vector, 16 bytes)", vec, g_api->LastError());
    float pos[4] = {};
    if (vec) memcpy(pos, r[0].v.data, 16);

    TW3SE_Value two[2] = {};
    float away[4] = {pos[0] + 3, pos[1] + 4, pos[2], 1};
    two[0].type = two[1].type = TW3SE_STRUCT;
    two[0].typeName = two[1].typeName = "Vector";
    two[0].count = two[1].count = 16;
    two[0].v.data = pos;
    two[1].v.data = away;
    n = g_api->Call(nullptr, "VecDistance", two, 2, r, 4);
    snprintf(d, sizeof d, "%.3f %s", n == 1 && r[0].type == TW3SE_FLOAT ? r[0].v.f : -1.0, g_api->LastError());
    Check("structs as arguments (VecDistance = 5)", n == 1 && r[0].type == TW3SE_FLOAT && fabs(r[0].v.f - 5.0) < 0.01, d);

    TW3SE_Value self = {};
    self.type = TW3SE_OBJECT;
    self.v.object = player;
    n = g_api->Call(player, "GetAttitude", &self, 1, r, 4);
    Check("an object as argument, an enum back as its name", n == 1 && r[0].type == TW3SE_STRING && !strncmp(r[0].v.s, "AIA_", 4),
          n == 1 && r[0].type == TW3SE_STRING ? r[0].v.s : g_api->LastError());

    TW3SE_Value f = {};
    int ok = g_api->GetField(player, "equippedSign", &f);
    Check("GetField reads an enum field", ok && f.type == TW3SE_STRING && !strncmp(f.v.s, "ST_", 3), ok ? (f.type == TW3SE_STRING ? f.v.s : "?") : g_api->LastError());
    char sign[64] = {};
    if (ok && f.type == TW3SE_STRING) strncpy(sign, f.v.s, sizeof sign - 1);

    TW3SE_Value icon = Str("apitest_icon");
    TW3SE_Value old = {};
    char oldIcon[256] = {};
    g_api->GetField(player, "companionNPCIconPath", &old);
    if (old.type == TW3SE_STRING) strncpy(oldIcon, old.v.s, sizeof oldIcon - 1);
    ok = g_api->SetField(player, "companionNPCIconPath", &icon) && g_api->GetField(player, "companionNPCIconPath", &f);
    Check("SetField writes a string field", ok && f.type == TW3SE_STRING && !strcmp(f.v.s, "apitest_icon"), g_api->LastError());
    TW3SE_Value back = Str(oldIcon);
    g_api->SetField(player, "companionNPCIconPath", &back);

    ok = g_api->GetField(player, "alchemyRecipes", &f);
    Check("an array field comes as a list", ok && f.type == TW3SE_ARRAY && (f.count == 0 || f.v.items[0].type == TW3SE_STRING), g_api->LastError());
    ok = g_api->GetField(player, "levelManager", &f);
    Check("an object field comes as an object", ok && f.type == TW3SE_OBJECT && f.v.object, g_api->LastError());
    ok = g_api->GetField(player, "noSuchField", &f);
    Check("a missing field is an error", !ok && strstr(g_api->LastError(), "noSuchField"), g_api->LastError());
    ok = g_api->SetField(player, "equippedSign", &icon);
    Check("a wrong value is refused with the reason", !ok && *g_api->LastError(), g_api->LastError());

    int h1 = g_api->HookScript(nullptr, "StrLen", &OnStrLenPlusOne, nullptr, 0);
    n = g_api->Call(nullptr, "StrLen", &a, 1, r, 4);
    Check("a hook calls the original and changes the result (7 + 1)", h1 > 0 && n == 1 && r[0].type == TW3SE_INT && r[0].v.i == 8, g_api->LastError());
    int h2 = g_api->HookScript(nullptr, "StrLen", &OnStrLenArg, nullptr, 5);
    n = g_api->Call(nullptr, "StrLen", &a, 1, r, 4);
    snprintf(d, sizeof d, "%lld", n == 1 && r[0].type == TW3SE_INT ? static_cast<long long>(r[0].v.i) : -1LL);
    Check("a second hook (higher priority) changes the argument (\"ab\": 2 + 1)", h2 > 0 && n == 1 && r[0].type == TW3SE_INT && r[0].v.i == 3, d);
    g_api->Unhook(h2);
    g_api->Unhook(h1);
    n = g_api->Call(nullptr, "StrLen", &a, 1, r, 4);
    Check("after Unhook the function is as before", n == 1 && r[0].type == TW3SE_INT && r[0].v.i == 7, g_api->LastError());

    void* cls = g_api->FindClassByName("CR4Player");
    const char* cn = g_api->ClassName(cls);
    Check("ClassName", cn && !strcmp(cn, "CR4Player"), cn ? cn : "NULL");
    const char* pn = g_api->ClassName(g_api->ClassParent(cls));
    Check("ClassParent", pn && !strcmp(pn, "CPlayer"), pn ? pn : "NULL");
    FieldHunt hunt = {0, false, ""};
    g_api->ForEachField(g_api->GetObjectClass(player), &OnField, &hunt);
    snprintf(d, sizeof d, "%d fields, equippedSign: %s", hunt.count, hunt.signType);
    Check("ForEachField finds inherited fields with their type", hunt.count > 50 && hunt.sign && !strcmp(hunt.signType, "ESignType"), d);
    bool getName = false;
    int methods = g_api->ForEachMethod(g_api->GetObjectClass(player), &OnMethod, &getName);
    snprintf(d, sizeof d, "%d methods", methods);
    Check("ForEachMethod finds inherited methods", methods > 100 && getName, d);
    int64_t ev = -1;
    Check("EnumValue", g_api->EnumValue("EDeathType", "EDT_IgniDeath", &ev) && ev == 1);
    Check("EnumValue refuses a wrong name", !g_api->EnumValue("EDeathType", "nope", &ev));

    ObjHunt oh = {player, false, 0};
    int visited = g_api->ForEachObject("CActor", &OnObject, &oh);
    snprintf(d, sizeof d, "%d actors", visited);
    Check("ForEachObject finds the player among the actors", oh.found && visited == oh.count && visited > 1, d);
    Check("ForEachObject refuses a wrong class", g_api->ForEachObject("NoSuchClass", &OnObject, &oh) == 0);

    TW3SE_Value seven = Int(7);
    n = g_api->Call(nullptr, "TW3SE_TestLatent", &seven, 1, r, 4);
    Check("a latent function starts (an OTHER)", n == 1 && r[0].type == TW3SE_OTHER && strstr(r[0].v.s, "latent"),
          n == 1 && r[0].type == TW3SE_OTHER ? r[0].v.s : g_api->LastError());

    snprintf(reply, size, "%s==== apiv18 %d/%d passed ====", g_out, g_pass, g_pass + g_fail);
}

// --- apiv21: the helpers of tw3se.hpp for interface 21. "apiv21" checks and asks example.com; "apiv21 web" a few
// seconds later shows what came back on the channels.
static std::string g_web, g_ping;
static void OnWeb(const char*, const char* msg, void*) { g_web = msg ? msg : ""; }
static void OnPing(const char*, const char* msg, void*) { g_ping = msg ? msg : ""; }

static void ApiV21(const char* args, char* reply, uint32_t size) {
    g_out[0] = 0;
    g_pass = g_fail = 0;
    char d[256];
    if (args && !strcmp(args, "web")) {
        snprintf(d, sizeof d, "%.60s", g_web.c_str());
        Check("http_request: the answer came on the channel (200)", g_web.rfind("200\n", 0) == 0, d);
        Check("send/on_message: the plugin got its own message", g_ping == "ping 21", g_ping.c_str());
        snprintf(reply, size, "%s==== apiv21 web %d/%d passed ====", g_out, g_pass, g_pass + g_fail);
        return;
    }
    Check("interface 21", g_api->interfaceVersion >= 21);
    Check("saved_set", tw3se::saved_set("ApiTest", "k1", "a]]b'c\"d"));
    std::string v = tw3se::saved_get("ApiTest", "k1");
    Check("saved_get gives the value back (with ]] and quotes)", v == "a]]b'c\"d", v.c_str());
    const char* odd = "\nends with ]";
    Check("saved_set with ] at the end and a line break first", tw3se::saved_set("ApiTest", "k[[2", odd));
    v = tw3se::saved_get("ApiTest", "k[[2");
    Check("saved_get gives it back", v == odd, v.c_str());
    Check("saved_set UTF-8", tw3se::saved_set("ApiTest", "k3", "Gr\xc3\xbc\xc3\x9f" "e\\"));
    v = tw3se::saved_get("ApiTest", "k3");
    Check("saved_get UTF-8 and a backslash", v == "Gr\xc3\xbc\xc3\x9f" "e\\", v.c_str());
    v = tw3se::saved_get("ApiTest", "nothing_here");
    Check("saved_get of a missing key is empty", v.empty(), v.c_str());
    uint32_t st = tw3se::game_state();
    snprintf(d, sizeof d, "0x%x", st);
    Check("game_state: in the game", (st & TW3SE_STATE_IN_GAME) && !(st & TW3SE_STATE_MAIN_MENU), d);
    static bool listening = false;
    if (!listening) {
        listening = tw3se::on_message("apitest.web", &OnWeb) && tw3se::on_message("apitest.ping", &OnPing);
        Check("on_message", listening);
    }
    g_web.clear();
    g_ping.clear();
    tw3se::send("apitest.ping", "ping 21");
    Check("http_request starts", tw3se::http_request("GET", "https://example.com/", "", "apitest.web"));
    snprintf(reply, size, "%s==== apiv21 %d/%d passed (then: apiv21 web) ====", g_out, g_pass, g_pass + g_fail);
}

// --- apiv22: blend shapes for plugins, in steps (tools/apiv22_test.py drives them; the copy loads between them):
//   apiv22 own <component>             DeformOwn, and DeformLoaded right after it (0: the copy needs ticks)
//   apiv22 load <component> <path>     DeformLoaded, DeformLoad, DeformShapeIndex, DeformWeights, DeformState
//   apiv22 end <component> <set>       DeformUnload, DeformState, DeformRelease, a second DeformRelease
static void ApiV22(const char* args, char* reply, uint32_t size) {
    g_out[0] = 0;
    g_pass = g_fail = 0;
    char d[256], step[16] = {}, path[200] = {};
    unsigned long long addr = 0;
    int set = 0;
    if (g_api->interfaceVersion < 22 || !args || sscanf_s(args, "%15s %llu", step, (unsigned)sizeof step, &addr) < 2) {
        snprintf(reply, size, "apiv22 own|load|end <component> ... (needs interface 22)");
        return;
    }
    void* c = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    if (!strcmp(step, "own")) {
        Check("DeformOwn", g_api->DeformOwn(c) == 1, g_api->LastError());
        snprintf(d, sizeof d, "%d", g_api->DeformLoaded(c));
        Check("DeformLoaded answers right after the copy (0 or 1)", d[0] == '0' || d[0] == '1', d);
    } else if (!strcmp(step, "load") && sscanf_s(args, "%*s %*llu %199s", path, (unsigned)sizeof path) == 1) {
        Check("DeformLoaded: the copy is on the graphics card", g_api->DeformLoaded(c) == 1);
        int32_t s = g_api->DeformLoad(c, path, 2.0f);
        Check("DeformLoad gives a set", s > 0, g_api->LastError());
        int inflate = g_api->DeformShapeIndex(s, "inflate");
        snprintf(d, sizeof d, "%d", inflate);
        Check("DeformShapeIndex(inflate) = 0", inflate == 0, d);
        Check("DeformShapeIndex of no such name = -1", g_api->DeformShapeIndex(s, "no_such_shape") == -1);
        float w[8] = {1.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0};
        int took = g_api->DeformWeights(s, w, 8);
        snprintf(d, sizeof d, "took %d", took);
        Check("DeformWeights takes as many as the set has (3)", took == 3, d);
        Check("DeformWeights of no set = 0", g_api->DeformWeights(999999, w, 3) == 0);
        int st = g_api->DeformState(s);
        snprintf(d, sizeof d, "%d", st);
        Check("DeformState loading or ready", st == TW3SE_DEFORM_LOADING || st == TW3SE_DEFORM_READY, d);
        snprintf(reply, size, "%sset %d\n==== apiv22 load %d/%d passed ====", g_out, s, g_pass, g_pass + g_fail);
        return;
    } else if (!strcmp(step, "end") && sscanf_s(args, "%*s %*llu %d", &set) == 1) {
        snprintf(d, sizeof d, "%d", g_api->DeformState(set));
        Check("DeformState ready before the end", d[0] == '0' + TW3SE_DEFORM_READY, d);
        Check("DeformUnload", g_api->DeformUnload(set) == 1);
        Check("DeformRelease", g_api->DeformRelease(c) == 1, g_api->LastError());
        int again = g_api->DeformRelease(c);  // (first, then LastError: MSVC evaluates the arguments right to left)
        const char* why = g_api->LastError();
        Check("a second DeformRelease = 0, with a reason", again == 0 && strstr(why, "no copy of its own"), why);
    } else {
        snprintf(reply, size, "apiv22 own|load|end <component> ...");
        return;
    }
    snprintf(reply, size, "%s==== apiv22 %s %d/%d passed ====", g_out, step, g_pass, g_pass + g_fail);
}

// --- apisync: does a blend-shape weight set in the pose callback show in the same frame as that callback's bones?
//   apisync <entity> <set> <mode>   mode 1: the hand bone moves, 2: the shape's first weight, 3: both, 0: off
// The figure is frozen (each mesh's bones as in the first frame seen) and every 6 ticks the change goes on or off,
// in the pose callback of the tick (tools/sync_test.py takes the frames with w3._shot_burst).
static void* g_syncEnt = nullptr;
static int32_t g_syncSet = 0;
static int g_syncMode = 0;
static volatile LONG g_syncTick = 0;
static SRWLOCK g_syncLock = SRWLOCK_INIT;
struct SyncMesh {
    void* mesh;
    int n;
    float local[1024 * 16];
};
static SyncMesh g_syncMeshes[8];
static int g_syncMeshCount = 0;

static void SyncTick(float, void*) { InterlockedIncrement(&g_syncTick); }
static void SyncPose(void* entity, TW3SE_Pose* p, void*) {
    if (!g_syncMode || entity != g_syncEnt || p->localStride != 16 || p->boneCount > 1024) return;
    bool on = (g_syncTick / 6) % 2 == 1;
    AcquireSRWLockExclusive(&g_syncLock);
    SyncMesh* m = nullptr;
    for (int i = 0; i < g_syncMeshCount; ++i)
        if (g_syncMeshes[i].mesh == p->mesh) m = &g_syncMeshes[i];
    if (!m && g_syncMeshCount < 8) {
        m = &g_syncMeshes[g_syncMeshCount++];
        m->mesh = p->mesh;
        m->n = p->boneCount;
        memcpy(m->local, p->local, sizeof(float) * 16 * p->boneCount);
    }
    if (m && m->n == p->boneCount) {
        memcpy(p->local, m->local, sizeof(float) * 16 * p->boneCount);  // frozen
        if (on && (g_syncMode & 1))
            for (int i = 0; i < p->boneCount; ++i)
                if (p->boneNames && p->boneNames[i] && !strcmp(p->boneNames[i], "r_hand")) {
                    p->local[i * 16 + 12] += 0.25f;  // the translation row: the hand 25 cm off
                    p->local[i * 16 + 14] += 0.25f;
                }
    }
    ReleaseSRWLockExclusive(&g_syncLock);
    if (g_syncMode & 2) {
        float w = on ? 1.0f : 0.0f;
        g_api->DeformWeights(g_syncSet, &w, 1);
    }
}
static void ApiSync(const char* args, char* reply, uint32_t size) {
    unsigned long long e = 0;
    int set = 0, mode = 0;
    if (g_api->interfaceVersion < 22 || !args || sscanf_s(args, "%llu %d %d", &e, &set, &mode) != 3) {
        snprintf(reply, size, "apisync <entity> <set> <mode 0-3>");
        return;
    }
    static bool registered = false;
    if (!registered) {
        g_api->RegisterOnPose(&SyncPose, nullptr);
        g_api->RegisterOnTick(&SyncTick, nullptr);
        registered = true;
    }
    void* ent = reinterpret_cast<void*>(static_cast<uintptr_t>(e));
    if (mode && g_syncEnt != ent) {
        AcquireSRWLockExclusive(&g_syncLock);
        g_syncMeshCount = 0;
        ReleaseSRWLockExclusive(&g_syncLock);
    }
    g_syncEnt = ent;
    g_syncSet = set;
    g_syncMode = mode;
    int watched = g_api->WatchPose(ent, mode ? 1 : 0);
    snprintf(reply, size, "apisync mode %d, watch %d, %d meshes frozen", mode, watched, g_syncMeshCount);
}

static void OnRenderRebuild(int phase, void*) {  // interface 25: the order shows in tw3se.log
    g_api->Log("ApiTest", "render rebuild: %s", phase == TW3SE_REBUILD_BEFORE ? "before" : "after");
}

TW3SE_EXPORT int TW3SE_PluginQuery(TW3SE_PluginInfo* info) {
    info->name = "ApiTest";
    info->author = "TW3SE tests";
    info->version = 0x00010000;
    info->minInterface = 17;
    return 1;
}

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    g_api = api;
    tw3se::init(api);
    if (api->interfaceVersion >= 21)
        api->RegisterCommand("apiv21", &ApiV21, "apiv21 [web]: checks the helpers of interface 21 (apitest plugin)");
    if (api->interfaceVersion >= 22)
        api->RegisterCommand("apiv22", &ApiV22, "apiv22 own|load|end <component> ...: blend shapes for plugins (apitest plugin)");
    if (api->interfaceVersion >= 22)
        api->RegisterCommand("apisync", &ApiSync, "apisync <entity> <set> <mode>: bone and blend shape changed in one pose callback (apitest plugin)");
    if (api->interfaceVersion >= 25) api->RegisterOnRenderRebuild(&OnRenderRebuild, nullptr);
    api->RegisterCommand("apifuzz", &Fuzz, "apifuzz: every plugin interface function with careless input (apitest plugin)");
    api->RegisterCommand("apiv18", &ApiV18, "apiv18: checks the typed calls, fields, hooks and catalog of interface 18 (apitest plugin)");
    return 1;
}
