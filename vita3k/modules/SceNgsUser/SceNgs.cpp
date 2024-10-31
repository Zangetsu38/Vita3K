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

#include <module/module.h>

#include "../SceProcessmgr/SceProcessmgr.h"

#include <audio/state.h>
#include <kernel/state.h>
#include <ngs/modules/compressor.h>
#include <ngs/modules/mixer.h>
#include <ngs/modules/pauser.h>
#include <ngs/modules/player.h>
#include <ngs/modules/reverb.h>
#include <ngs/state.h>
#include <ngs/system.h>
#include <util/log.h>
#include <util/tracy.h>
#include <util/vector_utils.h>

#include <algorithm>

TRACY_MODULE_NAME(SceNgs);

struct SceNgsVolumeMatrix {
    SceFloat32 matrix[SCE_NGS_MAX_SYSTEM_CHANNELS][SCE_NGS_MAX_SYSTEM_CHANNELS];
};

struct SceNgsPatchAudioPropInfo {
    SceInt32 out_channels;
    SceInt32 in_channels;
    SceNgsVolumeMatrix volume_matrix;
};

struct SceNgsVoiceInfo {
    SceUInt32 voice_state;
    SceUInt32 num_modules;
    SceUInt32 num_inputs;
    SceUInt32 num_outputs;
    SceUInt32 num_patches_per_output;
    SceUInt32 update_passed;
};

static_assert(sizeof(SceNgsPatchAudioPropInfo) == 24);

struct SceNgsPatchDeliveryInfo {
    Ptr<ngs::Voice> source_voice_handle;
    SceInt32 output_index;
    SceInt32 output_subindex;
    Ptr<ngs::Voice> dest_voice_handle;
    SceInt32 input_index;
};

static_assert(sizeof(SceNgsPatchDeliveryInfo) == 20);

enum SceNgsErrorCode : uint32_t {
    SCE_NGS_OK = 0,
    SCE_NGS_ERROR = 0x804A0001,
    SCE_NGS_ERROR_INVALID_ARG = 0x804A0002,
    SCE_NGS_ERROR_INVALID_STATE = 0x804A0010,
    SCE_NGS_ERROR_PARAM_OUT_OF_RANGE = 0x804A0009,
    SCE_NGS_ERROR_INVALID_HANDLE = 0x804A000C,
    SCE_NGS_SIZE_MISMATCH = 0x804A000D
};

enum SceNgsVoiceState : uint32_t {
    SCE_NGS_VOICE_STATE_AVAILABLE = 0,
    SCE_NGS_VOICE_STATE_ACTIVE = 1 << 0,
    SCE_NGS_VOICE_STATE_FINALIZE = 1 << 2,
    SCE_NGS_VOICE_STATE_UNLOADING = 1 << 3,
    SCE_NGS_VOICE_STATE_PENDING = 1 << 4,
    SCE_NGS_VOICE_STATE_PAUSED = 1 << 5,
    SCE_NGS_VOICE_STATE_KEY_OFF = 1 << 6
};

static constexpr SceUInt32 SCE_NGS_MODULE_FLAG_NOT_BYPASSED = 0;
static constexpr SceUInt32 SCE_NGS_MODULE_FLAG_BYPASSED = 2;

static const char *ngsBussTypeName(const ngs::BussType type) {
    switch (type) {
    case ngs::BussType::BUSS_MASTER:
        return "MASTER";
    case ngs::BussType::BUSS_COMPRESSOR:
        return "COMPRESSOR";
    case ngs::BussType::BUSS_SIDE_CHAIN_COMPRESSOR:
        return "SIDE_CHAIN_COMPRESSOR";
    case ngs::BussType::BUSS_DELAY:
        return "DELAY";
    case ngs::BussType::BUSS_DISTORTION:
        return "DISTORTION";
    case ngs::BussType::BUSS_ENVELOPE:
        return "ENVELOPE";
    case ngs::BussType::BUSS_EQUALIZATION:
        return "EQUALIZATION";
    case ngs::BussType::BUSS_MIXER:
        return "MIXER";
    case ngs::BussType::BUSS_PAUSER:
        return "PAUSER";
    case ngs::BussType::BUSS_PITCH_SHIFT:
        return "PITCH_SHIFT";
    case ngs::BussType::BUSS_REVERB:
        return "REVERB";
    case ngs::BussType::BUSS_SAS_EMULATION:
        return "SAS_EMULATION";
    case ngs::BussType::BUSS_SIMPLE:
        return "SIMPLE";
    case ngs::BussType::BUSS_ATRAC9:
        return "ATRAC9";
    case ngs::BussType::BUSS_SIMPLE_ATRAC9:
        return "SIMPLE_ATRAC9";
    case ngs::BussType::BUSS_SCREAM:
        return "SCREAM";
    case ngs::BussType::BUSS_SCREAM_ATRAC9:
        return "SCREAM_ATRAC9";
    case ngs::BussType::BUSS_NORMAL_PLAYER:
        return "NORMAL_PLAYER";
    default:
        return "UNKNOWN";
    }
}

static bool ngsVoiceHasActivePatch(const ngs::Voice *voice, MemState &mem);
static bool ngsVoiceHasAudibleOutput(const ngs::Voice *voice, int granularity);
static ngs::Voice *ngsFindMasterVoice(ngs::System *system, MemState &mem);
static bool ngsTryReplaceImplicitMasterPatch(ngs::Voice *source, ngs::Voice *dest, SceNgsPatchSetupInfo *patch_info, Ptr<ngs::Patch> *handle, MemState &mem);
static SceUInt32 ngsVoiceStateFromHLEState(const ngs::Voice *voice);

