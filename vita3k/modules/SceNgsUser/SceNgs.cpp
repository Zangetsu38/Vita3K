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

#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <ngs/state.h>
#include <ngs/system.h>
#include <util/log.h>
#include <util/tracy.h>

#include <algorithm>
#include <array>
#include <span>

TRACY_MODULE_NAME(SceNgs);

struct SceNgsPatchAudioPropInfo {
    SceInt32 out_channels;
    SceInt32 in_channels;
    SceNgsVolumeMatrix volume_matrix;
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
    SCE_NGS_ERROR_PATCH_NOT_AVAIL = 0x804A000E,
    SCE_NGS_SIZE_MISMATCH = 0x804A000D
};

static constexpr SceUInt32 SCE_NGS_MODULE_FLAG_NOT_BYPASSED = 0;
static constexpr SceUInt32 SCE_NGS_MODULE_FLAG_BYPASSED = 2;

enum SceNgsVoiceInitFlag {
    SCE_NGS_VOICE_INIT_BASE = 0,
    SCE_NGS_VOICE_INIT_ROUTING = 1,
    SCE_NGS_VOICE_INIT_PRESET = 2,
    SCE_NGS_VOICE_INIT_CALLBACKS = 4,
    SCE_NGS_VOICE_INIT_ALL = 7
};

static constexpr uint32_t SCE_NGS_SAMPLE_OFFSET_FROM_AT9_HEADER = 1 << 31;

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
    return UNIMPLEMENTED();
}

EXPORT(int, sceNgsPatchCreateRouting, Ptr<SceNgsPatchSetupInfo> patch_info, Ptr<Ptr<ngs::Patch>> handle) {
    TRACY_FUNC(sceNgsPatchCreateRouting, patch_info, handle);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!patch_info || !handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state)
        return RET_ERROR(SCE_NGS_ERROR);
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        const std::array ranges{ GuestMemoryRange{ patch_info.address(), sizeof(SceNgsPatchSetupInfo) },
            GuestMemoryRange{ handle.address(), sizeof(Address) } };
        const uint64_t pin = pin_guest_memory(mem, ranges);
        if (!pin)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        bool valid = true;
        Ptr<ngs::Patch> result;
        {
            const std::lock_guard lock(mem.protect_mutex);
            for (size_t i = 0; i < ranges.size(); ++i) {
                const uint64_t end = uint64_t(ranges[i].address) + ranges[i].size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    const bool denied = segment.perm == MemPerm::None || (i == 0 && segment.perm == MemPerm::WriteOnly) || (i == 1 && segment.perm == MemPerm::ReadOnly);
                    if (denied && uint64_t(start) < end && uint64_t(start) + segment.size > ranges[i].address)
                        valid = false;
                }
            }
            SceNgsPatchSetupInfo info{};
            auto *bytes = reinterpret_cast<uint8_t *>(&info);
            for (size_t i = 0; valid && i < sizeof(info); ++i)
                bytes[i] = *Ptr<const uint8_t>(patch_info.address() + static_cast<Address>(i)).get(mem);
            if (valid) {
                result = ngs::create_process_voice_patch(*state, mem, info);
                if (result) {
                    const Address value = result.address();
                    for (size_t i = 0; i < sizeof(value); ++i)
                        *Ptr<uint8_t>(handle.address() + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(value >> (i * 8));
                }
            }
        }
        unpin_guest_memory(mem, pin);
        if (!valid)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        if (!result)
            return RET_ERROR(SCE_NGS_ERROR);
        return SCE_NGS_OK;
    }
    const Ptr<ngs::Patch> result = ngs::create_process_voice_patch(*state, mem, *patch_info.get(mem));
    if (!result)
        return RET_ERROR(SCE_NGS_ERROR);
    *handle.get(mem) = result;
    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsPatchGetInfo, Ptr<ngs::Patch> patch_handle, Ptr<SceNgsPatchAudioPropInfo> prop_info, Ptr<SceNgsPatchDeliveryInfo> deli_info) {
    TRACY_FUNC(sceNgsPatchGetInfo, patch_handle, prop_info, deli_info);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!patch_handle) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Patch *patch = state ? ngs::find_process_patch(*state, mem, patch_handle) : nullptr;
    if (!patch)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    ngs::Voice *source = ngs::find_process_voice(*state, mem, patch->source);
    ngs::Voice *dest = ngs::find_process_voice(*state, mem, patch->dest);
    if (!source || !dest) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    SceNgsPatchAudioPropInfo prop{};
    SceNgsPatchDeliveryInfo delivery{};
    if (prop_info) {
        memcpy(prop.volume_matrix.matrix, patch->volume_matrix, sizeof(patch->volume_matrix));
        prop.in_channels = dest->rack->channels_per_voice;
        prop.out_channels = source->rack->channels_per_voice;
    }

    if (deli_info) {
        delivery.input_index = patch->dest_index;
        delivery.output_index = patch->output_index;
        delivery.output_subindex = patch->output_sub_index;
        delivery.source_voice_handle = patch->source;
        delivery.dest_voice_handle = patch->dest;
    }

    if (prop_info && !write_guest_memory(mem, prop_info.address(), &prop, sizeof(prop)))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (deli_info && !write_guest_memory(mem, deli_info.address(), &delivery, sizeof(delivery)))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !ngs::remove_process_voice_patch(*state, mem, patch)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return 0;
}

