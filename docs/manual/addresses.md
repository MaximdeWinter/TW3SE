# Addresses

A plugin gets the engine's functions, globals and field offsets by name: `GetAddress("CNamesPool::Get")`.
TW3SE finds each one on every game build (by its address on the known build, by a byte pattern on another). A
plugin that asks by name keeps working after a game patch; a name whose function the patch changed returns 0
until an update of TW3SE.

`GetAddress` returns 0 for a name that is missing on this build; check it before you call. A field
offset comes back as its number.

This list has 156 names (generated from `src\core\addresses.inc` by `tools\gen_addresses_md.py`).
Ask for a name that is missing in the TW3SE thread on Nexus Mods.

## Functions

- `CCommonGame::Tick`
- `CCommonGame::OnGameStart`
- `CContentManager::ScanContentPack`
- `CContentManager::CreatePackage`
- `CContentManager::AttachPackage`
- `CContentManager::LoadModioMods`
- `CNamesPool::Get`
- `CNamesPool::AddEntry`
- `CDLCManager::Scan`
- `CDLCManager::OnGameStarting`
- `CModHandlerSystem::LoadModsIntoGameSystems`
- `RegisterCoreScriptNatives`
- `CFunction::CFunction`
- `SRTTI::Get`
- `CRTTISystem::RegisterGlobalFunction`
- `Memory::Alloc`
- `funcLogChannel`
- `CGameSessionManager::RestoreSession`
- `funcLoadResource`
- `funcLoadResourceAsync`
- `CompareFunctionSignature`
- `CheckFunctionRedeclaration`
- `CheckWrapMethodCompat`
- `CClass::CClass`
- `CClass::AddProperty`
- `CClass::GetProperties`
- `CFunction::GetParameters`
- `IScriptable::GetClass`
- `CRTTISystem::FindType`
- `CClass::GetKind`
- `RegisterNative_CPPForceActorLOD`
- `ResolveScriptGlobalByName`
- `CScriptedClass::ForEachPropertyPair`
- `CFunction::CFunctionMethod`
- `funcGetComponent`
- `funcGetGuidHash`
- `funcGetWorldPosition`
- `funcTeleportWithRotation`
- `funcSetRawPlacement`
- `CScriptingSystem::ReloadScripts`
- `CScriptingSystem::CompileAllScripts`
- `CScriptingSystem::ApplyCompiledScripts`
- `GatherScriptFiles`
- `CScriptingSystem::CompileAndApplyAll`
- `CScriptingSystem::RelinkRTTI`
- `CDynamicLayer::CreateAndAttachEntity`
- `CDynamicLayer::InstantiateEntity`
- `CDynamicLayer::UnlinkEntity`
- `CDynamicLayer::RegisterEntityTag`
- `CDynamicLayer::FinishEntityAttach`
- `CDynamicLayer::NotifyEntityAttachListeners`
- `CEntity::OnLayerAttachInit`
- `CEntity::SyncAttachmentPlacement`
- `CScriptCompiler::ResolveLiteralKind`
- `CScriptCompiler::CreateLiteralNode`
- `CRemoteConnection::ExecCommand`
- `CScriptCompiler::CompileCall`
- `CScriptCompiler::DeclareGlobalFunction`
- `CEntity::AttachToWorld`
- `CLayer::AttachToWorld`
- `Renderer::UmbraOcclusionUser`
- `funcFoliageSendCreateSpeedTreeDynamicInstances`
- `funcFoliageCopyInstancePlacements`
- `funcFoliageSendRemoveSpeedTreeDynamicInstancesRadius`
- `funcFoliageSendRemoveSpeedTreeInstancesRadius`
- `funcFoliageSendRemoveSpeedTreeInstancesRect`
- `funcFoliageResolveResourceReference`
- `funcFoliageSendCreateSpeedTreeInstances`
- `funcFoliageSendUpdateSpeedTreeInstances`
- `funcFoliageSendUpdateGenericGrassMask`
- `funcFoliageSendUploadGenericGrassOccurrenceMap`
- `funcFoliageSendRefreshGenericGrass`
- `CCollisionCacheResolver::RegisterContentPacks`
- `CCollisionCacheResolver::CCollisionCacheResolver`
- `CTerrainTile::UpdateClipmapIfNeeded`
- `CGame::StartGame`
- `CGameSessionManager::StartNewGameSession`
- `CGameSessionManager::InitNewSessionGuid`
- `CGameSessionManager::RestoreFromSessionData`
- `CGameSessionManager::GetSessionDataForRestore`
- `CQuestThread::CQuestThread`
- `CQuestThread::~CQuestThread`
- `CQuestGraph::CQuestGraph`
- `funcNotifyListenerList`
- `CQuestsSystem::CQuestsSystem`
- `CQuestThread::ActivateBlock`
- `CQuestThread::TickActiveBlocks`
- `CQuestThread::ActivateInput`
- `CQuestThread::VisitActiveThreadsAndBlocks`
- `CGraphBlock::CGraphBlock`
- `funcBuildSaveGamesFolderPath`
- `CScriptThread::CScriptThread`
- `CScriptThread::~CScriptThread`
- `StartScriptThread`
- `InvokeFunctionOrStartLatentThread`
- `InvokeFunctionChecked`
- `TickScriptThreads`
- `CScriptThread::Step`
- `CObjectGC_Legacy::CObjectGC_Legacy`
- `CObjectGC_Legacy::~CObjectGC_Legacy`
- `CObjectsGC_MarkAndSweep::CObjectsGC_MarkAndSweep`
- `CObjectsGC_MarkAndSweep::MarkReachable`
- `DepotResolveAndLoad`
- `DepotCacheLookup`
- `CDirectory::FindFile`
- `CDiskFile::CreateAsyncFile`
- `CDiskFile::CDiskFile`
- `RenderCommandAlloc28`
- `CDrawableComponent::RefreshRenderProxies`
- `CMemoryFileReader::CMemoryFileReader`
- `CLocalizationManager::GetLocalizedText`

