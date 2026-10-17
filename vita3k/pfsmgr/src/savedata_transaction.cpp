// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "savedata_transaction.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#endif

namespace pfsmgr::detail {
namespace fs = std::filesystem;

namespace {
constexpr uint64_t max_file_size = 64 * 1024 * 1024;
constexpr uint64_t max_journal_size = 3 * max_file_size + 4096;
constexpr std::array<uint8_t, 8> magic{ 'V', '3', 'K', 'P', 'F', 'S', 'J', '1' };
constexpr std::array<uint8_t, 8> directory_magic{ 'V', '3', 'K', 'P', 'F', 'S', 'D', '1' };
constexpr std::array<uint8_t, 8> removal_magic{ 'V', '3', 'K', 'P', 'F', 'S', 'R', '1' };
constexpr size_t mac_size = 32;
constexpr auto directory_marker = ".vita3k-directory";

void sync_directory(const fs::path &path);
std::array<uint8_t, mac_size> authenticate(const Hash &key, std::span<const uint8_t> data);

void sync_file(const fs::path &path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "PFS: cannot open file for synchronization");
    const bool success = FlushFileBuffers(handle) != 0;
    const auto error = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!success)
        throw std::system_error(error, std::system_category(), "PFS: cannot synchronize file");
#else
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "PFS: cannot open file for synchronization");
    const int result = fsync(fd);
    const int error = errno;
    close(fd);
    if (result != 0)
        throw std::system_error(error, std::generic_category(), "PFS: cannot synchronize file");
#endif
}

void publish_directory(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
        throw fs::filesystem_error("PFS: cannot publish directory", from, to,
            std::error_code(GetLastError(), std::system_category()));
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1) != 0)
        throw std::system_error(errno, std::generic_category(), "PFS: cannot publish directory");
#elif defined(__APPLE__)
    if (renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) != 0)
        throw std::system_error(errno, std::generic_category(), "PFS: cannot publish directory");
#else
    throw std::runtime_error("PFS: safe directory publication is unavailable on this platform");
#endif
    sync_directory(to.parent_path());
    if (from.parent_path() != to.parent_path())
        sync_directory(from.parent_path());
}

void sync_directory(const fs::path &path) {
#ifndef _WIN32
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "PFS: cannot open directory for synchronization");
    const int result = fsync(fd);
    const int error = errno;
    close(fd);
    if (result != 0)
        throw std::system_error(error, std::generic_category(), "PFS: cannot synchronize directory");
#else
    (void)path;
#endif
}

void replace_file(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(GetLastError(), std::system_category(), "PFS: cannot publish file");
#else
    fs::rename(from, to);
#endif
    sync_directory(to.parent_path());
    if (from.parent_path() != to.parent_path())
        sync_directory(from.parent_path());
}

void write_file(const fs::path &path, std::span<const uint8_t> data) {
    const auto status = fs::symlink_status(path);
    if (fs::exists(status) && !fs::is_regular_file(status))
        throw std::runtime_error("PFS: invalid transaction file");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output || (!data.empty() && !output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()))))
            throw std::runtime_error("PFS: cannot write transaction file");
        output.flush();
        if (!output)
            throw std::runtime_error("PFS: cannot flush transaction file");
        output.close();
        if (!output)
            throw std::runtime_error("PFS: cannot close transaction file");
    }
    sync_file(path);
}

std::vector<uint8_t> read_file(const fs::path &path, uint64_t maximum) {
    if (!fs::is_regular_file(fs::symlink_status(path)))
        throw std::runtime_error("PFS: transaction input is not a regular file");
    const auto size = fs::file_size(path);
    if (size > maximum)
        throw std::runtime_error("PFS: transaction input too large");
    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input || (!result.empty() && !input.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(result.size()))))
        throw std::runtime_error("PFS: cannot read transaction input");
    return result;
}

std::array<uint8_t, mac_size> transaction_token(const Hash &key, std::span<const uint8_t> journal, uint8_t purpose) {
    std::vector<uint8_t> input;
    input.reserve(journal.size() + 1);
    input.push_back(purpose);
    input.insert(input.end(), journal.begin(), journal.end());
    return authenticate(key, input);
}

