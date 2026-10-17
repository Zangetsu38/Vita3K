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

#include <kernel/types.h>
#include <module/module.h>

#include <fmt/format.h>

#include <cstring>

EXPORT(int, _sceNpDrmCheckActData) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceNpDrmCheckDrmReset) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceNpDrmGetFixedRifName, Ptr<char> rif_name, uint64_t account_id) {
    LOG_INFO_SYNC("_sceNpDrmGetFixedRifName: rif_name={}, account_id={}", log_hex(rif_name.address()), log_hex(account_id));
    if (!rif_name)
        return RET_ERROR(0x80870001);
    return RET_ERROR(SCE_ERROR_ERRNO_ENOSYS);
}

EXPORT(int, _sceNpDrmGetRifInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceNpDrmGetRifName, Ptr<char> rif_name, uint64_t account_id) {
    LOG_INFO_SYNC("_sceNpDrmGetRifName: rif_name={}, account_id={}", log_hex(rif_name.address()), log_hex(account_id));
    const auto account_rif_name = fmt::format("{:016x}.rif", account_id);
    if (!rif_name)
        return RET_ERROR(0x80870001);
    for (size_t i = 0; i <= account_rif_name.size(); ++i) {
        if (!Ptr<char>(rif_name.address() + i).valid(emuenv.mem))
            return RET_ERROR(0x80870001);
    }
    std::memcpy(rif_name.get(emuenv.mem), account_rif_name.c_str(), account_rif_name.size() + 1);
    return 0;
}

EXPORT(int, _sceNpDrmGetRifNameForInstall) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceNpDrmPresetRifProvisionalFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, _sceNpDrmRemoveActData) {
    return UNIMPLEMENTED();
}
