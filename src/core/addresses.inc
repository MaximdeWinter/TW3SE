// The engine addresses TW3SE uses, for witcher3.exe 5.0 (PE timestamp 0x6abd8695) as the known build.
// FUNC(name, rva in the known build, signature)
// FUNC_OPT(name, rva, signature): like FUNC, but TW3SE runs without it - on a build that lacks it, only the
// feature that needs it stays off (addresses.cpp). FUNC rows are what TW3SE cannot start without
// GLOBAL(name, function, insn offset in function, disp offset in insn, insn length, anchor): address of the variable
// FIELD(name, function, offset of the disp32 in function, value in the known build, anchor, disp offset in anchor): a structure offset
// CALL(name, function, offset of a direct call to it in function, anchor): a function too short for a signature
// anchor: a pattern from the instruction on, unique in the function's first 0x2000 bytes (found there in other builds)
FUNC("CCommonGame::Tick", 0x01f43750, "40 57 48 83 EC 60 C5 F8 29 74 24 20")
FUNC_OPT("CCommonGame::OnGameStart", 0x01f47730, "40 53 48 83 EC 20 48 8B D9 45 33 C0 48 8B 89 B8 5E 01 00")
FUNC_OPT("CContentManager::ScanContentPack", 0x026cfe60, "48 8B C4 4C 89 40 18 48 89 50 10 48 89 48 08 55 57 48 8D 68 B9")
FUNC_OPT("CContentManager::CreatePackage", 0x026cfd50, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B EA 4C 8B F1 BF 01 00 00 00")
FUNC_OPT("CContentManager::AttachPackage", 0x026d4620, "40 53 56 48 83 EC 28 83 7A 44 01 48 8B F2")
FUNC_OPT("CContentManager::LoadModioMods", 0x026d6740, "48 89 54 24 10 48 89 4C 24 08 55 56 41 56 48 8D AC 24 C0 FE FF FF")
FUNC("CNamesPool::Get", 0x0031d930, "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 ?? E8 ?? ?? ?? ?? 48 8D 48 08 FF 15 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 ?? 38 05 ?? ?? ?? ?? 74 ?? 88 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 48 08 FF 15 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 83 C4 28 C3 CC CC CC CC CC CC 40 53")
FUNC("CNamesPool::AddEntry", 0x026b0e00, "48 89 5C 24 20 55 41 56 41 57 48 83 EC 20 4C 8B F2")
FUNC_OPT("CDLCManager::Scan", 0x020c7540, "44 88 44 24 18 89 54 24 10 48 89 4C 24 08")
FUNC_OPT("CDLCManager::OnGameStarting", 0x020c84c0, "48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8D B9 C0 00 00 00")
FUNC_OPT("CModHandlerSystem::LoadModsIntoGameSystems", 0x02464d20, "48 8B C4 48 89 48 08 55 53 41 55 48 8D 68 A1")
FUNC("RegisterCoreScriptNatives", 0x0273cf20, "48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 30 E8 ?? ?? ?? ??")
FUNC("CFunction::CFunction", 0x026f5810, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 F6 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B F9")
FUNC("SRTTI::Get", 0x0031fcf0, "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 48 08 FF 15 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 ?? ?? ?? ?? 38 05 ?? ?? ?? ?? 74 ?? 88 05 ?? ?? ?? ?? 33 C0 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ??")
FUNC("CRTTISystem::RegisterGlobalFunction", 0x026c5f30, "48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 44 8B 49 44")
FUNC_OPT("Memory::Alloc", 0x0031e220, "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 56 41 57 48 83 EC 50 48 89 48 C8")
FUNC("funcLogChannel", 0x0273cd80, "48 89 5C 24 08 57 48 83 EC 30 8B 05 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 89 44 24 48")
FUNC_OPT("CGameSessionManager::RestoreSession", 0x0236a020, "44 88 4C 24 20 44 88 44 24 18 55 53 56 57 41 55")
FUNC_OPT("funcLoadResource", 0x0273c5b0, "48 8B C4 4C 89 40 18 55 48 8D 68 A1 48 81 EC B0 00 00 00")
FUNC_OPT("funcLoadResourceAsync", 0x0273c0c0, "48 8B C4 55 48 8D 68 A1 48 81 EC 90 00 00 00 48 89 58 08 33 C9 48 89 70 F0")
FUNC_OPT("CompareFunctionSignature", 0x02778fc0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 4C 89 44 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 40")
FUNC_OPT("CheckFunctionRedeclaration", 0x02779d80, "40 56 57 41 54 41 55 48 83 EC 28 80 BA F4 00 00 00 00")
FUNC_OPT("CheckWrapMethodCompat", 0x0277d750, "48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC 88 00 00 00 33 F6 48 8B F9")
FUNC_OPT("CClass::CClass", 0x026bba40, "48 89 5C 24 08 57 48 83 EC 20 33 FF C7 41 08 FF FF FF FF 48 89 79 10")
FUNC_OPT("CClass::AddProperty", 0x026bacb0, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 BA 01 00 00 00")
FUNC_OPT("CClass::GetProperties", 0x026baa80, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8B F2 48 8B 49 10")
FUNC_OPT("CFunction::GetParameters", 0x026f4c50, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 59 30")
FUNC_OPT("IScriptable::GetClass", 0x0031fa70, "48 8B 41 18 C3 CC CC CC CC CC CC CC CC CC CC CC 8B 41 50")
FUNC("CRTTISystem::FindType", 0x026c4bc0, "48 89 4C 24 08 55 53 41 54 48 8D AC 24 50 FD FF FF")
FUNC_OPT("CClass::GetKind", 0x0031f6e0, "B8 02 00 00 00 C3 CC CC CC CC CC CC CC CC CC CC 8B 41 6C")
FUNC("RegisterNative_CPPForceActorLOD", 0x02065190, "40 53 48 83 EC 30 BA F8 00 00 00 48 8D 4C 24 20 41 B8 10 00 00 00 E8 ?? ?? ?? ?? 48 8B 5C 24 20 33 D2 4C 8B 44 24 28 48 8B CB E8 ?? ?? ?? ?? 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 89 44 24 40 48 8D 54 24 40 48 8B CB E8 ?? ?? ?? ?? 48 8B D8 E8 ?? ?? ?? ?? 48 8B D3 48 8B C8 48 83 C4 30 5B E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC 48 83 EC 68")
FUNC("ResolveScriptGlobalByName", 0x00bbad70, "44 89 4C 24 20 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 48 8D 6C 24 E1")
FUNC_OPT("CScriptedClass::ForEachPropertyPair", 0x026b5dd0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 DB 49 8B F8")
FUNC("CFunction::CFunctionMethod", 0x026f54c0, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 F6 48 89 51 08 48 8D 05 ?? ?? ?? ?? 48 8B F9")
FUNC_OPT("funcGetComponent", 0x02252900, "48 83 EC 38 48 89 5C 24 40 41 B9 0E 00 00 00 48 89 74 24 48")
FUNC_OPT("funcGetGuidHash", 0x0224d350, "48 FF 42 30 4D 85 C0 74 ?? 8B 81 D4 00 00 00 33 81 D0 00 00 00")
FUNC_OPT("funcGetWorldPosition", 0x020f86c0, "48 FF 42 30 48 8B 41 60 48 85 C0 74 ??")
FUNC_OPT("funcTeleportWithRotation", 0x02252c50, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B 42 30 48 8D 35 ?? ?? ?? ?? C5 F8 10 05 ?? ?? ?? ?? C5 F9 7F 44 24 30 48 8B DA 48 8B F9 44 0F B6 00 48 FF C0 48 89 42 30 4E 8B 0C C6 4D 85 C9 74 ?? 48 8B 0A 4C 8D 44 24 30 41 FF D1 48 8B 43 30 C5 F8 57 C0 C5 FA 11 44 24 20 C5 FA 11 44 24 24 C5 FA 11 44 24 28 0F B6 08 48 FF C0 48 89 43 30 4C 8B 0C CE 4D 85 C9 74 ?? 48 8B 0B 4C 8D 44 24 20 48 8B D3 41 FF D1 48 FF 43 30 4C 8D 44 24 20")
FUNC_OPT("funcSetRawPlacement", 0x0224d0e0, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B 42 30 48 8D 35 ?? ?? ?? ?? C5 F9 EF C0")
FUNC_OPT("CScriptingSystem::ReloadScripts", 0x02706220, "48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 B9")
FUNC_OPT("CScriptingSystem::CompileAllScripts", 0x02704d90, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70 49 8B F1")
FUNC_OPT("CScriptingSystem::ApplyCompiledScripts", 0x02704c10, "48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 70 80 B9 75 01 00 00 00")
// fills two lists with the script files to compile (empty 12-byte arrays in, rcx unused); the start-up compile
// and the REDkit editor's "Reload scripts" both gather with it first (hotreload.cpp)
FUNC_OPT("GatherScriptFiles", 0x026d8780, "4C 8B DC 4D 89 43 18 49 89 53 10 49 89 4B 08 55 56 41 56 49 8D 6B A1 48 81 EC D0 00 00 00")
// the whole start-up script pass: compile, prepare (only while +0x175 is 0, sets it), apply, finish; the REDkit
// editor's reload runs the same chain. (rcx unused, param2, files, a4, a5 byte) -> ok. Live reload calls it again.
FUNC_OPT("CScriptingSystem::CompileAndApplyAll", 0x02705e60, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 44 88 4C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C")
FUNC_OPT("CScriptingSystem::RelinkRTTI", 0x02704eb0, "40 53 48 83 EC 30 80 B9 75 01 00 00 00")
FUNC_OPT("CDynamicLayer::CreateAndAttachEntity", 0x0237fd60, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 84 C0")
FUNC_OPT("CDynamicLayer::InstantiateEntity", 0x0237e2c0, "48 89 5C 24 18 57 48 83 EC 40 48 8B 41 28")
FUNC_OPT("CDynamicLayer::UnlinkEntity", 0x026c7710, "40 53 48 83 EC 20 48 8B D9 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 4B 30")
FUNC_OPT("CDynamicLayer::RegisterEntityTag", 0x0237dee0, "48 89 5C 24 10 48 89 74 24 18 55 57 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 33 FF")
FUNC_OPT("CDynamicLayer::FinishEntityAttach", 0x0237e570, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 02 48 8B FA 48 8B F1")
FUNC_OPT("CDynamicLayer::NotifyEntityAttachListeners", 0x0237f240, "48 89 5C 24 08 57 48 83 EC 20 48 8B 59 30 48 8B FA")
FUNC_OPT("CEntity::OnLayerAttachInit", 0x0225af10, "40 53 48 83 EC 20 48 8B D9 4C 8D 4A 70")
FUNC_OPT("CEntity::SyncAttachmentPlacement", 0x02260730, "40 55 53 57 48 8D 6C 24 B9 48 81 EC 00 01 00 00")
FUNC_OPT("CScriptCompiler::ResolveLiteralKind", 0x027d0c90, "48 89 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 28 8B 05 ?? ?? ?? ??")
FUNC_OPT("CScriptCompiler::CreateLiteralNode", 0x027b61b0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00 8B 41 0C")
FUNC_OPT("CRemoteConnection::ExecCommand", 0x024d4de0, "48 8B C4 55 41 56 48 8B EC 48 83 EC 78 48 8B 0D ?? ?? ?? ??")
FUNC_OPT("CScriptCompiler::CompileCall", 0x027b6ed0, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 B8 40 10 00 00")
FUNC("CScriptCompiler::DeclareGlobalFunction", 0x0277c790, "40 53 41 54 41 57 48 83 EC 40 45 0F B6 E0")
FUNC_OPT("CEntity::AttachToWorld", 0x02255100, "40 53 57 48 83 EC 28 48 83 3D ?? ?? ?? ?? 00")
FUNC_OPT("CLayer::AttachToWorld", 0x0237ec00, "48 8B C4 48 89 50 10 41 54 41 55 41 56")
FUNC_OPT("Renderer::UmbraOcclusionUser", 0x01ca04a0, "40 57 48 83 EC 30 80 3D ?? ?? ?? ?? 00 48 8B F9 0F 84 ?? ?? ?? ??")
FUNC_OPT("funcFoliageSendCreateSpeedTreeDynamicInstances", 0x02545590, "48 8B C4 53 48 83 EC 60 48 89 68 08 49 8B E9")
FUNC_OPT("funcFoliageCopyInstancePlacements", 0x00893ff0, "48 89 5C 24 08 57 48 83 EC 20 33 C0 48 8B DA 89 41 08 48 8B F9 48 89 01 41 B9 7F 00 00 00")
FUNC_OPT("funcFoliageSendRemoveSpeedTreeDynamicInstancesRadius", 0x025451f0, "48 89 5C 24 18 48 89 6C 24 20 56 48 83 EC 50 48 8B 9A 90 00 00 00 49 8B E8 48 8B F1 C5 F8 29 74 24 40 C5 F8 28 F3 48 85 DB 0F 84 ?? ?? ?? ?? 48 89 7C 24 60 F0 FF 43 08 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 80 00 00 00 48 85 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 80 00 00 00 BA 28 00 00 00 48 8D 48 40 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 75 ?? E9 ?? ?? ?? ??")
FUNC_OPT("funcFoliageSendRemoveSpeedTreeInstancesRadius", 0x02545340, "48 89 5C 24 18 48 89 6C 24 20 56 48 83 EC 50 48 8B 9A 90 00 00 00 49 8B E8 48 8B F1 C5 F8 29 74 24 40 C5 F8 28 F3 48 85 DB 0F 84 ?? ?? ?? ?? 48 89 7C 24 60 F0 FF 43 08 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 80 00 00 00 48 85 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 80 00 00 00 BA 28 00 00 00 48 8D 48 40 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 75 ?? EB ??")
FUNC_OPT("funcFoliageSendRemoveSpeedTreeInstancesRect", 0x02545440, "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48 8B 9A 90 00 00 00")
FUNC_OPT("funcFoliageResolveResourceReference", 0x02347270, "48 89 5C 24 08 57 48 83 EC 20 48 83 B9 90 00 00 00 00")
FUNC_OPT("funcFoliageSendCreateSpeedTreeInstances", 0x025456f0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B F1")
FUNC_OPT("funcFoliageSendUpdateSpeedTreeInstances", 0x025457e0, "48 89 5C 24 18 56 48 83 EC 40 83 7A 08 00")
FUNC_OPT("funcFoliageSendUpdateGenericGrassMask", 0x025450f0, "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 89 5C 24 30 48 8B DA")
FUNC_OPT("funcFoliageSendUploadGenericGrassOccurrenceMap", 0x02545030, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 48 8B F2 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 80 00 00 00 48 85 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 80 00 00 00 BA 20 00 00 00 48 8D 48 40 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 75 ?? EB ?? 48 8D 1D ?? ?? ?? ?? 33 D2 48 8B CB 41 B8 00 08 00 00 E8 ?? ?? ?? ?? 48 8B 47 08")
FUNC_OPT("funcFoliageSendRefreshGenericGrass", 0x02544f60, "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 85 C9")
FUNC_OPT("CCollisionCacheResolver::RegisterContentPacks", 0x024d5a90, "40 53 55 41 55 41 56 41 57 48 83 EC 30 48 8B 05 ?? ?? ?? ??")
FUNC_OPT("CCollisionCacheResolver::CCollisionCacheResolver", 0x024d5e90, "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 33 DB BA 00 02 00 00")
FUNC_OPT("CTerrainTile::UpdateClipmapIfNeeded", 0x023869f0, "48 89 54 24 10 55 56 41 56 48 8D 6C 24 B9 48 81 EC C0 00 00 00 80 79 78 00")
FUNC_OPT("CGame::StartGame", 0x023730b0, "48 8B C4 48 89 50 10 48 89 48 08 55 53 41 56")
FUNC_OPT("CGameSessionManager::StartNewGameSession", 0x0236b830, "48 89 5C 24 08 57 48 83 EC 20 41 B1 01")
FUNC_OPT("CGameSessionManager::InitNewSessionGuid", 0x02369250, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 33 F6 C7 44 24 20 01 00 00 00")
FUNC_OPT("CGameSessionManager::RestoreFromSessionData", 0x01f791a0, "48 89 5C 24 20 57 48 81 EC 80 00 00 00 48 8B F9")
FUNC_OPT("CGameSessionManager::GetSessionDataForRestore", 0x02372590, "48 89 54 24 10 48 89 4C 24 08 53 57 41 57")
FUNC_OPT("CQuestThread::CQuestThread", 0x02072160, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? C6 83 80 00 00 00 01")
FUNC_OPT("CQuestThread::~CQuestThread", 0x02072010, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 10 48 81 C1 C4 00 00 00")
FUNC_OPT("CQuestGraph::CQuestGraph", 0x020707e0, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 33 F6 48 8D 05 ?? ?? ?? ?? 48 89 41 10 48 8D 05 ?? ?? ?? ?? 48 89 41 58")
FUNC_OPT("funcNotifyListenerList", 0x026ba820, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 0F B6 44 24 60")
FUNC_OPT("CQuestsSystem::CQuestsSystem", 0x01ff1200, "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 FF 66 C7 43 68 00 00")
FUNC_OPT("CQuestThread::ActivateBlock", 0x02072270, "40 53 56 41 57 48 83 EC 20 48 8B 41 60")
FUNC_OPT("CQuestThread::TickActiveBlocks", 0x020714d0, "40 53 55 48 83 EC 38 48 8B 69 60 48 8B D9")
FUNC_OPT("CQuestThread::ActivateInput", 0x02072920, "40 53 57 41 56 41 57 48 81 EC 58 01 00 00")
FUNC_OPT("CQuestThread::VisitActiveThreadsAndBlocks", 0x02072790, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 4C 8B F2 48 89 54 24 40")
FUNC_OPT("CGraphBlock::CGraphBlock", 0x023c27f0, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 05 ?? ?? ?? ?? 48 89 43 10 33 C0 48 89 43 58 89 43 60 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC CC CC 40 53 48 83 EC 20 48 8B 01 48 8B DA")
FUNC_OPT("funcBuildSaveGamesFolderPath", 0x01e74ed0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 60 4C 8B F2")
FUNC_OPT("CScriptThread::CScriptThread", 0x02721bb0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F0 8B EA")
FUNC_OPT("CScriptThread::~CScriptThread", 0x02721a60, "48 89 5C 24 18 55 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 89 74 24 30 48 89 01 48 8B D9 E8 ?? ?? ?? ??")
FUNC_OPT("StartScriptThread", 0x02706910, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 49 8B E9")
FUNC_OPT("InvokeFunctionOrStartLatentThread", 0x0217af80, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 40 4C 8B EA")
FUNC_OPT("InvokeFunctionChecked", 0x0217ad60, "40 56 57 48 83 EC 38 4C 8B 42 28 48 8B F2")
FUNC_OPT("TickScriptThreads", 0x02706760, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 48 8B 3D ?? ?? ?? ?? C5 F8 29 74 24 20")
FUNC_OPT("CScriptThread::Step", 0x02721d40, "40 53 56 57 41 56 41 57 48 83 EC 50 C5 F2 58 41 50")
FUNC_OPT("CObjectGC_Legacy::CObjectGC_Legacy", 0x02786ca0, "48 89 5C 24 08 57 48 83 EC 30 33 FF 48 8D 05 ?? ?? ?? ??")
FUNC_OPT("CObjectGC_Legacy::~CObjectGC_Legacy", 0x02786be0, "48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 48 8B 49 38")
FUNC_OPT("CObjectsGC_MarkAndSweep::CObjectsGC_MarkAndSweep", 0x027870f0, "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 83 C1 08 E8 ?? ?? ?? ?? BA 00 80 00 00")
FUNC_OPT("CObjectsGC_MarkAndSweep::MarkReachable", 0x02786e50, "48 89 4C 24 08 53 56 41 54 41 57 48 83 EC 48")
FUNC_OPT("DepotResolveAndLoad", 0x026e88d0, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B FA 49 8B E9")
FUNC_OPT("DepotCacheLookup", 0x026e8ba0, "40 53 48 83 EC 30 48 8B D9 48 8B C2")
FUNC_OPT("CDirectory::FindFile", 0x0270a000, "48 89 6C 24 18 56 57 41 55 41 56 41 57 48 83 EC 30 0F B6 AC 24 80 00 00 00")
FUNC_OPT("funcGetWeatherNames", 0x02392d10, "48 83 EC 48 48 8B 42 30 4C 8D 0D ?? ?? ?? ?? 48 89 5C 24 50 33 DB")
FUNC_OPT("TDynArray::GrowBy", 0x0035bf50, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 79 08 41 0F B7 F0 48 8B D9 8D 04 17 48 8B 11 89 41 08 48 85 D2 74 ?? 48 8B 0D ?? ?? ?? ?? 48 83 E1 F8 E8 ?? ?? ?? ?? EB ?? 33 C0 8B 4B 08 C1 E8 02")
FUNC_OPT("TDynArray::AllocatedCapacity", 0x027e74e0, "4C 8B 81 00 C9 00 00 48 8B C2 4C 8B 89 08 C9 00 00 49 2B C0 4D 2B C8 4C 8B D2")
FUNC_OPT("Dispatch", 0x026f44d0, "4C 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 90 01 00 00")
// the engine's own calls into a script function (CallFunction/CallEvent, inlined at 367 places): (CFunction*,
// context, parameter buffer laid out like the locals, result) -> ok; runs the bytecode, ignores the native bit
FUNC_OPT("ScriptCallDirect", 0x026f5580, "40 55 41 54 41 55 41 56 41 57 48 81 EC 70 01 00 00 48 8D 6C 24 20")
FUNC_OPT("Combine_Format1", 0x02364be0, "48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 41 57 48 81 EC B0 00 00 00")
FUNC_OPT("Combine_Format2", 0x02365450, "4C 8B DC 49 89 73 18 41 56 48 81 EC A0 00 00 00")
FUNC_OPT("ModelSpaceFillDispatch", 0x0246e860, "48 89 5C 24 08 57 48 83 EC 20 4C 8B 81 08 0F 00 00")
FUNC_OPT("Mimic_TrackSampleCall", 0x024c9328, "48 8B 8F 38 01 00 00 48 85 C9 74 17 48 8B 01 4C 8B CE 4C 8B 84 24 70 02 00 00 49 8B D6 FF 90 B0 01 00 00")
CALL("Memory::Free", "funcLogChannel", 0xc1, "E8 ?? ?? ?? ?? 48 8B 5C 24 40")
// the free of the old buffer when the render size changes (inside +0x2456200; quarantine.cpp, F28): the call of
// Memory::Free whose return address TW3SE recognises
FUNC_OPT("RenderResize::FreeOld", 0x02457177, "49 8B CF E8 ?? ?? ?? ?? 4C 8B BC 24 C8 00 00 00 48 81 C4 00 01 00 00")
// its call from the render-size change (+0x232d968): only frees under this call count
FUNC_OPT("RenderResize::Call", 0x0232dab4, "48 8D 4E 78 E8 ?? ?? ?? ?? 48 8D 4E 78")
// a node's attachment list (16-byte cells {value, next}): its remove and its append (attachfix.cpp, 20.11.: a
// worker thread and the main thread changed one at the same time)
FUNC_OPT("AttachList::Remove", 0x00871760, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 19 48 8B F1 48 85 DB 74 17 48 8B 02 0F 1F 00 48 8B 7B 08 48 39 03 74 1A 48 8B DF 48 85 FF 75 EF 32 C0 48 8B 5C 24 30")
FUNC_OPT("AttachList::Append", 0x004be270, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B DA 48 8B F1 BA 10 00 00 00 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 7C 24 20 33 D2 4C 8B 44 24 28")
CALL("Memory::AllocDefault", "RegisterNative_CPPForceActorLOD", 0x16, "E8 ?? ?? ?? ?? 48 8B 5C 24 20")
GLOBAL("GContentManager", "CContentManager::LoadModioMods", 0x24, 3, 7, "4C 8B 35 ?? ?? ?? ?? 4C 89 75 60")
GLOBAL("GGame", "CModHandlerSystem::LoadModsIntoGameSystems", 0x119, 3, 7, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 80 B8 10 01 00 00 00")
GLOBAL("GScriptNativeTable", "CFunction::CFunction", 0x65, 3, 7, "48 8D 15 ?? ?? ?? ?? 48 89 B7 F0 00 00 00")
GLOBAL("GScriptGlobals", "ResolveScriptGlobalByName", 0x66, 3, 7, "48 8B 05 ?? ?? ?? ?? 48 8B 58 20")
GLOBAL("GScriptMethodTable", "CFunction::CFunctionMethod", 0x66, 3, 7, "4C 8D 05 ?? ?? ?? ?? C5 F8 10 03")
GLOBAL("GScriptLogSink", "funcLogChannel", 0xb1, 2, 6, "FF 15 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 5C 24 40")
GLOBAL("GUseUmbraOcclusion", "Renderer::UmbraOcclusionUser", 0x6, 2, 7, "80 3D ?? ?? ?? ?? 00 48 8B F9")
GLOBAL("GCollisionCacheResolver", "CCollisionCacheResolver::CCollisionCacheResolver", 0x14, 3, 7, "48 89 05 ?? ?? ?? ?? 41 B9 02 00 00 00")
GLOBAL("GDepot", "funcLoadResource", 0x122, 3, 7, "48 8B 0D ?? ?? ?? ?? 4C 8D 4D F7")
// CDiskFile's vtable slot 5 (this, priority, out IAsyncFile*): a CAsyncFile_Fallback (+0x20 the file) whose reader
// comes from the file's slot 3; CBundleDiskFile replaces the slot with the bundle's async reader
FUNC_OPT("CDiskFile::CreateAsyncFile", 0x026eac80, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 49 8B F8 48 8B F1 41 B8 10 00 00 00 48 8D 4C 24 20 BA 28 00 00 00")
// CDiskFile(this, directory, &AnsiString name {data, u32 size with the 0}): copies the name, sets the base vtable,
// enters nothing anywhere; CBundleDiskFile's clone (slot 11) and CDirectory::CreateNewFile (slot 7) call it
FUNC_OPT("CDiskFile::CDiskFile", 0x026ed340, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 89 51 08 48 8D 05")
// a render command's memory of 0x28 bytes in the renderer's command ring (one of the per-size helpers the engine
// has; without a renderer a static scratch block). The command is built there and committed with its slot 1
FUNC_OPT("RenderCommandAlloc28", 0x004922f0, "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 80 00 00 00 48 85 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 80 00 00 00 BA 28 00 00 00")
// CDrawableComponent::RefreshRenderProxies(this): only while attached (+0x54 & 0x100000): out of the render scene,
// proxies destroyed and made anew (slots 0x3b0, 0x3a0), back in; the mesh setter ends with it
FUNC_OPT("CDrawableComponent::RefreshRenderProxies", 0x0234c2e0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 FF 48 8D 71 54 F7 41 54 00 00 10 00")
// CMemoryFileReader(this, data, size): two vtables, flags +0x08, data +0x18, size +0x20, offset +0x28; 0x30 bytes
FUNC_OPT("CMemoryFileReader::CMemoryFileReader", 0x026f6400, "48 8D 05 ?? ?? ?? ?? C7 41 08 52 00 10 00 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 10 48 8B C1 C7 41 0C A4 00 00 00")
// the localization manager's text for a string id: (manager, &out String, id, flag); funcGetLocStringById calls it,
// 18 callers incl. the story-scene code
// the localization manager's text for a string key: (manager, &out String, &key String). The script natives
// GetLocStringByKey and GetLocStringByKeyExt call it, and Flash's [[key]] texts (CScaleformTranslator::Translate)
FUNC_OPT("CLocalizationManager::GetStringByStringKey", 0x02352d90, "40 53 57 41 56 41 57 48 83 EC 48 48 83 79 08 00 4D 8B F8 4C 8B F2 48 8B F9 75 ?? B2 01 E8")
FUNC_OPT("CLocalizationManager::GetLocalizedText", 0x0235bf70, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 33 ED 41 8B D8 48 8B FA 48 8B F1 45 85 C0")
// every CObject of the engine: a pointer to {CObject** data, u32 count, ..., lock at +0x20}; freed slots are null
// (read in the GC's mark pass; 402668 live of 407539 in a Velen save)
GLOBAL("GObjectsMap", "CObjectsGC_MarkAndSweep::MarkReachable", 0x2e, 3, 7, "48 8B 35 ?? ?? ?? ?? 4C 8D B5 78 00 01 00")
FIELD("CCommonGame.dlcManager", "CCommonGame::OnGameStart", 0xf, 0x15eb8, "48 8B 89 ?? ?? ?? ?? 33 D2", 3)
FIELD("CDLCManager.isNewContentAvailable", "CDLCManager::Scan", 0x48, 0xe8, "44 88 AB ?? ?? ?? ?? 45 33 C0", 3)
FIELD("CNamesPool.entries", "CNamesPool::AddEntry", 0x108, 0x11830, "48 8B 95 ?? ?? ?? ?? 48 8B 7C 24 48", 3)
FIELD("CNamesPool.entryCount", "CNamesPool::AddEntry", 0x101, 0x11838, "8B 9D ?? ?? ?? ?? 48 8B 95 30 18 01 00", 2)
FIELD("CQuestsSystem.activeQuests", "CQuestsSystem::CQuestsSystem", 0xda, 0xec, "48 89 BB ?? ?? ?? ?? 89 BB F4 00 00 00", 3)
FIELD("CQuestsSystem.activeQuestsCount", "CQuestsSystem::CQuestsSystem", 0xe0, 0xf4, "89 BB ?? ?? ?? ?? 48 89 BB F8 00 00 00", 2)
FIELD("CQuestThread.listeners", "CQuestThread::CQuestThread", 0x4c, 0x84, "48 89 83 ?? ?? ?? ?? 89 83 8C 00 00 00", 3)
FIELD("CQuestThread.listenersCount", "CQuestThread::CQuestThread", 0x52, 0x8c, "89 83 ?? ?? ?? ?? 48 89 83 90 00 00 00", 2)
FIELD("CQuestThread.childThreads", "CQuestThread::CQuestThread", 0x59, 0x90, "48 89 83 ?? ?? ?? ?? 89 83 98 00 00 00", 3)
FIELD("CQuestThread.childThreadsCount", "CQuestThread::CQuestThread", 0x5f, 0x98, "89 83 ?? ?? ?? ?? 48 89 83 9C 00 00 00", 2)
FIELD("CQuestGraphBlock.flagsBufferOffset", "CQuestThread::ActivateBlock", 0x2a, 0xe0, "8B 82 ?? ?? ?? ?? 48 8B 4F 28", 2)
FIELD("CQuestGraphBlock.localDataOffset", "CQuestThread::ActivateBlock", 0x52, 0xb0, "8B 8A ?? ?? ?? ?? 49 8B D0", 2)
FIELD("CQuestGraphBlock.stateOffset", "CQuestThread::ActivateBlock", 0x79, 0x100, "8B 8E ?? ?? ?? ?? BA 04 00 00 00", 2)
FIELD("CFunction.code", "Dispatch", 0x201, 0x88, "48 8B 8E ?? ?? ?? ?? 8B BE 90 00 00 00", 3)
FIELD("CFunction.codeSize", "Dispatch", 0x207, 0x90, "8B BE ?? ?? ?? ?? 48 03 F9", 2)
// Dynamic decals (docs/FINDINGS_DECAL.md). The decal's init parameters (0x100 bytes) with the engine's
// defaults: (this) -> this; a leaf without its own .pdata entry
FUNC_OPT("SDynamicDecalInitParameters::Init", 0x0234b120, "33 D2 48 89 11 48 89 51 08 8B 05 ?? ?? ?? ??")
// the engine's helper that puts one dynamic decal on the world (footsteps): init parameters, the renderer makes
// the decal, AddDynamicDecalToScene. Read for the renderer, its decal slot, the world's render scene, the texture slot
FUNC_OPT("SpawnWorldDynamicDecal", 0x024d3290, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D 6C 24 E0 48 81 EC 20 01 00 00 48 8B 9A 40 54 00 00")
// the dismemberment wound's decal: made the same way, sent with AddDynamicDecalToSceneForProxies to the render
// proxies of the entity's mesh components. Read for the component's render proxy field
FUNC_OPT("SpawnWoundDynamicDecal", 0x02620740, "4C 8B DC 49 89 4B 08 55 57 41 55 49 8D AB D8 FE FF FF")
GLOBAL("GRender", "SpawnWorldDynamicDecal", 0x86, 3, 7, "48 8B 0D ?? ?? ?? ?? C5 FA 11 45 84")
FIELD("CWorld.renderScene", "SpawnWorldDynamicDecal", 0x1f, 0x5440, "48 8B 9A ?? ?? ?? ?? 48 8D 4C 24 20", 3)
FIELD("CBitmapTexture.GetRenderResourceSlot", "SpawnWorldDynamicDecal", 0x41, 0x1c0, "FF 90 ?? ?? ?? ?? 48 89 44 24 20", 2)
FIELD("IRender.CreateDynamicDecalSlot", "SpawnWorldDynamicDecal", 0x113, 0x1e8, "FF 90 ?? ?? ?? ?? 48 8B F8", 2)
FIELD("CDrawableComponent.renderProxy", "SpawnWoundDynamicDecal", 0x159, 0x158, "48 8B B8 ?? ?? ?? ?? 48 85 FF", 3)
// a mesh's dynamic decal target takes a new decal only below this many (cmp eax, imm8 - the engine has 2; the
// second one only with priority or a long lifetime). Its third byte is the limit; w3.decal.per_mesh writes it
FUNC_OPT("DecalTarget::LimitCheck", 0x01d471ea, "83 F8 ?? 0F 83 ?? ?? ?? ?? 83 F8 01 72 ??")
// Scenes (docs/engine/scenes.md). Found by log and assert strings in witcher3Release.exe (storySceneSystem.cpp,
// actor_scenes.cpp, storyScenePlayer.cpp), recognised by structure in 5.01b; signatures unique in 5.01b and 5.0.
// (controller, u32 reason): ends a running scene - despawns the actors it spawned, stops and flags its player
FUNC_OPT("CStorySceneController::Stop", 0x020af7e0, "40 57 41 56 48 83 EC 38 80 79 08 00")
// (player): stops the player and its controller with the forced reason; the scene system frees it on its next update
FUNC_OPT("CStoryScenePlayer::Stop", 0x01f2d620, "40 57 48 83 EC 40 80 B9 9B 04 00 00 00")
// (player, world): read for the player's controller field
FUNC_OPT("CStoryScenePlayer::OnDetached", 0x01f30f70, "48 89 5C 24 08 57 48 83 EC 20 80 B9 9B 04 00 00 00")
// Native methods (docs/engine/scripting.md). (class, CFunction*): appends to the class's function list; seen in
// every engine method registration right after CFunction::CFunctionMethod. Unique in 5.01b and 5.0.
FUNC_OPT("CClass::AddFunction", 0x026bad30, "48 89 5C 24 08 57 48 83 EC 20 48 8D 59 48 41 B8 18 00 00 00")
// (class, const CName*) -> CFunction*: the compiler's lookup for an import inside a class, own list then parents
FUNC_OPT("CClass::FindFunction", 0x026b9e60, "48 8B 41 48 4C 8B D2 44 8B 41 50 4E 8D 0C C0")
// (compiler, declaration, class, u8 canRedefine) -> bool: the class-function twin of DeclareGlobalFunction
// (log strings in both builds: "Cannot find function '%s::%s' to import.")
FUNC_OPT("CScriptCompiler::DeclareClassFunction", 0x0277b8c0, "44 88 4C 24 20 53 56 41 54 41 57 48 83 EC 78")
// Enums (docs/engine/scripting.md). From a native enum's static registration and the compiler's enum
// definition ("Cannot define empty enum"). (rtti, type): enters any IRTTIType under its name; an existing name stays
FUNC_OPT("CRTTISystem::RegisterType", 0x026c6300, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 48 8B 02 48 8B E9")
// (this, const CName*, u32 size in bytes, u8 scripted) -> this; object of 0x80 bytes
FUNC_OPT("CEnum::CEnum", 0x026f8bb0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 48 8D 05 ?? ?? ?? ?? 48 89 01 41 0F B6 F9")
// (this, const CName* option, i32 value)
FUNC_OPT("CEnum::Add", 0x026f8c60, "48 89 5C 24 08 48 89 74 24 10 44 89 44 24 18 57 48 83 EC 20")
// (rtti, const CName*, u32 size) -> CEnum*: the compiler's way for script enums (scripted flag set); reference only
FUNC_OPT("CRTTISystem::CreateScriptedEnum", 0x026c1a00, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 33 ED 45 8B F8")
// Animation sets (docs/engine/animation.md). Found from the cutscene actor code (cutsceneActor.cpp strings) and
// skeletalAnimationContainer.cpp asserts in witcher3Release.exe; recognised by structure in 5.01b.
// (component): rebuilds the container's set list from the entity template and the component's animationSets
FUNC_OPT("CAnimatedComponent::RebuildAnimationSets", 0x0242c9f0, "4C 8B DC 41 54 41 57 48 83 EC 58 48 8B 41 30")
// (container, const TDynArray<handle>*): replaces the container's set list
FUNC_OPT("CSkeletalAnimationContainer::SetAnimationSets", 0x0241c7d0, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 71 58")
// (container, CSkeletalAnimationSet*): removes one set from the list (the cutscene end does this)
FUNC_OPT("CSkeletalAnimationContainer::RemoveAnimationSet", 0x0241c880, "40 53 48 83 EC 20 48 C7 44 24 30 00 00 00 00")
// (TDynArray of 8-byte items, u32 count) -> old count: grows the array in memory class 2; the cutscene code uses it
// to push a set handle onto a container's list
FUNC_OPT("TDynArray::GrowBy8", 0x00331760, "48 89 5C 24 08 57 48 83 EC 20 8B 79 08 48 8B D9 8D 04 17 48 8B 11 89 41 08 48 85 D2 74 ?? 48 8B 0D ?? ?? ?? ?? 48 83 E1 F8 E8 ?? ?? ?? ?? EB ?? 33 C0 8B 4B 08 C1 E8 03 3B C8 76 ?? 8B D1 41 B9 02 00 00 00 D1 EA 41 B8 08 00 00 00")
FIELD("CStoryScenePlayer.controller", "CStoryScenePlayer::OnDetached", 0x1c, 0x650, "48 8B 89 ?? ?? ?? ?? BA 0E 00 00 00", 3)
FIELD("CStoryScenePlayer.isStopped", "CStoryScenePlayer::Stop", 0x8, 0x49b, "80 B9 ?? ?? ?? ?? 00 48 8B F9", 2)
FIELD("CAnimatedComponent.animationSets", "CAnimatedComponent::RebuildAnimationSets", 0x2c1, 0xde8, "49 8D 94 24 ?? ?? ?? ??", 4)
FIELD("CAnimatedComponent.animationContainer", "CAnimatedComponent::RebuildAnimationSets", 0x2b9, 0xe80, "49 8B 8C 24 ?? ?? ?? ?? 49 8D 94 24 E8 0D 00 00", 4)
// Latent functions, terrain, blur (docs/engine/scripting.md 17-18, terrain.md, render.md). Found by log and assert strings in
// witcher3Release.exe and editor.exe, recognised by structure in 5.01b; signatures unique in 5.01b and 5.0.
// Latent functions: (CFunction*, frame): frees a latent frame (locals, its buffer, the frame); every latent frame
// ends here, finished or killed. Called inside TickScriptThreads on the main thread
FUNC_OPT("CFunction::DestroyLatentFrame", 0x026f3a80, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 33 FF 48 8D 72 20")
// (thread): stops a script thread, moves its frames to the dead list, tells its listener (not finished)
FUNC_OPT("CScriptThread::Kill", 0x027222d0, "40 57 48 83 EC 20 80 79 48 00 48 8B F9")
// (thread): frees the frames on the dead list through CFunction::DestroyLatentFrame
FUNC_OPT("CScriptThread::FreeDeadFrames", 0x02721b30, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 48 8B F1 39 59 24")
// (CFunction*, callingFrame, buffer) -> bool: reads the call's arguments from the calling frame into the buffer
FUNC_OPT("CFunction::BindParameters", 0x026f3950, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 49 8B D8 4C 8B F1")
// (scripting system): kills every script thread; the script pass does it before new code is applied
FUNC_OPT("CScriptingSystem::KillAllThreads", 0x02701210, "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48 8B 99 8C 00 00 00")
// (context, compile result, u8 full) -> bool: the apply step of a script pass: types, compile, load, frame layouts
FUNC_OPT("CScriptingSystem::LoadCompiledStub", 0x02783670, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 41 0F B6 F0")
// Terrain. (clipmap, view info): the clipmap's per-frame update. Same function as CTerrainTile::UpdateClipmapIfNeeded,
// which is a CClipMap method
FUNC_OPT("CClipMap::Update", 0x023869f0, "48 89 54 24 10 55 56 41 56 48 8D 6C 24 B9 48 81 EC C0 00 00 00 80 79 78 00")
// (clipmap, view info, u8, bounds, out level updates): which parts of each clip level go to the renderer
FUNC_OPT("CClipMap::ComputeLevelUpdates", 0x02381bf0, "48 8B C4 4C 89 48 20 44 88 40 18 48 89 50 10 55 53")
// (tile, u32 mip) -> u16*: the tile's height map of that mip, loaded from disk first when it is not in memory
FUNC_OPT("CTerrainTile::GetLevelSyncHM", 0x0250f510, "40 53 48 83 EC 30 44 8B C2 41 B9 01 00 00 00")
// (tile, u32 mip) -> control map of that mip, same rules
FUNC_OPT("CTerrainTile::GetLevelSyncCM", 0x0250f570, "40 53 48 83 EC 30 44 8B C2 41 B9 02 00 00 00")
// (tile): takes the tile's collision out of the physics world and frees its collision data
FUNC_OPT("CTerrainTile::ReleaseCollision", 0x0250f330, "40 53 48 83 EC 20 48 8B D9 48 8B 89 A0 00 00 00 48 85 C9 74 ?? 48 8B D3")
// (tile, ?, world, tile box): creates the tile's collision from the collision cache; reference
FUNC_OPT("CTerrainTile::UpdateCollision", 0x0250ec00, "4C 89 4C 24 20 48 89 4C 24 08 55 41 54")
// (tile + 0x68, ...): builds the tile's cooked heightfields from its height map (8 x 8 pieces); reference
FUNC_OPT("CTerrainTile::CompileCollision", 0x0250cd70, "48 89 54 24 10 48 89 4C 24 08 55 56 57 41 54 48 8D AC 24 98 FE FF FF")
// (wrapper, tile): removes the tile's static actors from the scene and releases them
FUNC_OPT("CPhysicsTileWrapper::RemoveTerrainTile", 0x02515130, "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 6C 24 18 57 48 83 EC 20")
// (terrain proxy, u32 width, u32 height, const u32* pixels): the render-thread side of the overlay command
FUNC_OPT("CRenderProxy_Terrain::SetCustomOverlay", 0x01cb38c0, "4C 89 4C 24 20 89 54 24 10 55 53 56 57 41 54 41 56")
// Blur on scene change (postfx_loadingBlur.fx). (blur, ?, frame info, float, ...): draws the blurred picture
FUNC_OPT("CPostFxLoadingBlur::DrawFinal", 0x01d1d990, "48 8B C4 48 89 58 18 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 58 FE FF FF")
// (blur, ?, source target, ...): downsamples and blurs the source into the blur's own targets
FUNC_OPT("CPostFxLoadingBlur::Accumulate", 0x01d1d320, "4C 89 4C 24 20 48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 48 8D AC 24 C0 FE FF FF")
// (blur, u32 width, u32 height): checks the blur's targets against the source size divided by blur+0x34
FUNC_OPT("CPostFxLoadingBlur::EnsureTargets", 0x01d1c9e0, "48 89 4C 24 08 55 56 57 41 56 41 57 48 8D AC 24 60 FF FF FF")
// CTerrainTile -> its CPhysicsTileWrapper (null while the tile has no collision)
FIELD("CTerrainTile.collisionWrapper", "CTerrainTile::ReleaseCollision", 0xc, 0xa0, "48 8B 89 ?? ?? ?? ?? 48 85 C9", 3)
// CClipMap -> array of CTerrainTile*, index y * numTilesPerEdge + x
FIELD("CClipMap.terrainGrid", "CClipMap::ComputeLevelUpdates", 0x226, 0xb8, "48 8B 86 ?? ?? ?? ?? 4C 8B 04 C8", 3)
// Rest pose (docs/engine/animation.md "Rest pose and dangle skeletons"). Found by the assert texts of skeleton.cpp,
// animDangleConstraint.cpp, animSkeletalDangleConstraint.cpp and the class type names in witcher3Release.exe,
// recognised by structure and vtable position in 5.01b; signatures unique in 5.01b and 5.0.
// (constraint, A, unused): the skinning fill of every CAnimSkeletalDangleConstraint (Breast, Dress, Collar, Hood, ...):
// Combine_Format1 when constraint+0x68 is set, else Combine_Format2, with B = constraint + bonesMS
FUNC_OPT("CAnimSkeletalDangleConstraint::FillSkinningData", 0x02596980, "48 89 5C 24 08 57 48 83 EC 20 4C 8B 41 68 48 8D B9 AC 00 00 00")
// (constraint): sizes bonesMS and bonesWS to the rig's bone count (realloc, not cleared); fills bonesMS with the rest
// pose only when the constraint's own set-up did not succeed. Reference
FUNC_OPT("CAnimSkeletalDangleConstraint::OnAttached", 0x02595fa0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 30 48 8B 01 48 8B D9")
// (constraint, parentMS array, parentWS array, mapping array): copies the mapped bones from the parent's pose into
// bonesMS/bonesWS and empties the simulated list. Called by every constraint's update first
FUNC_OPT("CAnimSkeletalDangleConstraint::StartUpdate", 0x02595b40, "48 89 74 24 20 57 41 54 41 55 41 57 8B B9 B4 00 00 00")
// (constraint, parentMS array, parentWS array, mapping array): writes the simulated bones back into the parent's
// pose; the last step of a constraint's update (8 callers). After it bonesMS is final for the frame
FUNC_OPT("CAnimSkeletalDangleConstraint::EndUpdate", 0x02595d60, "48 85 C9 0F 84 ?? ?? ?? ?? 48 89 5C 24 18 48 89 7C 24 20 41 56 4C 8B 15 ?? ?? ?? ??")
// (constraint, ?, ?, parentMS array, parentWS array, mapping array): vtable +0x218 of the breast constraint. Reference
FUNC_OPT("CAnimDangleConstraint_Breast::Update", 0x02590470, "48 89 5C 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8B EC")
// (component+0x120, ...): the dangle component's per-frame call into its constraint (vtable +0x218), only while
// attached. Reference
FUNC_OPT("CAnimDangleComponent::UpdateConstraint", 0x0246a0c0, "40 53 48 8B 59 10 4D 8B D8 48 85 DB")
// (skeleton, TDynArray<Matrix>*): resizes the array to the bone count and fills it with the model-space rest pose
FUNC_OPT("CSkeleton::GetReferencePoseMSMatrices", 0x023620e0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 8B 59 70")
// (skeleton, Matrix* out, i32 bone) -> out: one bone's model-space rest matrix (identity when out of range)
FUNC_OPT("CSkeleton::GetBoneReferenceMS", 0x02362810, "48 81 EC 98 00 00 00 4C 8B CA 45 85 C0 78 ?? 44 3B 41 70 7D ?? 49 63 C0 4C 8D 04 40 48 8B 81 98 00 00 00")
// (skeleton, EngineQsTransform* out, i32 bone) -> out: one bone's rest transform local to its parent (48 bytes)
FUNC_OPT("CSkeleton::GetBoneReferenceLS", 0x02363bf0, "45 85 C0 78 ?? 44 3B 41 70 7D ?? C5 F8 57 C0 49 63 C0 4C 8D 04 40 48 8B 81 90 00 00 00")
// (const EngineQsTransform*, Matrix* out) -> out: row-major, translation in the last row, scale applied
FUNC_OPT("EngineQsTransform::ToMatrix", 0x004b2fe0, "4C 8B DC 48 81 EC B8 00 00 00 C5 F8 10 69 10")
// CAnimSkeletalDangleConstraint -> its model-space bone buffer {Matrix* data, u32 count}; B of its combines points here
FIELD("CAnimSkeletalDangleConstraint.bonesMS", "CAnimSkeletalDangleConstraint::FillSkinningData", 0x11, 0xac, "48 8D B9 ?? ?? ?? ?? 48 8B DA", 3)
FIELD("CAnimSkeletalDangleConstraint.bonesMSCount", "CAnimSkeletalDangleConstraint::StartUpdate", 0xe, 0xb4, "8B B9 ?? ?? ?? ?? 45 33 D2", 2)
// the bones the constraint simulated this frame {i32* data, u32 count}; emptied by StartUpdate
FIELD("CAnimSkeletalDangleConstraint.simulatedBones", "CAnimSkeletalDangleConstraint::OnAttached", 0xaa, 0xa0, "48 8D 8B ?? ?? ?? ?? E8", 3)
FIELD("CAnimSkeletalDangleConstraint.simulatedBonesCount", "CAnimSkeletalDangleConstraint::StartUpdate", 0x18, 0xa8, "44 89 91 ?? ?? ?? ?? 4D 8B F9", 3)
// vtable offset of IAnimDangleConstraint::GetSkeleton (this) -> CSkeleton* (the constraint's rig)
FIELD("IAnimDangleConstraint.vtGetSkeleton", "CAnimSkeletalDangleConstraint::OnAttached", 0x1d, 0x238, "FF 90 ?? ?? ?? ?? 48 8B F0 48 85 C0 0F 84", 2)
// CSkeleton -> the rest pose local to the parent, bone count x 48 bytes (position, rotation quaternion, scale; xyzw each)
FIELD("CSkeleton.referencePoseLS", "CSkeleton::GetBoneReferenceLS", 0x19, 0x90, "48 8B 81 ?? ?? ?? ?? C5 FC 11 02", 3)
FIELD("CSkeleton.boneCount", "CSkeleton::GetBoneReferenceLS", 0x8, 0x70, "44 3B 41 ?? 7D", 3)
// Scene time and animation sampling (docs/engine/scenes.md "Time of a running scene", animation.md "Sampling an animation"). Found by the
// log and assert texts of storyScenePlayer.cpp, storyScenePlayer_tick.cpp, storySceneSectionPlayingPlan.cpp,
// skeletalAnimation.cpp and behaviorGraphOutput.cpp in witcher3Release.exe, recognised by structure in 5.01b;
// signatures unique in 5.01b and 5.0.
// (player, u8 pause, u16 reason bits): sets or clears reason bits of the player's pause mask; on a change between
// zero and non-zero it tells the current element (voice line) it is paused or resumed. The tick does nothing while
// the mask is non-zero. Engine bits: 2 suggested by gameplay (cleared at a section change), 4 scene system,
// 8 waiting for a script block
FUNC_OPT("CStoryScenePlayer::Pause", 0x01f2e200, "0F B7 81 98 04 00 00 66 85 C0 41 0F 95 C1")
// (player, float dt) -> bool: the per-frame update; builds a tick context and runs ProcessState from state 0. Reference
FUNC_OPT("CStoryScenePlayer::Tick", 0x020b2d20, "48 8B C4 48 89 58 18 48 89 68 20 57 48 81 EC 80 04 00 00")
// (player, u32 state, tick context*) -> bool: the section state machine. A context with forceTime set moves the
// section to context.time: ahead inside the current element it plays the element on by the difference, otherwise it
// restarts the section and runs the elements up to that time in one call
FUNC_OPT("CStoryScenePlayer::ProcessState", 0x020b0e80, "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 28 FC FF FF")
// (player, element instance, float dt, events, ?) -> bool: advances one element (also backwards) and collects the
// events between the old and the new position. Read for the plan and element offsets
FUNC_OPT("CStoryScenePlayer::PlayElement", 0x020b0860, "48 8B C4 4C 89 48 20 48 89 50 10 53")
// (player): resets the events executor to the current section's start; the restart path calls it. Reference
FUNC_OPT("CStoryScenePlayer::ResetSectionState", 0x01f26e50, "40 53 48 83 EC 50 48 8B D9 48 8B 89 D0 01 00 00")
// (collector): the events collector inside a tick context (context + 0x30, 0x400 bytes); construct before ProcessState
FUNC_OPT("StorySceneEventsCollector::Construct", 0x022221f0, "40 53 48 83 EC 20 45 33 C9 C6 01 01")
// (collector): its destructor; call after ProcessState
FUNC_OPT("StorySceneEventsCollector::Destruct", 0x006674d0, "48 89 5C 24 18 55 48 83 EC 20 48 8B D9 48 89 7C 24 38 48 8B 89 C0 03 00 00")
// (animation, float seconds, u32 bones, u32 tracks, AnimQsTransform* bones out, float* tracks out) -> bool: samples
// the animation buffer in local space; returns true without writing when IsLoaded reports nothing resident
FUNC_OPT("CSkeletalAnimation::Sample", 0x023619a0, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 01 41 8B F9")
// (animation, u32 bones wanted, u32* bones available, u32* always-resident bones) -> u32: 2 all wanted bones
// resident, 1 only the available ones, 0 none
FUNC_OPT("CSkeletalAnimation::IsLoaded", 0x02361a90, "41 C7 00 00 00 00 00 48 8B 49 20 48 85 C9")
// (output, CSkeleton*, TDynArray<Matrix>* out, u32 max bones): turns the behavior graph's local pose into model
// space. For a CAnimatedComponent out is component + bonesMS (the B of the skinning combines)
FUNC_OPT("SBehaviorGraphOutput::GetBonesModelSpace", 0x02494920, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 10 A0 FF FF")
// (component): once per component and frame from OnUpdateTransformNode: takes the sampled local pose, calls
// GetBonesModelSpace into bonesMS, then ragdoll and pose constraints. Reference
FUNC_OPT("CAnimatedComponent::UpdatePoseFromBehavior", 0x0242ccd0, "48 8B C4 53 48 81 EC F0 00 00 00 48 89 68 10")
// (container, const CName*) -> CSkeletalAnimationSetEntry*: the lookup by name (88 callers); the animation is at
// entry + 0x20. Locks internally. A twin with another table follows it, the same bytes but for its addresses: the
// signature is the function, its padding and the twin's start (the twin is followed by something else; with
// only the twin's first 5 bytes it matched both in 5.0 and in 6adfbd7a, and the live build found neither)
FUNC_OPT("CSkeletalAnimationContainer::FindAnimation", 0x0241cf20, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 33 ED 48 8B DA 48 8B F1 48 85 FF 75 ?? BA 38 00 00 00 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 7C 24 20 33 D2 4C 8B 44 24 28 48 8B CF E8 ?? ?? ?? ?? 48 85 FF 74 ?? 48 8B CF FF 15 ?? ?? ?? ?? 48 89 6F 08 48 89 6F 10 48 89 6F 18 48 89 6F 20 48 89 6F 28 40 88 6F 30 39 6F 08 74 ?? 33 C9 E8 ?? ?? ?? ?? 48 89 6F 08 EB ?? 48 8B FD 48 89 3D ?? ?? ?? ?? 48 8B CF FF 15 ?? ?? ?? ?? 39 6F 0C 74 ?? 44 8B 03 33 D2 41 8B C0 F7 77 08 48 8B 47 28 48 8B 14 D0 48 85 D2 74 ?? 44 39 42 10 75 ?? 44 39 02 74 ?? 48 8B 52 18 48 85 D2 75 ?? 48 8B CF FF 15 ?? ?? ?? ?? 33 C0 48 8B 5C 24 40 48 8B 6C 24 48 48 8B 74 24 50 48 83 C4 30 5F C3 48 8D 4A 08 48 85 C9 74 ?? 48 8B 09 48 85 C9 74 ?? 4C 8B 4E 58 8B 46 60 4D 8D 04 C1 4D 3B C1 74 ?? 49 8B 50 F8 49 83 E8 08 48 8B D9 48 85 D2 74 ?? 48 8B 42 08 EB ?? 48 8B C5 48 3B 43 18 74 ?? 48 8B 5B 48 48 85 DB 75 ?? EB ?? 48 8B CF FF 15 ?? ?? ?? ?? 48 8B C3 EB ?? CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 3D ??")
FIELD("CStoryScenePlayer.pauseMask", "CStoryScenePlayer::Pause", 0x3, 0x498, "0F B7 81 ?? ?? ?? ?? 66 85 C0", 3)
FIELD("CStoryScenePlayer.planId", "CStoryScenePlayer::Pause", 0x39, 0x1e4, "44 8B 89 ?? ?? ?? ?? 41 83 F9 FF", 3)
FIELD("CStoryScenePlayer.plansCount", "CStoryScenePlayer::Pause", 0x46, 0x5e0, "44 8B 81 ?? ?? ?? ?? 33 C0", 3)
FIELD("CStoryScenePlayer.plans", "CStoryScenePlayer::Pause", 0x54, 0x5d8, "4C 8B 91 ?? ?? ?? ?? 0F 1F", 3)
// plan: +0x00 id (i32), +0x10 current element index, +0x50 element count, +0x54 element instances (pointers)
FIELD("StorySceneSectionPlayingPlan.currElement", "CStoryScenePlayer::Pause", 0x7c, 0x10, "48 63 42 ?? 3B 42", 3)
FIELD("StorySceneSectionPlayingPlan.numElements", "CStoryScenePlayer::Pause", 0x7f, 0x50, "3B 42 ?? 73 ?? 48 8B C8", 2)
FIELD("StorySceneSectionPlayingPlan.elements", "CStoryScenePlayer::Pause", 0x88, 0x54, "48 8B 42 ?? 41 0F B6 D3", 3)
// plan: its events (pointers) and count; per event two byte flags in the plan's instance buffer (buffer + 0x28 data)
// at the u32 offsets event + instanceVarA / instanceVarB (started, finished). The editor clears both before a jump
FIELD("StorySceneSectionPlayingPlan.events", "CStoryScenePlayer::PlayElement", 0x1b3, 0x6c, "49 8B 47 ?? 48 63 CB 48 8B 3C C8", 3)
FIELD("StorySceneSectionPlayingPlan.eventsCount", "CStoryScenePlayer::PlayElement", 0x191, 0x74, "41 39 5F ?? 0F 86", 3)
FIELD("StorySceneSectionPlayingPlan.instanceBuffer", "CStoryScenePlayer::PlayElement", 0x2fc, 0xb0, "49 8B 87 ?? ?? ?? ?? 41 8B 91 90", 3)
FIELD("CStorySceneEvent.instanceVarA", "CStoryScenePlayer::PlayElement", 0x303, 0x90, "41 8B 91 ?? ?? ?? ?? 48 8B 48 28 88 1C 0A 49 8B 87", 3)
FIELD("CStorySceneEvent.instanceVarB", "CStoryScenePlayer::PlayElement", 0x318, 0xa0, "41 8B 91 ?? ?? ?? ?? 48 8B 48 28 88 1C 0A 49 83", 3)
// element instance: start in the section (f32), duration (f32), time inside the element (f32); +0x25 running (u8)
FIELD("IStorySceneElementInstance.startTime", "CStoryScenePlayer::PlayElement", 0x9e, 0x1c, "48 8D 7A ?? 4D 8D B5", 3)
FIELD("IStorySceneElementInstance.duration", "CStoryScenePlayer::PlayElement", 0xae, 0x20, "C5 FA 10 42 ?? 41 88 06", 4)
FIELD("IStorySceneElementInstance.localTime", "CStoryScenePlayer::PlayElement", 0xbe, 0x18, "C5 7A 5C 5A ?? 48 8D 72", 4)
// player: the internal state (+0x00 u8 in tick, +0x50 current section, +0x58 pending next section, +0x64 plan id),
// the RestartSection request byte (script function RestartSection sets it; the next tick restarts at 0), scene time
FIELD("CStoryScenePlayer.internalState", "CStoryScenePlayer::Tick", 0x114, 0x180, "48 8D B9 ?? ?? ?? ?? C6 07 01", 3)
FIELD("CStoryScenePlayer.restartSectionRequest", "CStoryScenePlayer::Tick", 0xf2, 0x1fb, "4C 8D B1 ?? ?? ?? ?? 41 38 2E", 3)
FIELD("CStoryScenePlayer.sceneTime", "CStoryScenePlayer::Tick", 0x1a9, 0x1ec, "C5 CA 58 83 ?? ?? ?? ?? C5 CA 5E", 4)
FIELD("CStoryScenePlayer.currentSection", "CStoryScenePlayer::ResetSectionState", 0xc, 0x1d0, "48 8B 89 ?? ?? ?? ?? 48 85 C9 0F 84", 3)
// CAnimatedComponent: its model-space bone buffer {Matrix*, u32 count} and its behavior graph sample context
FIELD("CAnimatedComponent.bonesMS", "CAnimatedComponent::UpdatePoseFromBehavior", 0xca, 0xddc, "4C 8D 83 ?? ?? ?? ?? 48 8B C8 E8", 3)
FIELD("CAnimatedComponent.sampleContext", "CAnimatedComponent::UpdatePoseFromBehavior", 0x35, 0xe68, "48 8B 8B ?? ?? ?? ?? 41 3B C6", 3)
// SBehaviorGraphOutput: +0x00 local pose (AnimQsTransform*, 48 bytes each), +0x18 bone count (u32)
FIELD("SBehaviorGraphOutput.numBones", "SBehaviorGraphOutput::GetBonesModelSpace", 0x56, 0x18, "44 8B 41 ?? 45 3B C8", 3)
// CSkeletalAnimation: its animation buffer; +0x2c duration in seconds (f32), +0x10 its set entry
FIELD("CSkeletalAnimation.animBuffer", "CSkeletalAnimation::Sample", 0x4a, 0x20, "48 8B 4B ?? 48 85 C9 74 ?? 48 8B 01 44 8B C6", 3)
// A line of a scene (docs/engine/scenes.md "A line without a voice file"). Reference, unused at
// runtime; kept FUNC_OPT. Found by the assert texts of storySceneLine.cpp in
// witcher3Release.exe ("m_line", "!m_speechRunning"), matched into the known build by RTTI vtable reference (the
// instance's CStorySceneLine vtable, found by the RTTI type name technique), signatures unique in 5.0 and in the
// live 5.01b build (6adfbd7a; the live install has since moved past the RVAs this file documents - verify by
// signature, not by raw RVA, when the build changes without a new PE timestamp).
// A playing CStorySceneLine instance's one-time setup: builds the base IStorySceneElementInstance part, then the
// derived fields (m_line at +0x28 from its own 2nd argument, a weak speaker reference at +0x30, a voicetag-match
// byte at +0x38, an int copied from a virtual call on m_line at +0x3c, a player-side cache pointer at +0x48 copied
// from the player + 0x870, m_speechRunning at +0x50 reset to 0). Reference
FUNC_OPT("CStorySceneLine::OnStart", 0x020ea160, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 45 33 F6 4C 89 41 08")
// The per-tick lazy setup for the line's lipsync/speaker reference (vtable slot 13, called from
// CStoryScenePlayer::PlayElement after the generic time-advance). Returns early, doing nothing, while the player is
// paused, while m_speechRunning (+0x50) is already set, or while m_line's own "not ready" byte (+0xd3 of the object
// at this+0x28) is set - this last path is the lead for "a line without a voice file never finishes its one-time
// setup": the lipsync/speaker hookup simply never runs, every tick, for as long as that byte stays set. Once it
// does run, it sets m_speechRunning (+0x50) to 1 so it only runs once. Returns true while localTime < duration (the
// generic per-tick clamp of localTime to [0, duration] runs independently, in the vtable-slot-9 default no-op path
// shared by every element type - the time itself is not blocked here). Reference, not yet confirmed as the actual
// cause of the seek-into-a-voiceless-line hang (see scenes.md for the open question: where duration itself for
// such a line is computed, and whether that computation also waits on the +0xd3 byte).
FUNC_OPT("CStorySceneLine::Tick", 0x020e9e50, "40 55 57 48 81 EC 88 00 00 00 C5 FA 10 41 20")
// The line instance's destructor (vtable slot 0): releases the speaker reference at +0x30, then restores the base
// IStorySceneElementInstance vtable. Reference, for completeness only.
FUNC_OPT("CStorySceneLine::Destruct", 0x020ea110, "40 53 48 83 EC 20 83 79 3C 00 48 8D 05 ?? ?? ?? ??")
// A second construction path with the same field layout as OnStart, additionally resolving the speaker through a
// virtual call on the player (slot 0xb0) and allocating the instance itself (0x60 bytes) - likely the factory the
// section's playing plan uses when it first creates a line instance, as opposed to OnStart's simpler re-init.
// Reference, not disassembled past the field layout (same offsets as OnStart).
FUNC_OPT("CStorySceneLine::CreateInstance", 0x020eacb0, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 8B 81 88 00 00 00")
// Render resource lifetime (docs/FINDINGS_DEFORM_LIFETIME.md): the reference count every IRenderResource/
// IRenderObject (CRenderMesh among them) carries at +0x08 (u32), found by the RTTI vtable technique on
// IRenderResource/IRenderObject (identical base slots: both share the same destructor-dispatch and Release at
// their 1st/2nd vtable slot). Release is virtual, checked against 5.0 and the live 5.01b build, each unique.
// AddRef is a plain (non-virtual) increment right after Release in memory, not reached by any direct call found
// statically (likely inlined at its call sites). Call it directly by this address/signature instead of through a vtable slot.
FUNC_OPT("IRenderResource::Release", 0x0228e930, "B8 FF FF FF FF F0 0F C1 41 08 83 F8 01 75 ?? 48 8B 01 48 FF 60 08 C3 CC CC CC CC CC CC CC CC CC F0 FF 41 08")
FUNC_OPT("IRenderResource::AddRef", 0x0228e950, "F0 FF 41 08 C3 CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 6C 24 10")