void remove_staged_directory(const fs::path &directory) {
    const auto status = fs::symlink_status(directory);
    if (!fs::exists(status))
        return;
    if (!fs::is_directory(status))
        throw std::runtime_error("PFS: invalid staged directory");
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.path().filename() != directory_marker || !fs::is_regular_file(fs::symlink_status(entry.path())))
            throw std::runtime_error("PFS: unexpected staged directory contents");
    }
    fs::remove(directory / directory_marker);
    if (!fs::remove(directory))
        throw std::runtime_error("PFS: cannot remove staged directory");
    sync_directory(directory.parent_path());
}

void finish_directory(const fs::path &directory, const fs::path &stage, std::span<const uint8_t> token, bool committed) {
    const auto status = fs::symlink_status(directory);
    if (!fs::exists(status)) {
        if (committed)
            throw std::runtime_error("PFS: committed directory is missing");
        return;
    }
    if (!fs::is_directory(status))
        throw std::runtime_error("PFS: invalid published directory");
    const auto marker = directory / directory_marker;
    if (!fs::exists(fs::symlink_status(marker))) {
        if (!committed)
            throw std::runtime_error("PFS: cannot identify directory to roll back");
        return;
    }
    const auto actual = read_file(marker, mac_size);
    if (actual.size() != token.size() || CRYPTO_memcmp(actual.data(), token.data(), token.size()) != 0)
        throw std::runtime_error("PFS: invalid directory transaction marker");
    if (!committed) {
        for (const auto &entry : fs::directory_iterator(directory)) {
            if (entry.path().filename() != directory_marker)
                throw std::runtime_error("PFS: directory to roll back is not empty");
        }
        publish_directory(directory, stage);
        remove_staged_directory(stage);
        return;
    }
    fs::remove(marker);
    sync_directory(directory);
}

std::string safe_path(const fs::path &relative) {
    if (relative.empty() || relative.has_root_path())
        throw std::runtime_error("PFS: invalid transaction path");
    const auto utf8 = relative.generic_u8string();
    std::string name(utf8.begin(), utf8.end());
    if (name.size() > 255)
        throw std::runtime_error("PFS: transaction path too long");
    for (const auto &part : relative) {
        const auto text = part.generic_u8string();
        if (text.empty() || text == u8"." || text == u8".." || text.back() == u8'.')
            throw std::runtime_error("PFS: invalid transaction path component");
        for (const auto ch : text) {
            if (!((ch >= u8'a' && ch <= u8'z') || (ch >= u8'A' && ch <= u8'Z')
                    || (ch >= u8'0' && ch <= u8'9') || ch == u8'.' || ch == u8'_' || ch == u8'-'))
                throw std::runtime_error("PFS: unsupported transaction path component");
        }
        std::string base(text.begin(), text.end());
        base = base.substr(0, base.find('.'));
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; });
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL"
            || (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9'))
            throw std::runtime_error("PFS: reserved transaction path component");
    }
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; });
    if (upper == "SCE_SYS/SEALEDKEY" || upper.starts_with("SCE_PFS/.VITA3K-TRANSACTION")
        || (upper.starts_with("SCE_PFS/") && upper != "SCE_PFS/FILES.DB" && !upper.starts_with("SCE_PFS/ICV.DB/")))
        throw std::runtime_error("PFS: protected transaction path");
    return name;
}

fs::path checked_target(const fs::path &root, const fs::path &relative, bool directory = false, bool allow_other = false) {
    safe_path(relative);
    if (!fs::is_directory(fs::symlink_status(root)))
        throw std::runtime_error("PFS: invalid savedata root");
    fs::path current = root;
    for (const auto &part : relative.parent_path()) {
        current /= part;
        if (!fs::is_directory(fs::symlink_status(current)))
            throw std::runtime_error("PFS: invalid transaction parent");
    }
    current /= relative.filename();
    const auto status = fs::symlink_status(current);
    if (!allow_other && fs::exists(status) && !(directory ? fs::is_directory(status) : fs::is_regular_file(status)))
        throw std::runtime_error("PFS: invalid transaction target");
    return current;
}

void append_number(std::vector<uint8_t> &out, uint64_t number, size_t count) {
    for (size_t i = 0; i < count; ++i)
        out.push_back(static_cast<uint8_t>(number >> (8 * i)));
}

uint64_t take_number(std::span<const uint8_t> data, size_t &offset, size_t count) {
    if (offset > data.size() || count > data.size() - offset)
        throw std::runtime_error("PFS: truncated transaction journal");
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= uint64_t(data[offset++]) << (8 * i);
    return value;
}

