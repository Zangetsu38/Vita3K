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

#include "SceNpManager.h"

#include "util/types.h"

#include <config/settings.h>
#include <io/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <np/state.h>
#include <util/log.h>

#include <np/functions.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNpManager);

enum SceNpErrorCode : uint32_t {
    SCE_NP_ERROR_ALREADY_INITIALIZED = 0x80550001,
    SCE_NP_ERROR_NOT_INITIALIZED = 0x80550002,
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x80550003,
    SCE_NP_ERROR_UNKNOWN_PLATFORM_TYPE = 0x80550004
};

enum SceNpManagerErrorCode : uint32_t {
    SCE_NP_MANAGER_ERROR_ABORTED = 0x80550507,
    SCE_NP_MANAGER_ERROR_ALREADY_INITIALIZED = 0x80550501,
    SCE_NP_MANAGER_ERROR_OUT_OF_MEMORY = 0x80550504,
    SCE_NP_MANAGER_ERROR_NOT_INITIALIZED = 0x80550502,
    SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT = 0x80550503,
    SCE_NP_MANAGER_ERROR_INVALID_STATE = 0x80550506,
    SCE_NP_MANAGER_ERROR_ID_NOT_AVAIL = 0x80550509
};

static bool psn_signed_in_for_process(EmuEnvState &emuenv, MemState &mem) {
    {
        const std::lock_guard lock(emuenv.np.process_mutex);
        if (const auto state = emuenv.np.process_psn_signed_in.find(&mem); state != emuenv.np.process_psn_signed_in.end())
            return state->second;
    }

    const auto identity = emuenv.kernel.get_process_identity(mem);
    if (!identity)
        return emuenv.cfg.current_config.psn_signed_in;

    Config process_config;
    process_config = emuenv.cfg;
    config::set_current_config(process_config, emuenv.config_path, identity->app_path);
    return process_config.current_config.psn_signed_in;
}

int sce_np_manager_populate_np_id(const std::string &user_name, np::SceNpId &id) {
    if (user_name.length() > SCE_NP_ONLINEID_MAX_LENGTH) {
        LOG_ERROR("Your online ID has over 16 characters, try again with shorter name");
        return SCE_NP_MANAGER_ERROR_ID_NOT_AVAIL;
    }

    memset(&id, 0, sizeof(id));
    strcpy(id.handle.data, user_name.c_str());
    id.isIdValid = true;
    id.opt.platformType[0] = 'p';
    id.opt.platformType[1] = 's';
    id.opt.platformType[2] = 'p';
    id.opt.platformType[3] = '2';
    return 0;
}

