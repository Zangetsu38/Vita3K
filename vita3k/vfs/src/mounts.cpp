// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <io/functions.h>
#include <io/io.h>
#include <io/state.h>
#include <io/vfs.h>
#include <kernel/types.h>
#include <pfsmgr/mounts.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>
#include <vfs/mounts.h>

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace pfsmgr {

using fs_utils::is_plain_component;
constexpr int ADDCONT_ALREADY_OPEN = static_cast<int>(0x80010078u);

bool mount_theme_data(IOState &io, const fs::path &directory, std::string &mount_point, bool &is_protected) {
    mount_point.clear();
    is_protected = false;
    const auto content_id = fs_utils::path_to_utf8(directory.filename());
    std::error_code error;
    if (!is_plain_component(directory.parent_path().parent_path()) || !is_plain_component(directory.parent_path())
        || !is_plain_component(directory) || !fs::is_directory(directory, error) || error) {
        LOG_ERROR("Invalid theme directory '{}'", directory);
        return false;
    }
    try {
        is_protected = has_databases(directory);
        if (is_protected && (content_id.size() < 17 || content_id[6] != '-' || content_id[16] != '_')) {
            LOG_ERROR("Invalid protected theme content ID '{}'", content_id);
            return false;
        }
        const auto title_id = is_protected ? content_id.substr(7, 9) : std::string{};
        const auto alias = allocate_mount_name("tm");
        const auto pfs_name = allocate_mount_name("PD");
        if (alias.empty() || pfs_name.empty())
            return false;
        std::lock_guard lock(io.appmeta_mutex);
        io.pfs_mounts.mount_directory(pfs_name, directory, {}, directory.parent_path().parent_path() / "license" / title_id,
            title_id, Mode::ThemeData, is_protected ? content_id : std::string{}, MountScope::Shell);
        try {
            if (!io.virtual_pfs_mounts.emplace(MountKey{ MountType::ThemeData, alias }, pfs_name).second)
                throw std::runtime_error("duplicate theme mount alias");
        } catch (...) {
            io.pfs_mounts.unmount_directory(pfs_name);
            throw;
        }
        mount_point = alias;
        return true;
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Cannot mount theme '{}': {}", directory, error.what());
        return false;
    }
}

bool unmount_theme_data(IOState &io, const std::string &mount_point) {
    std::lock_guard lock(io.appmeta_mutex);
    const auto alias = io.virtual_pfs_mounts.find({ MountType::ThemeData, mount_point });
    if (alias == io.virtual_pfs_mounts.end())
        return false;
    if (!io.pfs_mounts.unmount_directory(alias->second)) {
        LOG_ERROR("Missing theme pseudo-drive for '{}'", mount_point);
        return false;
    }
    io.virtual_pfs_mounts.erase(alias);
    return true;
}

void clear_shell_mounts(IOState &io) {
    {
        std::lock_guard lock(io.fd_mutex);
        std::erase_if(io.std_files, [](const auto &entry) { return entry.second->is_shell_file(); });
        std::erase_if(io.dir_entries, [](const auto &entry) { return entry.second->is_shell_directory(); });
    }
    std::lock_guard lock(io.appmeta_mutex);
    std::erase_if(io.virtual_pfs_mounts, [](const auto &entry) { return entry.first.type == MountType::ThemeData; });
    io.pfs_mounts.clear(MountScope::Shell);
}

void clear_application_mounts(IOState &io) {
    std::lock_guard lock(io.appmeta_mutex);
    io.pfs_mounts.clear(MountScope::Application);
}

int mount_addcont(IOState &io, const fs::path &vita_fs_path, const std::string &mount_point, const std::string &entitlement) {
    if (mount_point != "addcont0:") {
        LOG_WARN("Unsupported additional content mount point '{}'", mount_point);
        return SCE_ERROR_ERRNO_EOPNOTSUPP;
    }
    const auto valid_id = [](const std::string &id, size_t length) {
        return id.size() == length && std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
    };
    if (!valid_id(io.addcont, 9) || !valid_id(entitlement, 16))
        return SCE_ERROR_ERRNO_EINVAL;
    std::lock_guard lock(io.appmeta_mutex);
    if (io.virtual_pfs_mounts.contains({ MountType::AddCont, entitlement }))
        return ADDCONT_ALREADY_OPEN;
    const auto ux0 = vita_fs_path / "ux0";
    const auto parent = ux0 / "addcont" / io.addcont;
    const auto source = parent / entitlement;
    std::error_code ec;
    if (!is_plain_component(ux0) || !is_plain_component(ux0 / "addcont") || !is_plain_component(parent)
        || !is_plain_component(source) || !fs::is_directory(source, ec) || ec)
        return SCE_ERROR_ERRNO_ENOENT;
    const auto canonical_source = fs::canonical(source, ec);
    if (ec)
        return SCE_ERROR_ERRNO_EIO;
    if (io.addcont_pfs_mount.empty())
        io.addcont_pfs_mount = allocate_mount_name("PD");
    if (io.addcont_pfs_mount.empty())
        return SCE_ERROR_ERRNO_ENOSPC;
    const auto name = io.addcont_pfs_mount + "/" + entitlement;
    try {
        io.pfs_mounts.mount_directory(name, source, {}, ux0 / "license" / io.addcont, io.addcont,
            Mode::AcidDirectory, {}, MountScope::Application, true);
        try {
            if (!io.virtual_pfs_mounts.emplace(MountKey{ MountType::AddCont, entitlement }, name).second)
                throw std::runtime_error("duplicate additional content mount alias");
        } catch (...) {
            io.pfs_mounts.unmount_directory(name);
            throw;
        }
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Cannot mount additional content '{}': {}", entitlement, error.what());
        return SCE_ERROR_ERRNO_EIO;
    }
    return 0;
}

int unmount_addcont(IOState &io, const std::string &mount_point, const std::string &entitlement) {
    if (mount_point != "addcont0:")
        return SCE_ERROR_ERRNO_EOPNOTSUPP;
    std::lock_guard lock(io.appmeta_mutex);
    const auto entry = io.virtual_pfs_mounts.find({ MountType::AddCont, entitlement });
    if (entry == io.virtual_pfs_mounts.end())
        return SCE_ERROR_ERRNO_ENOENT;
    if (!io.pfs_mounts.unmount_directory(entry->second, true)) {
        if (io.pfs_mounts.get_directory(entry->second))
            return SCE_ERROR_ERRNO_EBUSY;
        LOG_ERROR("Missing additional content mount for '{}'", entitlement);
        return SCE_ERROR_ERRNO_EIO;
    }
    io.virtual_pfs_mounts.erase(entry);
    return 0;
}

bool mount_readonly_appmeta(IOState &io, const fs::path &vita_fs_path, const std::string &title_id, std::string &mount_point) {
    mount_point.clear();
    if (title_id.empty() || title_id.size() > 32 || !std::all_of(title_id.begin(), title_id.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        })
        || string_utils::tolower(title_id) == "new")
        return false;
    const auto ux0 = vita_fs_path / "ux0";
    const auto appmeta = ux0 / "appmeta";
    const auto source = appmeta / title_id;
    std::error_code ec;
    if (!is_plain_component(ux0) || !is_plain_component(appmeta) || !is_plain_component(source) || !fs::is_directory(source, ec) || ec)
        return false;
    const auto canonical_source = fs::canonical(source, ec);
    if (ec)
        return false;
    const auto name = allocate_mount_name("ad");
    const auto pfs_name = allocate_mount_name("PD");
    if (name.empty() || pfs_name.empty())
        return false;
    std::lock_guard lock(io.appmeta_mutex);
    try {
        io.pfs_mounts.mount_directory(pfs_name, source, {}, {}, title_id, Mode::Mode20);
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Cannot mount app metadata for '{}': {}", title_id, error.what());
        return false;
    }
    try {
        if (!io.virtual_pfs_mounts.emplace(MountKey{ MountType::AppMeta, name }, pfs_name).second)
            throw std::runtime_error("duplicate app metadata mount alias");
    } catch (...) {
        io.pfs_mounts.unmount_directory(pfs_name);
        throw;
    }
    mount_point = name;
    return true;
}

