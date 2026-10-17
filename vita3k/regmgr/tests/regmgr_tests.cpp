// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <regmgr/functions.h>
#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

class AccountIdTest : public ::testing::Test {
protected:
	fs::path root;
	RegMgrState state;
	const std::array<uint8_t, 8> id{ 1, 2, 3, 4, 5, 6, 7, 8 };

	void SetUp() override {
		static unsigned sequence = 0;
		do {
			root = fs::path("regmgr-account-test-" + std::to_string(sequence++));
		} while (!fs::create_directory(root));
		fs::create_directories(root / "os0" / "kd");
		fs::create_directories(root / "vd0" / "registry");
		constexpr std::array<unsigned char, 16> key{ 0x89, 0xFA, 0x95, 0x48, 0xCB, 0x6D, 0x77, 0x9D, 0xA2, 0x25, 0x34, 0xFD, 0xA9, 0x35, 0x59, 0x6E };
		const std::string tmpl = "[BASE\n1=CONFIG/\n2=NP/\n3=account_id\n4=other\n[REG-BAS\n1/2/3=2:8:0\n1/2/4=2:4:0\n[REG-J1\n";
		std::ofstream file(root / "os0" / "kd" / "registry.db0", std::ios::binary);
		const std::array<char, 138> header{};
		file.write(header.data(), header.size());
		for (size_t i = 0; i < tmpl.size(); ++i)
			file.put(static_cast<char>(tmpl[i] ^ key[i % key.size()]));
	}

	void TearDown() override { fs::remove_all(root); }
	fs::path path() const { return root / "vd0" / "registry" / "system.dreg"; }
	std::vector<char> contents(const fs::path &file_path) const {
		std::ifstream file(file_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}
	void initialize() { regmgr::init_regmgr(state, root); }
};

TEST_F(AccountIdTest, RoundTripBackupAndOtherValues) {
	initialize();
	const auto original = contents(path());
	ASSERT_TRUE(state.system_dreg_valid);
	EXPECT_EQ(regmgr::get_account_id(state), (std::array<uint8_t, 8>{}));
	regmgr::set_account_id(state, id, true);
	EXPECT_EQ(contents(path().string() + ".bak"), original);
	EXPECT_EQ(regmgr::get_account_id(state), id);
	RegMgrState reloaded;
	regmgr::init_regmgr(reloaded, root);
	EXPECT_EQ(regmgr::get_account_id(reloaded), id);
	const std::array<char, 4> other{ 9, 8, 7, 6 };
	regmgr::set_bin_value(state, "/CONFIG/NP", "other", other.data(), other.size());
	EXPECT_EQ(regmgr::get_account_id(state), id);
	const auto changed = contents(path());
	EXPECT_THROW(regmgr::set_account_id(state, {}, true), std::system_error);
	EXPECT_EQ(contents(path()), changed);
	EXPECT_EQ(regmgr::get_account_id(state), id);
}

TEST_F(AccountIdTest, RejectsInvalidTemplateAndInconsistentState) {
	initialize();
	const auto original = contents(path());
	auto &entry = state.reg_template.at("/CONFIG/NP/").at(0);
	entry.type = REG_TYPE_INT;
	EXPECT_THROW(regmgr::set_account_id(state, id), std::runtime_error);
	entry.type = REG_TYPE_BIN;
	entry.size = 7;
	EXPECT_THROW(regmgr::set_account_id(state, id), std::runtime_error);
	entry.size = 8;
	state.system_dreg.at("/CONFIG/NP/").at("other").pop_back();
	EXPECT_THROW(regmgr::set_account_id(state, id), std::runtime_error);
	EXPECT_EQ(contents(path()), original);
}

TEST_F(AccountIdTest, RejectsMissingAccountTemplate) {
	initialize();
	const auto original = contents(path());
	state.reg_template.at("/CONFIG/NP/").erase(state.reg_template.at("/CONFIG/NP/").begin());
	EXPECT_THROW(regmgr::get_account_id(state), std::runtime_error);
	EXPECT_THROW(regmgr::set_account_id(state, id), std::runtime_error);
	EXPECT_EQ(contents(path()), original);
}

TEST_F(AccountIdTest, MalformedExistingRegistryIsNeverReinitialized) {
	initialize();
	auto truncated = contents(path());
	ASSERT_GT(truncated.size(), 1u);
	truncated.pop_back();
	{
		std::ofstream file(path(), std::ios::binary | std::ios::trunc);
		file.write(truncated.data(), truncated.size());
	}
	RegMgrState failed;
	regmgr::init_regmgr(failed, root);
	EXPECT_FALSE(failed.system_dreg_valid);
	EXPECT_THROW(regmgr::get_account_id(failed), std::runtime_error);
	EXPECT_THROW(regmgr::set_account_id(failed, id), std::runtime_error);
	regmgr::set_bin_value(failed, "/CONFIG/NP", "account_id", id.data(), id.size());
	regmgr::set_str_value(failed, "/CONFIG/NP", "other", "test", 4);
	EXPECT_EQ(contents(path()), truncated);
}

TEST_F(AccountIdTest, DetectsExternalModification) {
	initialize();
	auto modified = contents(path());
	modified.pop_back();
	{
		std::ofstream file(path(), std::ios::binary | std::ios::trunc);
		file.write(modified.data(), modified.size());
	}
	EXPECT_THROW(regmgr::set_account_id(state, id), std::runtime_error);
	EXPECT_EQ(contents(path()), modified);
	EXPECT_EQ(state.system_dreg.at("/CONFIG/NP/").at("account_id"), (std::vector<char>(8)));
}

#ifdef _WIN32
TEST_F(AccountIdTest, ReplacementFailurePreservesFileAndMemory) {
	initialize();
	const auto original = contents(path());
	HANDLE blocker = CreateFileW(path().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
	EXPECT_THROW(regmgr::set_account_id(state, id), std::system_error);
	CloseHandle(blocker);
	EXPECT_EQ(contents(path()), original);
	EXPECT_EQ(regmgr::get_account_id(state), (std::array<uint8_t, 8>{}));
}
#endif

} // namespace