std::array<uint8_t, mac_size> authenticate(const Hash &key, std::span<const uint8_t> data) {
    std::array<uint8_t, mac_size> result{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), result.data(), &length)
        || length != result.size())
        throw std::runtime_error("PFS: cannot authenticate transaction journal");
    return result;
}

void clean_journal(const fs::path &directory) {
    for (const auto &entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename();
        if ((name != "journal" && name != "journal.tmp" && name != "output.tmp" && name != "commit" && name != "commit.tmp")
            || !fs::is_regular_file(fs::symlink_status(entry.path())))
            throw std::runtime_error("PFS: unexpected transaction journal contents");
    }
    for (const auto *name : { "journal.tmp", "output.tmp", "journal", "commit.tmp", "commit" }) {
        fs::remove(directory / name);
        sync_directory(directory);
    }
    if (!fs::remove(directory))
        throw std::runtime_error("PFS: cannot remove transaction journal directory");
    sync_directory(directory.parent_path());
}

void restore_file(const fs::path &journal, const fs::path &target, const std::optional<std::vector<uint8_t>> &data) {
    if (!data) {
        fs::remove(target);
        sync_directory(target.parent_path());
        return;
    }
    const auto temporary = journal / "output.tmp";
    write_file(temporary, *data);
    replace_file(temporary, target);
}

std::vector<uint8_t> make_backup(const fs::path &root, const Hash &key, std::span<const SavedataChange> changes,
    const std::optional<fs::path> &directory, bool removal = false) {
    if (changes.empty() || changes.size() > 3)
        throw std::runtime_error("PFS: invalid transaction size");
    const auto pfs = root / "sce_pfs";
    if (!fs::is_directory(fs::symlink_status(root)) || !fs::is_directory(fs::symlink_status(pfs)))
        throw std::runtime_error("PFS: invalid transaction directory");
    const auto &header = removal ? removal_magic : directory ? directory_magic : magic;
    std::vector<uint8_t> backup(header.begin(), header.end());
    if (directory) {
        validate_savedata_directory_path(*directory);
        const auto name = safe_path(*directory);
        const auto target = checked_target(root, *directory, true);
        if (removal) {
            if (!fs::is_directory(fs::symlink_status(target)) || !fs::is_empty(target))
                throw std::runtime_error("PFS: savedata directory is missing or not empty");
        } else if (fs::exists(fs::symlink_status(target))) {
            throw std::runtime_error("PFS: savedata directory already exists");
        }
        append_number(backup, name.size(), 4);
        backup.insert(backup.end(), name.begin(), name.end());
    }
    append_number(backup, changes.size(), 4);
    std::set<std::string> paths;
    for (const auto &change : changes) {
        auto name = safe_path(change.path);
        auto folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; });
        if (!paths.insert(folded).second || (change.data && change.data->size() > max_file_size))
            throw std::runtime_error("PFS: invalid transaction change");
        const auto target = checked_target(root, change.path);
        const bool exists = fs::exists(fs::symlink_status(target));
        const auto original = exists ? read_file(target, max_file_size) : std::vector<uint8_t>{};
        append_number(backup, name.size(), 4);
        backup.insert(backup.end(), name.begin(), name.end());
        append_number(backup, exists, 1);
        append_number(backup, original.size(), 8);
        backup.insert(backup.end(), original.begin(), original.end());
    }
    const auto mac = authenticate(key, backup);
    backup.insert(backup.end(), mac.begin(), mac.end());
    return backup;
}
} // namespace

void validate_savedata_directory_path(const fs::path &directory) {
    auto folded = safe_path(directory);
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; });
    if (folded == "SCE_PFS" || folded.starts_with("SCE_PFS/"))
        throw std::runtime_error("PFS: cannot create a directory inside PFS metadata");
    for (const auto &part : directory) {
        if (part.generic_u8string().size() >= 64)
            throw std::runtime_error("PFS: savedata directory name too long");
    }
}

void publish_savedata_transaction(const fs::path &root, const Hash &key,
    std::span<const SavedataChange> changes) {
    const auto backup = make_backup(root, key, changes, std::nullopt);
    const auto pfs = root / "sce_pfs";
    const auto journal = pfs / ".vita3k-transaction";
    if (!fs::create_directory(journal))
        throw std::runtime_error("PFS: existing transaction requires recovery");
    sync_directory(pfs);
    write_file(journal / "journal.tmp", backup);
    replace_file(journal / "journal.tmp", journal / "journal");
    for (const auto &change : changes)
        restore_file(journal, checked_target(root, change.path), change.data);
    clean_journal(journal);
}

