// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <packages/sfo.h>
#include <pfsmgr/functions.h>
#include <app/functions.h>
#include <regmgr/functions.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;

class SavedataIdentityTest : public testing::Test {
protected:
	fs::path root;
	pfsmgr::SavedataIdentity identity{};

	void SetUp() override {
		std::random_device random;
		for (int attempt = 0; attempt < 100; ++attempt) {
			const auto candidate = fs::temp_directory_path() / ("vita3k-identity-test-" + std::to_string(random()));
			if (fs::create_directory(candidate)) {
				root = candidate;
				break;
			}
		}
		ASSERT_FALSE(root.empty());
		fs::create_directory(root / "ux0");
		for (size_t i = 0; i < identity.open_psid.size(); ++i)
			identity.open_psid[i] = static_cast<uint8_t>(i + 1);
		for (size_t i = 0; i < identity.account_id.size(); ++i)
			identity.account_id[i] = static_cast<uint8_t>(0xe0 + i);
	}

	void TearDown() override {
		if (!root.empty())
			fs::remove_all(root);
	}

	void create_registry_template() {
		fs::create_directories(root / "os0" / "kd");
		fs::create_directories(root / "vd0" / "registry");
		constexpr std::array<uint8_t, 16> key{ 0x89, 0xFA, 0x95, 0x48, 0xCB, 0x6D, 0x77, 0x9D, 0xA2, 0x25, 0x34, 0xFD, 0xA9, 0x35, 0x59, 0x6E };
		const std::string data = "[BASE\n1=CONFIG/\n2=NP/\n3=account_id\n[REG-BAS\n1/2/3=2:8:0\n[REG-J1\n";
		std::ofstream output(root / "os0" / "kd" / "registry.db0", std::ios::binary);
		const std::array<char, 138> header{};
		output.write(header.data(), header.size());
		for (size_t i = 0; i < data.size(); ++i)
			output.put(static_cast<char>(data[i] ^ key[i % key.size()]));
	}
};

TEST_F(SavedataIdentityTest, ImportsBothFieldsAndBacksUpPreviousIdentity) {
	const auto ux0 = root / "ux0";
	const pfsmgr::SavedataIdentity previous{ pfsmgr::get_or_create_open_psid(ux0), {} };
	EXPECT_TRUE(std::ranges::all_of(previous.account_id, [](uint8_t value) { return value == 0; }));

	pfsmgr::create_savedata(root / "SOURCE", { "SOURCE", "PCSF00024", identity });
	const auto source = root / "SOURCE" / "sce_sys" / "param.sfo";
	const auto parsed = sfo::read_savedata_identity(source);
	EXPECT_EQ(parsed.open_psid, identity.open_psid);
	EXPECT_EQ(parsed.account_id, identity.account_id);
	std::ifstream input(source, std::ios::binary);
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	const auto parsed_bytes = sfo::read_savedata_identity(bytes);
	EXPECT_EQ(parsed_bytes.open_psid, identity.open_psid);
	EXPECT_EQ(parsed_bytes.account_id, identity.account_id);
	pfsmgr::stage_savedata_identity(ux0, parsed, previous);
	ASSERT_TRUE(pfsmgr::pending_savedata_identity(ux0));
	EXPECT_THROW(pfsmgr::get_or_create_open_psid(ux0), std::runtime_error);
	pfsmgr::commit_savedata_identity(ux0);

	EXPECT_FALSE(pfsmgr::pending_savedata_identity(ux0));
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(ux0), identity.open_psid);
	size_t backups = 0;
	for (const auto &entry : fs::directory_iterator(ux0 / "data" / "vita3k")) {
		if (!entry.path().filename().string().starts_with("savedata-identity-backup-"))
			continue;
		++backups;
		std::array<uint8_t, 24> backup{};
		std::ifstream input(entry.path(), std::ios::binary);
		ASSERT_TRUE(input.read(reinterpret_cast<char *>(backup.data()), backup.size()));
		EXPECT_TRUE(std::equal(previous.open_psid.begin(), previous.open_psid.end(), backup.begin()));
		EXPECT_TRUE(std::equal(previous.account_id.begin(), previous.account_id.end(), backup.begin() + 16));
	}
	EXPECT_EQ(backups, 1);

	pfsmgr::create_savedata(root / "NEW", { "NEW", "PCSF00024", identity });
	const auto saved = sfo::read_savedata_identity(root / "NEW" / "sce_sys" / "param.sfo");
	EXPECT_EQ(saved.open_psid, identity.open_psid);
	EXPECT_EQ(saved.account_id, identity.account_id);
}