EXPORT(int, sceNgsRackGetRequiredMemorySize, ngs::System *system, SceNgsRackDescription *description, uint32_t *size) {
    TRACY_FUNC(sceNgsRackGetRequiredMemorySize, system, description, size);
    if (!size)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.cfg.current_config.ngs_enable) {
        if (!emuenv.kernel.is_primary_address_space(mem)) {
            const GuestMemoryRange output{ caller->cpu->get_reg(2), sizeof(uint32_t) };
            const uint64_t pin = pin_guest_memory(mem, std::span(&output, 1));
            if (!pin)
                return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
            bool valid = true;
            {
                const std::lock_guard lock(mem.protect_mutex);
                const uint64_t end = uint64_t(output.address) + output.size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly) && uint64_t(start) < end && uint64_t(start) + segment.size > output.address)
                        valid = false;
                }
                if (valid) {
                    for (size_t i = 0; i < sizeof(uint32_t); ++i)
                        *Ptr<uint8_t>(output.address + static_cast<Address>(i)).get(mem) = i == 0 ? 1 : 0;
                }
            }
            unpin_guest_memory(mem, pin);
            if (!valid)
                return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
            return 0;
        }
        *size = 1;
        return 0;
    }

    if (!system) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    }
    if (!description)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !std::ranges::contains(state->systems, system))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        const std::array ranges{ GuestMemoryRange{ caller->cpu->get_reg(1), sizeof(SceNgsRackDescription) },
            GuestMemoryRange{ caller->cpu->get_reg(2), sizeof(uint32_t) } };
        const uint64_t pin = pin_guest_memory(mem, ranges);
        if (!pin)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        bool valid = true;
        {
            const std::lock_guard lock(mem.protect_mutex);
            for (size_t i = 0; i < ranges.size(); ++i) {
                const uint64_t end = uint64_t(ranges[i].address) + ranges[i].size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    const bool denied = segment.perm == MemPerm::None || (i == 0 && segment.perm == MemPerm::WriteOnly) || (i == 1 && segment.perm == MemPerm::ReadOnly);
                    if (denied && uint64_t(start) < end && uint64_t(start) + segment.size > ranges[i].address)
                        valid = false;
                }
            }
            SceNgsRackDescription copied{};
            auto *bytes = reinterpret_cast<uint8_t *>(&copied);
            for (size_t i = 0; valid && i < sizeof(copied); ++i)
                bytes[i] = *Ptr<const uint8_t>(ranges[0].address + static_cast<Address>(i)).get(mem);
            uint32_t required = 0;
            if (valid)
                valid = ngs::get_process_rack_required_size(*state, mem, copied, required);
            if (valid) {
                for (size_t i = 0; i < sizeof(required); ++i)
                    *Ptr<uint8_t>(ranges[1].address + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(required >> (i * 8));
            }
        }
        unpin_guest_memory(mem, pin);
        if (!valid)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return 0;
    }
    if (!description->definition)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    auto definition = description->definition.get(mem);
    if (definition->output_count == 0 || definition->type >= ngs::BussType::BUSS_MAX)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (description->voice_count <= 0 || description->channels_per_voice < 0 || description->channels_per_voice > 2
        || description->max_patches_per_input < 0 || description->patches_per_output < 0) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }
    *size = ngs::Rack::get_required_memspace_size(mem, description);
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
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
        if (!state || !std::ranges::any_of(state->systems, [rack](const ngs::System *system) { return std::ranges::contains(system->racks, rack); }))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        if (index >= rack->voices.size())
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        const GuestMemoryRange output{ caller->cpu->get_reg(2), sizeof(Address) };
        const uint64_t pin = pin_guest_memory(mem, std::span(&output, 1));
        if (!pin)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        bool valid = true;
        {
            const std::lock_guard lock(mem.protect_mutex);
            const uint64_t end = uint64_t(output.address) + output.size;
            for (const auto &[start, segment] : mem.protect_tree) {
                if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly) && uint64_t(start) < end && uint64_t(start) + segment.size > output.address)
                    valid = false;
            }
            if (valid) {
                const Address value = rack->voices[index].address();
                for (size_t i = 0; i < sizeof(value); ++i)
                    *Ptr<uint8_t>(output.address + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(value >> (i * 8));
            }
        }
        unpin_guest_memory(mem, pin);
        if (!valid)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return SCE_NGS_OK;
    }

    if (index >= rack->voices.size()) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    *voice = rack->voices[index];
    return SCE_NGS_OK;
}