void publish_savedata_directory_transaction(const fs::path &root, const Hash &key,
    const fs::path &directory, std::span<const SavedataChange> changes) {
    const auto backup = make_backup(root, key, changes, directory);
    const auto pfs = root / "sce_pfs";
    const auto journal = pfs / ".vita3k-transaction";
    if (!fs::create_directory(journal))
        throw std::runtime_error("PFS: existing transaction requires recovery");
    sync_directory(pfs);
    write_file(journal / "journal.tmp", backup);
    replace_file(journal / "journal.tmp", journal / "journal");
    const auto token = transaction_token(key, backup, 'D');
    const auto stage = journal / "directory.tmp";
    if (!fs::create_directory(stage))
        throw std::runtime_error("PFS: cannot stage directory");
    sync_directory(journal);
    write_file(stage / directory_marker, token);
    sync_directory(stage);
    publish_directory(stage, checked_target(root, directory, true));
    for (const auto &change : changes)
        restore_file(journal, checked_target(root, change.path), change.data);
    const auto committed = transaction_token(key, backup, 'C');
    write_file(journal / "commit.tmp", committed);
    replace_file(journal / "commit.tmp", journal / "commit");
    finish_directory(checked_target(root, directory, true), stage, token, true);
    clean_journal(journal);
}

void publish_savedata_directory_removal(const fs::path &root, const Hash &key,
    const fs::path &directory, std::span<const SavedataChange> changes) {
    const auto backup = make_backup(root, key, changes, directory, true);
    const auto pfs = root / "sce_pfs";
    const auto journal = pfs / ".vita3k-transaction";
    if (!fs::create_directory(journal))
        throw std::runtime_error("PFS: existing transaction requires recovery");
    sync_directory(pfs);
    write_file(journal / "journal.tmp", backup);
    replace_file(journal / "journal.tmp", journal / "journal");
    const auto target = checked_target(root, directory, true);
    if (!fs::is_directory(fs::symlink_status(target)) || !fs::is_empty(target))
        throw std::runtime_error("PFS: savedata directory is missing or not empty");
    const auto stage = journal / "directory.tmp";
    publish_directory(target, stage);
    for (const auto &change : changes)
        restore_file(journal, checked_target(root, change.path), change.data);
    const auto committed = transaction_token(key, backup, 'C');
    write_file(journal / "commit.tmp", committed);
    replace_file(journal / "commit.tmp", journal / "commit");
    remove_staged_directory(stage);
    clean_journal(journal);
}