TEST_F(SavedataIdentityTest, RejectsInvalidInputWithoutReplacingIdentity) {
	const auto ux0 = root / "ux0";
	const pfsmgr::SavedataIdentity previous{ pfsmgr::get_or_create_open_psid(ux0), {} };
	pfsmgr::stage_savedata_identity(ux0, identity, previous);
	pfsmgr::commit_savedata_identity(ux0);
	std::ofstream(root / "invalid.sfo", std::ios::binary) << "invalid";
	EXPECT_THROW(sfo::read_savedata_identity(root / "invalid.sfo"), std::runtime_error);
	EXPECT_THROW(sfo::read_savedata_identity(std::vector<uint8_t>{ 0, 1, 2 }), std::runtime_error);
	EXPECT_THROW(sfo::read_savedata_identity(std::vector<uint8_t>(1024 * 1024 + 1)), std::runtime_error);
	EXPECT_THROW(pfsmgr::stage_savedata_identity(ux0, {}, identity), std::runtime_error);
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(ux0), identity.open_psid);
}

TEST_F(SavedataIdentityTest, MigratesLegacyImportedIdentityWithoutLosingEitherField) {
	const auto ux0 = root / "ux0";
	const auto legacy = ux0 / ".vita3k-console-identity";
	fs::create_directory(legacy);
	std::ofstream output(legacy / "savedata-identity", std::ios::binary);
	output.write(reinterpret_cast<const char *>(identity.open_psid.data()), identity.open_psid.size());
	output.write(reinterpret_cast<const char *>(identity.account_id.data()), identity.account_id.size());
	output.close();
	const auto pending = pfsmgr::pending_savedata_identity(ux0);
	ASSERT_TRUE(pending);
	EXPECT_EQ(pending->open_psid, identity.open_psid);
	EXPECT_EQ(pending->account_id, identity.account_id);
	pfsmgr::commit_savedata_identity(ux0);
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(ux0), identity.open_psid);
	EXPECT_EQ(fs::file_size(ux0 / "data" / "vita3k" / "openpsid.bin"), 16);
	EXPECT_TRUE(fs::exists(legacy / "savedata-identity"));
}

#ifdef _WIN32
TEST_F(SavedataIdentityTest, RegistryWriteFailureLeavesRecoverableTransaction) {
	create_registry_template();
	RegMgrState registry;
	const auto previous = app::get_savedata_identity(registry, root);
	const auto path = root / "vd0" / "registry" / "system.dreg";
	HANDLE blocker = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
	EXPECT_THROW(app::import_savedata_identity(registry, root, identity), std::runtime_error);
	CloseHandle(blocker);
	ASSERT_TRUE(pfsmgr::pending_savedata_identity(root / "ux0"));
	EXPECT_EQ(regmgr::get_account_id(registry), previous.account_id);
	RegMgrState restarted;
	const auto recovered = app::get_savedata_identity(restarted, root);
	EXPECT_EQ(recovered.open_psid, identity.open_psid);
	EXPECT_EQ(recovered.account_id, identity.account_id);
}
#endif

TEST_F(SavedataIdentityTest, MigratesLegacyDefaultOpenPsid) {
	const auto ux0 = root / "ux0";
	const auto legacy = ux0 / ".vita3k-console-identity";
	fs::create_directory(legacy);
	std::ofstream output(legacy / "openpsid", std::ios::binary);
	output.write(reinterpret_cast<const char *>(identity.open_psid.data()), identity.open_psid.size());
	output.close();
	EXPECT_FALSE(pfsmgr::pending_savedata_identity(ux0));
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(ux0), identity.open_psid);
	EXPECT_EQ(fs::file_size(ux0 / "data" / "vita3k" / "openpsid.bin"), 16);
}

TEST_F(SavedataIdentityTest, ExistingNewOpenPsidIsNotOverwrittenByLegacyIdentity) {
	const auto ux0 = root / "ux0";
	const auto current = pfsmgr::get_or_create_open_psid(ux0);
	const auto legacy = ux0 / ".vita3k-console-identity";
	fs::create_directory(legacy);
	std::ofstream output(legacy / "savedata-identity", std::ios::binary);
	output.write(reinterpret_cast<const char *>(identity.open_psid.data()), identity.open_psid.size());
	output.write(reinterpret_cast<const char *>(identity.account_id.data()), identity.account_id.size());
	output.close();
	EXPECT_FALSE(pfsmgr::pending_savedata_identity(ux0));
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(ux0), current);
}

TEST_F(SavedataIdentityTest, ImportUpdatesRegistryAndNewSfoAfterRestart) {
	create_registry_template();
	RegMgrState first;
	app::import_savedata_identity(first, root, identity);
	EXPECT_EQ(regmgr::get_account_id(first), identity.account_id);
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(root / "ux0"), identity.open_psid);

	RegMgrState restarted;
	const auto current = app::get_savedata_identity(restarted, root);
	EXPECT_EQ(current.open_psid, identity.open_psid);
	EXPECT_EQ(current.account_id, identity.account_id);
	pfsmgr::create_savedata(root / "NEW", { "NEW", "PCSF00024", current });
	const auto saved = sfo::read_savedata_identity(root / "NEW" / "sce_sys" / "param.sfo");
	EXPECT_EQ(saved.open_psid, identity.open_psid);
	EXPECT_EQ(saved.account_id, identity.account_id);

	const pfsmgr::AccountId changed{ 1, 2, 3, 4, 5, 6, 7, 8 };
	regmgr::set_account_id(restarted, changed);
	const auto updated = app::get_savedata_identity(restarted, root);
	EXPECT_EQ(updated.account_id, changed);
	pfsmgr::create_savedata(root / "AFTER", { "AFTER", "PCSF00024", updated });
	EXPECT_EQ(sfo::read_savedata_identity(root / "AFTER" / "sce_sys" / "param.sfo").account_id, changed);
}