// static ngs::Patch *g_last_created_patch = nullptr;

enum SceNgsVoiceInitFlag {
    SCE_NGS_VOICE_INIT_BASE = 0,
    SCE_NGS_VOICE_INIT_ROUTING = 1,
    SCE_NGS_VOICE_INIT_PRESET = 2,
    SCE_NGS_VOICE_INIT_CALLBACKS = 4,
    SCE_NGS_VOICE_INIT_ALL = 7
};

static constexpr uint32_t SCE_NGS_SAMPLE_OFFSET_FROM_AT9_HEADER = 1 << 31;

template <typename T>
static void write_default_module_preset(void *params_buffer, const T &params) {
    memcpy(params_buffer, &params, sizeof(T));
}

static void ngs_mark_rack_released(ngs::Rack *rack, MemState &mem, const bool remove_from_queue) {
    rack->is_released = true;

    for (const auto &voice : rack->voices) {
        ngs::Voice *voice_ptr = voice.get(mem);
        rack->system->voice_scheduler.released_voices_during_update.insert(voice_ptr);

        if (remove_from_queue) {
            rack->system->voice_scheduler.deque_voice(voice_ptr);
        }
    }
}

static void ngs_queue_rack_release(ngs::State &state, ngs::Rack *rack, const Ptr<void> callback, bool *completed = nullptr) {
    ngs::OperationPending op;
    op.type = ngs::PendingType::ReleaseRack;
    op.system = rack->system;
    op.release_data.state = &state;
    op.release_data.rack = rack;
    op.release_data.generation = rack->generation;
    op.release_data.completed = completed;
    op.release_data.callback = callback.address();
    rack->system->voice_scheduler.operations_pending.push(op);
}

EXPORT(int, sceNgsAT9GetSectionDetails, uint32_t samples_start, const uint32_t num_samples, uint32_t config_data, SceNgsAT9SkipBufferInfo *info) {
    TRACY_FUNC(sceNgsAT9GetSectionDetails, samples_start, num_samples, config_data, info);
    if (!emuenv.cfg.current_config.ngs_enable)
        return -1;

    if (!info)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    // Check magic!
    if ((config_data & 0xFF) != 0xFE)
        return RET_ERROR(SCE_NGS_ERROR);

    // the following content is reverse engineered
    const uint8_t sample_rate_index = ((config_data & (0b1111 << 12)) >> 12);
    const uint32_t frame_bytes = ((((config_data & 0xFF0000) >> 16) << 3) | ((config_data & (0b111 << 29)) >> 29)) + 1;

    int nb_samples_per_frame;
    if (sample_rate_index == 1)
        nb_samples_per_frame = 64;
    else if (sample_rate_index == 4)
        nb_samples_per_frame = 128;
    else if (sample_rate_index == 7)
        nb_samples_per_frame = 256;
    else
        return RET_ERROR(SCE_NGS_ERROR);

    const bool is_superframe = static_cast<bool>(config_data & (0b11 << 27));

    // SCE_NGS_SAMPLE_OFFSET_FROM_AT9_HEADER was added in sdk 3.36
    const int sdk_version = CALL_EXPORT(sceKernelGetMainModuleSdkVersion);
    const bool is_sdk_recent = sdk_version >= (336 << 16);

    if (is_sdk_recent && (samples_start & SCE_NGS_SAMPLE_OFFSET_FROM_AT9_HEADER)) {
        samples_start &= ~SCE_NGS_SAMPLE_OFFSET_FROM_AT9_HEADER;

        // remove nb_samples_per_frame from it
        samples_start = (samples_start > nb_samples_per_frame) ? (samples_start - nb_samples_per_frame) : 0;
    }

    info->is_super_packet = static_cast<bool>(is_superframe);
    if (is_superframe) {
        // there are 4 frames per superframe
        const int superframe_bytes = frame_bytes * 4;
        const int nb_samples_per_superframe = nb_samples_per_frame * 4;

        samples_start += nb_samples_per_frame;

        const int superframes_offset = samples_start / nb_samples_per_superframe;

        info->start_byte_offset = superframes_offset * superframe_bytes;

        const int start_skip_samples = samples_start - superframes_offset * nb_samples_per_superframe;
        info->start_skip = static_cast<SceInt16>(start_skip_samples);

        const int total_superframes = (samples_start + num_samples + nb_samples_per_superframe - 1) / nb_samples_per_superframe;
        const int total_bytes_read = (total_superframes - superframes_offset) * superframe_bytes;
        info->num_bytes = total_bytes_read;

        info->end_skip = static_cast<SceInt16>(total_superframes * nb_samples_per_superframe - (samples_start + num_samples));

        // some special case, make sure to put a good amound of skipped samples
        if (start_skip_samples < nb_samples_per_frame && superframes_offset > 0) {
            // transfer one superframe into the skipped samples
            info->start_byte_offset -= superframe_bytes;
            info->start_skip += nb_samples_per_superframe;
            info->num_bytes += superframe_bytes;
        }
    } else {
        const int frames_offset = samples_start / nb_samples_per_frame;
        info->start_byte_offset = frames_offset * frame_bytes;
        // a frame is always added to skip
        const int start_skip_samples = (samples_start + nb_samples_per_frame) - frames_offset * nb_samples_per_frame;
        info->start_skip = static_cast<SceInt16>(start_skip_samples);

        const int total_frames = (start_skip_samples + num_samples + nb_samples_per_frame - 1) / nb_samples_per_frame;
        info->num_bytes = total_frames * frame_bytes;

        info->end_skip = static_cast<SceInt16>(total_frames * nb_samples_per_frame - (start_skip_samples + num_samples));
    }
    return 0;
}