void recover_savedata_transaction(const fs::path &root, const Hash &key) {
    const auto pfs = root / "sce_pfs";
    if (!fs::is_directory(fs::symlink_status(root)) || !fs::is_directory(fs::symlink_status(pfs)))
        throw std::runtime_error("PFS: invalid transaction directory");
    const auto journal = pfs / ".vita3k-transaction";
    const auto status = fs::symlink_status(journal);
    if (!fs::exists(status))
        return;
    if (!fs::is_directory(status))
        throw std::runtime_error("PFS: invalid transaction journal");
    const auto file = journal / "journal";
    if (!fs::exists(fs::symlink_status(file))) {
        clean_journal(journal);
        return;
    }
    const auto bytes = read_file(file, max_journal_size);
    if (bytes.size() < magic.size() + 4 + mac_size)
        throw std::runtime_error("PFS: invalid transaction journal header");
    const bool removal = std::equal(removal_magic.begin(), removal_magic.end(), bytes.begin());
    const bool is_directory = removal || std::equal(directory_magic.begin(), directory_magic.end(), bytes.begin());
    if (!is_directory && !std::equal(magic.begin(), magic.end(), bytes.begin()))
        throw std::runtime_error("PFS: invalid transaction journal header");
    const std::span<const uint8_t> content(bytes.data(), bytes.size() - mac_size);
    const auto mac = authenticate(key, content);
    if (CRYPTO_memcmp(mac.data(), bytes.data() + content.size(), mac.size()) != 0)
        throw std::runtime_error("PFS: transaction journal authentication failed");
    size_t offset = magic.size();
    std::optional<fs::path> directory;
    if (is_directory) {
        const auto length = take_number(content, offset, 4);
        if (length == 0 || length > 255 || length > content.size() - offset)
            throw std::runtime_error("PFS: invalid directory journal path");
        const std::string name(reinterpret_cast<const char *>(content.data() + offset), static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        directory = fs::u8path(name);
        checked_target(root, *directory, true, true);
    }
    const auto count = take_number(content, offset, 4);
    if (count == 0 || count > 3)
        throw std::runtime_error("PFS: invalid transaction journal count");
    std::vector<SavedataChange> originals;
    std::set<std::string> paths;
    for (uint64_t i = 0; i < count; ++i) {
        const auto length = take_number(content, offset, 4);
        if (length == 0 || length > 255 || length > content.size() - offset)
            throw std::runtime_error("PFS: invalid transaction journal path");
        std::string name(reinterpret_cast<const char *>(content.data() + offset), static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        auto folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; });
        if (!paths.insert(folded).second)
            throw std::runtime_error("PFS: duplicate transaction journal path");
        const fs::path relative = fs::u8path(name);
        checked_target(root, relative);
        const auto exists = take_number(content, offset, 1);
        const auto size = take_number(content, offset, 8);
        if (exists > 1 || size > max_file_size || size > content.size() - offset || (!exists && size != 0))
            throw std::runtime_error("PFS: invalid transaction journal entry");
        std::optional<std::vector<uint8_t>> original;
        if (exists)
            original.emplace(content.begin() + offset, content.begin() + offset + static_cast<size_t>(size));
        offset += static_cast<size_t>(size);
        originals.push_back({ relative, std::move(original) });
    }
    if (offset != content.size())
        throw std::runtime_error("PFS: invalid transaction journal length");
    bool committed = false;
    if (directory && fs::exists(fs::symlink_status(journal / "commit"))) {
        const auto expected = transaction_token(key, bytes, 'C');
        const auto actual = read_file(journal / "commit", mac_size);
        if (actual.size() != expected.size() || CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) != 0)
            throw std::runtime_error("PFS: invalid directory commit marker");
        committed = true;
    }
    if (!committed) {
        for (const auto &original : originals)
            restore_file(journal, checked_target(root, original.path), original.data);
    }
    if (directory) {
        const auto stage = journal / "directory.tmp";
        const auto staged = fs::symlink_status(stage);
        if (removal) {
            if (fs::exists(staged)) {
                if (!fs::is_directory(staged) || !fs::is_empty(stage))
                    throw std::runtime_error("PFS: invalid staged directory removal");
                if (committed)
                    remove_staged_directory(stage);
                else
                    publish_directory(stage, checked_target(root, *directory, true, true));
            } else if (!committed && !fs::is_directory(fs::symlink_status(checked_target(root, *directory, true)))) {
                throw std::runtime_error("PFS: directory to restore is missing");
            }
        } else if (fs::exists(staged)) {
            if (committed)
                throw std::runtime_error("PFS: committed directory remains staged");
            remove_staged_directory(stage);
        } else {
            const auto token = transaction_token(key, bytes, 'D');
            finish_directory(checked_target(root, *directory, true), stage, token, committed);
        }
    }
    clean_journal(journal);
}

} // namespace pfsmgr::detail

namespace pfsmgr {

namespace {

std::string random_stage_name(const char *prefix) {
    std::array<uint8_t, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("PFS: cannot generate temporary name");
    std::ostringstream name;
    name << prefix << std::hex << std::setfill('0');
    for (const auto byte : random)
        name << std::setw(2) << static_cast<unsigned>(byte);
    return name.str();
}

OpenPsId read_open_psid(const std::filesystem::path &directory) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::symlink_status(directory)))
        throw std::runtime_error("PFS: invalid emulated identity directory");
    const auto bytes = detail::read_file(directory / "openpsid", OpenPsId{}.size());
    if (bytes.size() != OpenPsId{}.size())
        throw std::runtime_error("PFS: invalid emulated OpenPSID size");
    OpenPsId id{};
    std::copy(bytes.begin(), bytes.end(), id.begin());
    return id;
}