TEST_F(SavedataIdentityTest, RecoversInterruptedImportBeforeUsingIdentity) {
	create_registry_template();
	RegMgrState first;
	const auto previous = app::get_savedata_identity(first, root);
	pfsmgr::stage_savedata_identity(root / "ux0", identity, previous);

	RegMgrState restarted;
	app::recover_savedata_identity(restarted, root);
	EXPECT_EQ(regmgr::get_account_id(restarted), identity.account_id);
	EXPECT_EQ(pfsmgr::get_or_create_open_psid(root / "ux0"), identity.open_psid);
	EXPECT_FALSE(pfsmgr::pending_savedata_identity(root / "ux0"));
}

TEST_F(SavedataIdentityTest, MigratesLegacyPairIntoRegistry) {
	create_registry_template();
	const auto legacy = root / "ux0" / ".vita3k-console-identity";
	fs::create_directory(legacy);
	std::ofstream output(legacy / "savedata-identity", std::ios::binary);
	output.write(reinterpret_cast<const char *>(identity.open_psid.data()), identity.open_psid.size());
	output.write(reinterpret_cast<const char *>(identity.account_id.data()), identity.account_id.size());
	output.close();
	RegMgrState registry;
	app::recover_savedata_identity(registry, root);
	const auto current = app::get_savedata_identity(registry, root);
	EXPECT_EQ(current.open_psid, identity.open_psid);
	EXPECT_EQ(current.account_id, identity.account_id);
	EXPECT_EQ(regmgr::get_account_id(registry), identity.account_id);
	EXPECT_TRUE(fs::exists(legacy / "savedata-identity"));
}

TEST_F(SavedataIdentityTest, OptionalInstalledFirmwareRegistryRoundTrip) {
	const auto *source = std::getenv("VITA3K_TEST_FIRMWARE_PATH");
	if (!source || !*source)
		GTEST_SKIP() << "VITA3K_TEST_FIRMWARE_PATH not set";
	const fs::path firmware(source);
	fs::create_directories(root / "os0" / "kd");
	fs::create_directories(root / "vd0" / "registry");
	fs::copy_file(firmware / "os0" / "kd" / "registry.db0", root / "os0" / "kd" / "registry.db0");
	fs::copy_file(firmware / "vd0" / "registry" / "system.dreg", root / "vd0" / "registry" / "system.dreg");
	RegMgrState registry;
	app::import_savedata_identity(registry, root, identity);
	RegMgrState restarted;
	const auto current = app::get_savedata_identity(restarted, root);
	ASSERT_TRUE(current.open_psid == identity.open_psid);
	ASSERT_TRUE(current.account_id == identity.account_id);
}

TEST_F(SavedataIdentityTest, RejectsMissingAndMalformedSavedataFields) {
	pfsmgr::create_savedata(root / "SOURCE", { "SOURCE", "PCSF00024", identity });
	const auto source = root / "SOURCE" / "sce_sys" / "param.sfo";
	std::ifstream input(source, std::ios::binary);
	const std::vector<uint8_t> original{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
	SfoFile parsed;
	ASSERT_TRUE(sfo::load(parsed, original));
	const auto account = std::ranges::find_if(parsed.entries, [](const auto &entry) { return entry.data.first == "ACCOUNT_ID"; });
	const auto params = std::ranges::find_if(parsed.entries, [](const auto &entry) { return entry.data.first == "PARAMS"; });
	ASSERT_NE(account, parsed.entries.end());
	ASSERT_NE(params, parsed.entries.end());
	const auto candidate = root / "candidate.sfo";
	const auto write_candidate = [&](const std::vector<uint8_t> &bytes) {
		std::ofstream output(candidate, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	};

	auto invalid = original;
	invalid.resize(20);
	write_candidate(invalid);
	EXPECT_THROW(sfo::read_savedata_identity(candidate), std::runtime_error);

	invalid = original;
	const auto account_index = static_cast<size_t>(account - parsed.entries.begin());
	invalid[sizeof(SfoHeader) + account_index * sizeof(SfoIndexTableEntry) + 4] = 7;
	write_candidate(invalid);
	EXPECT_THROW(sfo::read_savedata_identity(candidate), std::runtime_error);

	invalid = original;
	invalid[parsed.header.key_table_start + params->entry.key_offset] = 'X';
	write_candidate(invalid);
	EXPECT_THROW(sfo::read_savedata_identity(candidate), std::runtime_error);
}
} // namespace