EXPORT(int, sceNgsModuleGetNumPresets, ngs::System *system, const SceUInt32 module, SceUInt32 *num_presets) {
    TRACY_FUNC(sceNgsModuleGetNumPresets, system, module, num_presets);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNgsModuleGetPreset, ngs::System *system, const SceUInt32 module, const SceUInt32 preset_index, void *params_buffer) {
    TRACY_FUNC(sceNgsModuleGetPreset, system, module, preset_index, params_buffer);

    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!params_buffer)
        return SCE_NGS_ERROR_INVALID_ARG;

    switch (module) {
    case 0x5CE1: {
        SceNgsCompressorParams params{};
        params.desc.id = SCE_NGS_COMPRESSOR_PARAMS_STRUCT_ID;
        params.desc.size = sizeof(params);
        params.fRatio = 1.0f;
        write_default_module_preset(params_buffer, params);
        break;
    }
    case 0x5CE5: {
        SceNgsPauserParams params{};
        params.desc.id = SCE_NGS_PAUSER_PARAMS_STRUCT_ID;
        params.desc.size = sizeof(params);
        write_default_module_preset(params_buffer, params);
        break;
    }
    case 0x5CE6: {
        SceNgsPlayerParams params{};
        params.descriptor.id = SCE_NGS_PLAYER_PARAMS_STRUCT_ID;
        params.descriptor.size = sizeof(params);
        params.playback_frequency = system ? static_cast<float>(system->sample_rate) : 48000.0f;
        params.playback_scalar = 1.0f;
        params.channels = 2;
        params.channel_map[0] = 0;
        params.channel_map[1] = 1;
        params.type = ParameterAudioTypePCM;
        write_default_module_preset(params_buffer, params);
        break;
    }
    case 0x5CE7: {
        SceNgsReverbParams params{};
        params.desc.id = SCE_NGS_REVERB_PARAMS_STRUCT_ID;
        params.desc.size = sizeof(params);
        params.fDiffusion = 1.0f;
        params.fDensity = 1.0f;
        write_default_module_preset(params_buffer, params);
        break;
    }
    case 0x5CE9: {
        SceNgsMixerParams params{};
        params.desc.id = SCE_NGS_MIXER_PARAMS_STRUCT_ID;
        params.desc.size = sizeof(params);
        params.fGainIn[0] = 1.0f;
        params.fGainIn[1] = 1.0f;
        write_default_module_preset(params_buffer, params);
        break;
    }
    default:
        memset(params_buffer, 0, sizeof(SceNgsParamsDescriptor));
        LOG_DEBUG("sceNgsModuleGetPreset: returning zeroed params for unknown module={} preset_index={}",
            module, preset_index);
        break;
    }

    LOG_DEBUG("sceNgsModuleGetPreset: returning empty preset for module={} preset_index={}",
        module, preset_index);

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsPatchCreateRouting, SceNgsPatchSetupInfo *patch_info, Ptr<ngs::Patch> *handle) {
    TRACY_FUNC(sceNgsPatchCreateRouting, patch_info, handle);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!patch_info || !handle) {
        // LOG_DEBUG("NGS patch create failed: null patch_info or handle");
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (!patch_info->source || !patch_info->dest) {
        // LOG_DEBUG("NGS patch create failed: null source or dest handle");
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    // Make the scheduler order this right based on dependencies request
    ngs::Voice *source = patch_info->source.get(emuenv.mem);
    //  ngs::Voice *dest = patch_info->dest.get(emuenv.mem);

    if (!source /* || !dest*/) {
        // LOG_DEBUG("NGS patch create failed: unresolved source or dest voice (source={} dest={})", source != nullptr, dest != nullptr);
        return RET_ERROR(SCE_NGS_ERROR);
    }

    *handle = source->rack->system->voice_scheduler.patch(emuenv.mem, patch_info);

    if (!*handle) {
        /*      if (ngsTryReplaceImplicitMasterPatch(source, dest, patch_info, handle, emuenv.mem)) {
                    g_last_created_patch = handle->get(emuenv.mem);
                    return SCE_NGS_OK;
                }
        */
        // LOG_DEBUG("NGS patch create failed: {} {}:{} -> {} in={}",
        //     ngsBussTypeName(source->rack->vdef->type), patch_info->source_output_index, patch_info->source_output_subindex,
        //     ngsBussTypeName(dest->rack->vdef->type), patch_info->dest_input_index);
        return RET_ERROR(SCE_NGS_ERROR);
    }

    // g_last_created_patch = handle->get(emuenv.mem);
    //  LOG_DEBUG("NGS patch created: {} {}:{} -> {} in={} (source_state={} dest_state={})",
    //  ngsBussTypeName(source->rack->vdef->type), patch_info->source_output_index, patch_info->source_output_subindex,
    //  dest ? ngsBussTypeName(dest->rack->vdef->type) : "UNKNOWN", patch_info->dest_input_index,
    //  ngsVoiceStateFromHLEState(source), dest ? ngsVoiceStateFromHLEState(dest) : 0);

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsPatchGetInfo, ngs::Patch *patch, SceNgsPatchAudioPropInfo *prop_info, SceNgsPatchDeliveryInfo *deli_info) {
    TRACY_FUNC(sceNgsPatchGetInfo, patch, prop_info, deli_info);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!patch) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (prop_info) {
        memcpy(prop_info->volume_matrix.matrix, patch->volume_matrix, sizeof(patch->volume_matrix));
        prop_info->in_channels = patch->dest->rack->channels_per_voice;
        prop_info->out_channels = patch->source->rack->channels_per_voice;
    }

    if (deli_info) {
        deli_info->input_index = patch->dest_index;
        deli_info->output_index = patch->output_index;
        deli_info->output_subindex = patch->output_sub_index;
        deli_info->source_voice_handle = Ptr<ngs::Voice>(patch->source, emuenv.mem);
        deli_info->dest_voice_handle = Ptr<ngs::Voice>(patch->dest, emuenv.mem);
    }

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsPatchRemoveRouting, Ptr<ngs::Patch> patch) {
    TRACY_FUNC(sceNgsPatchRemoveRouting, patch);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!patch) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (!patch.get(emuenv.mem)->source->remove_patch(emuenv.mem, patch)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return 0;
}