std::vector<uint8_t> build_savedata_sfo(const SavedataMetadata &metadata) {
    if (metadata.directory.empty() || metadata.directory.size() > 62
        || metadata.directory == "." || metadata.directory == ".."
        || metadata.directory.find_first_of("\\/:") != std::string::npos
        || metadata.directory.find('\0') != std::string::npos
        || metadata.title_id.empty() || metadata.title_id.size() > 11
        || metadata.title_id.find('\0') != std::string::npos)
        throw std::runtime_error("PFS: invalid savedata SFO metadata");

    const auto put16 = [](std::vector<uint8_t> &out, size_t offset, uint16_t value) {
        for (size_t i = 0; i < 2; ++i)
            out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    const auto put32 = [](std::vector<uint8_t> &out, size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i)
            out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    std::array<uint8_t, 1024> params{};
    params[0] = 1;
    std::copy(metadata.identity.open_psid.begin(), metadata.identity.open_psid.end(), params.begin() + 4);
    params[24] = 1;
    std::copy(metadata.title_id.begin(), metadata.title_id.end(), params.begin() + 40);
    params[72] = 1;
    const std::string parent = "/" + metadata.directory;
    const std::array<uint8_t, 8> zero{};
    const std::array<uint8_t, 4> category{ 's', 'd', 0, 0 };
    const std::array<uint8_t, 8> version{ '0', '.', '0', '0', 0, 0, 0, 0 };
    const auto parent_bytes = std::span(reinterpret_cast<const uint8_t *>(parent.c_str()), parent.size() + 1);
    struct Field {
        const char *name;
        uint16_t format;
        uint32_t size;
        std::span<const uint8_t> value;
    };
    const std::array fields{
        Field{ "ACCOUNT_ID", 4, 8, metadata.identity.account_id },
        Field{ "ATTRIBUTE", 0x0404, 4, std::span(zero).first(4) },
        Field{ "CATEGORY", 0x0204, 4, std::span(category).first(3) },
        Field{ "DETAIL", 0x0204, 1024, std::span(zero).first(1) },
        Field{ "PARAMS", 4, 1024, params },
        Field{ "PARENT_DIRECTORY", 0x0204, 64, parent_bytes },
        Field{ "SAVEDATA_VER", 0x0204, 8, std::span(version).first(6) },
        Field{ "TITLE", 0x0204, 128, std::span(zero).first(1) }
    };
    constexpr size_t header_size = 20;
    constexpr size_t entry_size = 16;
    const size_t key_start = header_size + fields.size() * entry_size;
    size_t key_bytes = 0;
    size_t data_bytes = 0;
    for (const auto &field : fields) {
        key_bytes += std::char_traits<char>::length(field.name) + 1;
        data_bytes += field.size;
    }
    const size_t data_start = (key_start + key_bytes + 3) & ~size_t{ 3 };
    std::vector<uint8_t> result(data_start + data_bytes);
    put32(result, 0, 0x46535000);
    put32(result, 4, 0x101);
    put32(result, 8, static_cast<uint32_t>(key_start));
    put32(result, 12, static_cast<uint32_t>(data_start));
    put32(result, 16, static_cast<uint32_t>(fields.size()));
    size_t key_offset = 0;
    size_t data_offset = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto &field = fields[i];
        const auto name_size = std::char_traits<char>::length(field.name) + 1;
        const auto entry = header_size + i * entry_size;
        put16(result, entry, static_cast<uint16_t>(key_offset));
        put16(result, entry + 2, field.format);
        put32(result, entry + 4, static_cast<uint32_t>(field.value.size()));
        put32(result, entry + 8, field.size);
        put32(result, entry + 12, static_cast<uint32_t>(data_offset));
        std::copy_n(field.name, name_size, result.begin() + key_start + key_offset);
        std::copy(field.value.begin(), field.value.end(), result.begin() + data_start + data_offset);
        key_offset += name_size;
        data_offset += field.size;
    }
    return result;
}

} // namespace

namespace {
std::mutex identity_mutex;

std::filesystem::path identity_directory(const std::filesystem::path &ux0) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::symlink_status(ux0)))
        throw std::runtime_error("PFS: invalid emulated identity location");
    auto directory = ux0;
    for (const auto *component : { "data", "vita3k" }) {
        directory /= component;
        if (!fs::exists(fs::symlink_status(directory)) && fs::create_directory(directory))
            detail::sync_directory(directory.parent_path());
        if (!fs::is_directory(fs::symlink_status(directory)))
            throw std::runtime_error("PFS: invalid emulated identity directory");
    }
    return directory;
}