## Functions that some builds lack

- `funcGetWeatherNames`
- `TDynArray::GrowBy`
- `TDynArray::AllocatedCapacity`
- `Dispatch`
- `ScriptCallDirect`
- `Combine_Format1`
- `Combine_Format2`
- `ModelSpaceFillDispatch`
- `Mimic_TrackSampleCall`
- `SDynamicDecalInitParameters::Init`
- `SpawnWorldDynamicDecal`
- `SpawnWoundDynamicDecal`
- `DecalTarget::LimitCheck`

## Call sites

- `Memory::Free`
- `Memory::AllocDefault`

## Globals

- `GContentManager`
- `GGame`
- `GScriptNativeTable`
- `GScriptGlobals`
- `GScriptMethodTable`
- `GScriptLogSink`
- `GUseUmbraOcclusion`
- `GCollisionCacheResolver`
- `GDepot`
- `GObjectsMap`
- `GRender`

## Field offsets

- `CCommonGame.dlcManager`
- `CDLCManager.isNewContentAvailable`
- `CNamesPool.entries`
- `CNamesPool.entryCount`
- `CQuestsSystem.activeQuests`
- `CQuestsSystem.activeQuestsCount`
- `CQuestThread.listeners`
- `CQuestThread.listenersCount`
- `CQuestThread.childThreads`
- `CQuestThread.childThreadsCount`
- `CQuestGraphBlock.flagsBufferOffset`
- `CQuestGraphBlock.localDataOffset`
- `CQuestGraphBlock.stateOffset`
- `CFunction.code`
- `CFunction.codeSize`
- `CWorld.renderScene`
- `CBitmapTexture.GetRenderResourceSlot`
- `IRender.CreateDynamicDecalSlot`
- `CDrawableComponent.renderProxy`
