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

#include <ngs/modules/envelope.h>
#include <util/log.h>

#include <algorithm>

namespace ngs {

bool EnvelopeModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (!data.is_bypassed) {
        const SceNgsEnvelopeParams *params = data.get_parameters<SceNgsEnvelopeParams>(mem);
        if (params->desc.id == SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID) {
            SceNgsEnvelopeStates *state = data.get_state<SceNgsEnvelopeStates>();

            if (!data.parent->is_keyed_off) {
                state->nReleasing = 0;
                state->fPosition = 0.0f;
                return false;
            }

            if (!state->nReleasing) {
                state->nReleasing = 1;
                state->fPosition = 0.0f;
            }

            if (params->uReleaseMsecs == 0) {
                return true;
            }

            const uint64_t release_samples = std::max<uint64_t>(1, static_cast<uint64_t>(params->uReleaseMsecs) * data.parent->rack->system->sample_rate / 1000);
            state->fPosition += static_cast<float>(data.parent->rack->system->granularity);
            return state->fPosition >= static_cast<float>(release_samples);
        }

        LOG_WARN_ONCE("Game is using unimplemented envelope audio module");

        if (data.parent->is_keyed_off) {
            return true;
        }
    }

    return false;
}
} // namespace ngs