EXPORT(SceUInt32, sceNgsRackInit, ngs::System *system, SceNgsBufferInfo *info, const SceNgsRackDescription *description, Ptr<ngs::Rack> *rack) {
    TRACY_FUNC(sceNgsRackInit, system, info, description, rack);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }
    if (!info || !system || !description || !rack) {
        LOG_ERROR("sceNgsRackInit received null arguments: system={}, info={}, description={}, rack={}",
            system == nullptr, info == nullptr, description == nullptr, rack == nullptr);
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !std::ranges::contains(state->systems, system))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        const char *failure_reason = nullptr;
        const std::array args{ GuestMemoryRange{ caller->cpu->get_reg(1), sizeof(SceNgsBufferInfo) },
            GuestMemoryRange{ caller->cpu->get_reg(2), sizeof(SceNgsRackDescription) },
            GuestMemoryRange{ caller->cpu->get_reg(3), sizeof(Address) } };
        const uint64_t args_pin = pin_guest_memory(mem, args);
        if (!args_pin) {
            LOG_ERROR("sceNgsRackInit could not pin arguments: system={:#x}, info={:#x}, description={:#x}, rack={:#x}",
                caller->cpu->get_reg(0), args[0].address, args[1].address, args[2].address);
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        }
        SceNgsBufferInfo copied_info{};
        SceNgsRackDescription copied_description{};
        bool valid = true;
        {
            const std::lock_guard lock(mem.protect_mutex);
            for (size_t i = 0; i < args.size(); ++i) {
                const uint64_t end = uint64_t(args[i].address) + args[i].size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    const bool denied = segment.perm == MemPerm::None || (i < 2 && segment.perm == MemPerm::WriteOnly) || (i == 2 && segment.perm == MemPerm::ReadOnly);
                    if (denied && uint64_t(start) < end && uint64_t(start) + segment.size > args[i].address) {
                        valid = false;
                        failure_reason = "argument permissions";
                    }
                }
            }
            auto *info_bytes = reinterpret_cast<uint8_t *>(&copied_info);
            auto *desc_bytes = reinterpret_cast<uint8_t *>(&copied_description);
            for (size_t i = 0; valid && i < sizeof(copied_info); ++i)
                info_bytes[i] = *Ptr<const uint8_t>(args[0].address + static_cast<Address>(i)).get(mem);
            for (size_t i = 0; valid && i < sizeof(copied_description); ++i)
                desc_bytes[i] = *Ptr<const uint8_t>(args[1].address + static_cast<Address>(i)).get(mem);
        }
        uint32_t required = 0;
        if (valid && !copied_info.data) {
            valid = false;
            failure_reason = "null rack storage";
        } else if (valid && !ngs::get_process_rack_required_size(*state, mem, copied_description, required)) {
            valid = false;
            failure_reason = "invalid rack description or voice definition";
        } else if (valid && required > copied_info.size) {
            valid = false;
            failure_reason = "rack storage is smaller than required";
        }
        const GuestMemoryRange storage{ copied_info.data.address(), copied_info.size };
        const uint64_t storage_pin = valid ? pin_guest_memory(mem, std::span(&storage, 1)) : 0;
        if (valid && !storage_pin) {
            valid = false;
            failure_reason = "rack storage could not be pinned";
        }
        bool initialized = false;
        if (storage_pin) {
            {
                const std::lock_guard lock(mem.protect_mutex);
                const uint64_t end = uint64_t(storage.address) + storage.size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    if (segment.perm != MemPerm::ReadWrite && uint64_t(start) < end && uint64_t(start) + segment.size > storage.address) {
                        valid = false;
                        failure_reason = "rack storage permissions";
                    }
                }
                uint8_t *base = copied_info.data.cast<uint8_t>().get(mem);
                if (reinterpret_cast<uintptr_t>(base) % alignof(Address) != 0) {
                    valid = false;
                    failure_reason = "rack storage alignment";
                }
                for (uint32_t offset = 0; valid && offset < storage.size;) {
                    if (Ptr<uint8_t>(storage.address + offset).get(mem) != base + offset) {
                        valid = false;
                        failure_reason = "rack storage is not contiguous";
                    }
                    offset += std::min(storage.size - offset, uint32_t(4096 - ((storage.address + offset) & 4095)));
                }
                if (uint64_t(args[2].address) < end && uint64_t(args[2].address) + sizeof(Address) > storage.address) {
                    valid = false;
                    failure_reason = "rack output overlaps rack storage";
                }
                if (!ngs::is_process_storage_available(*state, storage.address, storage.size) || !ngs::is_process_storage_available(*state, args[2].address, args[2].size)) {
                    valid = false;
                    failure_reason = "rack storage or output overlaps existing NGS storage";
                }
                if (valid) {
                    initialized = ngs::init_rack(*state, mem, system, &copied_info, &copied_description);
                    if (initialized) {
                        const Address value = storage.address;
                        for (size_t i = 0; i < sizeof(value); ++i)
                            *Ptr<uint8_t>(args[2].address + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(value >> (i * 8));
                    }
                }
            }
            unpin_guest_memory(mem, storage_pin);
        } else {
            valid = false;
        }
        unpin_guest_memory(mem, args_pin);
        if (!valid) {
            LOG_ERROR("sceNgsRackInit rejected secondary arguments ({}): info={:#x}, description={:#x}, storage={:#x}+{:#x}, required={:#x}",
                failure_reason ? failure_reason : "unknown validation failure", args[0].address, args[1].address, storage.address, storage.size, required);
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        }
        if (!initialized)
            return RET_ERROR(SCE_NGS_ERROR);
        return SCE_NGS_OK;
    }
    if (!description->definition)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (!ngs::init_rack(*state, mem, system, info, description)) {
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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !std::ranges::any_of(state->systems, [rack](const ngs::System *system) { return std::ranges::contains(system->racks, rack); }))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    ngs::System *system = rack->system;
    auto &scheduler = system->voice_scheduler;
    std::unique_lock<std::recursive_mutex> lock(scheduler.mutex);
    if (scheduler.pending_rack_releases.contains(rack))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);

    if (callback) {
        ngs::OperationPending op;
        op.type = ngs::PendingType::ReleaseRack;
        op.system = system;
        op.release_data.state = state;
        op.release_data.rack = rack;
        op.release_data.callback = callback.address();
        scheduler.pending_rack_releases.insert(rack);
        scheduler.operations_pending.push(op);
    } else if (!scheduler.is_updating) {
        ngs::release_rack(*state, mem, system, rack);
    } else {
        if (scheduler.updating_thread_id == thread_id) {
            ngs::release_rack(*state, mem, system, rack);
            return SCE_NGS_OK;
        }

        while (scheduler.is_updating) {
            lock.unlock();
            {
                ThreadHostWait host_wait(emuenv.kernel.get_thread(thread_id));
                lock.lock();
                if (scheduler.is_updating)
                    scheduler.condvar.wait(lock);
                lock.unlock();
            }
            lock.lock();
        }
        if (!std::ranges::contains(system->racks, rack))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        ngs::release_rack(*state, mem, system, rack);
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
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        const std::array ranges{ GuestMemoryRange{ caller->cpu->get_reg(0), sizeof(SceNgsSystemInitParams) },
            GuestMemoryRange{ caller->cpu->get_reg(1), sizeof(uint32_t) } };
        const uint64_t pin = pin_guest_memory(mem, ranges);
        if (!pin)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        bool valid = true;
        {
            const std::lock_guard lock(mem.protect_mutex);
            for (size_t i = 0; i < ranges.size(); ++i) {
                const uint64_t end = uint64_t(ranges[i].address) + ranges[i].size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    const bool denied = segment.perm == MemPerm::None || (i == 0 && segment.perm == MemPerm::WriteOnly) || (i == 1 && segment.perm == MemPerm::ReadOnly);
                    if (denied && uint64_t(start) < end && uint64_t(start) + segment.size > ranges[i].address)
                        valid = false;
                }
            }
            if (valid) {
                SceNgsSystemInitParams copied_params{};
                auto *bytes = reinterpret_cast<uint8_t *>(&copied_params);
                for (size_t i = 0; i < sizeof(copied_params); ++i)
                    bytes[i] = *Ptr<const uint8_t>(ranges[0].address + static_cast<Address>(i)).get(mem);
                const uint32_t value = emuenv.cfg.current_config.ngs_enable ? ngs::System::get_required_memspace_size(&copied_params) : 1;
                for (size_t i = 0; i < sizeof(value); ++i)
                    *Ptr<uint8_t>(ranges[1].address + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(value >> (i * 8));
            }
        }
        unpin_guest_memory(mem, pin);
        if (!valid)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return 0;
    }
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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller || !params || !system || !memspace)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state)
        return RET_ERROR(SCE_NGS_ERROR);
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        const Address params_addr = caller->cpu->get_reg(2);
        const Address output_addr = caller->cpu->get_reg(3);
        if (memspace_size < sizeof(ngs::System))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        const std::array ranges{ GuestMemoryRange{ memspace.address(), memspace_size },
            GuestMemoryRange{ params_addr, sizeof(SceNgsSystemInitParams) }, GuestMemoryRange{ output_addr, sizeof(Address) } };
        const uint64_t pin = pin_guest_memory(mem, ranges);
        if (!pin)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        bool valid = true;
        bool initialized = false;
        {
            const std::lock_guard lock(mem.protect_mutex);
            for (size_t i = 0; i < ranges.size(); ++i) {
                const uint64_t end = uint64_t(ranges[i].address) + ranges[i].size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    const bool denied = segment.perm == MemPerm::None || (i == 1 && segment.perm == MemPerm::WriteOnly) || (i == 0 && segment.perm != MemPerm::ReadWrite) || (i == 2 && segment.perm == MemPerm::ReadOnly);
                    if (denied && uint64_t(start) < end && uint64_t(start) + segment.size > ranges[i].address)
                        valid = false;
                }
            }
            uint8_t *base = memspace.cast<uint8_t>().get(mem);
            if (reinterpret_cast<uintptr_t>(base) % alignof(ngs::System) != 0)
                valid = false;
            for (uint32_t offset = 0; valid && offset < memspace_size;) {
                if (Ptr<uint8_t>(memspace.address() + offset).get(mem) != base + offset)
                    valid = false;
                const uint32_t chunk = std::min(memspace_size - offset, uint32_t(4096 - ((memspace.address() + offset) & 4095)));
                offset += chunk;
            }
            if (uint64_t(output_addr) < uint64_t(memspace.address()) + memspace_size && uint64_t(output_addr) + sizeof(Address) > memspace.address())
                valid = false;
            if (!ngs::is_process_storage_available(*state, memspace.address(), memspace_size) || !ngs::is_process_storage_available(*state, output_addr, sizeof(Address)))
                valid = false;
            SceNgsSystemInitParams copied_params{};
            auto *param_bytes = reinterpret_cast<uint8_t *>(&copied_params);
            for (size_t i = 0; valid && i < sizeof(copied_params); ++i)
                param_bytes[i] = *Ptr<const uint8_t>(params_addr + static_cast<Address>(i)).get(mem);
            if (valid && copied_params.max_racks >= 0 && copied_params.max_voices >= 0) {
                initialized = ngs::init_system(*state, mem, &copied_params, memspace, memspace_size);
                if (initialized) {
                    const Address value = memspace.address();
                    for (size_t i = 0; i < sizeof(value); ++i)
                        *Ptr<uint8_t>(output_addr + static_cast<Address>(i)).get(mem) = static_cast<uint8_t>(value >> (i * 8));
                }
            } else {
                valid = false;
            }
        }
        unpin_guest_memory(mem, pin);
        if (!valid)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        if (!initialized)
            return RET_ERROR(SCE_NGS_ERROR);
        return SCE_NGS_OK;
    }
    if (!ngs::init_system(*state, mem, params, memspace, memspace_size)) {
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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !std::ranges::contains(state->systems, system))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);

    {
        std::unique_lock<std::recursive_mutex> lock(system->voice_scheduler.mutex);
        if (system->voice_scheduler.is_updating) {
            LOG_WARN_ONCE("sceNgsSystemRelease called during a ngs update, contact devs if your game softlocks now.");

            while (system->voice_scheduler.is_updating) {
                lock.unlock();
                {
                    ThreadHostWait host_wait(emuenv.kernel.get_thread(thread_id));
                    lock.lock();
                    if (system->voice_scheduler.is_updating)
                        system->voice_scheduler.condvar.wait(lock);
                    lock.unlock();
                }
                lock.lock();
            }
        }
    }

    ngs::release_system(*state, mem, system);

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsSystemSetFlags, ngs::System *system, const SceUInt32 system_flags) {
    TRACY_FUNC(sceNgsSystemSetFlags, system, system_flags);
    if (!emuenv.cfg.current_config.ngs_enable)
        return 0;

    if (!system)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, caller->cpu->mem);
    if (!state || !std::ranges::contains(state->systems, system))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);

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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !std::ranges::contains(state->systems, system))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!system->voice_scheduler.update(emuenv.kernel, mem, thread_id))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceBypassModule, Ptr<ngs::Voice> voice_handle, const SceUInt32 module, const SceUInt32 bypass_flag) {
    TRACY_FUNC(sceNgsVoiceBypassModule, voice_handle, module, bypass_flag);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *voice = state ? ngs::find_process_voice(*state, mem, voice_handle) : nullptr;
    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_ATRAC9);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetCompressorBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetCompressorBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_COMPRESSOR);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetCompressorSideChainBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetCompressorSideChainBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_SIDE_CHAIN_COMPRESSOR);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetDelayBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetDelayBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_DELAY);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetDistortionBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetDistortionBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_DISTORTION);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetEnvelopeBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetEnvelopeBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_ENVELOPE);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetEqBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetEqBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_EQUALIZATION);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetMasterBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetMasterBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("sceNgsVoiceDefGetMasterBuss: unknown thread {}", thread_id);
        return {};
    }
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state) {
        LOG_ERROR("sceNgsVoiceDefGetMasterBuss: cannot initialize process NGS state");
        return {};
    }
    return ngs::get_voice_definition(*state, mem, ngs::BussType::BUSS_MASTER);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetMixerBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetMixerBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_MIXER);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetPauserBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetPauserBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_PAUSER);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetPitchShiftBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetPitchShiftBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_PITCH_SHIFT);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetReverbBuss) {
    TRACY_FUNC(sceNgsVoiceDefGetReverbBuss);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_REVERB);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetSasEmuVoice) {
    TRACY_FUNC(sceNgsVoiceDefGetSasEmuVoice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_SAS_EMULATION);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetScreamAtrac9Voice) {
    TRACY_FUNC(sceNgsVoiceDefGetScreamAtrac9Voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_SCREAM_ATRAC9);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetScreamVoice) {
    TRACY_FUNC(sceNgsVoiceDefGetScreamVoice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_SCREAM);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetSimpleAtrac9Voice) {
    TRACY_FUNC(sceNgsVoiceDefGetSimpleAtrac9Voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_SIMPLE_ATRAC9);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetSimpleVoice) {
    TRACY_FUNC(sceNgsVoiceDefGetSimpleVoice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_SIMPLE);
}