SavedataIdentity read_identity_file(const std::filesystem::path &path) {
    const auto bytes = detail::read_file(path, 24);
    if (bytes.size() != 24)
        throw std::runtime_error("PFS: invalid savedata identity size");
    SavedataIdentity identity{};
    std::copy_n(bytes.begin(), identity.open_psid.size(), identity.open_psid.begin());
    std::copy_n(bytes.begin() + identity.open_psid.size(), identity.account_id.size(), identity.account_id.begin());
    if (std::ranges::all_of(identity.open_psid, [](uint8_t byte) { return byte == 0; }))
        throw std::runtime_error("PFS: invalid savedata OpenPSID");
    return identity;
}

std::array<uint8_t, 24> encode_identity(const SavedataIdentity &identity) {
    std::array<uint8_t, 24> bytes{};
    std::copy(identity.open_psid.begin(), identity.open_psid.end(), bytes.begin());
    std::copy(identity.account_id.begin(), identity.account_id.end(), bytes.begin() + identity.open_psid.size());
    return bytes;
}

void publish_identity_file(const std::filesystem::path &directory, const std::filesystem::path &target, std::span<const uint8_t> bytes, bool replace = false) {
    namespace fs = std::filesystem;
    const auto temporary = directory / random_stage_name("identity-new-");
    if (fs::exists(fs::symlink_status(temporary)))
        throw std::runtime_error("PFS: identity staging file already exists");
    try {
        detail::write_file(temporary, bytes);
        if (replace)
            detail::replace_file(temporary, target);
        else
            detail::publish_directory(temporary, target);
    } catch (const std::exception &) {
        std::error_code error;
        fs::remove(temporary, error);
        if (error)
            throw std::system_error(error, "PFS: cannot remove staged identity");
        throw;
    }
}

std::optional<SavedataIdentity> pending_identity(const std::filesystem::path &ux0, const std::filesystem::path &directory) {
    namespace fs = std::filesystem;
    const auto pending = directory / "identity-pending";
    if (fs::exists(fs::symlink_status(pending)))
        return read_identity_file(pending);
    const auto legacy = ux0 / ".vita3k-console-identity" / "savedata-identity";
    if (!fs::exists(fs::symlink_status(directory / "openpsid.bin")) && fs::exists(fs::symlink_status(legacy))) {
        if (!fs::is_directory(fs::symlink_status(legacy.parent_path())))
            throw std::runtime_error("PFS: invalid legacy identity directory");
        const auto identity = read_identity_file(legacy);
        publish_identity_file(directory, pending, encode_identity(identity));
        return identity;
    }
    return std::nullopt;
}
} // namespace

std::optional<SavedataIdentity> pending_savedata_identity(const std::filesystem::path &ux0) {
    const std::lock_guard lock(identity_mutex);
    return pending_identity(ux0, identity_directory(ux0));
}

OpenPsId get_or_create_open_psid(const std::filesystem::path &ux0) {
    namespace fs = std::filesystem;
    const std::lock_guard lock(identity_mutex);
    const auto directory = identity_directory(ux0);
    if (pending_identity(ux0, directory))
        throw std::runtime_error("PFS: savedata identity transaction requires registry recovery");
    const auto target = directory / "openpsid.bin";
    if (fs::exists(fs::symlink_status(target))) {
        const auto bytes = detail::read_file(target, OpenPsId{}.size());
        if (bytes.size() != OpenPsId{}.size())
            throw std::runtime_error("PFS: invalid emulated OpenPSID size");
        OpenPsId id{};
        std::copy(bytes.begin(), bytes.end(), id.begin());
        return id;
    }
    const auto legacy = ux0 / ".vita3k-console-identity";
    OpenPsId id{};
    if (fs::exists(fs::symlink_status(legacy)))
        id = read_open_psid(legacy);
    else if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1)
        throw std::runtime_error("PFS: cannot generate emulated OpenPSID");
    publish_identity_file(directory, target, id);
    return id;
}

void stage_savedata_identity(const std::filesystem::path &ux0, const SavedataIdentity &identity, const SavedataIdentity &previous) {
    namespace fs = std::filesystem;
    if (std::ranges::all_of(identity.open_psid, [](uint8_t byte) { return byte == 0; }))
        throw std::runtime_error("PFS: invalid savedata OpenPSID");
    const std::lock_guard lock(identity_mutex);
    const auto directory = identity_directory(ux0);
    if (pending_identity(ux0, directory))
        throw std::runtime_error("PFS: savedata identity transaction already pending");
    const auto backup = directory / random_stage_name("savedata-identity-backup-");
    if (fs::exists(fs::symlink_status(backup)))
        throw std::runtime_error("PFS: savedata identity backup already exists");
    detail::write_file(backup, encode_identity(previous));
    detail::sync_directory(directory);
    publish_identity_file(directory, directory / "identity-pending", encode_identity(identity));
}