EXPORT(int, sceNgsRackGetRequiredMemorySize, ngs::System *system, SceNgsRackDescription *description, uint32_t *size) {
    TRACY_FUNC(sceNgsRackGetRequiredMemorySize, system, description, size);
    if (!emuenv.cfg.current_config.ngs_enable) {
        *size = 1;
        return 0;
    }

    if (!system) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    }
    if (!description || !description->definition || !size)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    auto definition = description->definition.get(emuenv.mem);
    if (definition->output_count == 0 || definition->type >= ngs::BussType::BUSS_MAX)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (description->voice_count <= 0 || description->channels_per_voice < 0 || description->channels_per_voice > 2
        || description->max_patches_per_input < 0 || description->patches_per_output < 0) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }
    *size = ngs::Rack::get_required_memspace_size(emuenv.mem, description);
    return 0;
}

EXPORT(SceUInt32, sceNgsRackGetVoiceHandle, ngs::Rack *rack, const uint32_t index, Ptr<ngs::Voice> *voice) {
    TRACY_FUNC(sceNgsRackGetVoiceHandle, rack, index, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!rack || !voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (index >= rack->voices.size()) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    *voice = rack->voices[index];
    // LOG_DEBUG("NGS rack get voice handle: buss={} index={} handle={:#X}", ngsBussTypeName(rack->vdef->type), index, voice->address());
    return SCE_NGS_OK;
}

EXPORT(SceUInt32, sceNgsRackInit, ngs::System *system, SceNgsBufferInfo *info, const SceNgsRackDescription *description, Ptr<ngs::Rack> *rack) {
    TRACY_FUNC(sceNgsRackInit, system, info, description, rack);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }
    if (!info || !system || !description || !description->definition || !rack) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (!ngs::init_rack(emuenv.ngs, emuenv.mem, system, info, description)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    *rack = info->data.cast<ngs::Rack>();
    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsRackRelease, ngs::Rack *rack, Ptr<void> callback) {
    TRACY_FUNC(sceNgsRackRelease, rack, callback);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!rack)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    std::unique_lock<std::recursive_mutex> lock(rack->system->voice_scheduler.mutex);
    if (rack->is_released)
        return SCE_NGS_OK;

    if (!rack->system->voice_scheduler.is_updating) {
        ngs::release_rack(emuenv.ngs, emuenv.mem, rack->system, rack);
    } else if (!callback) {
        if (rack->system->voice_scheduler.updating_thread_id == thread_id) {
            ngs_mark_rack_released(rack, emuenv.mem, false);

            ngs::release_rack(emuenv.ngs, emuenv.mem, rack->system, rack);
            return SCE_NGS_OK;
        }

        ngs_mark_rack_released(rack, emuenv.mem, true);

        bool completed = false;
        ngs_queue_rack_release(emuenv.ngs, rack, Ptr<void>(), &completed);

        rack->system->voice_scheduler.condvar.wait(lock, [&completed] {
            return completed;
        });
    } else {
        ngs_mark_rack_released(rack, emuenv.mem, true);

        ngs_queue_rack_release(emuenv.ngs, rack, callback);
    }

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsRackSetParamErrorCallback) {
    TRACY_FUNC(sceNgsRackSetParamErrorCallback);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNgsSystemGetRequiredMemorySize, SceNgsSystemInitParams *params, uint32_t *size) {
    TRACY_FUNC(sceNgsSystemGetRequiredMemorySize, params, size);
    if (!params || !size)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (!emuenv.cfg.current_config.ngs_enable) {
        *size = 1;
        return 0;
    }
    *size = ngs::System::get_required_memspace_size(params); // System struct size
    return 0;
}

EXPORT(SceUInt32, sceNgsSystemInit, Ptr<void> memspace, const uint32_t memspace_size, SceNgsSystemInitParams *params,
    Ptr<ngs::System> *system) {
    TRACY_FUNC(sceNgsSystemInit, memspace, memspace_size, params, system);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!ngs::init_system(emuenv.ngs, emuenv.mem, params, memspace, memspace_size)) {
        return RET_ERROR(SCE_NGS_ERROR); // TODO: Better error code
    }

    *system = memspace.cast<ngs::System>();
    return SCE_NGS_OK;
}