EXPORT(Ptr<ngs::VoiceDefinition>, sceNgsVoiceDefGetTemplate1) {
    TRACY_FUNC(sceNgsVoiceDefGetTemplate1);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return Ptr<ngs::VoiceDefinition>(0);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_ERROR("{}: unknown thread {}", export_name, thread_id);
        return {};
    }
    return ngs::get_process_voice_definition(emuenv.ngs, emuenv.mem, caller->cpu->mem, ngs::BussType::BUSS_NORMAL_PLAYER);
}

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

EXPORT(SceInt32, sceNgsVoiceGetInfo, Ptr<ngs::Voice> voice_handle, Ptr<SceNgsVoiceInfo> info_handle) {
    TRACY_FUNC(sceNgsVoiceGetInfo, voice_handle, info_handle);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice_handle || !info_handle) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        std::unique_lock lock(emuenv.ngs.process_states_mutex, std::defer_lock);
        {
            ThreadHostWait host_wait(caller);
            lock.lock();
        }
        const auto process = emuenv.ngs.process_states.find(&mem);
        SceNgsVoiceInfo result{};
        if (process == emuenv.ngs.process_states.end() || !ngs::get_process_voice_info(*process->second, mem, voice_handle, result))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        if (!write_guest_memory(mem, info_handle.address(), &result, sizeof(result)))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return SCE_NGS_OK;
    }
    ngs::Voice *voice = voice_handle.get(mem);
    SceNgsVoiceInfo *info = info_handle.get(mem);
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
    ngs::Patch *output_patch = nullptr;
    if (*patch) {
        if (!patch->valid(emuenv.mem)) {
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        }

        output_patch = patch->get(emuenv.mem);
    }

    if (!output_patch || (output_patch->output_sub_index == -1)) {
        if (output_patch)
            *patch = Ptr<ngs::Patch>(0);

        return RET_ERROR(SCE_NGS_ERROR_PATCH_NOT_AVAIL);
    }

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoiceGetParamsOutOfRange) {
    TRACY_FUNC(sceNgsVoiceGetParamsOutOfRange);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceNgsVoiceGetStateData, Ptr<ngs::Voice> voice, const SceUInt32 module, Ptr<void> output, const SceUInt32 mem_size) {
    TRACY_FUNC(sceNgsVoiceGetStateData, voice, module, output, mem_size);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
        std::vector<uint8_t> data;
        if (!state || !ngs::get_process_voice_state(*state, mem, voice, module, output ? mem_size : 0, data))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        if (output && mem_size) {
            const GuestMemoryRange range{ output.address(), mem_size };
            const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
            if (!pin)
                return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
            bool valid = true;
            {
                const std::lock_guard lock(mem.protect_mutex);
                const uint64_t end = uint64_t(range.address) + range.size;
                for (const auto &[start, segment] : mem.protect_tree) {
                    if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly) && uint64_t(start) < end && uint64_t(start) + segment.size > range.address)
                        valid = false;
                }
                for (uint32_t i = 0; valid && i < mem_size; ++i)
                    *Ptr<uint8_t>(range.address + i).get(mem) = i < data.size() ? data[i] : 0;
            }
            unpin_guest_memory(mem, pin);
            if (!valid)
                return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        }
        return SCE_NGS_OK;
    }
    ngs::ModuleData *storage = voice.get(mem)->module_storage(module);
    if (!storage)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    if (output) {
        memset(output.get(mem), 0, mem_size);
        memcpy(output.get(mem), storage->guest_state_data.data(), std::min<std::size_t>(mem_size, storage->guest_state_data.size()));
    }

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoiceInit, Ptr<ngs::Voice> voice_handle, const SceNgsVoicePreset *preset, const SceUInt32 init_flags) {
    TRACY_FUNC(sceNgsVoiceInit, voice_handle, preset, init_flags);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *voice = state ? ngs::find_process_voice(*state, mem, voice_handle) : nullptr;
    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);

    if (voice->state == ngs::VoiceState::VOICE_STATE_ACTIVE)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);

    std::lock_guard<std::mutex> guard(*voice->voice_mutex);

    if (init_flags == SCE_NGS_VOICE_INIT_BASE || init_flags == SCE_NGS_VOICE_INIT_ALL) {
        voice->state = ngs::VoiceState::VOICE_STATE_AVAILABLE;
    }

    if (init_flags & SCE_NGS_VOICE_INIT_ROUTING) {
        // reset all patches
        for (auto &patches : voice->patches) {
            for (auto &patch : patches) {
                if (patch) {
                    patch.get(mem)->output_sub_index = -1;
                }
            }
        }
    }

    if (init_flags & SCE_NGS_VOICE_INIT_PRESET) {
        if (!preset) {
            STUBBED("Default preset not implemented");
            for (size_t i = 0; i < voice->rack->modules.size(); i++) {
                if (voice->rack->modules[i])
                    voice->rack->modules[i]->set_default_preset(mem, voice->datas[i]);
            }
        } else if (!voice->set_preset(mem, preset)) {
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

EXPORT(SceInt32, sceNgsVoiceKeyOff, Ptr<ngs::Voice> voice) {
    TRACY_FUNC(sceNgsVoiceKeyOff, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return SCE_NGS_OK;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *owned = state ? ngs::find_process_voice(*state, mem, voice) : nullptr;
    if (!owned)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!owned->rack->system->voice_scheduler.off(mem, owned))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);
    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoiceKill, Ptr<ngs::Voice> voice) {
    TRACY_FUNC(sceNgsVoiceKill, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *owned = state ? ngs::find_process_voice(*state, mem, voice) : nullptr;
    if (!owned)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    owned->rack->system->voice_scheduler.stop(mem, owned);

    return 0;
}

EXPORT(SceUInt32, sceNgsVoiceLockParams, Ptr<ngs::Voice> voice_handle, SceUInt32 module, SceUInt32 param_interface_id, Ptr<SceNgsBufferInfo> buf_handle) {
    TRACY_FUNC(sceNgsVoiceLockParams, voice_handle, module, param_interface_id, buf_handle);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!buf_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    if (!emuenv.cfg.current_config.ngs_enable) {
        const SceNgsBufferInfo result{ Ptr<void>(alloc(mem, 10, "SceNgs buffer stub")), 10 };
        if (!result.data)
            return RET_ERROR(SCE_NGS_ERROR);
        if (!write_guest_memory(mem, buf_handle.address(), &result, sizeof(result))) {
            free(mem, result.data.address());
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        }
        return 0;
    }

    if (!voice_handle) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    if (!emuenv.kernel.is_primary_address_space(mem)) {
        std::unique_lock lock(emuenv.ngs.process_states_mutex, std::defer_lock);
        {
            ThreadHostWait host_wait(caller);
            lock.lock();
        }
        const auto process = emuenv.ngs.process_states.find(&mem);
        ngs::Voice *voice = process != emuenv.ngs.process_states.end() ? ngs::find_process_voice(*process->second, mem, voice_handle) : nullptr;
        if (!voice)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        ngs::ModuleData *data = voice->module_storage(module);
        if (!data)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        SceNgsBufferInfo *info = data->lock_params(mem);
        if (!info)
            return RET_ERROR(SCE_NGS_ERROR);
        const std::array ranges{ GuestMemoryRange{ buf_handle.address(), sizeof(SceNgsBufferInfo) },
            GuestMemoryRange{ info->data.address(), sizeof(param_interface_id) } };
        const uint64_t pin = info->size >= sizeof(param_interface_id) ? pin_guest_memory(mem, ranges) : 0;
        bool valid = pin != 0;
        if (pin) {
            {
                const std::lock_guard protect_lock(mem.protect_mutex);
                for (const GuestMemoryRange &range : ranges) {
                    const uint64_t end = uint64_t(range.address) + range.size;
                    for (const auto &[start, segment] : mem.protect_tree) {
                        if ((segment.perm == MemPerm::None || segment.perm == MemPerm::ReadOnly)
                            && uint64_t(start) < end && uint64_t(start) + segment.size > range.address)
                            valid = false;
                    }
                }
                if (valid) {
                    const auto *id_bytes = reinterpret_cast<const uint8_t *>(&param_interface_id);
                    const auto *info_bytes = reinterpret_cast<const uint8_t *>(info);
                    for (size_t i = 0; i < sizeof(param_interface_id); ++i)
                        *Ptr<uint8_t>(ranges[1].address + static_cast<Address>(i)).get(mem) = id_bytes[i];
                    for (size_t i = 0; i < sizeof(*info); ++i)
                        *Ptr<uint8_t>(ranges[0].address + static_cast<Address>(i)).get(mem) = info_bytes[i];
                }
            }
            unpin_guest_memory(mem, pin);
        }
        if (!valid) {
            data->cancel_params_lock();
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        }
        return SCE_NGS_OK;
    }

    ngs::Voice *voice = voice_handle.get(mem);
    SceNgsBufferInfo *buf = buf_handle.get(mem);
    ngs::ModuleData *data = voice->module_storage(module);
    if (!data) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    SceNgsBufferInfo *info = data->lock_params(mem);
    if (!info) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    info->data.cast<SceNgsParamsDescriptor>().get(mem)->id = param_interface_id;

    *buf = *info;
    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoicePatchSetVolume, Ptr<ngs::Patch> patch_handle, const SceInt32 output_channel, const SceInt32 input_channel, const SceFloat32 vol) {
    TRACY_FUNC(sceNgsVoicePatchSetVolume, patch_handle, output_channel, input_channel, vol);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!patch_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        if (output_channel < 0 || output_channel >= SCE_NGS_MAX_SYSTEM_CHANNELS || input_channel < 0 || input_channel >= SCE_NGS_MAX_SYSTEM_CHANNELS)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        std::unique_lock lock(emuenv.ngs.process_states_mutex, std::defer_lock);
        {
            ThreadHostWait host_wait(caller);
            lock.lock();
        }
        const auto process = emuenv.ngs.process_states.find(&mem);
        if (process == emuenv.ngs.process_states.end())
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        const uint32_t offset = output_channel * SCE_NGS_MAX_SYSTEM_CHANNELS + input_channel;
        if (!ngs::set_process_patch_volumes(*process->second, mem, patch_handle, offset, &vol, 1))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return SCE_NGS_OK;
    }
    ngs::Patch *patch = patch_handle.get(mem);
    if (patch->output_sub_index == -1)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    patch->volume_matrix[output_channel][input_channel] = vol;

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoicePatchSetVolumes, Ptr<ngs::Patch> patch_handle, const SceInt32 output_channel, Ptr<const SceFloat32> volumes_handle, const SceInt32 vols) {
    TRACY_FUNC(sceNgsVoicePatchSetVolumes, patch_handle, output_channel, volumes_handle, vols);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!patch_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        if (output_channel < 0 || output_channel >= SCE_NGS_MAX_SYSTEM_CHANNELS || vols < 0)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        const uint32_t count = std::min(vols, SCE_NGS_MAX_SYSTEM_CHANNELS);
        std::array<SceFloat32, SCE_NGS_MAX_SYSTEM_CHANNELS> copied{};
        if (count && (!volumes_handle || !read_guest_memory(mem, volumes_handle.address(), copied.data(), count * sizeof(SceFloat32))))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        std::unique_lock lock(emuenv.ngs.process_states_mutex, std::defer_lock);
        {
            ThreadHostWait host_wait(caller);
            lock.lock();
        }
        const auto process = emuenv.ngs.process_states.find(&mem);
        if (process == emuenv.ngs.process_states.end())
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        if (!ngs::set_process_patch_volumes(*process->second, mem, patch_handle, output_channel * SCE_NGS_MAX_SYSTEM_CHANNELS, copied.data(), count))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return SCE_NGS_OK;
    }
    ngs::Patch *patch = patch_handle.get(mem);
    const SceFloat32 *volumes = volumes_handle.get(mem);
    if (patch->output_sub_index == -1)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    for (int i = 0; i < std::min(vols, 2); i++)
        patch->volume_matrix[output_channel][i] = volumes[i];

    return SCE_NGS_OK;
}