void commit_savedata_identity(const std::filesystem::path &ux0) {
    namespace fs = std::filesystem;
    const std::lock_guard lock(identity_mutex);
    const auto directory = identity_directory(ux0);
    const auto pending = pending_identity(ux0, directory);
    if (!pending)
        throw std::runtime_error("PFS: no savedata identity transaction pending");
    publish_identity_file(directory, directory / "openpsid.bin", pending->open_psid, true);
    if (!fs::remove(directory / "identity-pending"))
        throw std::runtime_error("PFS: cannot remove completed identity transaction");
    detail::sync_directory(directory);
}

void create_savedata(const std::filesystem::path &root, const SavedataMetadata &metadata) {
    namespace fs = std::filesystem;
    const auto parent = root.parent_path();
    if (root.empty() || root.filename().empty() || !fs::is_directory(fs::symlink_status(parent)))
        throw std::runtime_error("PFS: invalid new savedata location");
    if (fs::exists(fs::symlink_status(root)))
        throw std::runtime_error("PFS: savedata already exists");

    const auto filename = root.filename().u8string();
    if (metadata.directory != std::string(filename.begin(), filename.end()))
        throw std::runtime_error("PFS: savedata SFO directory does not match target");
    const auto sfo = build_savedata_sfo(metadata);
    const auto stage = parent / random_stage_name(".vita3k-new-");
    if (!fs::create_directory(stage))
        throw std::runtime_error("PFS: temporary savedata directory already exists");
    try {
        const auto initial = prepare_initial_savedata();
        const auto key = unseal_savedata_key(std::span(initial.sealedkey.data).first(0x50));
        const auto sfo_file = encode_savedata_file(sfo, 3, 0x4006, key);
        const auto database = insert_savedata_file_table(initial.files_db,
            FileEntry{ 1, "PARAM.SFO", 3, 0x4006, sfo.size(), sfo_file.entry_hash }, derive_savedata_secret(key, 0));
        const auto pfs = stage / "sce_pfs";
        const auto icv = pfs / "icv.db";
        const auto sys = stage / "sce_sys";
        if (!fs::create_directory(pfs) || !fs::create_directory(icv) || !fs::create_directory(sys))
            throw std::runtime_error("PFS: cannot create initial savedata directories");
        detail::write_file(pfs / "files.db", database);
        detail::write_file(icv / "00000001.icv", initial.directory_icv);
        detail::write_file(icv / "00000002.icv", initial.sealedkey.icv);
        detail::write_file(icv / "00000003.icv", sfo_file.icv);
        detail::write_file(sys / "sealedkey", initial.sealedkey.data);
        detail::write_file(sys / "param.sfo", sfo_file.data);
        detail::sync_directory(icv);
        detail::sync_directory(pfs);
        detail::sync_directory(sys);
        detail::sync_directory(stage);
        const Layer verified(stage);
        const auto sealedkey = verified.read(fs::path("sce_sys") / "sealedkey");
        const auto verified_sfo = verified.read(fs::path("sce_sys") / "param.sfo");
        if (!sealedkey || sealedkey->size() != 0x50 || !verified_sfo || *verified_sfo != sfo)
            throw std::runtime_error("PFS: cannot verify initial savedata");
        detail::publish_directory(stage, root);
    } catch (const std::exception &error) {
        std::error_code cleanup_error;
        for (const auto &path : { stage / "sce_sys" / "param.sfo", stage / "sce_sys" / "sealedkey",
                 stage / "sce_pfs" / "icv.db" / "00000003.icv", stage / "sce_pfs" / "icv.db" / "00000002.icv",
                 stage / "sce_pfs" / "icv.db" / "00000001.icv", stage / "sce_pfs" / "files.db",
                 stage / "sce_sys", stage / "sce_pfs" / "icv.db", stage / "sce_pfs", stage }) {
            std::error_code ec;
            fs::remove(path, ec);
            if (ec && !cleanup_error)
                cleanup_error = ec;
        }
        if (cleanup_error)
            throw std::runtime_error(std::string(error.what()) + "; temporary savedata cleanup failed: " + cleanup_error.message());
        throw;
    }
}

} // namespace pfsmgr