bool unmount_appmeta(IOState &io, const std::string &mount_point) {
    if (!is_mount_name(mount_point, "ad"))
        return false;
    std::lock_guard lock(io.appmeta_mutex);
    const auto alias = io.virtual_pfs_mounts.find({ MountType::AppMeta, mount_point });
    if (alias == io.virtual_pfs_mounts.end())
        return false;
    if (!io.pfs_mounts.unmount_directory(alias->second)) {
        LOG_ERROR("Missing app metadata mount for '{}'", mount_point);
        return false;
    }
    io.virtual_pfs_mounts.erase(alias);
    return true;
}

bool mount_readonly_game_data(IOState &io, const fs::path &vita_fs_path, const std::string &title_id, bool require_patch, std::string &mount_point) {
    mount_point.clear();
    if (title_id.empty() || title_id.size() > 32 || !std::all_of(title_id.begin(), title_id.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }))
        return false;
    const auto ux0 = vita_fs_path / "ux0";
    const auto app = ux0 / "app";
    const auto source = app / title_id;
    const auto patch_parent = ux0 / "patch";
    const auto patch = patch_parent / title_id;
    std::error_code ec;
    if (!is_plain_component(ux0) || !is_plain_component(app) || !is_plain_component(source) || !fs::is_directory(source, ec) || ec)
        return false;
    const auto canonical_source = fs::canonical(source, ec);
    if (ec)
        return false;
    const bool patch_exists = fs::exists(patch, ec);
    if (ec || (require_patch && !patch_exists))
        return false;
    fs::path canonical_patch;
    if (patch_exists) {
        if (!is_plain_component(patch_parent) || !is_plain_component(patch) || !fs::is_directory(patch, ec) || ec)
            return false;
        canonical_patch = fs::canonical(patch, ec);
        if (ec)
            return false;
    }
    const auto name = allocate_mount_name("gp");
    const auto pfs_name = allocate_mount_name("PD");
    if (name.empty() || pfs_name.empty())
        return false;
    std::lock_guard lock(io.appmeta_mutex);
    try {
        const auto license_directory = ux0 / "license" / title_id;
        io.pfs_mounts.mount_directory(pfs_name, source, patch_exists ? patch : fs::path{}, license_directory, title_id, Mode::GameData);
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Cannot mount PFS game data for '{}': {}", title_id, error.what());
        return false;
    }
    try {
        if (!io.virtual_pfs_mounts.emplace(MountKey{ MountType::GameData, name }, pfs_name).second)
            throw std::runtime_error("duplicate game data mount alias");
    } catch (...) {
        io.pfs_mounts.unmount_directory(pfs_name);
        throw;
    }
    mount_point = name;
    return true;
}