EXPORT(SceInt32, sceNgsVoicePatchSetVolumesMatrix, Ptr<ngs::Patch> patch_handle, Ptr<const SceNgsVolumeMatrix> matrix_handle) {
    TRACY_FUNC(sceNgsVoicePatchSetVolumesMatrix, patch_handle, matrix_handle);
    if (!emuenv.cfg.current_config.ngs_enable)
        return 0;

    if (!patch_handle || !matrix_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    if (!emuenv.kernel.is_primary_address_space(mem)) {
        std::unique_lock lock(emuenv.ngs.process_states_mutex, std::defer_lock);
        {
            ThreadHostWait host_wait(caller);
            lock.lock();
        }
        const auto process = emuenv.ngs.process_states.find(&mem);
        if (process == emuenv.ngs.process_states.end())
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
        if (!ngs::set_process_patch_matrix(*process->second, mem, patch_handle, matrix_handle))
            return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
        return SCE_NGS_OK;
    }
    ngs::Patch *patch = patch_handle.get(mem);
    const SceNgsVolumeMatrix *matrix = matrix_handle.get(mem);
    if (patch->output_sub_index == -1)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    memcpy(patch->volume_matrix, matrix->matrix, sizeof(matrix->matrix));

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoicePause, Ptr<ngs::Voice> voice) {
    TRACY_FUNC(sceNgsVoicePause, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *owned = state ? ngs::find_process_voice(*state, mem, voice) : nullptr;
    if (!owned)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (owned->is_paused)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_STATE);

    if (!owned->rack->system->voice_scheduler.pause(mem, owned)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return SCE_NGS_OK;
}

EXPORT(SceUInt32, sceNgsVoicePlay, Ptr<ngs::Voice> voice) {
    TRACY_FUNC(sceNgsVoicePlay, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    if (!state || !ngs::find_process_voice(*state, mem, voice))
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!ngs::play_process_voice(*state, mem, voice)) {
        return RET_ERROR(SCE_NGS_ERROR);
    }

    return SCE_NGS_OK;
}

EXPORT(int, sceNgsVoiceResume, Ptr<ngs::Voice> voice) {
    TRACY_FUNC(sceNgsVoiceResume, voice);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *owned = state ? ngs::find_process_voice(*state, mem, voice) : nullptr;
    if (!owned)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!owned->is_paused)
        return SCE_NGS_OK;

    if (!owned->rack->system->voice_scheduler.resume(mem, owned)) {
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

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);

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

    if (!voice->voice_mutex)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);

    const std::lock_guard<std::mutex> guard(*voice->voice_mutex);
    ngs::ModuleData *storage = voice->module_storage(module);
    if (!storage) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    storage->callback = callback;
    storage->user_data = user_data;

    return 0;
}

