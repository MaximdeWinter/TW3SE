// Addresses in editor.exe (The Witcher 3 REDkit, bin\x64_RedKit), PE timestamp 0x6ac574c5 (2026-10-06).
// Same macro format as src/core/addresses.inc - see that file's header for the macro reference.
// A _ed suffix marks a name without a retail counterpart.
FUNC("RegisterCoreScriptNatives", 0x03b4c910, "48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 20 E8 ?? ?? ?? ??")
FUNC("CFunction::CFunction", 0x03a99140, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 33 F6 48 8D 05 ?? ?? ?? ??")
FUNC("CRTTISystem::RegisterGlobalFunction", 0x03a149a0, "40 57 41 56 41 57 48 83 EC 40 4C 8B FA 48 8B F9")
FUNC("Memory::AllocDefault", 0x004edb10, "4C 8B DC 49 89 5B 08 49 89 6B 10 56 57 41 56 48 83 EC 70")
FUNC("CNamesPool::AddEntry", 0x03a92c50, "48 89 5C 24 08 57 48 83 EC 30 48 8B DA 48 8B F9 48 85 D2 74 ?? 80 3A 00 74 ?? EB ??")
FUNC("CScriptCompiler::DeclareGlobalFunction", 0x03ba8c00, "40 53 55 56 48 83 EC 30 48 83 BA 00 01 00 00 00 41 0F B6 E8")
// CName::CName(this, const char*): what CFunction::CFunction calls to turn the native's name into a CName, the
// anchor for CNamesPool::Get (the engine's "name string -> CName" constructor).
FUNC("CName::CName", 0x039e09d0, "48 89 5C 24 08 57 48 83 EC 30 48 8B DA 48 8B F9 48 85 D2 74 ?? 80 3A 00 74 ?? 48 8D 15 ?? ?? ?? ??")
CALL("SRTTI::Get", "RegisterCoreScriptNatives", 0x5ac, "E8 ?? ?? ?? ?? 48 8B D3")
CALL("CNamesPool::Get", "CName::CName", 0x2d, "E8 ?? ?? ?? ?? 48 8B D3")
// The native-table singleton: a POINTER SLOT to an object whose +0x94 holds the number of table entries (read
// from ScriptNativeTable::Add below). The handler array itself is static - see GScriptNativeTable at the end.
GLOBAL("GScriptNativeTable_ed_Singleton", "CFunction::CFunction", 0x68, 3, 7, "48 8B 0D ?? ?? ?? ?? 48 81 C1 94 00 00 00")
// The native handler table itself. In editor.exe it is a static array of up to
// 0x1000 handler pointers (opcodes first, then every registered native - CFunction+0xe0 indexes it, as in
// retail); the count lives in the singleton above at +0x94. Its add-function stores into it with a RIP-relative
// lea, which is the anchor here. ReadParam and the call paths use it under the retail name.
FUNC("ScriptNativeTable::Add", 0x03b84370, "48 89 5C 24 08 57 48 83 EC 30 48 8B FA 48 8B D9 48 85 D2 74 ?? 81 3B 00 10 00 00")
GLOBAL("GScriptNativeTable", "ScriptNativeTable::Add", 0x1f, 3, 7, "48 8D 0D ?? ?? ?? ?? 48 89 3C C1")

// Reload, log and tick (each signature has exactly one match in build 0x6ac574c5).

// The compiler's call compile: every function call passes here while scripts compile. The check that keeps
// Log/LogChannel quiet during a reload is here too, with its flag on a separate global pointer at +0x184 (retail:
// +0x17c on the scripting object itself).
FUNC("CScriptCompiler::CompileCall", 0x03c055a0, "40 55 53 56 57 41 57 48 8D AC 24 B0 F0 FF FF")

// The two built-in natives that write log lines (retail: funcLogChannel/funcLog). The editor has no function
// pointer slot like GScriptLogSink, see RedLogFormat_ed below.
FUNC("funcLogChannel", 0x03b4b780, "48 89 5C 24 08 57 48 83 EC 70 8B 05 ?? ?? ?? ?? 48 8B DA 48 8B 52 08")
FUNC("funcLog", 0x03b4b6f0, "40 53 48 83 EC 40 48 8B DA 48 8D 4C 24 30 48 8D 15 ?? ?? ?? ??")

// The non-variadic log step funcLogChannel calls when its channel is on: four fixed parameters (category hash,
// category name, level, finished text). funcLog (no channel) goes elsewhere - see FINDINGS_REDKIT.md.
FUNC("ScriptLogChannelText_ed", 0x017bc8e0, "48 89 5C 24 10 48 89 6C 24 18 57 41 56 41 57 48 83 EC 60 48 8B 3D ?? ?? ?? ??")

// In place of GScriptLogSink: funcLog, the channel branch of funcLogChannel and the compiler's own warnings
// ("CompileFiles is legacy...") all go through this one formatting log step (category hash in ecx, category name
// in rdx, level in r8d, format string in r9, further arguments on the stack). Every script log line is caught by a
// trampoline on its entry. Details in FINDINGS_REDKIT.md.
FUNC("RedLogFormat_ed", 0x017bc7b0, "4C 89 4C 24 20 53 55 56 57 41 56 41 57 B8 88 40 01 00")

// ReloadScripts' inner worker, found by its progress text "Compiling scripts (finding script files)...". It
// compiles every .ws file again, as in retail (scripting.md section 9).
FUNC("CScriptingSystem::CompileAllScripts", 0x03ba5870, "48 8B C4 4C 89 40 18 55 53 41 54 48 8D A8 58 F7 FF FF")
// Its two callers differ from retail's ReloadScripts(this, param2): CompileFiles_ed (0x03ba5730, the "legacy"
// path) checks a lock flag on another global object, and 0x03a8a4b0 times the run (the editor's "Reload scripts"
// menu entry). Neither is in the table.

// CCommonGame::OnGameStart: vtable slot 108 (retail: 101), found from the RTTI type name ".?AVCCommonGame@@"
// (FINDINGS_REDKIT.md). The same form as in retail (two calls and a tail jump over one field) with one more call at
// its start.
FUNC("CCommonGame::OnGameStart", 0x01d0ce30, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B E8 5E 01 00")
FIELD("CCommonGame.dlcManager", "CCommonGame::OnGameStart", 0x11, 0x15ee8, "48 8B 8B ?? ?? ?? ?? 45 33 C0", 3)
// The two DLC functions OnGameStart calls.
FUNC("CDLCManager::Scan", 0x01fc23a0, "44 88 44 24 18 89 54 24 10 48 89 4C 24 08 55")
FUNC("CDLCManager::OnGameStarting", 0x01fc4510, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F1 E8 ?? ?? ?? ?? 84 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? BA B6 02 00 00")

// CRemoteConnection, vtable slot 1 (as in retail, link.md): takes a network packet and appends {id, text} to a
// TDynArray at this+0x34 (count +0x3c), the retail offsets. This is the receiving side; the place that empties the
// queue each tick and runs it is not in the table.
FUNC("CRemoteConnection::OnPacket_ed", 0x032092f0, "40 53 55 56 57 48 81 EC 48 08 00 00")

// The function the main loop calls each pass through vtable slot 11 (0x58): CEditorEngine's override (the base
// CBaseEngine has its own at 0x28cc450). (this, deltaTime in xmm1), the form of retail's CCommonGame::Tick. It runs
// in edit mode and in Play (71 % and 96 % of the stack samples), so it is the editor's tick without Play.
FUNC("EditorEngineTick_ed", 0x009019c0, "48 89 5C 24 10 55 56 57 48 8D AC 24 20 FE FF FF")

// CNamesPool.entries/entryCount and three CRTTISystem fields (global functions), all at the retail offsets.
// CFunction's fields come from the constructor with the opcode: four as in retail, OverriddenFunction 0x8 further.
// GScriptGlobals is the pointer slot GScriptNativeTable_ed_Singleton above.
FIELD("CNamesPool.entries", "CNamesPool::AddEntry", 0x1d6, 0x11830, "48 8D 8E ?? ?? ?? ?? 48 C7 47 08 00 00 00 00", 3)
FIELD("CNamesPool.entryCount", "CNamesPool::AddEntry", 0x1ee, 0x11838, "8B 8E ?? ?? ?? ?? 48 8B 44 24 58", 2)
FIELD("CRTTISystem.globalFuncCapacity", "CRTTISystem::RegisterGlobalFunction", 0x2c, 0x40, "F7 77 ?? 48 8B 47 60 48 8B 14 D0", 2)
FIELD("CRTTISystem.globalFuncCount", "CRTTISystem::RegisterGlobalFunction", 0x1c, 0x44, "44 8B 4F ?? 45 85 C9 74 ??", 3)
FIELD("CRTTISystem.globalFuncBuckets", "CRTTISystem::RegisterGlobalFunction", 0x30, 0x60, "48 8B 47 ?? 48 8B 14 D0", 3)
FIELD("CFunction.Name", "CFunction::CFunction", 0x29, 0x10, "89 41 ?? 89 71 14 C7 41 18 03 00 00 00", 2)
FIELD("CFunction.Flags", "CFunction::CFunction", 0x2f, 0x18, "C7 41 ?? 03 00 00 00 48 89 71 20", 2)
FIELD("CFunction.ReturnType", "CFunction::CFunction", 0x37, 0x20, "48 89 71 ?? 48 89 71 28", 3)
FIELD("CFunction.Parameters", "CFunction::CFunction", 0x3b, 0x28, "48 89 71 ?? 89 71 30 48 89 71 34", 3)
FIELD("CFunction.OverriddenFunction", "CFunction::CFunction", 0x61, 0xf0, "48 89 B7 ?? ?? ?? ?? 48 8B D3", 3)

// The game console's command executor: takes a line, finds the global function it names in the RTTI, checks the
// exec bit (CFunction.Flags 0x100), calls it and logs "Result: '%s'" through the same log step as the natives.
// (rcx, rdx, r8b): rcx is unused in the visible body (likely a context passed on); rdx is the line as an AnsiString
// object (pointer + length); r8b a bool that keeps the "unknown function" error quiet. Main thread only, like the
// other RTTI calls.
FUNC("ConsoleExecCommand_ed", 0x03a8c060, "40 55 53 41 56 48 8D 6C 24 B9 48 81 EC B0 00 00 00 83 7A 08 01")

// The CProperty constructor, five fields at the retail offsets. The editor's CProperty is larger than retail's
// 0x30 bytes (more fields after these five, up to 0x50 at least).
FUNC("CProperty::CProperty", 0x03aa1900, "40 53 48 83 EC 20 48 89 51 08 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B D9 48 8B 44 24 50 8B 10")
FIELD("CProperty.Type", "CProperty::CProperty", 0x9, 0x8, "48 89 51 ?? 48 8D 05 ?? ?? ?? ??", 3)
FIELD("CProperty.Name", "CProperty::CProperty", 0x24, 0x10, "89 51 ?? 48 8B 54 24 58", 2)
FIELD("CProperty.OwnerClass", "CProperty::CProperty", 0x30, 0x18, "4C 89 41 ?? 44 89 49 20", 3)
FIELD("CProperty.ByteOffset", "CProperty::CProperty", 0x34, 0x20, "44 89 49 ?? 48 C7 41 28 00 00 00 00", 3)
FIELD("CProperty.Flags", "CProperty::CProperty", 0x2c, 0x24, "89 41 ?? 4C 89 41 18 44 89 49 20", 2)

// CRTTISystem's type table (types and classes by name id), separate from the global function table above, found
// by the error text "Type %s is being registered for the second time!" and the lookup before it. Its offsets are
// 0xa0 (capacity), 0xa4 (count), 0xc0 (buckets).
FUNC("FindType_ed", 0x03a15680, "40 55 56 48 8D 6C 24 B1 48 81 EC A8 00 00 00 44 8B 02")
FUNC("RegisterType_ed", 0x03a14560, "48 89 5C 24 18 55 56 41 56 48 83 EC 40 4C 8B F2 48 8B F1")
FIELD("CRTTISystem.typeCapacity", "FindType_ed", 0x4a, 0xa0, "F7 B1 ?? ?? ?? ?? 48 8B 81 C0 00 00 00", 2)
FIELD("CRTTISystem.typeCount", "FindType_ed", 0x28, 0xa4, "83 B9 ?? ?? ?? ?? 00 48 89 9C 24 A0 00 00 00", 2)
FIELD("CRTTISystem.typeBuckets", "FindType_ed", 0x51, 0xc0, "48 8B 81 ?? ?? ?? ?? 48 8B 14 D0", 3)

// The table at 0x10/0x30 exists in the editor too: FindType_ed looks in the table above first and, when that
// fails, in this smaller one. RegisterType_ed writes into it directly.
FIELD("CRTTISystem.classCapacity", "RegisterType_ed", 0xcc, 0x10, "F7 76 ?? 48 8B 46 30 8B EA", 2)
FIELD("CRTTISystem.classCount", "RegisterType_ed", 0x1a6, 0x14, "FF 46 ?? 48 8B 7C 24 68", 2)
FIELD("CRTTISystem.classBuckets", "RegisterType_ed", 0xd0, 0x30, "48 8B 46 ?? 8B EA 48 8B 0C D0", 3)

// GScriptMethodTable. CFunction::CFunctionMethod is the second constructor of CFunction (told apart by this+0x08,
// a real parameter here, and its 1-byte flags). It registers through ScriptMethodTable::Add into a table of
// 24-byte entries (scripting.md), source file scriptNativeFunctionMap.cpp like ScriptNativeTable::Add.
FUNC("CFunction::CFunctionMethod", 0x03a98fe0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 33 ED 48 89 51 08")
FUNC("ScriptMethodTable::Add", 0x03b84290, "48 89 5C 24 08 57 48 83 EC 30 48 83 3A 00 48 8B FA")
GLOBAL("GScriptMethodTable", "ScriptMethodTable::Add", 0x20, 3, 7, "48 8D 15 ?? ?? ?? ?? C5 F8 10 07")

// CClass: the anchor is the common base constructor CClass::CClass calls after it sets its vtable. All seven
// fields at the retail offsets.
FUNC("CClass::CClass", 0x039e1e20, "48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 30 45 33 F6 C7 41 08 FF FF FF FF")
FIELD("CClass.parent", "CClass::CClass", 0x1d, 0x10, "4C 89 71 ?? 48 8D 05 ?? ?? ?? ??", 3)
FIELD("CClass.name", "CClass::CClass", 0x3b, 0x2c, "89 41 ?? 4C 89 71 30 44 89 71 38", 2)
FIELD("CClass.properties", "CClass::CClass", 0x3f, 0x30, "4C 89 71 ?? 44 89 71 38", 3)
FIELD("CClass.propertyCount", "CClass::CClass", 0x43, 0x38, "44 89 71 ?? 4C 89 71 3C", 3)
FIELD("CClass.functions", "CClass::CClass", 0x4f, 0x48, "4C 89 71 ?? 44 89 71 50", 3)
FIELD("CClass.functionCount", "CClass::CClass", 0x53, 0x50, "44 89 71 ?? 4C 89 71 54", 3)
FIELD("CClass.size", "CClass::CClass", 0x5f, 0x60, "44 89 41 ?? 44 89 71 64", 3)

// The garbage collector's mark function (retail: CObjectsGC_MarkAndSweep::MarkReachable); it runs over objects
// of many types.
FUNC("GC_MarkReachable_ed", 0x039e7470, "40 53 57 41 54 41 56 41 57 48 83 EC 60")
// CEntity's components, anchored at CEntity's own accessor ("lea rax,[this+0xf0]; ret"); GC_MarkReachable_ed
// reads the same offset when it walks an entity's components.
FUNC("CEntity::GetComponents_ed", 0x007fb3f0, "48 8D 81 F0 00 00 00 C3 CC CC CC CC CC CC CC CC 48 8D 05 ?? ?? ?? ??")
FIELD("CEntity.components", "CEntity::GetComponents_ed", 0x3, 0xf0, "48 8D 81 ?? ?? ?? ?? C3", 3)
FIELD("CEntity.componentCount", "GC_MarkReachable_ed", 0x41, 0xf8, "44 8B B7 ?? ?? ?? ?? 33 DB", 3)

// CEntity.guid, from a vtable function that copies a 16-byte GUID. The retail offset.
FUNC("CEntity::GetGUID_ed", 0x026bedc0, "48 89 5C 24 10 57 48 83 EC 60 48 8B F9 48 8D 4C 24 50")
FIELD("CEntity.guid", "CEntity::GetGUID_ed", 0x1b, 0xcc, "C5 F8 10 87 ?? ?? ?? ??", 4)

// The editor's own crash guard, in front of TW3SE's SEH blocks. EditorCrashHandlerState_ed gives (made once) an
// object that registers both callbacks below at the start: AddVectoredExceptionHandler (priority 1, before every
// __except block in the process) and SetUnhandledExceptionFilter. EditorVectoredHandler_ed lets 0x40010006 (a
// debug output notice) and code 4 pass and handles 0xc0000005 (access violation) itself, before TW3SE would see it.
// EditorUnhandledFilter_ed is the second, later hook.
FUNC("EditorCrashHandlerState_ed", 0x0017c0830, "48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 ?? ?? ?? ??")
FUNC("EditorVectoredHandler_ed", 0x0017c3160, "40 53 48 83 EC 20 48 8B 11 48 8B D9 44 8B 02")
FUNC("EditorUnhandledFilter_ed", 0x0017c31e0, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8B D3 48 8B C8")

// CContentManager's bundles, found by a bounds check ("bundleIndex < m_bundles.Size()"). Its vtable has nine
// entries; ScanContentPack, CreatePackage and AttachPackage are not virtual, as in retail.
FUNC("CContentManager::GetBundle_ed", 0x03ae5050, "48 89 5C 24 08 57 48 83 EC 30 8B DA 48 8B F9 3B 51 50 73 ?? 3B 5F 50 72 ?? 48 8D 05 ?? ?? ?? ??")
FIELD("CContentManager.bundles", "CContentManager::GetBundle_ed", 0x2e, 0x48, "48 8D 4F ?? 8B D3 E8 ?? ?? ?? ?? 48 8B 5C 24 40 48 8B 00 48 83 C4 30", 3)
FIELD("CContentManager.bundleCount", "CContentManager::GetBundle_ed", 0x11, 0x50, "3B 51 ?? 73 ?? 3B 5F 50 72 ?? 48 8D 05 ?? ?? ?? ??", 2)

// GContentManager, anchored at the null check in CDLCManager::Scan ("Fatal: GContentManager"). The same instance
// as in retail, so the bare name.
GLOBAL("GContentManager", "CDLCManager::Scan", 0x4de, 3, 8, "48 83 3D ?? ?? ?? ?? 00 75 ??")

// Attaches an entity's components: one parameter (this). Called directly from the layer attach loop and from the
// entity pool's reuse path; walks this+0xf0/+0xf8 (the component list) and calls two vtable functions per
// component. At the game start all calls come through the entity pool path; loading Kaer Morhen in edit mode runs
// no layer attach.
FUNC("CEntity::AttachComponentsToWorld_ed", 0x026c5290, "4C 8B DC 53 41 57 48 81 EC 88 00 00 00")
// CEntity::AttachToWorld(this, world), found by the error text "...AttachToWorld() was called for entity..." in
// entity.cpp. Sets this+0x54 |= 0x200000, calls vtable slot 53 (0x1a8) with (this, world), checks
// this+0x54 & 0x100000 (an "IsAttached()" assert of the editor build) and clears the flag. Many callers across
// script and gameplay code; the layer attach loop takes its own path. Retail calls slot 48 (0x180): CEntity has five
// more virtual methods in the editor.
FUNC("CEntity::AttachToWorld", 0x026c56b0, "48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00")
// The vtable function it calls (the "OnAttached" counterpart): checks the same field at entry (an
// "IsAttaching()" assert), then sets this+0x54 |= 0x100000.
FUNC("CEntity::OnAttachedHandler_ed", 0x026bea20, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F2 48 8B F9 E8 ?? ?? ?? ?? F7 47 54 00 00 20 00")

// Teleport: void(CEntity*, Vector* pos, EulerAngles* rot), two vtable calls 8 bytes apart (set position, then
// rotation), always true; the same form as retail's. In the editor it calls +0x228/+0x230, which are its inner
// setters.
FUNC("CEntity::Teleport_ed", 0x026c7940, "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 49 8B F8 48 8B D9 FF 90 28 02 00 00")
// CEntity.vtTeleport, anchored where Teleport is called through the vtable (so overrides, the player's among
// them, are reached): the native "TeleportWithRotation" (funcTeleportWithRotation_ed) calls vtable+0x500. The same
// value comes from the CGameplayEntity vtable.
FUNC("funcTeleportWithRotation_ed", 0x026ce520, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 C5 F8 10 05 ?? ?? ?? ?? 48 8B FA 33 DB 48 8B 52 08 48 8B F1")
FIELD("CEntity.vtTeleport", "funcTeleportWithRotation_ed", 0xa1, 0x500, "FF 90 ?? ?? ?? ?? 48 8B 5C 24 50", 2)

// Pose hook: the retail signature of ModelSpaceFillDispatch matches in the editor as it is. It calls one of two
// functions depending on a pointer field (+0xf08 in the sub-object, component+0x1028). The field +0xcbc
// (component+0xddc, the compact buffer) is used as in retail.
FUNC("ModelSpaceFillDispatch_ed", 0x026e80b0, "48 89 5C 24 08 57 48 83 EC 20 4C 8B 81 08 0F 00 00")
FUNC("Combine_Format1", 0x026e31f0, "4C 8B DC 49 89 5B 20 4D 89 43 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC D0 00 00 00")
FUNC("Combine_Format2", 0x026e3900, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC B0 00 00 00 48 8B 59 20")

// CFoliageScene's fields, checked in the destructor's cleanup (it releases the four owned sub-objects in turn).
// dispatcher, resourceHandler and collisionHandler sit at the retail offsets (player.+30.+40.+40 leads to
// CFoliageScene, from there +60/+68/+80/+88).
FUNC("CFoliageScene::DtorRelease_ed", 0x02df14c0, "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 8B F2 48 8D 05 ?? ?? ?? ??")
FIELD("CFoliageScene.dispatcher", "CFoliageScene::DtorRelease_ed", 0x7b, 0x68, "48 8B 4B ?? 48 85 C9 74 ?? 48 8B 01 BA 01 00 00 00 FF 10", 3)
FIELD("CFoliageScene.resourceHandler", "CFoliageScene::DtorRelease_ed", 0x65, 0x80, "48 8B 8B ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 BA 01 00 00 00 FF 10 48 8B 4B 68", 3)
FIELD("CFoliageScene.collisionHandler", "CFoliageScene::DtorRelease_ed", 0x38, 0x88, "48 8B 8B ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 BA 01 00 00 00 FF 10 48 8B 43 78", 3)

// Foliage: funcFoliageResolveResourceReference has the retail signature (so the bare name);
// funcFoliageCopyInstancePlacements_ed is the editor's. 0x1efed20 has the same start but is a setter
// ([rcx+0x90] = rdx). The editor's counterpart of the resolve is the virtual method CSRTBaseTree vtable +0x240: makes
// the render object when needed, keeps it at +0xa8 and returns it with a reference (retail: +0x90). The editor's
// send function CreateDynamic calls it itself.
FUNC("funcFoliageResolveResourceReference", 0x02dc2ce0, "48 89 5C 24 08 57 48 83 EC 20 48 83 B9 A8 00 00 00 00 48 8B DA")
FUNC("funcFoliageCopyInstancePlacements_ed", 0x02d66190, "48 89 5C 24 08 57 48 83 EC 20 33 C0 48 8B DA 89 41 08 48 8B F9 48 89 01 41 B9 7F 00 00 00")

// Handle blocks: the release logic (count down; at the last reference with the object still there, discard the
// object through vtable+0x18; at zero free the block) is built into 46 places in the editor, all with the retail
// offsets: count +0x00, flags +0x04, object +0x08. This is one of them, used as the anchor.
FUNC("HandleBlock_ReleaseSite_ed", 0x0055df70, "40 55 56 57 48 81 EC D0 00 00 00")
FIELD("HandleBlock.flags", "HandleBlock_ReleaseSite_ed", 0x6d4, 0x4, "8B 41 ?? A8 02 74 ?? 48 83 79 08 00", 2)
FIELD("HandleBlock.object", "HandleBlock_ReleaseSite_ed", 0x6dc, 0x8, "48 83 79 ?? 00 74 ?? 8B 41 04", 3)

// CObject.handleBlock: the deepest common base constructor makes the block here: 0x10 bytes, refCount = 1,
// flags = 0, HandleBlock.object = this, and the block's address into this+0x08.
FUNC("CObject::CObject_ed", 0x039ed390, "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? BA 10 00 00 00 48 89 01 48 8B D9 8B CA 41 B8 1A 00 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 ?? C7 00 01 00 00 00 C7 40 04 00 00 00 00 48 89 58 08 48 89 43 08 48 8B C3 48 83 C4 20 5B C3 48 C7 43 08 00 00 00 00 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC 40 53")
FIELD("CObject.handleBlock", "CObject::CObject_ed", 0x3e, 0x8, "48 89 43 ?? 48 8B C3 48 83 C4 20 5B C3 48 C7 43 08 00 00 00 00 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC 40 53", 3)

// CQuestThread::CQuestThread matches as in retail, the four fields too (anchors and values from addresses.inc).
FUNC("CQuestThread::CQuestThread", 0x0206f450, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? C6 83 80 00 00 00 01 48 89 03 48 8D 05 ?? ?? ?? ?? 48 89 43 10 33 C0")
FIELD("CQuestThread.listeners", "CQuestThread::CQuestThread", 0x4c, 0x84, "48 89 83 ?? ?? ?? ?? 89 83 8C 00 00 00", 3)
FIELD("CQuestThread.listenersCount", "CQuestThread::CQuestThread", 0x52, 0x8c, "89 83 ?? ?? ?? ?? 48 89 83 90 00 00 00", 2)
FIELD("CQuestThread.childThreads", "CQuestThread::CQuestThread", 0x59, 0x90, "48 89 83 ?? ?? ?? ?? 89 83 98 00 00 00", 3)
FIELD("CQuestThread.childThreadsCount", "CQuestThread::CQuestThread", 0x5f, 0x98, "89 83 ?? ?? ?? ?? 48 89 83 9C 00 00 00", 2)

// Dispatch, found by a back reference to GScriptNativeTable (where Dispatch jumps through the table by the
// native's index); its bytes differ from retail's (likely other registers for the same code). CFunction.Flags
// (+0x18), the native index (+0xe0), code and codeSize (+0x88/+0x90) are as in retail.
FUNC("Dispatch", 0x03a995f0, "40 55 41 54 41 55 41 56 41 57 48 81 EC D0 01 00 00")
FIELD("CFunction.codeSize", "Dispatch", 0x339, 0x90, "8B B7 ?? ?? ?? ?? 48 03 B7 88 00 00 00", 2)
FIELD("CFunction.code", "Dispatch", 0x340, 0x88, "48 03 B7 ?? ?? ?? ?? 80 7B 3C 02", 3)

// GGame: three pointer slots hold the running CR4Game; this one (0xbefc780) is the one used most (2863 places).
FUNC("GGameAnchorFn_ed", 0x005204b0, "40 53 48 83 EC 30 8B 81 18 01 00 00 48 8B D9 C5 F8 29 74 24 20")
GLOBAL("GGame", "GGameAnchorFn_ed", 0x3a, 3, 7, "48 8B 05 ?? ?? ?? ?? 48 8B 88 F0 00 00 00")

// Array growth: the editor has no TDynArray::GrowBy; the PushBack opcode (GetType() == 3) calls this function:
// (CRTTIArrayType*, TDynArray*, count) -> old count. Reallocates to the new count, zeroes the new elements and
// constructs them through the element type (+0x50). Found with hwwatch on a script array's data pointer.
FUNC("CRTTIArrayType::AddArrayElement", 0x03a04d90, "40 55 41 56 41 57 48 83 EC 30 41 8B E8 4C 8B F2 4C 8B F9 41 83 F8 01")

// Foliage send functions, found by the RTTI names of the render commands (CRenderCommand_CreateSpeedTreeDynamic-
// Instances / _RemoveSpeedTreeDynamicInstancesRadius): vtable -> constructor (0x2aab950 / 0x2aabb10) -> its only
// caller. In retail they are free functions, in the editor virtual methods of CFoliageRenderCommandDispatcher (slot 6
// = +0x30, slot 8 = +0x40; 5 and 9 are the non-dynamic twins). The dispatcher is the first argument in both builds,
// as foliage.cpp calls them. The editor gets the resource's render object through its vtable (+0x238/+0x240), retail
// from +0x90. The signatures keep the constructor call (it tells them from the twin). RemoveDynamicRadius is in the
// build twice (the twin 0x100 before, the same bytes): the known build checks the RVA only, another build leaves it
// out.
FUNC("funcFoliageSendCreateSpeedTreeDynamicInstances", 0x035f7fc0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B FA 4C 8D 15 ?? ?? ?? ?? BA C5 9D 1C 81 49 8B F1 49 8B E8 4C 8B F1 B0 43 0F 1F 40 00 0F 1F 84 00 00 00 00 00 0F BE C0 4D 8D 52 01 33 C2 69 D0 93 01 00 01 41 0F B6 02 84 C0 75 ?? B9 50 00 00 00 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 0F 48 8D 54 24 38 4C 8B 81 40 02 00 00 48 8B CF 41 FF D0 49 8B 4E 08 48 8B F8 48 89 4C 24 30 48 85 C9 74 ?? E8 ?? ?? ?? ?? 4C 8B CD 48 89 74 24 20 4C 8B C7 48 8D 54 24 30 48 8B CB E8 E4 38 4B FF")
FUNC("funcFoliageSendRemoveSpeedTreeDynamicInstancesRadius", 0x035f8290, "48 89 74 24 18 57 48 83 EC 50 48 8B 02 4C 8B CA 48 8B F9 48 8D 54 24 68 C5 F8 29 74 24 40 49 8B C9 49 8B F0 C5 F8 28 F3 FF 90 38 02 00 00 48 83 7C 24 68 00 0F 84 ?? ?? ?? ?? 48 89 5C 24 60 4C 8D 0D ?? ?? ?? ?? BA C5 9D 1C 81 B0 52 0F 1F 00 0F BE C0 4D 8D 49 01 33 C2 69 D0 93 01 00 01 41 0F B6 01 84 C0 75 ?? B9 28 00 00 00 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 4C 24 68 48 89 4C 24 30 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 4F 08 48 89 4C 24 38 48 85 C9 74 ?? E8 ?? ?? ?? ?? 4C 8B CE 4C 8D 44 24 30 48 8D 54 24 38 48 8B CB C5 FA 11 74 24 20 E8 C5 37 4B FF")

// CQuestThread::ActivateBlock, found by the traits of the retail function: thread+0x60 -> +0x70, the block's
// search in m_activeBlocks (+0x68/+0x70, step 0x18), the listener loop (+0x84/+0x8c), and byte [thread+0xc0], 0xfd.
// (thread, block, arg) as in retail. The editor checks a block's instance data in a helper (0x2014df0) at
// block+0xf8 (retail: +0xe0; blocks with graph sockets have their fields 0x18 further on). questlive needs the hook
// only (the GUID comes through RTTI).
FUNC("CQuestThread::ActivateBlock", 0x0206fb70, "40 55 56 41 55 48 83 EC 60 48 8B EA 4D 8B E8 48 8B 51 60 48 8B F1 48 85 D2 0F 84")