bool unmount_game_data(IOState &io, const std::string &mount_point) {
    if (!is_mount_name(mount_point, "gp"))
        return false;
    std::lock_guard lock(io.appmeta_mutex);
    if (mount_point == io.app_game_base_mount || mount_point == io.app_game_patch_mount)
        return false;
    const auto alias = io.virtual_pfs_mounts.find({ MountType::GameData, mount_point });
    if (alias == io.virtual_pfs_mounts.end())
        return false;
    if (!io.pfs_mounts.unmount_directory(alias->second))
        return false;
    io.virtual_pfs_mounts.erase(alias);
    return true;
}

static void prepare_copy_destination(const fs::path &destination, bool directory) {
    const auto status = fs::symlink_status(destination);
    if (!fs::exists(status))
        return;
    if (!fs::is_regular_file(status) && !fs::is_directory(status))
        throw fs::filesystem_error("Unsupported copy destination", destination, std::make_error_code(std::errc::operation_not_supported));
    if (fs::is_directory(status) != directory)
        fs::remove_all(destination);
}

static void copy_metadata(const fs::path &source, const fs::path &destination) {
    const auto status = fs::symlink_status(source);
    if (!fs::exists(status))
        throw fs::filesystem_error("Required appmeta source is missing", source, std::make_error_code(std::errc::no_such_file_or_directory));
    if (fs::is_symlink(status))
        throw fs::filesystem_error("Symlinks are not supported in appmeta sources", source, std::make_error_code(std::errc::operation_not_supported));
    if (fs::is_regular_file(status)) {
        prepare_copy_destination(destination, false);
        fs::create_directories(destination.parent_path());
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
        return;
    }
    if (!fs::is_directory(status))
        throw fs::filesystem_error("Unsupported appmeta source", source, std::make_error_code(std::errc::operation_not_supported));
    prepare_copy_destination(destination, true);
    fs::create_directories(destination);
    for (const auto &entry : fs::directory_iterator(source))
        copy_metadata(entry.path(), destination / entry.path().filename());
}