EXPORT(int, sceNpAuthAbortOAuthRequest) {
    TRACY_FUNC(sceNpAuthAbortOAuthRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthCreateOAuthRequest) {
    TRACY_FUNC(sceNpAuthCreateOAuthRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthDeleteOAuthRequest) {
    TRACY_FUNC(sceNpAuthDeleteOAuthRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetAuthorizationCode) {
    TRACY_FUNC(sceNpAuthGetAuthorizationCode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCheckCallback) {
    TRACY_FUNC(sceNpCheckCallback);

    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    const MemState *address_space = &thread->cpu->mem;
    const SceNpServiceState state = psn_signed_in_for_process(emuenv, thread->cpu->mem) ? SCE_NP_SERVICE_STATE_SIGNED_IN : SCE_NP_SERVICE_STATE_SIGNED_OUT;
    std::vector<SceNpServiceStateCallback> callbacks;
    {
        const std::lock_guard lock(emuenv.np.callback_mutex);
        for (const auto &[_, np_callback] : emuenv.np.cbs) {
            if (np_callback.address_space == address_space)
                callbacks.push_back(np_callback);
        }
    }
    for (const auto &np_callback : callbacks)
        thread->call_guest(np_callback.pc, { static_cast<uint32_t>(state), 0, np_callback.data });

    return STUBBED("Stub");
}

EXPORT(int, sceNpGetServiceState, Ptr<SceNpServiceState> state) {
    TRACY_FUNC(sceNpGetServiceState, state.address());
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    MemState &mem = caller->cpu->mem;
    if (!state || !state.valid(mem))
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    SceNpServiceState *state_data = state.get(mem);
    if (!state_data)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    *state_data = psn_signed_in_for_process(emuenv, mem) ? SCE_NP_SERVICE_STATE_SIGNED_IN : SCE_NP_SERVICE_STATE_SIGNED_OUT;

    return STUBBED("Stub");
}

EXPORT(int, sceNpInit, Ptr<const np::CommunicationConfig> comm_config, Ptr<void> dontcare) {
    TRACY_FUNC(sceNpInit, comm_config, dontcare);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    MemState &mem = caller->cpu->mem;
    np::CommunicationConfig config{};
    np::CommunicationID communication_id{};
    const np::CommunicationID *comm_id = nullptr;
    if (comm_config) {
        if (!read_guest_memory(mem, comm_config.address(), &config, sizeof(config)))
            return SCE_NP_ERROR_INVALID_ARGUMENT;
        if (config.comm_id) {
            if (!read_guest_memory(mem, config.comm_id.address(), &communication_id, sizeof(communication_id)))
                return SCE_NP_ERROR_INVALID_ARGUMENT;
            comm_id = &communication_id;
        }
    }

    const bool process_psn_signed_in = psn_signed_in_for_process(emuenv, mem);
    const std::lock_guard lock(emuenv.np.process_mutex);
    if (emuenv.np.initialized_processes.contains(&mem))
        return SCE_NP_ERROR_ALREADY_INITIALIZED;
    if (!emuenv.np.inited && !init(emuenv.np, comm_id))
        return SCE_NP_ERROR_NOT_INITIALIZED;
    emuenv.np.initialized_processes.insert(&mem);
    emuenv.np.process_psn_signed_in[&mem] = process_psn_signed_in;

    return 0;
}

EXPORT(int, sceNpManagerGetAccountRegion) {
    TRACY_FUNC(sceNpManagerGetAccountRegion);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpManagerGetCachedParam) {
    TRACY_FUNC(sceNpManagerGetCachedParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpManagerGetChatRestrictionFlag, Ptr<SceInt> isRestricted) {
    TRACY_FUNC(sceNpManagerGetChatRestrictionFlag, isRestricted.address());
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    MemState &mem = caller->cpu->mem;
    if (!isRestricted || !isRestricted.valid(mem))
        return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;
    SceInt *is_restricted = isRestricted.get(mem);
    if (!is_restricted)
        return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;

    *is_restricted = 0; // User is never restricted
    return 0;
}

EXPORT(int, sceNpManagerGetContentRatingFlag, Ptr<SceInt> isRestricted, Ptr<SceInt> age) {
    TRACY_FUNC(sceNpManagerGetContentRatingFlag, isRestricted.address(), age.address());
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    MemState &mem = caller->cpu->mem;
    if (!isRestricted || !isRestricted.valid(mem) || !age || !age.valid(mem))
        return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;
    SceInt *is_restricted = isRestricted.get(mem);
    SceInt *age_data = age.get(mem);
    if (!is_restricted || !age_data)
        return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;

    *is_restricted = 0; // User is never restricted
    *age_data = 21; // Assume user is 21 years old
    return STUBBED("isRestricted = 0; age = 21; return 0;");
}

EXPORT(int, sceNpManagerGetNpId, Ptr<np::SceNpId> id) {
    TRACY_FUNC(sceNpManagerGetNpId, id.address());
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    MemState &mem = caller->cpu->mem;
    if (!id || !id.valid(mem))
        return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;
    np::SceNpId *id_data = id.get(mem);
    if (!id_data)
        return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;

    return sce_np_manager_populate_np_id(emuenv.io.user_name, *id_data);
}

EXPORT(int, SceNpManager_E45E4003, Ptr<void> account_info) {
    TRACY_FUNC(SceNpManager_E45E4003, account_info.address());
    if (!account_info)
        return RET_ERROR(SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT);
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller || !account_info.valid(caller->cpu->mem))
        return RET_ERROR(SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT);

    LOG_WARN_ONCE("NP account information is unavailable; refusing to return an uninitialized account record");
    return RET_ERROR(SCE_NP_MANAGER_ERROR_ID_NOT_AVAIL);
}

EXPORT(int, sceNpRegisterServiceStateCallback, Ptr<void> callback, Ptr<void> data) {
    TRACY_FUNC(sceNpRegisterServiceStateCallback, callback.address(), data.address());
    const auto caller = emuenv.kernel.get_thread(thread_id);
    if (!caller || !callback || !is_valid_addr(caller->cpu->mem, callback.address() & ~1))
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    const MemState *address_space = &caller->cpu->mem;
    SceUID cid;
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        cid = emuenv.kernel.get_next_uid();
    }
    {
        const std::lock_guard lock(emuenv.np.callback_mutex);
        if (const auto previous = emuenv.np.state_cb_ids.find(address_space); previous != emuenv.np.state_cb_ids.end())
            emuenv.np.cbs.erase(previous->second);
        emuenv.np.cbs[cid] = { .pc = callback.address(), .data = data.address(), .address_space = address_space };
        emuenv.np.state_cb_ids[address_space] = cid;
    }
    return 0;
}

EXPORT(void, sceNpTerm) {
    TRACY_FUNC(sceNpTerm);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller) {
        LOG_WARN("NP library termination called from an unknown thread");
        return;
    }
    const MemState *address_space = &caller->cpu->mem;
    const std::lock_guard process_lock(emuenv.np.process_mutex);
    if (emuenv.np.initialized_processes.erase(address_space) == 0) {
        LOG_WARN("NP library not initialized for this process but termination got called");
        return;
    }
    emuenv.np.process_psn_signed_in.erase(address_space);
    {
        const std::lock_guard callback_lock(emuenv.np.callback_mutex);
        if (const auto callback = emuenv.np.state_cb_ids.find(address_space); callback != emuenv.np.state_cb_ids.end()) {
            emuenv.np.cbs.erase(callback->second);
            emuenv.np.state_cb_ids.erase(callback);
        }
    }
    if (emuenv.np.initialized_processes.empty())
        deinit(emuenv.np);
}

EXPORT(int, sceNpUnregisterServiceStateCallback) {
    TRACY_FUNC(sceNpUnregisterServiceStateCallback);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (!caller)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    const MemState *address_space = &caller->cpu->mem;
    const std::lock_guard lock(emuenv.np.callback_mutex);
    if (const auto callback = emuenv.np.state_cb_ids.find(address_space); callback != emuenv.np.state_cb_ids.end()) {
        emuenv.np.cbs.erase(callback->second);
        emuenv.np.state_cb_ids.erase(callback);
    }
    return 0;
}