EXPORT(int, sceNgsSystemLock) {
    TRACY_FUNC(sceNgsSystemLock);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceNgsSystemRelease, ngs::System *system) {
    TRACY_FUNC(sceNgsSystemRelease, system);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!system)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    {
        std::unique_lock<std::recursive_mutex> lock(system->voice_scheduler.mutex);
        if (system->voice_scheduler.is_updating) {
            LOG_WARN_ONCE("sceNgsSystemRelease called during a ngs update, contact devs if your game softlocks now.");

            system->voice_scheduler.condvar.wait(lock);
        }
    }

    ngs::release_system(emuenv.ngs, emuenv.mem, system);

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsSystemSetFlags, ngs::System *system, const SceUInt32 system_flags) {
    TRACY_FUNC(sceNgsSystemSetFlags, system, system_flags);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!system)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    system->flags = system_flags;
    LOG_DEBUG("sceNgsSystemSetFlags: flags={:#X}", system_flags);

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsSystemSetParamErrorCallback) {
    TRACY_FUNC(sceNgsSystemSetParamErrorCallback);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNgsSystemUnlock) {
    TRACY_FUNC(sceNgsSystemUnlock);
    return UNIMPLEMENTED();
}

EXPORT(SceUInt32, sceNgsSystemUpdate, ngs::System *system) {
    TRACY_FUNC(sceNgsSystemUpdate, system);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }
    system->voice_scheduler.update(emuenv.kernel, emuenv.mem, thread_id);

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceBypassModule, ngs::Voice *voice, const SceUInt32 module, const SceUInt32 bypass_flag) {
    TRACY_FUNC(sceNgsVoiceBypassModule, voice, module, bypass_flag);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    ngs::ModuleData *storage = voice->module_storage(module);

    if (!storage)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    // no need to lock a mutex for this
    storage->is_bypassed = (bypass_flag & SCE_NGS_MODULE_FLAG_BYPASSED);

    return SCE_NGS_OK;
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetAtrac9Voice) {
    TRACY_FUNC(sceNgsVoiceDefGetAtrac9Voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_ATRAC9);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetCompressorBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetCompressorBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_COMPRESSOR);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetCompressorSideChainBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetCompressorSideChainBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_SIDE_CHAIN_COMPRESSOR);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetDelayBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetDelayBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_DELAY);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetDistortionBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetDistortionBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_DISTORTION);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetEnvelopeBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetEnvelopeBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_ENVELOPE);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetEqBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetEqBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_EQUALIZATION);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetMasterBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetMasterBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_MASTER);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetMixerBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetMixerBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_MIXER);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetPauserBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetPauserBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_PAUSER);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetPitchShiftBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetPitchShiftBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_PITCH_SHIFT);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetReverbBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetReverbBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_REVERB);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetSasEmuVoice) {
    TRACY_FUNC(sceNgsVoiceDefGetSasEmuVoice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_SAS_EMULATION);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetScreamAtrac9Voice) {
    TRACY_FUNC(sceNgsVoiceDefGetScreamAtrac9Voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_SCREAM_ATRAC9);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetScreamVoice) {
    TRACY_FUNC(sceNgsVoiceDefGetScreamVoice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_SCREAM);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetSimpleAtrac9Voice) {
    TRACY_FUNC(sceNgsVoiceDefGetSimpleAtrac9Voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_SIMPLE_ATRAC9);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetSimpleVoice) {
    TRACY_FUNC(sceNgsVoiceDefGetSimpleVoice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_SIMPLE);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetTemplate1) {
    TRACY_FUNC(sceNgsVoiceDefGetTemplate1);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    return ngs::get_voice_definition(emuenv.ngs, emuenv.mem, ngs::BussType::BUSS_NORMAL_PLAYER);
}

/*
static bool ngsVoiceHasActivePatch(const ngs::Voice *voice, MemState &mem) {
    for (const auto &patches : voice->patches) {
        for (const auto &patch_ptr : patches) {
            if (patch_ptr && patch_ptr.get(mem)->output_sub_index != -1) {
                return true;
            }
        }
    }

    return false;
}

static bool ngsVoiceHasAudibleOutput(const ngs::Voice *voice, const int granularity) {
    const float *source_data = reinterpret_cast<const float *>(voice->products[0].data);

    for (int i = 0; i < granularity * 2; i++) {
        if (std::abs(source_data[i]) > 0.0001f) {
            return true;
        }
    }

    return false;
}


static ngs::Voice *ngsFindMasterVoice(ngs::System *system, MemState &mem) {
    if (!system) {
        return nullptr;
    }

    for (ngs::Rack *rack : system->racks) {
        if (rack && rack->vdef && rack->vdef->type == ngs::BussType::BUSS_MASTER && !rack->voices.empty()) {
            return rack->voices[0].get(mem);
        }
    }

    return nullptr;
}

static bool ngsTryReplaceImplicitMasterPatch(ngs::Voice *source, ngs::Voice *dest, SceNgsPatchSetupInfo *patch_info, Ptr<ngs::Patch> *handle, MemState &mem) {
    if (!source || !dest || !patch_info || !handle || !source->rack || !source->rack->system) {
        return false;
    }

    if (patch_info->source_output_index < 0 || patch_info->source_output_index >= static_cast<SceInt32>(source->patches.size())) {
        return false;
    }

    ngs::Voice *master_voice = ngsFindMasterVoice(source->rack->system, mem);
    if (!master_voice || dest == master_voice) {
        return false;
    }

    Ptr<ngs::Patch> implicit_master_patch;
    int active_patch_count = 0;

    for (const Ptr<ngs::Patch> &patch_ptr : source->patches[patch_info->source_output_index]) {
        if (!patch_ptr) {
            continue;
        }

        ngs::Patch *patch = patch_ptr.get(mem);
        if (!patch || patch->output_sub_index == -1) {
            continue;
        }

        active_patch_count++;

        if (patch->dest == master_voice) {
            implicit_master_patch = patch_ptr;
            continue;
        }

        return false;
    }

    if (!implicit_master_patch || active_patch_count != 1) {
        return false;
    }

    ngs::Patch *patch = implicit_master_patch.get(mem);
    if (patch_info->source_output_subindex != -1 && patch->output_sub_index != patch_info->source_output_subindex) {
        return false;
    }

    const SceInt32 previous_subindex = patch->output_sub_index;
    patch->output_sub_index = -1;

    *handle = source->rack->system->voice_scheduler.patch(mem, patch_info);
    if (!*handle) {
        patch->output_sub_index = previous_subindex;
        return false;
    }

    //("NGS patch create replaced implicit MASTER route: {} {}:{} -> {} in={}",
    //    ngsBussTypeName(source->rack->vdef->type), patch_info->source_output_index, patch_info->source_output_subindex,
    //    ngsBussTypeName(dest->rack->vdef->type), patch_info->dest_input_index);

    return true;
}
*/