enum class CopySelection { AppmetaRoot,
    AppmetaRetail,
    All,
    ThemeRoot };

static void copy_mounted_data(IOState &io, const fs::path &vita_fs_path, const std::string &source,
    const fs::path &destination, CopySelection selection, const fs::path &patch_root, size_t depth = 0) {
    const char *context = selection == CopySelection::ThemeRoot ? "theme" : "appmeta";
    if (depth > 128)
        throw std::runtime_error(std::string(context) + " directory nesting is too deep");
    const auto fd = open_dir(io, source.c_str(), vita_fs_path, context);
    if (fd < 0)
        throw std::runtime_error(std::string("Cannot open mounted ") + context + " directory: " + source);
    struct DirectoryGuard {
        IOState &io;
        SceUID fd;
        const char *context;
        ~DirectoryGuard() {
            if (fd >= 0 && close_dir(io, fd, context) < 0)
                LOG_WARN("Cannot close {} directory descriptor {}", context, fd);
        }
    } directory{ io, fd, context };
    bool prepared = false;
    const auto prepare_directory = [&] {
        if (!prepared) {
            prepare_copy_destination(destination, true);
            fs::create_directories(destination);
            prepared = true;
        }
    };
    if (selection != CopySelection::AppmetaRetail)
        prepare_directory();
    bool found_required_file = false;
    SceIoDirent entry{};
    int result = 0;
    while ((result = read_dir(io, fd, &entry, vita_fs_path, context)) > 0) {
        const std::string name(entry.d_name);
        if (selection == CopySelection::AppmetaRoot && name != "param.sfo" && name != "param2.sfo"
            && name != "icon0.png" && name != "pic0.png" && name != "livearea" && name != "retail")
            continue;
        if (selection == CopySelection::AppmetaRetail && name != "livearea")
            continue;
        const auto component = fs_utils::utf8_to_path(name);
        if (name.empty() || name == "." || name == ".." || name.find_first_of("\\/:") != std::string::npos
            || component.has_root_path() || component.filename() != component)
            throw std::runtime_error(std::string("Invalid mounted ") + context + " entry: " + name);
        const auto child_source = source + "/" + name;
        const auto child_destination = destination / component;
        if (!patch_root.empty()) {
            // Existing appmeta already contains prior updates; only apply files supplied by this patch.
            const auto resolved = expand_path(io, child_source.c_str(), vita_fs_path);
            if (resolved.empty())
                throw std::runtime_error(std::string("Cannot resolve mounted ") + context + " entry: " + child_source);
            const auto relative = resolved.lexically_relative(patch_root);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                continue;
        }
        prepare_directory();
        const auto file_type = entry.d_stat.st_attr & SCE_SO_IFMT;
        if (selection == CopySelection::ThemeRoot && file_type == SCE_SO_IFDIR)
            continue;
        if (selection == CopySelection::ThemeRoot && name == "theme.xml" && file_type == SCE_SO_IFREG)
            found_required_file = true;
        if (selection == CopySelection::AppmetaRoot && name == "param.sfo") {
            if (file_type != SCE_SO_IFREG)
                throw std::runtime_error("Invalid mounted appmeta param.sfo");
            found_required_file = true;
        }
        if (selection == CopySelection::AppmetaRoot && name == "retail" && file_type != SCE_SO_IFDIR)
            throw std::runtime_error("Invalid retail metadata directory");
        if (file_type == SCE_SO_IFDIR) {
            copy_mounted_data(io, vita_fs_path, child_source, child_destination,
                selection == CopySelection::AppmetaRoot && name == "retail" ? CopySelection::AppmetaRetail : CopySelection::All, patch_root, depth + 1);
        } else if (file_type == SCE_SO_IFREG) {
            std::vector<uint8_t> data;
            if (!read_file_to_buffer(io, child_source.c_str(), vita_fs_path, data, context))
                throw std::runtime_error(std::string("Cannot read mounted ") + context + " file: " + child_source);
            prepare_copy_destination(child_destination, false);
            std::ofstream output(child_destination, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size())))
                throw fs::filesystem_error(std::string("Cannot write ") + context + " file", child_destination, std::make_error_code(std::errc::io_error));
            output.close();
            if (!output)
                throw fs::filesystem_error(std::string("Cannot finish writing ") + context + " file", child_destination, std::make_error_code(std::errc::io_error));
        } else {
            throw std::runtime_error(std::string("Unsupported mounted ") + context + " entry: " + child_source);
        }
    }
    if (result < 0)
        throw std::runtime_error(std::string("Cannot read mounted ") + context + " directory: " + source);
    if (selection == CopySelection::AppmetaRoot && !found_required_file)
        throw std::runtime_error("Mounted game is missing sce_sys/param.sfo");
    if (selection == CopySelection::ThemeRoot && !found_required_file)
        throw std::runtime_error("Mounted theme is missing theme.xml");
    if (close_dir(io, fd, context) < 0)
        throw std::runtime_error(std::string("Cannot close mounted ") + context + " directory: " + source);
    directory.fd = -1;
}

