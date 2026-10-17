// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <app/functions.h>
#include <pfsmgr/functions.h>
#include <regmgr/functions.h>

#include <stdexcept>

namespace app {

static void ensure_identity_registry(RegMgrState &registry, const fs::path &vita_fs_path) {
	if (registry.system_dreg_path.empty())
		regmgr::init_regmgr(registry, vita_fs_path);
}

void recover_savedata_identity(RegMgrState &registry, const fs::path &vita_fs_path) {
	const auto ux0 = vita_fs_path / "ux0";
	const auto pending = pfsmgr::pending_savedata_identity(ux0);
	if (!pending)
		return;
	ensure_identity_registry(registry, vita_fs_path);
	regmgr::set_account_id(registry, pending->account_id);
	pfsmgr::commit_savedata_identity(ux0);
}

pfsmgr::SavedataIdentity get_savedata_identity(RegMgrState &registry, const fs::path &vita_fs_path) {
	recover_savedata_identity(registry, vita_fs_path);
	ensure_identity_registry(registry, vita_fs_path);
	const auto account_id = regmgr::get_account_id(registry);
	return { pfsmgr::get_or_create_open_psid(vita_fs_path / "ux0"), account_id };
}

void import_savedata_identity(RegMgrState &registry, const fs::path &vita_fs_path, const pfsmgr::SavedataIdentity &identity) {
	const auto previous = get_savedata_identity(registry, vita_fs_path);
	const auto ux0 = vita_fs_path / "ux0";
	pfsmgr::stage_savedata_identity(ux0, identity, previous);
	try {
		recover_savedata_identity(registry, vita_fs_path);
	} catch (const std::runtime_error &error) {
		throw std::runtime_error(std::string("Identity import is pending and must be recovered before launching an application: ") + error.what());
	}
}

} // namespace app