EXPORT(SceInt32, sceNgsVoiceSetParamsBlock, Ptr<ngs::Voice> voice_handle, Ptr<const SceNgsModuleParamHeader> header,
    const SceUInt32 size, Ptr<SceInt32> pNumErrors) {
    TRACY_FUNC(sceNgsVoiceSetParamsBlock, voice_handle, header, size, pNumErrors);
    if (!emuenv.cfg.current_config.ngs_enable)
        return SCE_NGS_OK;

    if (!voice_handle || !header)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    ngs::State *state = ngs::get_process_state(emuenv.ngs, emuenv.mem, mem);
    ngs::Voice *voice = state ? ngs::find_process_voice(*state, mem, voice_handle) : nullptr;
    if (!voice)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);

    const std::lock_guard<std::mutex> guard(*voice->voice_mutex);

    const SceInt32 num_errors = voice->parse_params_block(mem, header.get(mem), size);
    if (pNumErrors)
        *pNumErrors.get(mem) = num_errors;

    if ((voice->rack->system->flags & ngs::system_flag_module_param_checking) == ngs::system_flag_no_module_param_checking)
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

EXPORT(SceInt32, sceNgsVoiceUnlockParams, Ptr<ngs::Voice> voice_handle, const SceUInt32 module_index) {
    TRACY_FUNC(sceNgsVoiceUnlockParams, voice_handle, module_index);
    if (!emuenv.cfg.current_config.ngs_enable) {
        return 0;
    }

    if (!voice_handle)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);

    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    MemState &mem = caller->cpu->mem;
    const bool secondary = !emuenv.kernel.is_primary_address_space(mem);
    std::unique_lock lock(emuenv.ngs.process_states_mutex, std::defer_lock);
    ngs::Voice *voice = nullptr;
    if (secondary) {
        {
            ThreadHostWait host_wait(caller);
            lock.lock();
        }
        const auto process = emuenv.ngs.process_states.find(&mem);
        voice = process != emuenv.ngs.process_states.end() ? ngs::find_process_voice(*process->second, mem, voice_handle) : nullptr;
        if (!voice)
            return RET_ERROR(SCE_NGS_ERROR_INVALID_HANDLE);
    } else
        voice = voice_handle.get(mem);

    ngs::ModuleData *data = voice->module_storage(module_index);

    if (!data) {
        return RET_ERROR(SCE_NGS_ERROR_INVALID_ARG);
    }

    const bool params_valid = data->unlock_params(mem, secondary);
    if (secondary && !params_valid)
        return RET_ERROR(SCE_NGS_ERROR);
    if ((voice->rack->system->flags & ngs::system_flag_module_param_checking) == ngs::system_flag_no_module_param_checking)
        return SCE_NGS_OK;

    if (!params_valid) {
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