bool copy_theme_data(IOState &io, const fs::path &vita_fs_path, const std::string &content_id, const fs::path &destination) {
    if (content_id.empty() || content_id == "." || content_id == ".." || content_id.find_first_of("\\/:") != std::string::npos) {
        LOG_ERROR("Invalid theme content ID '{}'", content_id);
        return false;
    }
    std::string mount_point;
    bool is_protected = false;
    if (!mount_theme_data(io, vita_fs_path / "ux0" / "theme" / fs_utils::utf8_to_path(content_id), mount_point, is_protected))
        return false;
    struct MountGuard {
        IOState &io;
        const std::string &point;
        bool mounted = true;
        ~MountGuard() {
            if (mounted && !unmount_theme_data(io, point))
                LOG_ERROR("Cannot unmount theme data '{}'", point);
        }
    } mount{ io, mount_point };
    try {
        copy_mounted_data(io, vita_fs_path, mount_point, destination, CopySelection::ThemeRoot, {});
        if (!unmount_theme_data(io, mount_point))
            throw std::runtime_error("Cannot unmount theme data: " + mount_point);
        mount.mounted = false;
        return true;
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Cannot copy theme '{}': {}", content_id, error.what());
        return false;
    }
}

bool copy_appmeta(IOState &io, const fs::path &vita_fs_path, const std::string &title_id,
    const fs::path &destination, const fs::path &existing_appmeta, bool require_patch) {
    std::string mount_point;
    if (!mount_readonly_game_data(io, vita_fs_path, title_id, require_patch, mount_point)) {
        LOG_ERROR("Cannot mount game data for appmeta: {}", title_id);
        return false;
    }
    struct MountGuard {
        IOState &io;
        const std::string &point;
        bool mounted = true;
        ~MountGuard() {
            if (mounted && !unmount_game_data(io, point))
                LOG_WARN("Cannot unmount appmeta game data '{}'", point);
        }
    } mount{ io, mount_point };
    try {
        if (!existing_appmeta.empty())
            copy_metadata(existing_appmeta, destination);
        const auto patch = vita_fs_path / "ux0" / "patch" / title_id;
        copy_mounted_data(io, vita_fs_path, mount_point + "/sce_sys", destination, CopySelection::AppmetaRoot,
            existing_appmeta.empty() ? fs::path{} : fs::absolute(patch));
        if (!unmount_game_data(io, mount_point))
            throw std::runtime_error("Cannot unmount appmeta game data: " + mount_point);
        mount.mounted = false;
        return true;
    } catch (const std::runtime_error &error) {
        LOG_ERROR("Cannot copy appmeta for '{}': {}", title_id, error.what());
        return false;
    }
}

