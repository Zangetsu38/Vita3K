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

#include <cpu/state.h>
#include <kernel/state.h>
#include <mem/functions.h>

#include <ngs/modules/atrac9.h>
#include <ngs/state.h>
#include <ngs/system.h>
#include <util/lock_and_find.h>
#include <util/log.h>

#include <util/vector_utils.h>

#include <cstring>
#include <limits>

namespace ngs {
Rack::Rack(System *mama, const Ptr<void> memspace, const uint32_t memspace_size)
    : MempoolObject(memspace, memspace_size)
    , system(mama)
    , storage_size(memspace_size) {}

System::System(const Ptr<void> memspace, const uint32_t memspace_size)
    : MempoolObject(memspace, memspace_size)
    , storage_size(memspace_size)
    , max_voices(0)
    , granularity(0)
    , sample_rate(0)
    , flags(default_system_flags) {}

bool Patch::is_active() const {
    return output_sub_index != -1;
}

void VoiceInputManager::init(const uint32_t granularity, const uint16_t total_input) {
    inputs.resize(total_input);

    for (auto &input : inputs) {
        // FLTP and maximum channel count
        input.resize(granularity * 8);
    }

    reset_inputs();
}

void VoiceInputManager::reset_inputs() {
    for (auto &input : inputs) {
        std::ranges::fill(input, 0);
    }
}

VoiceInputManager::PCMInput *VoiceInputManager::get_input_buffer_queue(const int32_t index) {
    if (index >= inputs.size()) {
        return nullptr;
    }

    return &inputs[index];
}

int32_t VoiceInputManager::receive(const MemState &mem, ngs::Patch *patch, const VoiceProduct &product) {
    Voice *source = patch->source.get(mem);
    Voice *dest = patch->dest.get(mem);
    if (!source || !dest) {
        return -1;
    }

    PCMInput *input = get_input_buffer_queue(patch->dest_index);

    if (!input) {
        return -1;
    }

    float *dest_buffer = reinterpret_cast<float *>(input->data());
    const float *data_to_mix_in = reinterpret_cast<const float *>(product.data);

    float volume_matrix[2][2];
    memcpy(volume_matrix, patch->volume_matrix, sizeof(volume_matrix));

    // we always use stereo internally, so make sure not to add too many channels
    if (source->rack->channels_per_voice == 1) {
        volume_matrix[1][0] = 0.0f;
        volume_matrix[1][1] = 0.0f;
    }

    if (dest->rack->channels_per_voice == 1) {
        volume_matrix[0][1] = 0.0f;
        volume_matrix[1][1] = 0.0f;
    }

    // Try mixing, also with the use of this volume matrix
    // Dest is our voice to receive this data.
    for (int32_t k = 0; k < dest->rack->system->granularity; k++) {
        dest_buffer[k * 2] = std::clamp(dest_buffer[k * 2] + data_to_mix_in[k * 2] * volume_matrix[0][0]
                + data_to_mix_in[k * 2 + 1] * volume_matrix[1][0],
            -1.0f, 1.0f);
        dest_buffer[k * 2 + 1] = std::clamp(dest_buffer[k * 2 + 1] + data_to_mix_in[k * 2] * volume_matrix[0][1] + data_to_mix_in[k * 2 + 1] * volume_matrix[1][1], -1.0f, 1.0f);
    }

    return 0;
}

ModuleData::ModuleData()
    : callback(0)
    , user_data(0)
    , is_bypassed(false)
    , flags(0) {
}

SceNgsBufferInfo *ModuleData::lock_params(MemState &mem) {
    const std::lock_guard<std::mutex> guard(*parent->voice_mutex);

    // Save a copy of previous set of data
    if (flags & PARAMS_LOCK) {
        return nullptr;
    }

    last_info.resize(info.size);
    if (!read_guest_memory(mem, info.data.address(), last_info.data(), info.size)) {
        LOG_ERROR("NGS parameter buffer at {:#x} is not readable (size {})", info.data.address(), info.size);
        return nullptr;
    }

    flags |= PARAMS_LOCK;

    return &info;
}

void ModuleData::cancel_params_lock() {
    const std::lock_guard<std::mutex> guard(*parent->voice_mutex);
    flags &= ~PARAMS_LOCK;
}

bool ModuleData::unlock_params(MemState &mem, bool validate_memory) {
    const std::lock_guard<std::mutex> guard(*parent->voice_mutex);

    if (!(flags & PARAMS_LOCK))
        return false;

    if (validate_memory) {
        if (info.size < parent->rack->modules[index]->get_buffer_parameter_size() || last_info.size() < info.size)
            return false;
    }
    if (validate_memory && info.size) {
        const GuestMemoryRange range{ info.data.address(), info.size };
        const uint64_t pin = pin_guest_memory(mem, std::span(&range, 1));
        if (!pin)
            return false;
        bool valid = true;
        {
            const std::lock_guard protect_lock(mem.protect_mutex);
            const uint64_t end = uint64_t(range.address) + range.size;
            for (const auto &[start, segment] : mem.protect_tree) {
                if (segment.perm != MemPerm::ReadWrite && uint64_t(start) < end && uint64_t(start) + segment.size > range.address)
                    valid = false;
            }
            // Parameter callbacks use a native pointer spanning the whole buffer.
            const auto base = reinterpret_cast<uintptr_t>(Ptr<const uint8_t>(range.address).get(mem));
            for (uint64_t address = (uint64_t(range.address) / STANDARD_PAGE_SIZE + 1) * STANDARD_PAGE_SIZE; address < end; address += STANDARD_PAGE_SIZE) {
                if (reinterpret_cast<uintptr_t>(Ptr<const uint8_t>(static_cast<Address>(address)).get(mem)) != base + (address - range.address))
                    valid = false;
            }
            if (valid)
                parent->rack->modules[index]->on_param_change(mem, *this);
        }
        unpin_guest_memory(mem, pin);
        if (!valid)
            return false;
        flags &= ~PARAMS_LOCK;
        return true;
    }

    parent->rack->modules[index]->on_param_change(mem, *this);
    flags &= ~PARAMS_LOCK;
    return true;
}

void ModuleData::invoke_callback(KernelState &kernel, const MemState &mem, const SceUID thread_id, const uint32_t reason1,
    const uint32_t reason2, Address reason_ptr) {
    return parent->invoke_callback(kernel, mem, thread_id, callback, user_data, parent->rack->modules[index]->module_id(),
        reason1, reason2, reason_ptr);
}

void Voice::init(Rack *mama) {
    rack = mama;
    state = VoiceState::VOICE_STATE_AVAILABLE;
    is_pending = false;
    is_paused = false;
    is_keyed_off = false;

    datas.resize(mama->modules.size());

    for (uint32_t i = 0; i < MAX_OUTPUT_PORT; i++)
        patches[i].resize(mama->patches_per_output);

    inputs.init(rack->system->granularity, 1);
    voice_mutex = std::make_unique<std::mutex>();
}

Ptr<Patch> Voice::patch(const MemState &mem, const int32_t index, int32_t subindex, int32_t dest_index, Ptr<Voice> source, Ptr<Voice> dest) {
    const std::lock_guard<std::mutex> guard(*voice_mutex);

    if (index < 0 || index >= MAX_OUTPUT_PORT || subindex < -1) {
        // We don't have enough port for you!
        return {};
    }

    // Look if another patch has already been there
    if (subindex == -1) {
        for (int32_t i = 0; i < patches[index].size(); i++) {
            if (!patches[index][i] || (patches[index])[i].get(mem)->output_sub_index == -1) {
                subindex = i;
                break;
            }
        }
    }

    if (subindex < 0 || subindex >= patches[index].size()) {
        return {};
    }

    if (patches[index][subindex] && patches[index][subindex].get(mem)->output_sub_index != -1) {
        // You just hit an occupied subindex! You won't get to eat, stay in detention.
        return {};
    }

    if (!patches[index][subindex]) {
        // Create the patch incase it's not yet existed
        patches[index][subindex] = rack->alloc<Patch>();
        if (!patches[index][subindex])
            return {};
        new (patches[index][subindex].get(mem)) Patch();
    }

    Patch *patch = patches[index][subindex].get(mem);

    patch->output_sub_index = subindex;
    patch->output_index = index;
    patch->dest_index = dest_index;
    patch->dest = dest;
    patch->source = source;

    // Initialize the matrix
    memset(patch->volume_matrix, 0, sizeof(patch->volume_matrix));

    return patches[index][subindex];
}

bool Voice::remove_patch(const MemState &mem, const Ptr<Patch> patch) {
    if (!patch || !voice_mutex) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(*voice_mutex);
    bool found = false;
    for (auto &patches_1 : patches) {
        if (std::ranges::contains(patches_1, patch)) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    // Try to unroute. Free the destination index
    patch.get(mem)->output_sub_index = -1;

    return true;
}

ModuleData *Voice::module_storage(const uint32_t index) {
    if (index >= datas.size()) {
        return nullptr;
    }

    return &datas[index];
}

void Voice::transition(const MemState &mem, const VoiceState new_state) {
    const VoiceState old = state;
    state = new_state;

    for (size_t i = 0; i < datas.size(); i++) {
        rack->modules[i]->on_state_change(mem, datas[i], old);
    }
}

bool Voice::parse_params(const MemState &mem, const SceNgsModuleParamHeader *header) {
    ModuleData *storage = module_storage(header->module_id);

    if (!storage)
        return false;

    if (storage->flags & ModuleData::PARAMS_LOCK)
        return false;

    const auto *descr = reinterpret_cast<const SceNgsParamsDescriptor *>(header + 1);
    if (descr->size > storage->info.size)
        return false;

    memcpy(storage->info.data.get(mem), descr, descr->size);

    return true;
}

SceInt32 Voice::parse_params_block(const MemState &mem, const SceNgsModuleParamHeader *header, const SceUInt32 size) {
    const SceUInt8 *data = reinterpret_cast<const SceUInt8 *>(header);
    const SceUInt8 *data_end = data + size;

    SceInt32 num_error = 0;

    // after first loop, check if other module exist
    while (data < data_end) {
        if (!parse_params(mem, header))
            num_error++;

        // increment by the size of the header alone + the descriptor size
        data += sizeof(SceNgsModuleParamHeader) + reinterpret_cast<const SceNgsParamsDescriptor *>(header + 1)->size;

        // set new header for next module
        header = reinterpret_cast<const SceNgsModuleParamHeader *>(data);
    }

    return num_error;
}

bool Voice::set_preset(const MemState &mem, const SceNgsVoicePreset *preset) {
    // we ignore the name for now
    const uint8_t *data_origin = reinterpret_cast<const uint8_t *>(preset);

    if (preset->preset_data_offset) {
        const auto *preset_data = reinterpret_cast<const SceNgsModuleParamHeader *>(data_origin + preset->preset_data_offset);
        auto nb_errors = parse_params_block(mem, preset_data, preset->preset_data_size);
        if (nb_errors > 0)
            return false;
    }

    if (preset->bypass_flags_offset) {
        const auto *bypass_flags = reinterpret_cast<const SceUInt32 *>(data_origin + preset->bypass_flags_offset);
        // should we disable bypass on all modules first?
        for (SceUInt32 i = 0; i < preset->bypass_flags_nb; i++) {
            ModuleData *module_data = module_storage(*bypass_flags);
            if (!module_data)
                return false;
            module_data->is_bypassed = true;

            bypass_flags++;
        }
    }

    return true;
}

void Voice::invoke_callback(KernelState &kernel, const MemState &mem, const SceUID thread_id, Ptr<void> callback, Ptr<void> user_data,
    const uint32_t module_id, const uint32_t reason1, const uint32_t reason2, Address reason_ptr) {
    if (!callback) {
        return;
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    const Address callback_info_addr = thread->cpu->stack_alloc(sizeof(SceNgsCallbackInfo));

    SceNgsCallbackInfo *info = Ptr<SceNgsCallbackInfo>(callback_info_addr).get(mem);
    info->rack_handle = Ptr<void>(rack, mem);
    info->voice_handle = Ptr<void>(this, mem);
    info->module_id = module_id;
    info->callback_reason = reason1;
    info->callback_reason_2 = reason2;
    info->callback_ptr = Ptr<void>(reason_ptr);
    info->userdata = user_data;

    thread->call_guest(callback.address(), { callback_info_addr });
    thread->cpu->stack_free(sizeof(SceNgsCallbackInfo));
}

uint32_t System::get_required_memspace_size(SceNgsSystemInitParams *parameters) {
    return sizeof(System);
}

uint32_t Rack::get_required_memspace_size(MemState &mem, SceNgsRackDescription *description) {
    uint32_t buffer_size = 0;
    if (description->definition)
        // multiply by 2 because there are 2 copies of each buffer
        buffer_size = 2 * get_voice_definition_size(description->definition.get(mem));

    return sizeof(ngs::Rack) + description->voice_count * (sizeof(ngs::Voice) + buffer_size + description->patches_per_output * description->definition.get(mem)->output_count * sizeof(ngs::Patch));
}

bool init(State &ngs, MemState &mem) {
    return voice_definition_init(ngs, mem);
}

void deinit(State &ngs, MemState &mem) {
    {
        const std::lock_guard lock(ngs.process_states_mutex);
        for (auto &[process_mem, state] : ngs.process_states) {
            while (!state->systems.empty())
                release_system(*state, *process_mem, state->systems.back());
            if (state->definitions)
                free(*process_mem, state->definitions.address());
        }
        ngs.process_states.clear();
    }
    while (!ngs.systems.empty()) {
        release_system(ngs, mem, ngs.systems.back());
    }

    ngs.definitions = Ptr<VoiceDefinition>(0);
}

State *get_process_state(State &ngs, MemState &primary_mem, MemState &process_mem) {
    if (&primary_mem == &process_mem)
        return &ngs;
    const std::lock_guard lock(ngs.process_states_mutex);
    const auto existing = ngs.process_states.find(&process_mem);
    if (existing != ngs.process_states.end())
        return existing->second.get();
    auto state = std::make_unique<State>();
    if (!init(*state, process_mem))
        return nullptr;
    State *result = state.get();
    ngs.process_states.emplace(&process_mem, std::move(state));
    return result;
}

void release_process_state(State &ngs, MemState &process_mem) {
    const std::lock_guard lock(ngs.process_states_mutex);
    const auto it = ngs.process_states.find(&process_mem);
    if (it == ngs.process_states.end())
        return;
    State &state = *it->second;
    while (!state.systems.empty())
        release_system(state, process_mem, state.systems.back());
    if (state.definitions)
        free(process_mem, state.definitions.address());
    ngs.process_states.erase(it);
}

bool init_system(State &ngs, const MemState &mem, SceNgsSystemInitParams *parameters, Ptr<void> memspace, const uint32_t memspace_size) {
    // Reserve first memory allocation for our System struct
    System *sys = memspace.cast<System>().get(mem);
    sys = new (sys) System(memspace, memspace_size);

    sys->racks.resize(parameters->max_racks);

    sys->max_voices = parameters->max_voices;
    sys->granularity = parameters->granularity;
    sys->sample_rate = parameters->sample_rate;

    // Alloc first block for System struct
    if (!sys->alloc_raw(sizeof(System))) {
        return false;
    }

    ngs.systems.push_back(sys);
    return true;
}

void release_system(State &ngs, const MemState &mem, System *system) {
    // this function assumes no ngs mutex is being held
    for (Rack *rack : system->racks) {
        if (!rack)
            continue;
        for (const auto &voice : rack->voices) {
            system->voice_scheduler.deque_voice(voice.get(mem));
            voice.get(mem)->~Voice();
        }
        rack->~Rack();
    }

    system->racks.clear();

    vector_utils::erase_first(ngs.systems, system);
    system->~System();
}

bool init_rack(State &ngs, const MemState &mem, System *system, SceNgsBufferInfo *init_info, const SceNgsRackDescription *description) {
    Rack *rack = init_info->data.cast<Rack>().get(mem);
    rack = new (rack) Rack(system, init_info->data, init_info->size);

    // Alloc first block for Rack
    if (!rack->alloc<Rack>()) {
        return false;
    }

    if (description->definition)
        apply_voice_definition(description->definition.get(mem), rack->modules);
    else
        rack->modules.clear();

    // Initialize voice definition
    rack->channels_per_voice = description->channels_per_voice;
    rack->max_patches_per_input = description->max_patches_per_input;
    rack->patches_per_output = description->patches_per_output;

    // Alloc spaces for voice
    rack->voices.resize(description->voice_count);
    rack->vdef = description->definition.get(mem);

    for (auto &voice : rack->voices) {
        voice = rack->alloc<Voice>();

        if (!voice) {
            return false;
        }

        Voice *v = voice.get(mem);
        new (v) Voice();
        v->init(rack);

        // Allocate parameter buffer info for each voice
        for (size_t i = 0; i < rack->modules.size(); i++) {
            v->datas[i].info.size = rack->modules[i]->get_buffer_parameter_size();
            v->datas[i].info.data = rack->alloc_raw(v->datas[i].info.size);
            // from the behavior of games, it looks like the other info buffer (there are two copies because of VoiceLock) is located right after the first
            // one, so copy this behavior, to avoid a game overwriting some important ngs struct
            rack->alloc_raw(v->datas[i].info.size);

            v->datas[i].parent = v;
            v->datas[i].index = static_cast<uint32_t>(i);
            rack->modules[i]->initialize_voice_data(v->datas[i]);
        }
    }

    system->racks.push_back(rack);

    return true;
}

void release_rack(State &ngs, const MemState &mem, System *system, Rack *rack) {
    // this function should only be called outside of ngs update and with the scheduler mutex acquired (except when releasing the system)
    if (!rack)
        return;

    auto &scheduler = system->voice_scheduler;
    if (scheduler.is_updating) {
        for (const auto &voice : rack->voices)
            scheduler.released_voices_during_update.insert(voice.get(mem));
    }

    for (Rack *other : system->racks) {
        if (!other || other == rack)
            continue;
        for (const auto &voice : other->voices) {
            Voice *source = voice.get(mem);
            const std::lock_guard lock(*source->voice_mutex);
            for (const auto &patches : source->patches) {
                for (const auto &patch_ptr : patches) {
                    Patch *patch = patch_ptr.get(mem);
                    if (patch && std::ranges::contains(rack->voices, patch->dest)) {
                        patch->output_sub_index = -1;
                        patch->dest = nullptr;
                    }
                }
            }
        }
    }

    // remove all queued voices
    for (const auto &voice : rack->voices) {
        Voice *v = voice.get(mem);
        system->voice_scheduler.deque_voice(voice.get(mem));
        // clean up host resources per voice before destroying
        for (size_t i = 0; i < rack->modules.size() && i < v->datas.size(); i++) {
            if (rack->modules[i])
                rack->modules[i]->cleanup_voice_state(v->datas[i]);
        }
        // no need to free the voice from the rack
        v->~Voice();
    }

    // A module callback can release its own rack before process() has returned.
    if (scheduler.processing_rack == rack)
        scheduler.processing_module_keepalive = std::move(rack->modules[scheduler.processing_module_index]);

    // remove from system
    vector_utils::erase_first(system->racks, rack);

    // free pointer memory
    rack->~Rack();
}

bool is_process_storage_available(const State &ngs, Address address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    const auto overlaps = [address, end](Address other, uint32_t other_size) {
        return uint64_t(address) < uint64_t(other) + other_size && uint64_t(other) < end;
    };
    if (overlaps(ngs.definitions.address(), static_cast<uint32_t>(BussType::BUSS_MAX) * sizeof(VoiceDefinition)))
        return false;
    for (const System *system : ngs.systems) {
        if (overlaps(system->memspace.address(), system->storage_size))
            return false;
        for (const Rack *rack : system->racks) {
            if (rack && overlaps(rack->memspace.address(), rack->storage_size))
                return false;
        }
    }
    return true;
}

bool get_process_rack_required_size(State &ngs, const MemState &mem, const SceNgsRackDescription &description, uint32_t &size) {
    const uint64_t base = ngs.definitions.address();
    const uint64_t address = description.definition.address();
    if (!base || address < base || address >= base + static_cast<uint32_t>(BussType::BUSS_MAX) * sizeof(VoiceDefinition) || (address - base) % sizeof(VoiceDefinition) != 0 || description.voice_count <= 0 || description.channels_per_voice < 0 || description.channels_per_voice > 2 || description.max_patches_per_input < 0 || description.patches_per_output < 0)
        return false;
    VoiceDefinition *definition = description.definition.get(mem);
    if (definition->type >= BussType::BUSS_MAX || definition->output_count == 0 || definition->output_count > MAX_OUTPUT_PORT)
        return false;
    const uint64_t per_voice = sizeof(Voice) + uint64_t(2) * get_voice_definition_size(definition) + uint64_t(description.patches_per_output) * definition->output_count * sizeof(Patch);
    const uint64_t max_size = std::numeric_limits<uint32_t>::max();
    if (per_voice > (max_size - sizeof(Rack)) / static_cast<uint32_t>(description.voice_count))
        return false;
    size = static_cast<uint32_t>(sizeof(Rack) + uint64_t(description.voice_count) * per_voice);
    return true;
}

Voice *find_process_voice(State &ngs, const MemState &mem, Ptr<Voice> voice) {
    if (!voice)
        return nullptr;
    for (System *system : ngs.systems) {
        for (Rack *rack : system->racks) {
            if (rack && std::ranges::contains(rack->voices, voice))
                return voice.get(mem);
        }
    }
    return nullptr;
}

Patch *find_process_patch(State &ngs, const MemState &mem, Ptr<Patch> patch) {
    if (!patch)
        return nullptr;
    for (System *system : ngs.systems) {
        if (!system)
            continue;
        for (Rack *rack : system->racks) {
            if (!rack)
                continue;
            for (const Ptr<Voice> voice_handle : rack->voices) {
                Voice *voice = voice_handle.get(mem);
                if (!voice)
                    continue;
                for (const auto &patches : voice->patches) {
                    if (std::ranges::contains(patches, patch))
                        return patch.get(mem);
                }
            }
        }
    }
    return nullptr;
}

bool get_process_voice_info(State &ngs, const MemState &mem, Ptr<Voice> voice, SceNgsVoiceInfo &info) {
    Voice *owned = find_process_voice(ngs, mem, voice);
    if (!owned)
        return false;
    const std::lock_guard lock(*owned->voice_mutex);
    switch (owned->state) {
    case VOICE_STATE_AVAILABLE: info.voice_state = SCE_NGS_VOICE_STATE_AVAILABLE; break;
    case VOICE_STATE_ACTIVE: info.voice_state = SCE_NGS_VOICE_STATE_ACTIVE; break;
    case VOICE_STATE_FINALIZING: info.voice_state = SCE_NGS_VOICE_STATE_FINALIZE; break;
    case VOICE_STATE_UNLOADING: info.voice_state = SCE_NGS_VOICE_STATE_UNLOADING; break;
    default: return false;
    }
    if (owned->is_pending)
        info.voice_state |= SCE_NGS_VOICE_STATE_PENDING;
    if (owned->is_paused)
        info.voice_state |= SCE_NGS_VOICE_STATE_PAUSED;
    if (owned->is_keyed_off)
        info.voice_state |= SCE_NGS_VOICE_STATE_KEY_OFF;
    info.num_modules = static_cast<SceUInt32>(owned->datas.size());
    info.num_inputs = static_cast<SceUInt32>(owned->inputs.inputs.size());
    info.num_outputs = owned->rack->vdef->output_count;
    info.num_patches_per_output = static_cast<SceUInt32>(owned->rack->patches_per_output);
    info.update_passed = owned->frame_count;
    return true;
}

bool set_process_patch_volumes(State &ngs, const MemState &mem, Ptr<Patch> patch, uint32_t offset, const float *volumes, uint32_t count) {
    constexpr uint32_t channels = SCE_NGS_MAX_SYSTEM_CHANNELS * SCE_NGS_MAX_SYSTEM_CHANNELS;
    if (offset > channels || count > channels - offset || (count && !volumes))
        return false;
    Patch *owned = find_process_patch(ngs, mem, patch);
    if (!owned)
        return false;
    Voice *source = find_process_voice(ngs, mem, owned->source);
    if (!source)
        return false;
    const std::lock_guard lock(*source->voice_mutex);
    if (!owned->is_active())
        return false;
    if (count)
        std::memcpy(reinterpret_cast<uint8_t *>(owned->volume_matrix) + offset * sizeof(float), volumes, count * sizeof(float));
    return true;
}

bool set_process_patch_matrix(State &ngs, MemState &mem, Ptr<Patch> patch, Ptr<const SceNgsVolumeMatrix> matrix) {
    std::array<float, SCE_NGS_MAX_SYSTEM_CHANNELS * SCE_NGS_MAX_SYSTEM_CHANNELS> copied{};
    if (!matrix || !read_guest_memory(mem, matrix.address(), copied.data(), sizeof(copied)))
        return false;
    return set_process_patch_volumes(ngs, mem, patch, 0, copied.data(), static_cast<uint32_t>(copied.size()));
}

bool play_process_voice(State &ngs, const MemState &mem, Ptr<Voice> voice) {
    Voice *owned_voice = find_process_voice(ngs, mem, voice);
    if (!owned_voice)
        return false;
    owned_voice->is_pending = true;
    const bool played = owned_voice->rack->system->voice_scheduler.play(mem, owned_voice);
    owned_voice->is_pending = false;
    return played;
}

bool get_process_voice_state(State &ngs, const MemState &mem, Ptr<Voice> voice, uint32_t module, uint32_t max_size, std::vector<uint8_t> &data) {
    Voice *owned_voice = find_process_voice(ngs, mem, voice);
    if (!owned_voice)
        return false;
    const std::lock_guard lock(*owned_voice->voice_mutex);
    const ModuleData *storage = owned_voice->module_storage(module);
    if (!storage)
        return false;
    const size_t count = std::min<size_t>(max_size, storage->guest_state_data.size());
    data.assign(storage->guest_state_data.begin(), storage->guest_state_data.begin() + count);
    return true;
}

Ptr<Patch> create_process_voice_patch(State &ngs, const MemState &mem, SceNgsPatchSetupInfo &info) {
    Voice *source = find_process_voice(ngs, mem, info.source);
    Voice *dest = find_process_voice(ngs, mem, info.dest);
    if (!source || !dest || source->rack->system != dest->rack->system || info.source_output_index < 0 || info.source_output_index >= MAX_OUTPUT_PORT || static_cast<uint32_t>(info.source_output_index) >= source->rack->vdef->output_count || info.source_output_subindex < -1 || info.dest_input_index < 0 || static_cast<size_t>(info.dest_input_index) >= dest->inputs.inputs.size())
        return {};
    return source->rack->system->voice_scheduler.patch(mem, &info);
}

bool remove_process_voice_patch(State &ngs, const MemState &mem, Ptr<Patch> patch) {
    if (!patch)
        return false;
    for (System *system : ngs.systems) {
        if (!system)
            continue;
        for (Rack *rack : system->racks) {
            if (!rack)
                continue;
            for (const Ptr<Voice> voice_handle : rack->voices) {
                Voice *voice = voice_handle.get(mem);
                if (!voice)
                    continue;
                for (const auto &patches : voice->patches) {
                    if (std::ranges::contains(patches, patch))
                        return voice->remove_patch(mem, patch);
                }
            }
        }
    }
    return false;
}

Ptr<VoiceDefinition> get_voice_definition(State &ngs, MemState &mem, ngs::BussType type) {
    return ngs.definitions + static_cast<int>(type);
}

Ptr<VoiceDefinition> get_process_voice_definition(State &ngs, MemState &primary_mem, MemState &process_mem, ngs::BussType type) {
    State *state = get_process_state(ngs, primary_mem, process_mem);
    if (!state) {
        LOG_ERROR("Cannot initialize NGS definitions for the calling process");
        return {};
    }
    return get_voice_definition(*state, process_mem, type);
}
} // namespace ngs