static SceUInt32 ngsVoiceStateFromHLEState(const ngs::Voice *voice) {
    SceUInt32 state;
    switch (voice->state) {
    case ngs::VoiceState::VOICE_STATE_AVAILABLE:
        state = SCE_NGS_VOICE_STATE_AVAILABLE;
        break;

    case ngs::VoiceState::VOICE_STATE_ACTIVE:
        state = SCE_NGS_VOICE_STATE_ACTIVE;
        break;

    case ngs::VoiceState::VOICE_STATE_FINALIZING:
        state = SCE_NGS_VOICE_STATE_FINALIZE;
        break;

    case ngs::VoiceState::VOICE_STATE_UNLOADING:
        state = SCE_NGS_VOICE_STATE_UNLOADING;
        break;

    default:
        assert(false && "Invalid voice state to translate");
        return 0;
    }

    if (voice->is_pending)
        state |= SCE_NGS_VOICE_STATE_PENDING;

    if (voice->is_paused)
        state |= SCE_NGS_VOICE_STATE_PAUSED;

    if (voice->is_keyed_off)
        state |= SCE_NGS_VOICE_STATE_KEY_OFF;

    return state;
}

EXPORT(SceInt32, sceNgsVoiceGetInfo, ngs::Voice *voice, SceNgsVoiceInfo *info) {
    TRACY_FUNC(sceNgsVoiceGetInfo, voice, info);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice || !info) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const std::lock_guard<std::mutex> guard(*voice->voice_mutex);

    info->voice_state = ngsVoiceStateFromHLEState(voice);
    info->num_modules = static_cast<SceUInt32>(voice->datas.size());
    info->num_inputs = static_cast<SceUInt32>(voice->inputs.inputs.size());
    info->num_outputs = voice->rack->vdef->output_count;
    info->num_patches_per_output = static_cast<SceUInt32>(voice->rack->patches_per_output);
    info->update_passed = voice->frame_count;

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceGetModuleBypass, ngs::Voice *voice, const SceUInt32 module, SceUInt32 *bypass_flag) {
    TRACY_FUNC(sceNgsVoiceGetModuleBypass, voice, module, bypass_flag);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    ngs::ModuleData *storage = voice->module_storage(module);

    if (!storage)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    if (storage->is_bypassed)
        *bypass_flag = SCE_NGS_MODULE_FLAG_BYPASSED;
    else
        *bypass_flag = SCE_NGS_MODULE_FLAG_NOT_BYPASSED;

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoiceGetModuleType, ngs::Voice *voice, const SceUInt32 module, SceUInt32 *module_type) {
    TRACY_FUNC(sceNgsVoiceGetModuleType, voice, module, module_type);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice || !module_type)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (module >= voice->rack->modules.size())
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    *module_type = voice->rack->modules[module]->module_id();
    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceGetOutputPatch, ngs::Voice *voice, const SceInt32 output_index, const SceInt32 output_subindex, Ptr<ngs::Patch> *patch) {
    TRACY_FUNC(sceNgsVoiceGetOutputPatch, voice, output_index, output_subindex, patch);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice || !patch) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if ((output_subindex < 0) || (output_index < 0)) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if ((output_index >= static_cast<SceInt32>(voice->rack->vdef->output_count)) || (output_subindex >= voice->rack->patches_per_output)) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    *patch = voice->patches[output_index][output_subindex];
    if (!(*patch) || (patch->get(emuenv.mem))->output_sub_index == -1) {
        LOG_WARN_ONCE("Getting non-existen output patch port {}:{}", output_index, output_subindex);
        *patch = Ptr<ngs::Patch>(0);
    }

    return 0;
}