bool read_update_history(IOState &io, const fs::path &vita_fs_path, const std::string &app_path,
    const std::string &title_id, int language, std::vector<uint8_t> &xml) {
    xml.clear();
    if (app_path != "ux0:app/" + title_id && app_path != "ux0:/app/" + title_id) {
        LOG_WARN("Unsupported application path for update history: '{}'", app_path);
        return false;
    }
    std::string mount_point;
    if (!mount_readonly_game_data(io, vita_fs_path, title_id, false, mount_point)) {
        LOG_WARN("Cannot mount game data for update history of '{}'", title_id);
        return false;
    }
    struct MountGuard {
        IOState &io;
        const std::string &point;
        ~MountGuard() {
            if (!unmount_game_data(io, point))
                LOG_WARN("Failed to unmount update history source '{}'", point);
        }
    } mount{ io, mount_point };

    const auto directory = mount_point + "/sce_sys/changeinfo/";
    const auto localized = directory + fmt::format("changeinfo_{:02}.xml", language);
    int open_error = 0;
    bool read = read_file_to_buffer(io, localized.c_str(), vita_fs_path, xml, "update_history", 16 * 1024 * 1024, &open_error, true);
    if (!read && open_error == SCE_ERROR_ERRNO_ENOENT) {
        const auto fallback = directory + "changeinfo.xml";
        read = read_file_to_buffer(io, fallback.c_str(), vita_fs_path, xml, "update_history", 16 * 1024 * 1024, &open_error, true);
    }
    if (!read || xml.empty()) {
        LOG_WARN("Failed to read update history for '{}' (open error {})", title_id, open_error);
        return false;
    }
    return true;
}

bool read_manual_pages(IOState &io, const fs::path &vita_fs_path, const std::string &app_path,
    const std::string &title_id, int language, const std::function<bool(const std::vector<uint8_t> &, unsigned int)> &consume_page) {
    const bool mount_game = app_path == "ux0:app/" + title_id || app_path == "ux0:/app/" + title_id;
    std::string mount_point;
    if (mount_game && !mount_readonly_game_data(io, vita_fs_path, title_id, false, mount_point)) {
        LOG_ERROR("Cannot mount manual for '{}'", title_id);
        return false;
    }
    struct MountGuard {
        IOState &io;
        const std::string &point;
        ~MountGuard() {
            if (!point.empty() && !unmount_game_data(io, point))
                LOG_WARN("Cannot unmount manual source '{}'", point);
        }
    } mount{ io, mount_point };

    const auto read_page = [&](const fs::path &relative, std::vector<uint8_t> &data) {
        // Read manual image from file buffer and check if it exists
        if (mount_game) {
            int open_error = 0;
            const auto path = mount_point + "/" + relative.generic_string();
            if (read_file_to_buffer(io, path.c_str(), vita_fs_path, data, "manual", std::numeric_limits<int>::max(), &open_error, true))
                return 0;
            if (open_error == SCE_ERROR_ERRNO_ENOENT) {
                try {
                    const auto host = expand_path(io, path.c_str(), vita_fs_path);
                    if (host.empty())
                        return SCE_ERROR_ERRNO_ENOENT;
                    std::error_code error;
                    const auto status = fs::symlink_status(host, error);
                    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status)))
                        return SCE_ERROR_ERRNO_ENOENT;
                } catch (const fs::filesystem_error &error) {
                    LOG_ERROR("Cannot inspect manual page '{}': {}", path, error.what());
                }
            }
            LOG_ERROR("Cannot open or read mounted manual page '{}'", path);
            return SCE_ERROR_ERRNO_EIO;
        }
        const auto path = vfs::resolve_app_path(vita_fs_path, app_path, relative, title_id);
        std::error_code error;
        const auto status = fs::status(path, error);
        if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status)))
            return SCE_ERROR_ERRNO_ENOENT;
        if (error || !fs::is_regular_file(status) || !fs_utils::read_data(path, data) || data.size() > std::numeric_limits<int>::max()) {
            LOG_ERROR("Cannot read manual page '{}'", path);
            return SCE_ERROR_ERRNO_EIO;
        }
        return 0;
    };

    fs::path base = "sce_sys/manual";
    auto localized = base / fmt::format("{:02}", language);
    std::vector<uint8_t> data;
    auto result = read_page(localized / "001.png", data);
    if (result == 0)
        base = std::move(localized);
    else if (result == SCE_ERROR_ERRNO_ENOENT)
        result = read_page(base / "001.png", data);
    if (result != 0) {
        LOG_ERROR("Cannot read first manual page for '{}' (error {})", title_id, result);
        return false;
    }
    if (!consume_page(data, 1))
        return false;
    for (unsigned int page = 2; page <= 999; ++page) {
        // Get manual page path
        result = read_page(base / fmt::format("{:03}.png", page), data);
        if (result == SCE_ERROR_ERRNO_ENOENT)
            return true;
        if (result != 0) {
            LOG_ERROR("Cannot read manual page {} for '{}' (error {})", page, title_id, result);
            return false;
        }
        if (!consume_page(data, page))
            return false;
    }
    return true;
}

