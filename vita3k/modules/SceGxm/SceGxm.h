// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <module/module.h>

struct EmuEnvState;
struct GxmState;
struct MemState;
struct SceGxmInitializeParams;
struct SceGxmRenderTargetParams;
struct SceGxmRenderTarget;
struct SceGxmSyncObject;

SceInt32 terminate_secondary_process_gxm(EmuEnvState &emuenv, MemState &mem, SceUID caller_id);
SceInt32 sce_gxm_create_render_target(EmuEnvState &emuenv, SceUID thread_id, const SceGxmRenderTargetParams *params, Ptr<SceGxmRenderTarget> *renderTarget, bool internal_api, const char *export_name);
bool sce_gxm_rendering_context_is_within_scene(EmuEnvState &emuenv, SceUID thread_id);
bool sce_gxm_sync_object_is_within_scene(GxmState &gxm, Ptr<SceGxmSyncObject> syncObject);

DECL_EXPORT(int, sceGxmInitialize, const SceGxmInitializeParams *params);
DECL_EXPORT(int, sceGxmSyncObjectDestroy, Ptr<SceGxmSyncObject> syncObject);
DECL_EXPORT(int, sceGxmCreateRenderTarget, const SceGxmRenderTargetParams *params, Ptr<SceGxmRenderTarget> *renderTarget);
DECL_EXPORT(int, sceGxmGetRenderTargetMemSize, Ptr<const SceGxmRenderTargetParams> params, Ptr<uint32_t> hostMemSize);
DECL_EXPORT(int, sceGxmMapFragmentUsseMemory, Ptr<void> base, uint32_t size, Ptr<uint32_t> offset);
DECL_EXPORT(int, sceGxmMapVertexUsseMemory, Ptr<void> base, uint32_t size, Ptr<uint32_t> offset);