EXPORT(int, sceNgsVoiceGetParamsOutOfRange) {
    TRACY_FUNC(sceNgsVoiceGetParamsOutOfRange);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceNgsVoiceGetStateData, ngs::Voice *voice, const SceUInt32 module, void *mem, const SceUInt32 mem_size) {
    TRACY_FUNC(sceNgsVoiceGetStateData, voice, module, mem, mem_size);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    ngs::ModuleData *storage = voice->module_storage(module);
    if (!storage)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    if (mem) {
        memset(mem, 0, mem_size);
        memcpy(mem, storage->voice_state_data.data(), std::min<std::size_t>(mem_size, storage->voice_state_data.size()));
    }

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceInit, ngs::Voice *voice, const SceNgsVoicePreset *preset, const SceUInt32 init_flags) {
    TRACY_FUNC(sceNgsVoiceInit, voice, preset, init_flags);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    if (voice->state == ngs::VoiceState::VOICE_STATE_ACTIVE)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);

    std::lock_guard<std::mutex> guard(*voice->voice_mutex);

    if (init_flags == SCE_NGS_VOICE_INIT_BASE || init_flags == SCE_NGS_VOICE_INIT_ALL) {
        voice->state = ngs::VoiceState::VOICE_STATE_AVAILABLE;
    }

    if (init_flags & SCE_NGS_VOICE_INIT_ROUTING) {
        /*      uint32_t active_patch_count = 0;
                for (const auto &patches : voice->patches) {
                    for (const auto &patch : patches) {
                        if (patch && patch.get(emuenv.mem)->output_sub_index != -1) {
                            active_patch_count++;
                        }
                    }
                }

                if (active_patch_count > 0) {
                    // LOG_DEBUG("NGS voice routing reset: buss={} active_patches={}", ngsBussTypeName(voice->rack->vdef->type), active_patch_count);
                }
        */
        // reset all patches
        for (auto &patches : voice->patches) {
            for (auto &patch : patches) {
                if (patch) {
                    patch.get(emuenv.mem)->output_sub_index = -1;
                }
            }
        }

        /*
                if (voice->rack && voice->rack->vdef) {
                    const auto type = voice->rack->vdef->type;
                    const bool should_route_to_master = type == ngs::BussType::BUSS_COMPRESSOR
                        || type == ngs::BussType::BUSS_SIDE_CHAIN_COMPRESSOR
                        || type == ngs::BussType::BUSS_DELAY
                        || type == ngs::BussType::BUSS_DISTORTION
                        || type == ngs::BussType::BUSS_ENVELOPE
                        || type == ngs::BussType::BUSS_EQUALIZATION
                        || type == ngs::BussType::BUSS_MIXER
                        || type == ngs::BussType::BUSS_PAUSER
                        || type == ngs::BussType::BUSS_PITCH_SHIFT
                        || type == ngs::BussType::BUSS_REVERB
                        || type == ngs::BussType::BUSS_SAS_EMULATION;

                    if (should_route_to_master) {
                        ngs::Rack *master_rack = nullptr;
                        for (ngs::Rack *candidate : voice->rack->system->racks) {
                            if (candidate && candidate->vdef && candidate->vdef->type == ngs::BussType::BUSS_MASTER) {
                                master_rack = candidate;
                                break;
                            }
                        }

                        if (master_rack && !master_rack->voices.empty()) {
                            ngs::Voice *master_voice = master_rack->voices[0].get(emuenv.mem);
                            bool already_routed_to_master = false;

                            for (const auto &patch : voice->patches[0]) {
                                if (patch && patch.get(emuenv.mem)->output_sub_index != -1 && patch.get(emuenv.mem)->dest == master_voice) {
                                    already_routed_to_master = true;
                                    break;
                                }
                            }

                            if (!already_routed_to_master) {
                                voice->patch(emuenv.mem, 0, -1, 0, master_voice);
                                LOG_DEBUG("Re-routed NGS voice to MASTER after routing reset: buss={}", ngsBussTypeName(type));
                            }
                        }
                    }
                }
        */
    }

    if (init_flags & SCE_NGS_VOICE_INIT_PRESET) {
        if (!preset) {
            STUBBED("Default preset not implemented");
            for (size_t i = 0; i < voice->rack->modules.size(); i++) {
                if (voice->rack->modules[i])
                    voice->rack->modules[i]->set_default_preset(emuenv.mem, voice->datas[i]);
            }
        } else if (!voice->set_preset(emuenv.mem, preset)) {
            return RET_ERROR(SCE_NGS_ERROR);
        }
    }

    if (init_flags & SCE_NGS_VOICE_INIT_CALLBACKS) {
        for (auto &module_data : voice->datas) {
            module_data.callback = Ptr<void>();
        }
    }

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceKeyOff, ngs::Voice *voice) {
    TRACY_FUNC(sceNgsVoiceKeyOff, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return SCE_NGS_OK;
    }
    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    voice->is_keyed_off = true;
    if (!voice->rack->system->voice_scheduler.off(emuenv.mem, voice)) {
        voice->is_keyed_off = false;
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);
    }

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoiceKill, ngs::Voice *voice) {
    TRACY_FUNC(sceNgsVoiceKill, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    voice->rack->system->voice_scheduler.stop(emuenv.mem, voice);

    return 0;
}

EXPORT(SceUInt32, sceNgsVoiceLockParams, ngs::Voice *voice, SceUInt32 module, SceUInt32 param_interface_id, SceNgsBufferInfo *buf) {
    TRACY_FUNC(sceNgsVoiceLockParams, voice, module, param_interface_id, buf);
    if (!emuenv.cfg.current_config.ngs_enable) {
        *buf = {
            Ptr<void>(alloc(emuenv.mem, 10, "SceNgs buffer stub")), 10
        };

        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    ngs::ModuleData *data = voice->module_storage(module);
    if (!data) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    SceNgsBufferInfo *info = data->lock_params(emuenv.mem);
    if (!info) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    info->data.cast<SceNgsParamsDescriptor>().get(emuenv.mem)->id = param_interface_id;

    *buf = *info;
    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoicePatchSetVolume, ngs::Patch *patch, const SceInt32 output_channel, const SceInt32 input_channel, const SceFloat32 vol) {
    TRACY_FUNC(sceNgsVoicePatchSetVolume, patch, output_channel, input_channel, vol);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!patch || patch->output_sub_index == -1)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    patch->volume_matrix[output_channel][input_channel] = vol;

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoicePatchSetVolumes, ngs::Patch *patch, const SceInt32 output_channel, const SceFloat32 *volumes, const SceInt32 vols) {
    TRACY_FUNC(sceNgsVoicePatchSetVolumes, patch, output_channel, volumes, vols);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!patch || patch->output_sub_index == -1)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    for (int i = 0; i < std::min(vols, 2); i++)
        patch->volume_matrix[output_channel][i] = volumes[i];

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoicePatchSetVolumesMatrix, ngs::Patch *patch, const SceNgsVolumeMatrix *matrix) {
    TRACY_FUNC(sceNgsVoicePatchSetVolumesMatrix, patch, matrix);
    if (!emuenv.cfg.current_config.ngs_enable)
        return 0;

    /*
        if (!patch && g_last_created_patch && g_last_created_patch->output_sub_index != -1) {
            LOG_DEBUG_ONCE("sceNgsVoicePatchSetVolumesMatrix: patch is null, using last created patch");
            patch = g_last_created_patch;
        }
    */
    if (!patch || patch->output_sub_index == -1) {
        // LOG_DEBUG("NGS patch set volume matrix failed: patch={} output_sub_index={}", patch != nullptr, patch ? patch->output_sub_index : -2);
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    memcpy(patch->volume_matrix, matrix->matrix, sizeof(matrix->matrix));

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoicePause, ngs::Voice *voice) {
    TRACY_FUNC(sceNgsVoicePause, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (voice->is_paused)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);

    if (!voice->rack->system->voice_scheduler.pause(emuenv.mem, voice)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return SCE_NGS_OK;
}

EXPORT(SceUInt32, sceNgsVoicePlay, ngs::Voice *voice) {
    TRACY_FUNC(sceNgsVoicePlay, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    voice->is_pending = true;
    if (!voice->rack->system->voice_scheduler.play(emuenv.mem, voice)) {
        voice->is_pending = false;
        return RET_ERROR(SCE_NGS_ERROR);
    }
    voice->is_pending = false;

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoiceResume, ngs::Voice *voice) {
    TRACY_FUNC(sceNgsVoiceResume, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (!voice->rack->system->voice_scheduler.resume(emuenv.mem, voice)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceSetFinishedCallback, ngs::Voice *voice, Ptr<void> callback, Ptr<void> user_data) {
    TRACY_FUNC(sceNgsVoiceSetFinishedCallback, voice, callback, user_data);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    voice->finished_callback = callback;
    voice->finished_callback_user_data = user_data;

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceSetModuleCallback, ngs::Voice *voice, const SceUInt32 module, Ptr<void> callback, Ptr<void> user_data) {
    TRACY_FUNC(sceNgsVoiceSetModuleCallback, voice, module, callback, user_data);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    ngs::ModuleData *storage = voice->module_storage(module);
    if (!storage) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    storage->callback = callback;
    storage->user_data = user_data;

    return 0;
}

EXPORT(SceInt32, sceNgsVoiceSetParamsBlock, ngs::Voice *voice, const SceNgsModuleParamHeader *header,
    const SceUInt32 size, SceInt32 *pNumErrors) {
    TRACY_FUNC(sceNgsVoiceSetParamsBlock, voice, header, size, pNumErrors);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const std::lock_guard<std::mutex> guard(*voice->voice_mutex);

    const SceInt32 num_errors = voice->parse_params_block(emuenv.mem, header, size);
    if (pNumErrors != nullptr) {
        *pNumErrors = num_errors;
    }

    if (voice->rack->system->flags & 0)
        return SCE_NGS_OK;

    if (num_errors == 0)
        return SCE_NGS_OK;
    else
        return RET_ERROR(SCE_NGS_ERROR);
}

EXPORT(SceInt32, sceNgsVoiceSetPreset, ngs::Voice *voice, const SceNgsVoicePreset *preset) {
    TRACY_FUNC(sceNgsVoiceSetPreset, voice, preset);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice || !preset)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const std::lock_guard<std::mutex> guard(*voice->voice_mutex);
    if (!voice->set_preset(emuenv.mem, preset))
        return RET_ERROR(SCE_NGS_ERROR);

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceUnlockParams, ngs::Voice *voice, const SceUInt32 module_index) {
    TRACY_FUNC(sceNgsVoiceUnlockParams, voice, module_index);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    ngs::ModuleData *data = voice->module_storage(module_index);

    if (!data) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (voice->rack->system->flags & 0) {
        data->unlock_params(emuenv.mem);
        return SCE_NGS_OK;
    }

    if (!data->unlock_params(emuenv.mem)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return SCE_NGS_OK;
}

EXPORT(int, sceSulphaNgsGetDefaultConfig) {
    TRACY_FUNC(sceSulphaNgsGetDefaultConfig);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsGetNeededMemory) {
    TRACY_FUNC(sceSulphaNgsGetNeededMemory);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsInit) {
    TRACY_FUNC(sceSulphaNgsInit);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsSetRackName) {
    TRACY_FUNC(sceSulphaNgsSetRackName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsSetSampleName) {
    TRACY_FUNC(sceSulphaNgsSetSampleName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsSetSynthName) {
    TRACY_FUNC(sceSulphaNgsSetSynthName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsSetVoiceName) {
    TRACY_FUNC(sceSulphaNgsSetVoiceName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsShutdown) {
    TRACY_FUNC(sceSulphaNgsShutdown);
    return UNIMPLEMENTED();
}

EXPORT(int, sceSulphaNgsTrace) {
    TRACY_FUNC(sceSulphaNgsTrace);
    return UNIMPLEMENTED();
}