bool mount_application_game_data(IOState &io, const fs::path &vita_fs_path) {
    if (io.app_path != "ux0:app/" + io.title_id || io.title_id.empty())
        return true;
    if (io.title_id.size() > 32 || !std::all_of(io.title_id.begin(), io.title_id.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        })) {
        LOG_ERROR("Cannot mount application with invalid title ID '{}'.", io.title_id);
        return false;
    }
    if (!io.app_game_base_mount.empty()) {
        LOG_ERROR("Application game data is already mounted for '{}'", io.title_id);
        return false;
    }
    std::string mount;
    if (!mount_readonly_game_data(io, vita_fs_path, io.title_id, false, mount)) {
        LOG_ERROR("Cannot mount application game data for '{}'", io.title_id);
        return false;
    }
    const auto patch_mount = allocate_mount_name("gp");
    const auto patch_pfs_mount = allocate_mount_name("PD");
    if (patch_mount.empty() || patch_pfs_mount.empty()) {
        LOG_ERROR("Cannot allocate patch mount for '{}'", io.title_id);
        if (!unmount_game_data(io, mount))
            LOG_ERROR("Cannot release rejected application game data mount '{}'", mount);
        return false;
    }
    std::unique_lock lock(io.appmeta_mutex);
    const auto base_alias = io.virtual_pfs_mounts.find({ MountType::GameData, mount });
    const auto base = base_alias == io.virtual_pfs_mounts.end() ? std::nullopt : io.pfs_mounts.get_directory(base_alias->second);
    if (!base) {
        LOG_ERROR("Missing application game mount '{}'", mount);
        return false;
    }
    if (!base->patch.empty()) {
        const auto base_name = base_alias->second;
        try {
            const auto [alias, inserted] = io.virtual_pfs_mounts.emplace(MountKey{ MountType::GameData, patch_mount }, patch_pfs_mount);
            if (!inserted)
                throw std::runtime_error("duplicate application patch alias");
            try {
                if (!io.pfs_mounts.split_patch(base_name, patch_pfs_mount))
                    throw std::runtime_error("cannot separate application patch mount");
            } catch (...) {
                io.virtual_pfs_mounts.erase(alias);
                throw;
            }
            io.app_game_patch_mount = patch_mount;
        } catch (const std::runtime_error &error) {
            LOG_ERROR("Cannot mount application patch for '{}': {}", io.title_id, error.what());
            lock.unlock();
            if (!unmount_game_data(io, mount))
                LOG_ERROR("Cannot release rejected application game data mount '{}'", mount);
            return false;
        }
    }
    io.app_game_base_mount = mount;
    LOG_INFO("Mounted application game layers for '{}'", io.title_id);
    return true;
}

} // namespace pfsmgr