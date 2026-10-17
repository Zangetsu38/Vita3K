// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <kernel/types.h>

#include <cstdint>

inline bool secondary_import_should_log(uint64_t occurrence) {
    return occurrence == 1;
}

inline bool secondary_import_allowed(uint32_t nid, uint32_t arg0, uint32_t arg2) {
    switch (nid) {
    case 0x008402C6: // sceGxmMapFragmentUsseMemory
    case 0x5213F74B: // sceDisplayGetFrameBufInternal (caller-process output)
    case 0x1B04A1D6: // scePowerGetGpuClockFrequency (constant)
    case 0x1FBB0FE1: // sceKernelPollEventFlag (caller-process output)
    case 0x2255B2A5: // sceKernelCreateTimer (caller-process name, process-owned timer)
    case 0x23F79274: // sceRtcGetCurrentTick (shared clock, caller-process output)
    case 0xCDDD25FE: // sceRtcGetCurrentNetworkTick (shared clock, caller-process output)
    case 0x1282C436: // sceRtcConvertUtcToLocalTime (caller-process input and output)
    case 0xCD89F464: // sceRtcSetTick (caller-process input and output)
    case 0xCCEA2B54: // sceRtcFormatRFC3339 (shared clock, caller-process input and output)
    case 0x2D18AEEC: // sceRtcParseRFC3339 (caller-process input and output)
    case 0xD8EBBB7E: // sceClibLookCtypeTable (pure lookup, no process memory)
    case 0x3EFD3165: // sceKernelGetTimerTimeWide (caller-owned or openable timer)
    case 0x441673E3: // sceAvcdecDecodeAvailableSize (caller-process handle and decoder ownership)
    case 0x48091E0C: // sceKernelStartTimer (caller-owned or openable timer)
    case 0x5DBC1960: // sceKernelGetTimerBaseWide (caller-owned or openable timer)
    case 0x869E9F20: // sceKernelStopTimer (caller-owned or openable timer)
    case 0x91BE107E: // sceAppMgrSaveDataSlotFileOpen (caller-process IO and buffers)
    case 0xC176317B: // sceAppMgrSaveDataSlotFileGetParam (caller-process IO and output memory)
    case 0x93F0D89F: // sceAppUtilSaveDataSlotGetParam (caller-process IO and buffers)
    case 0xE82BB69B: // sceAvcdecCreateDecoder (caller-process buffers and decoder ownership)
    case 0x9E45DA09: // sceKernelLibcClock (shared clock, no process memory)
    case 0x5182E212: // sceKernelGetSystemSwVersion (caller-process output)
    case 0xA4DE5B69: // sceErrorGetExternalString (caller-process output)
    case 0xBC416CFA: // sceErrorHistoryPostError (caller-process input)
    case 0x04D9F484: // sceNpInit (caller-process configuration input)
    case 0x2415F8A4: // sceKernelStopUnloadModule (caller address space)
    case 0x086867A8: // _sceKernelStopModule (caller address space)
    case 0x86EAEA0A: // _sceKernelStopUnloadModule (caller address space)
    case 0x8E4A7716: // _sceKernelUnloadModule (caller address space)
    case 0x324F1B66: // sceCtrlGetControllerPortInfo (shared input state, caller-process output)
    case 0x3B2CBA09: // sceKernelStopModule (caller address space)
    case 0xBD59D652: // sceTouchActivateRegion (no caller-process memory)
    case 0x2D94EAEA: // sceTouchClearRegion (no caller-process memory)
    case 0x04440622: // sceTouchPeekRegion (shared input state, caller-process output)
    case 0x2CF6D7E2: // sceTouchPeekRegionExt (shared input state, caller-process output)
    case 0x93B5635B: // sceTouchReadRegion (shared input state, caller-process output)
    case 0x9F0ACAF9: // sceTouchReadRegionExt (shared input state, caller-process output)
    case 0x6134DFBE: // sceTouchSetRegion (no caller-process memory)
    case 0x47E5DD7D: // sceAppMgrReceiveEventNum (per-process events, caller-process output)
    case 0x04B7766E: // scePowerRegisterCallback (checks callback ownership and process identity)
    case 0x4CB87CA7: // sceKernelClearEventFlag (kernel event-flag object)
    case 0x4DD98588: // sceGxmGetRenderTargetMemSizeInternal (caller-process memory)
    case 0x5694B569: // sceGxmMapVertexUsseMemoryInternal (caller-process memory)
    case 0x1987920E: // sceKernelUnloadModule (caller address space)
    case 0x207AF96B: // sceGxmCreateRenderTarget
    case 0x5840162C: // sceKernelDeleteEventFlag (kernel event-flag reference)
    case 0x120F03AF: // sceKernelWaitEvent (waits on the calling thread, uses caller-process outputs)
    case 0xA0490795: // sceKernelWaitEventCB (same caller-thread and caller-process memory handling)
    case 0x83C0E2AF: // sceKernelWaitEventFlag (caller-process output)
    case 0x89F86A1E: // sceDisplaySetFrameBufInternal (routes through caller-process display path)
    case 0xB9391D22: // sceGxmMapFragmentUsseMemoryInternal (caller-process memory)
    case 0xFD7A4EEC: // sceAppMgrSaveDataSlotFileClose (caller-process IO and buffer)
    case 0x6A6013E1: // sceGxmSyncObjectCreate
    case 0x05032658: // sceGxmShaderPatcherCreate
    case 0x2B528462: // sceGxmShaderPatcherRegisterProgram
    case 0x4ED2E49D: // sceGxmShaderPatcherCreateFragmentProgram
    case 0xB7BBA6D5: // sceGxmShaderPatcherCreateVertexProgram
    case 0xB19CF7E9: // sceKernelCreateCallback (caller-process memory and thread ownership)
    case 0xBDB32767: // sceMotionGetState (shared sensor state, caller-process output)
    case 0xC8A0F04E: // sceGxmCreateRenderTargetInternal (caller-process memory)
    case 0xE53E41F6: // sceKernelCheckCallback (current thread only)
    case 0xE6DB2494: // sceKernelCreateSimpleEvent (tracks caller process)
    case 0xEC94DFF7: // sceKernelSetEventFlag (kernel event-flag object)
    case 0xBE2743D1: // sceGxmShaderPatcherReleaseFragmentProgram
    case 0xAC1FF2DA: // sceGxmShaderPatcherReleaseVertexProgram
    case 0x41BBD792: // sceGxmGetPrecomputedDrawSize
    case 0x85DE8506: // sceGxmGetPrecomputedFragmentStateSize
    case 0x9D83CA3B: // sceGxmGetPrecomputedVertexStateSize
    case 0xB32917F0: // sceGxmSetYuvProfile (no-op stub)
    case 0x91236858: // sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer
    case 0x34BF64E3: // sceGxmPrecomputedVertexStateSetDefaultUniformBuffer
    case 0x8734FF4E: // sceGxmBeginScene
    case 0x62312BF8: // sceGxmTransferCopy (caller-process address space)
    case 0xD10F7EAD: // sceGxmTransferDownscale (caller-process address space)
    case 0x69DDFF5E: // sceGxmTransferFill (caller-process address space)
    case 0x45229C39: // sceGxmTransferFinish (waits for renderer work)
    case 0xEC82DB20: // sceGxmIsInitializationInternal (queries the caller's GXM display queue)
    case 0xFE300E2F: // sceGxmEndScene
    case 0x9F448E79: // sceGxmNotificationWait
    case 0xEC5C26B5: // sceGxmDisplayQueueAddEntry
    case 0x29C34DF5: // sceGxmSetFragmentTexture
    case 0x70CBC2D5: // sceClibStrlcat (caller-process memory)
    case 0x2CDFCD1C: // sceClibStrlcpy (caller-process memory)
    case 0x814F61EB: // sceGxmSetViewportEnable (caller-process context)
    case 0x9EB4380F: // sceGxmSetVertexTexture (caller-process context)
    case 0x895DF2E9: // sceGxmSetVertexStream
    case 0x31FF8ABD: // sceGxmSetVertexProgram
    case 0x7B1FABB6: // sceGxmReserveFragmentDefaultUniformBuffer
    case 0x97118913: // sceGxmReserveVertexDefaultUniformBuffer
    case 0xAD2F48D9: // sceGxmSetFragmentProgram
    case 0xBC059AFC: // sceGxmDraw
    case 0x14C4E7D3: // sceGxmDrawInstanced (caller-process GXM state and memory)
    case 0x86456F7B: // sceGxmColorSurfaceSetClip (no cross-process memory access)
    case 0xCFAD5A3A: // sceAppMgrReceiveEvent (per-process events, caller-process output)
    case 0xDD6AABFA: // sceGxmTextureSetPalette (caller-process texture state)
    case 0xF0704CF3: // sceTouchGetPixelDensity (caller-process output)
    case 0xFA6BE467: // sceClibVsnprintf (caller-process memory and varargs)
    case 0xED3F78B8: // sceGxmDrawPrecomputed
    case 0xCECB584A: // sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer
    case 0xBE5A68EF: // sceGxmPrecomputedVertexStateGetDefaultUniformBuffer
    case 0xB7626A93: // sceGxmSetPrecomputedVertexState
    case 0xF8952750: // sceGxmSetPrecomputedFragmentState
    case 0xEA0FC310: // sceGxmSetFragmentUniformBuffer
    case 0x65DD0C84: // sceGxmSetUniformDataF
    case 0xC68015E4: // sceGxmSetVertexUniformBuffer
    case 0xA197F096: // sceGxmPrecomputedDrawInit
    case 0xB6C6F571: // sceGxmPrecomputedDrawSetAllVertexStreams
    case 0x884D0D08: // sceGxmPrecomputedDrawSetParams
    case 0x6C936214: // sceGxmPrecomputedDrawSetVertexStream
    case 0xE297D7AF: // sceGxmPrecomputedFragmentStateInit
    case 0x29118BF1: // sceGxmPrecomputedFragmentStateSetTexture
    case 0xB452F1FB: // sceGxmPrecomputedFragmentStateSetUniformBuffer
    case 0xBE937F8D: // sceGxmPrecomputedVertexStateInit
    case 0xDBF97ED6: // sceGxmPrecomputedVertexStateSetUniformBuffer
    case 0x06FF9151: // sceGxmProgramGetParameter
    case 0x6679BEF0: // sceGxmTextureInitLinearStrided
    case 0x11DC8DC9: // sceGxmTextureInitCube
    case 0xE3DF5E3B: // sceGxmTextureInitCubeArbitrary
    case 0xE6F0DB27: // sceGxmTextureInitTiled
    case 0x0C44ACD7: // sceGxmDepthStencilSurfaceSetForceLoadMode (caller-process surface)
    case 0x12AAA7AF: // sceGxmDepthStencilSurfaceSetForceStoreMode
    case 0x32F280F0: // sceGxmDepthStencilSurfaceSetBackgroundDepth (caller-process surface)
    case 0xF5D3F3E8: // sceGxmDepthStencilSurfaceSetBackgroundStencil (caller-process surface)
    case 0x6AF88A5D: // sceGxmProgramParameterGetName
    case 0xAAFD61D5: // _sceGxmProgramParameterGetSemantic
    case 0xB85CC13E: // sceGxmProgramParameterGetSemanticIndex
    case 0xD5D5FCCD: // sceGxmProgramGetParameterCount
    case 0xE6D9C4CE: // sceGxmProgramParameterGetSemantic
    case 0x871E5009: // sceGxmProgramParameterIsRegFormat (caller-process program and parameter)
    case 0xED8B6C69: // sceGxmProgramCheck
    case 0x06752183: // sceGxmSetFrontPointLineWidth
    case 0x14BD831F: // sceGxmSetFrontDepthFunc
    case 0x17B3BF86: // sceGxmSetBackDepthBias
    case 0x1A68C8D2: // sceGxmSetBackStencilFunc
    case 0x3EB3380B: // sceGxmSetViewport
    case 0x575958A8: // sceGxmSetFrontFragmentProgramEnable
    case 0x70C86868: // sceGxmSetRegionClip
    case 0x855814C4: // sceGxmTextureSetData
    case 0x8699ECF4: // sceGxmTextureSetUAddrModeSafe
    case 0xFA22F6CC: // sceGxmTextureSetVAddrModeSafe
    case 0xB65EE6F7: // sceGxmTextureSetLodBias
    case 0xB79E43DD: // sceGxmTextureSetLodMin
    case 0xD2DC4643: // sceGxmTextureSetMipmapCount
    case 0x866A0517: // sceGxmSetBackStencilRef
    case 0x8DCB0EDB: // sceGxmSetBackPointLineWidth
    case 0x8FA6FE44: // sceGxmSetFrontStencilRef
    case 0xA4297E57: // sceGxmProgramIsNativeColorUsed
    case 0xAAA97F81: // sceGxmSetFrontDepthBias
    case 0xB042A4D2: // sceGxmSetBackDepthFunc
    case 0xB8645A9A: // sceGxmSetFrontStencilFunc
    case 0xC18B706B: // sceGxmSetBackDepthWriteEnable
    case 0xCA9D41D1: // sceGxmDepthStencilSurfaceInit
    case 0xE1CA72AE: // sceGxmSetCullMode
    case 0xE26B4834: // sceGxmSetBackFragmentProgramEnable
    case 0xED0F6E25: // sceGxmColorSurfaceInit
    case 0xF32CBF34: // sceGxmSetFrontDepthWriteEnable
    case 0xF66EC6FE: // sceGxmSetBackPolygonMode
    case 0xFD93209D: // sceGxmSetFrontPolygonMode
    case 0x1CA9FE0B: // sceGxmTextureSetMipFilter
    case 0x126CDAA3: // sceGxmTextureSetVAddrMode
    case 0x2DB6026C: // sceGxmColorSurfaceGetData
    case 0x416764E3: // sceGxmTextureSetMinFilter
    case 0x4281763E: // sceGxmTextureSetUAddrMode
    case 0x4811AECB: // sceGxmTextureInitLinear
    case 0x52FDE962: // sceGxmColorSurfaceGetType
    case 0x613639FA: // sceGxmColorSurfaceInitDisabled
    case 0x8FA3F9C3: // sceGxmProgramGetDefaultUniformBufferSize
    case 0xA41DB0D6: // sceGxmDepthStencilSurfaceInitDisabled
    case 0xBC52320E: // sceGxmVertexProgramGetProgram
    case 0xD572D547: // sceGxmTextureInitSwizzled
    case 0xF33D9980: // sceGxmColorSurfaceGetStrideInPixels
    case 0xF3C1C6C6: // sceGxmColorSurfaceGetFormat
    case 0xF65D4917: // sceGxmTextureGetType
    case 0xE868D2B3: // sceGxmTextureGetFormat (caller-process texture)
    case 0xF23FCE81: // sceGxmTextureGetGammaMode (caller-process texture)
    case 0xA6D9F4DA: // sceGxmTextureSetGammaMode (caller-process texture)
    case 0xF5C89643: // sceGxmColorSurfaceSetGammaMode (caller-process surface)
    case 0x5420A086: // sceGxmTextureGetHeight (caller-process texture)
    case 0x2DE55DA5: // sceGxmTextureGetLodBias (caller-process texture)
    case 0xBE524A2C: // sceGxmTextureGetLodMin (caller-process texture)
    case 0xAE7FBB51: // sceGxmTextureGetMagFilter (caller-process texture)
    case 0x920666C6: // sceGxmTextureGetMinFilter (caller-process texture)
    case 0xCE94CA15: // sceGxmTextureGetMipFilter (caller-process texture)
    case 0xF7B7B1E4: // sceGxmTextureGetMipmapCount (caller-process texture)
    case 0x4CC42929: // sceGxmTextureGetMipmapCountUnsafe (caller-process texture)
    case 0x512BB86C: // sceGxmTextureGetNormalizeMode (caller-process texture)
    case 0x0D189C30: // sceGxmTextureGetPalette (caller-process texture)
    case 0xB0BD52F3: // sceGxmTextureGetStride (caller-process texture)
    case 0x2AE22788: // sceGxmTextureGetUAddrMode (caller-process texture)
    case 0xC037DA83: // sceGxmTextureGetUAddrModeSafe (caller-process texture)
    case 0x46136CA9: // sceGxmTextureGetVAddrMode (caller-process texture)
    case 0xD2F0D9C1: // sceGxmTextureGetVAddrModeSafe (caller-process texture)
    case 0x126A3EB3: // sceGxmTextureGetWidth (caller-process texture)
    case 0xFA695FD7: // sceGxmTextureSetMagFilter
    case 0xA9C3CED6: // sceCtrlPeekBufferPositive
    case 0x15F81E8C: // sceCtrlPeekBufferPositive2
    case 0xA59454D3: // sceCtrlPeekBufferPositiveExt
    case 0x860BF292: // sceCtrlPeekBufferPositiveExt2
    case 0x104ED1A7: // sceCtrlPeekBufferNegative
    case 0x81A89660: // sceCtrlPeekBufferNegative2
    case 0x67E7AB83: // sceCtrlReadBufferPositive
    case 0xC4226A3E: // sceCtrlReadBufferPositive2
    case 0xE2D99296: // sceCtrlReadBufferPositiveExt
    case 0xA7178860: // sceCtrlReadBufferPositiveExt2
    case 0x15F96FB0: // sceCtrlReadBufferNegative
    case 0x27A0C5FB: // sceCtrlReadBufferNegative2
    case 0xA497B150: // sceCtrlSetSamplingMode
    case 0xB1DC4378: // sceCtrlSetSamplingModeExt
    case 0x10A2CA25: // sceTouchGetPanelInfo
    case 0xFF082DF0: // sceTouchPeek
    case 0x3AD3D0A1: // sceTouchPeek2
    case 0x169A1D58: // sceTouchRead
    case 0x1B9C5D14: // sceTouchSetSamplingState
    case 0x13CDFC43: // sceTouchSetSamplingStateExt (shared touch state)
    case 0x0D5399CF: // sceNgsVoiceDefGetSimpleVoice
    case 0x0E0ACB68: // sceNgsVoiceDefGetCompressorBuss
    case 0x14EF65A0: // sceNgsVoiceDefGetAtrac9Voice
    case 0x1AF83512: // sceNgsVoiceDefGetCompressorSideChainBuss
    case 0x1F51C2BA: // sceNgsVoiceDefGetSasEmuVoice
    case 0x214485D6: // sceNgsVoiceDefGetPauserBuss
    case 0x45CF2A73: // sceNgsVoiceDefGetSimpleAtrac9Voice
    case 0x4D705E3E: // sceNgsVoiceDefGetDelayBuss
    case 0x79A121D1: // sceNgsVoiceDefGetMasterBuss
    case 0x9DCF50F5: // sceNgsVoiceDefGetReverbdailleurs Buss
    case 0xAAD90DEB: // sceNgsVoiceDefGetDistortionBuss
    case 0xCD63A2BF: // sceNgsVoiceDefGetScreamAtrac9Voice
    case 0xCE53BC33: // sceNgsVoiceDefGetScreamVoice
    case 0xE0AC8776: // sceNgsVoiceDefGetMixerBuss
    case 0xE9B572B7: // sceNgsVoiceDefGetTemplate1
    case 0xF6B68C31: // sceNgsVoiceDefGetEnvelopeBuss
    case 0xF964120E: // sceNgsVoiceDefGetEqBuss
    case 0xFBE515D4: // sceNgsVoiceDefGetPitchShiftBuss
    case 0x64D80013: // sceNgsSystemSetFlags
    case 0x6CE8B36F: // sceNgsSystemGetRequiredMemorySize
    case 0x0A92E4EC: // sceNgsRackInit
    case 0x477318C0: // sceNgsRackGetRequiredMemorySize
    case 0x4A25BEBC: // sceNgsSystemRelease
    case 0x684F080C: // sceNgsSystemUpdate
    case 0xDD5CA10B: // sceNgsRackRelease
    case 0xED14CF4A: // sceNgsSystemInit
    case 0xD668B49C: // sceNgsPatchCreateRouting
    case 0xD0C9AE5A: // sceNgsPatchRemoveRouting
    case 0x0E291AAD: // sceNgsVoiceKill
    case 0x54CFB981: // sceNgsVoiceResume
    case 0xBB13373D: // sceNgsVoiceKeyOff
    case 0xD7786E99: // sceNgsVoicePause
    case 0xC9B8C0B4: // sceNgsVoiceGetStateData
    case 0x5551410D: // sceNgsVoiceGetInfo
    case 0xFB8174B1: // sceNgsVoiceSetParamsBlock (caller-process memory)
    case 0xA0F5402D: // sceNgsVoicePatchSetVolumesMatrix
    case 0xAB6BEF8F: // sceNgsVoiceLockParams
    case 0x3D46D8A7: // sceNgsVoiceUnlockParams
    case 0xFA0A0F34: // sceNgsVoicePlay
    case 0x24E909A8: // sceNgsVoiceSetModuleCallback
    case 0xFE1A98E9: // sceNgsRackGetVoiceHandle
    case 0x0A93EA96: // sceNgsSystemUnlock (unimplemented stub)
    case 0x251AF6A9: // sceSulphaNgsSetRackName (unimplemented stub)
    case 0x2F3F7515: // sceSulphaNgsSetSynthName (unimplemented stub)
    case 0x4CBE08F3: // sceNgsVoiceGetParamsOutOfRange (unimplemented stub)
    case 0x508975BD: // sceSulphaNgsSetVoiceName (unimplemented stub)
    case 0x534B6E3F: // sceNgsRackSetParamErrorCallback (unimplemented stub)
    case 0x54EC5B8D: // sceSulphaNgsSetSampleName (unimplemented stub)
    case 0x5ADD22DC: // sceNgsSystemSetParamErrorCallback (unimplemented stub)
    case 0x5C71FE09: // sceNgsModuleGetNumPresets (unimplemented stub)
    case 0x5FD8AEDB: // sceSulphaNgsGetDefaultConfig (unimplemented stub)
    case 0x793E3E8C: // sceSulphaNgsGetNeededMemory (unimplemented stub)
    case 0x98703DBC: // sceNgsPatchGetInfo
    case 0x9AB87E71: // sceNgsVoiceBypassModule
    case 0xAFCD824F: // sceSulphaNgsInit (unimplemented stub)
    case 0xB9D971F2: // sceNgsSystemLock (unimplemented stub)
    case 0xC58298A7: // sceNgsModuleGetPreset (unimplemented stub)
    case 0xD124BFB1: // sceSulphaNgsShutdown (unimplemented stub)
    case 0xDC7C0F05: // sceSulphaNgsTrace (unimplemented stub)
    case 0x61C4AAC4: // sceFiosOverlayResolveWithRangeSync02
    case 0x8CCA471A: // _sceFiosKernelOverlayResolveWithRangeSync
    case 0x8DAD1FED: // sceFiosKernelOverlayResolveWithRangeSync02
    case 0x02DB3F5F: // sceAudioOutOutput
    case 0xB6FDE0BA: // sceDisplayGetVcount (shared physical vblank counter)
    case 0x9423560C: // sceDisplayWaitSetFrameBuf (waits on shared display vblank)
    case 0x7D9864A8: // sceDisplayWaitSetFrameBufMulti (waits on shared display vblank)
    case 0xAB1D9466: // sceGxmGetDisplayQueueThreadIdInternal (per-process queue thread)
    case 0x5E86319A: // _sceAppMgrGetAppState (process-local events and caller memory)
    case 0x90530F2F: // sceCommonDialogUpdate (no-op update entry point)
    case 0x755FF270: // sceMsgDialogInit (caller-process memory)
    case 0xBB3BFC89: // sceMsgDialogGetResult (caller-process output)
    case 0x0150A451: // sceMsgDialogProgressBarSetMsg (caller-process memory)
    case 0xA38A4A0D: // sceNetCheckDialogInit (caller-process memory and network state)
    case 0xB05FCE9E: // sceNetCheckDialogGetResult (caller-process output)
    case 0xBF5248FA: // sceSaveDataDialogInit (caller-process IO and memory)
    case 0x19192C8B: // sceSaveDataDialogContinue (caller-process IO and memory)
    case 0x6C49924B: // sceSaveDataDialogFinish (caller-process IO and memory)
    case 0xB2FF576E: // sceSaveDataDialogGetResult (caller-process output)
    case 0xC42F41E9: // sceAppMgrSetBgmProxyApp (updates the caller's AppMgr state)
    case 0xC648EA9B: // sceAppMgrIsPidShellAndCrashed (read-only primary process query)
    case 0x0E8D27E8: // sceAppMgrGetRawPath (caller-process buffers only)
    case 0x3E98E422: // sceIoFlockForSystem (validated caller fd and synchronized file locks)
    case 0x143BC4D6: // sceKernelIsCDialogAvailable
    case 0x2F6020B7: // ksceKernelIsCDialogAvailable
    case 0x70FDE8F1: // sceRtcGetCurrentClock (caller-memory output)
    case 0x0A05E201: // sceRtcConvertLocalTimeToUtc (caller-memory input/output)
    case 0x0572EDDC: // sceRtcGetCurrentClockLocalTime (caller-memory output)
    case 0x4373B548: // __sce_aeabi_idiv0 (no process state)
    case 0x02EEDF17: // ksceKernelSetPermission (unimplemented stub)
    case 0x0733D8AE: // sceGxmFinish
    case 0x099134F5: // sceGxmUnmapVertexUsseMemory
    case 0x0C8A38E1: // sceKernelExitThread
    case 0x0FB972F9: // sceKernelGetThreadId
    case 0x01414F0B: // ksceKernelGetThreadCurrentPriority
    case 0x12FB1767: // sceAudioOutGetAdopt
    case 0x14E9DBD7: // sceClibMemcpy
    case 0x9CC2BFDF: // sceClibMemcmp
    case 0x660D1F6D: // sceClibStrncmp
    case 0xA2FB4D9D: // sceClibStrcmp
    case 0xAC595E68: // sceClibStrnlen
    case 0x614076B7: // sceClibStrchr
    case 0x6E728AAE: // sceClibStrrchr
    case 0xB54C0BE4: // sceClibStrncasecmp
    case 0xCEFDD143: // strrchr
    case 0xE265498B: // sceClibStrstr
    case 0x633CAA54: // sceGxmProgramFindParameterBySemantic
    case 0x7FFFDD7A: // _sceGxmProgramFindParameterBySemantic
    case 0xBF5E2090: // sceGxmProgramGetSize
    case 0xA949A803: // sceGxmShaderPatcherGetProgramFromId
    case 0x9C0180E1: // sceKernelDelayThreadCB
    case 0x34938242: // sceKernelDelayThreadCB200
    case 0x678374AD: // sceClibMspaceCalloc
    case 0x3B9E301A: // sceClibMspaceCreate
    case 0x3D25FCE9: // sceGxmPadHeartbeat (pointer validation only)
    case 0xAE1A21EC: // sceClibMspaceDestroy
    case 0x9C56B4D1: // sceClibMspaceFree
    case 0x86EF7680: // sceClibMspaceMalloc
    case 0x3C847D57: // sceClibMspaceMemalign
    case 0x774891D6: // sceClibMspaceRealloc
    case 0x8CC1D38E: // sceClibMspaceMallocStats (currently a no-memory-access stub)
    case 0x0C7B834B: // sceKernelWaitSema
    case 0x0A10C1C8: // sceKernelCreateMsgPipe (copies secondary name; records creator process)
    case 0xE78BCCF7: // sceKernelDeleteMsgPipe (checks creator process)
    case 0x0E1CB9F6: // sceKernelOpenMsgPipe (caller-owned reference to an openable pipe)
    case 0x70E2A6D2: // sceKernelGetMsgPipeCreatorId (caller memory; shared pipe metadata)
    case 0x0CA71EA2: // sceKernelSendMsgPipe (caller-process buffers)
    case 0xDFC670E0: // sceKernelTrySendMsgPipe (caller-process buffers)
    case 0x5615B006: // sceKernelTryReceiveMsgPipe (caller-process output buffer)
    case 0x1305A065: // sceKernelCloseMsgPipe (releases caller-owned reference)
    case 0x120AFC8C: // sceKernelUnlockLwMutex2
    case 0x174692B4: // sceKernelWaitSemaCB
    case 0x1997DC17: // sceGxmProgramParameterGetCategory
    case 0x1A372EC8: // sceKernelUnlockMutex
    case 0x1B58FA3B: // strcmp
    case 0x1BD67366: // sceKernelCreateSema
    case 0xCBE235C7: // sceKernelOpenSema (openable kernel semaphore shared across processes)
    case 0xA2D81F9E: // sceKernelCloseSema (validates shared semaphore without deleting it)
    case 0xBC996A7A: // ksceKernelMemcpyUserToKernel (caller-process memory)
    case 0x6D88EF8A: // ksceKernelMemcpyKernelToUser (caller-process memory)
    case 0x1D17DECF: // sceKernelExitDeleteThread
    case 0x1E82E5D0: // ksceKernelUnlockMutex
    case 0x20C228E4: // SceThreadmgrForDriver_20C228E4 (unimplemented stub)
    case 0x2252890C: // sceKernelPowerTick (no-op)
    case 0x244E76D2: // sceKernelDeleteLwMutex
    case 0x277794C4: // sceGxmProgramFindParameterByName
    case 0x297AA2AE: // sceKernelCreateSema_16XX
    case 0x2A43912D: // ksceKernelRegisterProcEventHandler
    case 0x2BE3E066: // sceKernelGetProcessParam
    case 0x2DCC4AFA: // sceKernelLoadStartModule (loads into the caller's address space)
    case 0x2D635A00: // sceKernelGetExtraTty
    case 0x2E3B02A1: // sceClibMemcpy_safe
    case 0x33B83B70: // __cxa_atexit
    case 0x34EFD876: // sceIoWrite
    case 0x391B74B7: // ksceDebugPrintf (checked secondary formatting)
    case 0x3CE187B6: // scePowerSetConfigurationMode
    case 0x3801D7D6: // ksceKernelCreateProcessLocalStorage
    case 0x3DED57CC: // ksceKernelUnregisterProcEventHandler
    case 0x414CC813: // ksceKernelInvokeProcEventHandler
    case 0x4010AD65: // sceKernelGetMemBlockInfoByAddr
    case 0x40C88316: // kmemcpy
    case 0x422A221A: // sceIoDclose
    case 0x48C7EAE6: // sceKernelCreateLwCond
    case 0x5341BD46: // sceGxmTextureGetData
    case 0x56085DFB: // sceAudiodecCreateDecoderExternal (process-local decoder state)
    case 0x49252B9B: // sceIoLseek32
    case 0x499EA781: // sceKernelUnlockLwMutex_0
    case 0x4B675D05: // sceKernelDelayThread
    case 0x4C4672BF: // sceKernelGetProcessTime
    case 0x4EA13FEA: // sceKernelFreeMemBlockForVM
    case 0x52315AD7: // sceIoPread
    case 0x595D3FA6: // sceKernelGetSemaInfo
    case 0x5C79D59A: // sceGxmProgramParameterGetResourceIndex
    case 0x5BC341E4: // sceAudioOutOpenPort
    case 0x5DFB9CA0: // sceAppUtilSystemParamGetInt
    case 0x607647BA: // sceAppUtilSaveDataDataSave (caller-process memory and savedata IO)
    case 0x98630136: // sceAppUtilSaveDataSlotSetParam (caller-process parameter and savedata IO)
    case 0xC3CB128B: // sceAppUtil_C3CB128B (caller-process output buffer)
    case 0xC0418B1C: // sceAppUtil_C0418B1C (per-process savedata mount state)
    case 0xCD0E5C59: // sceAppUtil_CD0E5C59 (per-process callback state)
    case 0x2DB7BE3B: // sceAppUtilDrmOpen (caller-process parameters and process-local IO)
    case 0x6A140498: // sceAppUtilDrmClose (caller-process parameters and process-local IO)
    case 0x632980D7: // sceClibMemset
    case 0x64167F11: // sceAudioOutSetVolume
    case 0x69E2E6B5: // sceAudioOutReleasePort
    case 0x57F8CD25: // sceIoGetstatByFd
    case 0x5795E898: // sceDisplayWaitVblankStart
    case 0x66D6BF05: // sceKernelCancelSema
    case 0x6C60AC61: // sceIoOpen
    case 0x72FC1F54: // sceKernelTryLockMutex
    case 0x7595D9AA: // sceKernelExitProcess (deferred AppMgr cleanup for secondary process)
    case 0x736753C8: // sceClibMemmove
    case 0x7A410B64: // sceDisplaySetFrameBuf
    case 0x7B9023C3: // sceGxmProgramParameterGetType
    case 0x80CCEDBB: // sceGxmUnmapFragmentUsseMemory
    case 0x721F6CB3: // sceKernelDeleteLwCond
    case 0x50572FDA: // sceKernelCreateCond
    case 0x879E6EBD: // sceKernelDeleteCond
    case 0xC88D44AD: // sceKernelWaitCond
    case 0x6DC1F0D8: // memset
    case 0x7205BFDB: // memcpy
    case 0x866EF048: // sceKernelPollSema
    case 0x8713D662: // _sceIoDread
    case 0x8E7E11F2: // _sceIoGetstat
    case 0x8DE6FEB7: // sceRtcGetTime_t
    case 0x8BDE825A: // sceGxmGetNotificationRegion
    case 0x811313B3: // sceRtcGetTickResolution
    case 0x828C68E8: // sceGxmUnmapMemory
    case 0x8F1ACC32: // _sceIoMkdir
    case 0x8FFFF5A8: // sceIoPwrite
    case 0x91FA6614: // sceKernelUnlockLwMutex
    case 0x9379E2D5: // _sceFiosKernelOverlayGetList
    case 0x97C4A7C4: // sceKernelDelayThread200
    case 0x9DCB4B7A: // sceKernelGetProcessId
    case 0x9D953C22: // ksceKernelLoadStartModuleForPid (same-process system modules)
    case 0x49A3EDC7: // ksceKernelStopUnloadModuleForPid (PID-scoped module lookup)
    case 0x02D3D0C1: // ksceKernelStopUnloadSharedModuleForPid (target-process module mappings)
    case 0x9A5370C4: // sceAudioOutGetRestSample
    case 0x9C8B6624: // sceIoDread
    case 0x9EF798C1: // sceKernelTryLockLwMutex_16XX
    case 0x4397FC4E: // sceKernelGetAllowedSdkVersionOnSystem (compatibility version constant)
    case 0x9F793F84: // sceKernelGetMainModuleSdkVersion
    case 0x99BA173E: // sceIoLseek
    case 0xA33B99D1: // sceKernelFindMemBlockByAddr
    case 0xA6A2C915: // sceKernelTryLockLwMutex
    case 0xA91E15EE: // sceKernelFreeMemBlock
    case 0x0AB9BF5C: // kmemset
    case 0xA9283DD0: // sceIoDopen
    case 0xAAED7419: // sceAppMgrAcquireBgmPortWithPriority
    case 0xB0F1E4EC: // sceGxmInitialize
    case 0xB627DE66: // sceGxmTerminate
    case 0xB9770A13: // SceQafMgrForDriver_B9770A13 (unimplemented stub)
    case 0xA604764A: // _sceIoLseek
    case 0xAF5C218D: // memmove
    case 0xAF80F39C: // ksceKernelGetProcessLocalStorageAddrForPid
    case 0xAFCEAB96: // sceAppMgrAcquireBgmPort
    case 0xB110C123: // sceKernelGetProcessTimeWide
    case 0xB291C959: // sceGxmGetRenderTargetMemSize
    case 0xB220B00B: // sceAppUtilShutdown
    case 0xB295EB61: // sceKernelGetTLSAddr
    case 0xB538BF48: // __cxa_finalize
    case 0xB8EF5818: // sceKernelGetMemBlockBase
    case 0xB8BA0D07: // sceAudioOutSetConfig
    case 0xB9D5EBDE: // sceKernelAllocMemBlock
    case 0xBD2998D1: // sceGxmProgramParameterGetComponentCount
    case 0xBCA5B623: // sceIoGetstat
    case 0xBECD35C8: // sceCommonDialogSetConfigParam (unimplemented stub)
    case 0xBFE02B3A: // __cxa_set_dso_handle_main
    case 0xC1727F59: // sceKernelGetStdin
    case 0xC5C11EE7: // sceKernelCreateThread
    case 0xC61E34FC: // sceGxmMapMemory
    case 0xC70B8886: // sceIoClose
    case 0xC995DE02: // sceRtcGetTime64_t
    case 0xCB78710D: // sceKernelDeleteMutex
    case 0xCC67B6FD: // _sceIoOpen
    case 0xCCDABA04: // sceAudiodecDecode (decoder state is process-local)
    case 0xDAFFE671: // sceAppUtilInit
    case 0xDBA8D061: // sceGxmProgramParameterGetArraySize
    case 0xDDB395A9: // sceKernelWaitThreadEnd
    case 0xC54941ED: // sceKernelWaitThreadEndCB (same-process thread and pinned caller outputs)
    case 0xD90FC293: // sceFiosKernelOverlayGetList02
    case 0xDB712ABC: // sceAudiodecGetContextSize (caller memory only)
    case 0xE84CE5B4: // sceGxmCreateContext
    case 0xE2ADEF8D: // ksceKernelLoadStartSharedModuleForPid (private process mappings)
    case 0xDD0A13B8: // sceDisplayWaitVblankStartMulti
    case 0xDA6EC8EF: // sceKernelCreateLwMutex
    case 0xDB32948A: // sceKernelDeleteSema
    case 0xE2D7E137: // sceKernelAllocMemBlockForVM
    case 0xE4EA05BB: // sceAudiodecDeleteDecoderExternal (process-local decoder state)
    case 0xE7A24E16: // sceAudiodecDeleteDecoder (caller-process control and decoder state)
    case 0xE20ED0F3: // sceIoRemove
    case 0xE5AA625C: // sceKernelGetStdout
    case 0xE6B761D1: // sceKernelSignalSema
    case 0xE6C53567: // _sceIoGetstatByFd
    case 0xE6E614B5: // _sceIoDopen
    case 0xE1878282: // sceKernelWaitLwCond
    case 0x8FA54B07: // sceKernelWaitLwCondCB
    case 0x3AC63B9A: // sceKernelSignalLwCond
    case 0xE5241A0C: // sceKernelSignalLwCondAll
    case 0x6ED2E2DC: // sceKernelSignalCond
    case 0xC2E7AC22: // sceKernelSignalCondAll
    case 0x1269F4EC: // sceKernelSignalCondTo
    case 0x03A48771: // sceKernelGetProcessTitleId (per-process title ID, caller-process output)
    case 0x6731BB2D: // sceVideodecQueryMemSize (bridge pointers use caller-process memory)
    case 0x97E95EDB: // sceAvcdecQueryDecoderMemSize (bridge pointers use caller-process memory)
    case 0x66F65155: // sceAvcdecDecodeGetPictureWithWorkPictureInternal (caller-process work-picture memory)
    case 0xD6190A06: // sceAvcdecDecode (process-owned decoder handle and caller-process buffers)
    case 0x25F31020: // sceAvcdecDecodeFlush (process-owned decoder handle)
    case 0x8A0E359E: // sceAvcdecDeleteDecoder (process-owned decoder handle)
    case 0x1BBDE3D9: // sceKernelDeleteThread (same-process thread only)
    case 0x3A5F4924: // sceVideodecTermLibrary (releases only caller-owned decoder state)
    case 0xF0B4C892: // sceCodecEngineOpenUnmapMemBlock
    case 0x1A3A53E5: // sceCodecEngineAllocMemoryFromUnmapMemBlock
    case 0x6A31831D: // sceCodecEngineFreeMemoryFromUnmapMemBlock
    case 0x95EA3B3E: // sceCodecEngineCloseUnmapMemBlock
    case 0xE9F973B1: // sceKernelGetProcessTimeLow
    case 0xF1AE5654: // sceKernelGetThreadCpuAffinityMask
    case 0xFFF8F7F0: // sceAppMgrIsGameProgram (existing constant result)
    case 0xE9F91EC8: // sceIoRmdir
    case 0xEA5C52F5: // _sceKernelWaitThreadEnd
    case 0xEDC939E1: // __aeabi_atexit
    case 0xEDDC5FB2: // sceGxmDestroyContext
    case 0xED53334A: // sceKernelCreateMutex
    case 0xF08DE149: // sceKernelStartThread
    case 0xF3717E37: // sceAppMgrReleaseBgmPort
    case 0x7333C2F1: // sceAppMgrSetBgmSubPriority
    case 0xF737E369: // sceIoRename
    case 0x9670D39F: // sceIoMkdir
    case 0xF4EE4FA9: // sceKernelGetSystemTimeWide
    case 0xFA5E3ADA: // sceKernelGetStderr
    case 0xFA26BC62: // sceClibPrintf
    case 0xFA437510: // sceGxmMapVertexUsseMemory
    case 0xFBAA026E: // ksceKernelCreateMutex
    case 0xFDB32293: // sceIoRead
    case 0x0C733AE2: // sceAppMgrAppParamGetString (caller-process memory)
    case 0x1DD808D1: // sceFiosOverlayGetList02 (read-only shared overlay list)
    case 0x347C1BDB: // sceRegMgrSystemParamGetInt (read-only system setting)
    case 0x7FFE2CDF: // sceRegMgrSystemParamGetBin (caller-process output)
    case 0x877ADB3F: // sceRegMgrSystemParamGetStr (caller-process output)
    case 0xD5A73557: // sceRegMgrSystemParamSetBin (caller-process input)
    case 0xCB3246E3: // sceRegMgrSystemParamSetStr (caller-process input)
    case 0x16DDF3DC: // sceRegMgrGetKeyInt (shared registry; output is caller-process memory)
    case 0x17FB1D9E: // SceNpTrophy_17FB1D9E (caller-process session output)
    case 0x62E37599: // SceNpTrophy_62E37599 (caller-process status output)
    case 0x34516838: // sceNpTrophyInit (initializes caller-process trophy state)
    case 0xBA2B7F2A: // sceNpTrophyGetGameInfo (caller-process trophy state and outputs)
    case 0xC8D2A4DE: // sceNpTrophyGetTrophyUnlockState (caller-process trophy state and outputs)
    case 0xC49FD33F: // sceNpTrophyCreateContext (caller-process state, IO, and validated guest pointers)
    case 0x97D19AB6: // SceNpTrophy_97D19AB6 (same-process title handle)
    case 0xD70DAFA7: // SceNpTrophy_D70DAFA7 (caller-process title output)
    case 0x0B98D646: // sceRegMgrGetKeyBin (shared registry; caller-process output)
    case 0xE188382F: // sceRegMgrGetKeyStr (shared registry; caller-process output)
    case 0x5B161504: // sceRegMgrGetKeys (shared registry; caller-process array)
    case 0x58421DD1: // sceRegMgrGetKeysInfo (caller-process array)
    case 0x566A1793: // sceRegMgrSetKeyBin (shared registry; caller-process input)
    case 0xD72EA399: // sceRegMgrSetKeyInt (shared registry; caller-process category and name)
    case 0x41D320C5: // sceRegMgrSetKeyStr (shared registry; caller-process input)
    case 0x34E550DA: // sceRegMgrSetKeys (shared registry; caller-process array)
    case 0xA86F0A71: // sceRegMgrAddRegistryCallback (same-process callback and caller-process path)
    case 0x4993876C: // sceAppMgrGameDataMount (caller-process memory and IO)
    case 0x4570DC15: // sceAppMgrDestroyAppByName (caller identity, deferred AppMgr cleanup)
    case 0x5E375921: // sceAppMgrUmount (caller-process memory and IO)
    case 0x5F22E192: // sceAppMgrIsNonGameProgram (caller-process identity)
    case 0x6E5CBCA2: // sceAppMgrCheckRifGD (caller-process memory)
    case 0x72067C6B: // sceSharedFbBegin (per-address-space state)
    case 0x79A0160A: // sceSysmoduleLoadModule (caller address space and process IO)
    case 0x2399BF45: // sceSysmoduleLoadModuleInternal (caller address space and process IO)
    case 0x8154D696: // sceRegMgrUtilityGetInt (read-only system setting)
    case 0xEFCFA182: // sceRegMgrUtilityGetBin (caller-process output)
    case 0x1FE8949E: // sceRegMgrUtilityGetStr (caller-process output)
    case 0x7304DC12: // sceRegMgrUtilitySetBin (caller-process input)
    case 0x4898C1E2: // sceRegMgrUtilitySetStr (caller-process input)
    case 0x819954FB: // sceSharedFbGetInfo (per-address-space state)
    case 0x8516D040: // sceKernelCreateEventFlag (kernel object, caller-independent)
    case 0x8848ADDD: // sceDisplayGetMaximumFrameBufResolution (system constant)
    case 0x8CBA03D5: // sceClibSnprintf (caller-process memory)
    case 0x9B2422E0: // sceAppMgrGetAppParam (per-process launch parameter)
    case 0x9B38EAEC: // vshKernelCheckModelCapability (system model query)
    case 0x06AD7CBB: // vshSblUtMgrHasComTestFlag (constant stub)
    case 0xE493EFF4: // vshSysconIsIduMode (read-only configuration query)
    case 0x9E57853D: // sceAppMgrAppParamGetInt (caller-process state)
    case 0xA04F5FAC: // sceGxmVshInitialize (per-process GXM state)
    case 0xA2CB322F: // sceKernelGetModelForCDialog (system model query)
    case 0xA37E6383: // sceClibStrncat (caller-process memory)
    case 0xADAA658E: // sceAppMgrConvertVs0UserDrivePath (caller-process memory)
    case 0xB358E1B6: // _sceSharedFbOpen (per-address-space state)
    case 0xBBE82155: // sceKernelLoadModule (caller address space and process IO)
    case 0x702425D5: // sceKernelStartModule (module lookup in caller address space)
    case 0xBD0139F2: // sceKernelChangeThreadPriority (thread-specific state)
    case 0xC458D60A: // sceClibStrncpy (caller-process memory)
    case 0x8EB8DFBB: // sceKernelOpenMemBlock (per-process memblock state)
    case 0xD6C643B5: // sceSharedFbCreate (per-address-space state)
    case 0xCE0F02F0: // sceKernelIsGameBudget (caller-process identity)
    case 0xEC8DDAAD: // sceKernelCDialogSetLeaseLimit (no-op stub)
    case 0x832B4A65: // sceKernelGetSubbudgetInfo (side-effect-free stub)
    case 0xB9C9C9CF: // sceIoSetThreadDefaultPriorityForSystem (no-op stub)
    case 0x54ABACFA: // sceIoIoctl (side-effect-free stub)
    case 0xFC5A62B6: // sceSharedFbEnd (per-address-space state)
    case 0xF3917021: // SceNetInternal_F3917021 (caller-process memory)
    case 0x1ADF9BB1: // sceNetAccept (process-local NetState)
    case 0x1296A94B: // sceNetBind (process-local NetState)
    case 0xFEC1166D: // sceNetClearDnsCache (process-local NetState)
    case 0x11E5B6F6: // sceNetConnect (process-local NetState)
    case 0x3B24E75F: // sceNetDumpAbort (stub)
    case 0xAFF9FA4D: // sceNetDumpCreate (stub)
    case 0x82DDCF63: // sceNetDumpDestroy (stub)
    case 0x04042925: // sceNetDumpRead (stub)
    case 0xAE3F4AC6: // sceNetEmulationGet (stub)
    case 0x561DFD03: // sceNetEmulationSet (stub)
    case 0x93FCC4E8: // sceNetEpollAbort (process-local NetState)
    case 0x4C8764AC: // sceNetEpollControl (process-local NetState)
    case 0xF9D102AE: // sceNetEpollCreate (process-local NetState)
    case 0x7915CAF3: // sceNetEpollDestroy (process-local NetState)
    case 0x45CE337D: // sceNetEpollWait (process-local NetState)
    case 0x92D3E767: // sceNetEpollWaitCB (stub)
    case 0xE37F34AA: // sceNetErrnoLoc (caller thread TLS)
    case 0x84334EB2: // sceNetEtherNtostr (caller-process memory)
    case 0xEEC6D75F: // sceNetEtherStrton (caller-process memory)
    case 0x06C05518: // sceNetGetMacAddress (caller-process output)
    case 0x138CF1D6: // sceNetGetSockIdInfo (stub)
    case 0xB1AF6840: // sceNetGetSockInfo (stub)
    case 0xA86F8FE5: // sceNetGetStatisticsInfo (stub)
    case 0x2348D353: // sceNetGetpeername (process-local NetState)
    case 0x1C66A6DB: // sceNetGetsockname (process-local NetState)
    case 0xBA652062: // sceNetGetsockopt (process-local NetState)
    case 0x4C30B03C: // sceNetHtonl (stateless conversion)
    case 0x12C19209: // sceNetHtonll (stateless conversion)
    case 0x9FA3207B: // sceNetHtons (stateless conversion)
    case 0x98839B74: // sceNetInetNtop (caller-process memory)
    case 0xD5EEB048: // sceNetInetPton (caller-process memory)
    case 0xEB03E265: // sceNetInit (process-local NetState)
    case 0x7A8DA094: // sceNetListen (process-local NetState)
    case 0xD2EAA645: // sceNetNtohl (stateless conversion)
    case 0xFB3336A6: // sceNetNtohll (stateless conversion)
    case 0x07845128: // sceNetNtohs (stateless conversion)
    case 0x023643B7: // sceNetRecv (process-local NetState)
    case 0xB226138B: // sceNetRecvfrom (process-local NetState)
    case 0xDE94C6FE: // sceNetRecvmsg (stub)
    case 0x38EBBD57: // sceNetResolverAbort (process-local NetState)
    case 0x6DA29319: // sceNetResolverCreate (process-local NetState)
    case 0x3559F098: // sceNetResolverDestroy (process-local NetState)
    case 0x874EF500: // sceNetResolverGetError (stub)
    case 0x0424AE26: // sceNetResolverStartAton (process-local NetState)
    case 0x1EB11857: // sceNetResolverStartNtoa (process-local NetState)
    case 0xE3DD8CD9: // sceNetSend (process-local NetState)
    case 0x99C579AE: // sceNetSendmsg (process-local NetState)
    case 0x52DB31D5: // sceNetSendto (process-local NetState)
    case 0xD62EF218: // sceNetSetDnsInfo (stub)
    case 0x065505CA: // sceNetSetsockopt (process-local NetState)
    case 0x658B903B: // sceNetShowIfconfig (stub)
    case 0x338EDC2E: // sceNetShowNetstat (stub)
    case 0x6AB3B74B: // sceNetShowRoute (stub)
    case 0x69E50BB5: // sceNetShutdown (process-local NetState)
    case 0xF084FCE3: // sceNetSocket (process-local NetState)
    case 0x891C1B9B: // sceNetSocketAbort (process-local NetState)
    case 0x29822B4D: // sceNetSocketClose (process-local NetState)
    case 0xEA3CC286: // sceNetTerm (process-local NetState)
    case 0xED43B79A: // sceNetCtlAdhocDisconnect (process-local NetCtlState)
    case 0x7118C99D: // sceNetCtlAdhocGetInAddr (process-local NetCtlState)
    case 0x77586C59: // sceNetCtlAdhocGetPeerList (process-local NetCtlState)
    case 0x7AE0ED19: // sceNetCtlAdhocGetResult (process-local NetCtlState)
    case 0x0961A561: // sceNetCtlAdhocGetState (process-local NetCtlState)
    case 0xFFA9D594: // sceNetCtlAdhocRegisterCallback (process-local NetCtlState)
    case 0xA4471E10: // sceNetCtlAdhocUnregisterCallback (process-local NetCtlState)
    case 0xDFFC3ED4: // sceNetCtlCheckCallback (process-local NetCtlState)
    case 0xDA130450: // sceNetCtlGetIfStat (process-local NetCtlState)
    case 0x4DDD6149: // sceNetCtlGetNatInfo (process-local NetCtlState)
    case 0x4FB4BEA1: // sceNetCtlGetPhoneMaxDownloadableSize (process-local NetCtlState)
    case 0xB26D07F3: // sceNetCtlInetGetInfo (process-local NetCtlState)
    case 0x6B20EC02: // sceNetCtlInetGetResult (process-local NetCtlState)
    case 0x6D26AC68: // sceNetCtlInetGetState (process-local NetCtlState)
    case 0xEAEE6185: // sceNetCtlInetRegisterCallback (process-local NetCtlState)
    case 0xD0C3BF3F: // sceNetCtlInetUnregisterCallback (process-local NetCtlState)
    case 0x495CA1DB: // sceNetCtlInit (process-local NetCtlState)
    case 0xCD188648: // sceNetCtlTerm (process-local NetCtlState)
    case 0x719DD1DA: // SceNetCtl_719DD1DA (process-local NetCtlState)
    case 0x50AE9FF9: // sceScreenShotDisable (no guest pointers)
    case 0x58E4CC90: // _sceAppMgrWorkDirMountById (caller-process IO and memory)
    case 0x80E86E42: // sceAppMgrWorkDirMountById (caller-process IO and memory)
    case 0x656E0ABD: // sceAppMgrGetStatusByName (caller-process output)
    case 0x7AA73378: // sceKernelPowerLock (unimplemented stub)
    case 0x466C0CBD: // sceKernelPowerUnlock (unimplemented stub)
    case 0xCC18FBAE: // sceKernelChangeThreadVfpException (current-thread TLS)
    case 0x3E9172EA: // _sceFiosKernelOverlayThreadSetDisabled (unimplemented stub)
    case 0x08769991: // _sceUlobjMgrStartSupportingUserlevelObject (unimplemented stub)
    case 0x50F2F2AA: // _sceUlobjMgrRegisterLibultProtocolRevision (unimplemented stub)
    case 0x8D9C5461: // sceKernelGetThreadInfo (same-process thread and caller output)
    case 0xADCA94E5: // sceKernelWaitSignal (current thread and caller timeout)
    case 0xD4C367B2: // sceKernelSendSignal (same-process target thread)
    case 0xE0C1DB92: // sceTouchGetPixelDensity2 (caller-process output)
    case 0xB18370C2: // sceTouchEnableTouchForce (shared touch-device setting)
    case 0x6E4997D7: // sceAppMgrSendNotificationRequest (unimplemented stub)
    case 0x0039BE45: // sceKernelLibcTime (shared clock and caller output)
    case 0xE737B1DF: // sceKernelWaitEventFlagCB (kernel event flag and caller output)
    case 0x9246A673: // sceNpLookupInit (unimplemented stub)
    case 0x441D8B4E: // sceNpAuthInit (unimplemented stub)
    case 0x805CC20F: // sceVoiceInit (unimplemented stub)
    case 0xB02E0B26: // _sceFiosKernelOverlayGetRecommendedScheduler (unimplemented stub)
    case 0xB98C5B0D: // sceGxmDisplayQueueFinish (waits on the caller's GXM queue)
    case 0xF1AF65A3: // sceVideodecInitLibrary (process-owned decoder state)
    case 0x4EBC6977: // sceNpTrophyCreateHandle (caller-process output)
    case 0x60C575B1: // sceNpManagerGetChatRestrictionFlag (caller-process output)
    case 0xAF0073B2: // sceNpManagerGetContentRatingFlag (caller-process output)
    case 0x3C94B4B4: // sceNpManagerGetNpId (caller-process output)
    case 0xE45E4003: // SceNpManager_E45E4003 (caller-process address)
    case 0xD6758B4E: // sceAppMgrSetInfobarStateForCommonDialog (unimplemented stub)
    case 0x433D71F4: // sceCtrlSetButtonIntercept (unimplemented stub)
    case 0xB56413BB: // sceGxmRenderingContextIsWithinSceneInternal (caller GXM state)
    case 0x8A1A6FFC: // sceAppMgrGetRecommendedScreenOrientation (unimplemented stub)
    case 0x5C7BE748: // sceAppMgrSetShellScreenOrientation (unimplemented stub)
    case 0x334E9FD3: // vshMotionNoiseFilterIsAvailable (unimplemented stub)
    case 0x743A066F: // sceHidKeyboardRegisterEnumHintCallback (unimplemented stub)
    case 0x1C740A7A: // sceHidKeyboardRegisterReadHintCallback (unimplemented stub)
    case 0x6184B939: // sceAudioOutOpenExtPort (unimplemented stub)
    case 0xF45AA706: // sceSblQafMgrIsAllowRemoteSysmoduleLoad (unimplemented stub)
    case 0xB680E3A0: // sceKernelCloseMemBlock (unimplemented stub)
    case 0xDFA8BAF8: // scePowerUnregisterCallback (checks callback address-space ownership)
    case 0xD469676B: // sceKernelDeleteCallback (checks callback address-space ownership)
    case 0x8068D61E: // sceHidKeyboardUnregisterEnumHintCallback (unimplemented stub)
    case 0xF73C5FBA: // sceHidKeyboardUnregisterReadHintCallback (unimplemented stub)
    case 0xF103AF8A: // sceGxmShaderPatcherUnregisterProgram (caller-process memory and callback)
    case 0x49553737: // sceGxmRenderTargetGetDriverMemBlock (caller-process memory)
    case 0x0B94C50A: // sceGxmDestroyRenderTarget (caller-process GXM state)
    case 0xEAA5B100: // sceGxmShaderPatcherDestroy (caller-process memory and callback)
    case 0xC29B9B82: // sceGxmUnmapFragmentUsseMemoryInternal (unimplemented stub)
    case 0xD7506735: // sceGxmUnmapVertexUsseMemoryInternal (unimplemented stub)
    case 0xDB4CC1D0: // sceKernelCDialogSessionClose (unimplemented stub)
    case 0x54060DF6: // sceNpGetServiceState (shared service state and caller output)
    case 0x44239C35: // sceNpRegisterServiceStateCallback (process-owned callback)
    case 0x3B0AE9A9: // sceNpCheckCallback (process-owned callbacks)
    case 0xD9E6E56C: // sceNpUnregisterServiceStateCallback (process-owned callback)
    case 0x19E40AE1: // sceNpTerm (process-owned NP state)
    case 0xAD6403E0: // sceNpCommonGetNpEnviroment (unimplemented stub)
    case 0xAB1E42C4: // sceKernelDeleteTimer (checks timer process ownership)
    case 0x17653D70: // sceAppMgrSaveDataSlotGetParam (caller-process IO and memory)
    case 0xE6057A85: // sceAppUtilSaveDataSlotSearch (caller-process IO and memory)
    case 0x28034AC9: // sceMotionStartSampling (shared sensor state)
    case 0xAF32CB1D: // sceMotionStopSampling (shared sensor state)
    case 0x0FD2CDA2: // sceMotionReset (shared sensor state)
        return true;
    case 0xC3C26339: // sceSysmoduleLoadModuleInternalWithArg
        return arg0 == SCE_SYSMODULE_INTERNAL_PAF;
    case 0x46E7BE7B: // sceKernelLockLwMutex
    case 0xA7819967: // sceKernelLockLwMutex_0
    case 0x3148C6B6: // sceKernelLockLwMutexCB (caller-process work area)
    case 0x1D8D7945: // sceKernelLockMutex
    case 0x16AC80C5: // ksceKernelLockMutex
        return arg2 == 0;
    default:
        return false;
    }
}