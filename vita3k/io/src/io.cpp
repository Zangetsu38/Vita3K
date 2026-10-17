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

#include <io/device.h>
#include <io/functions.h>
#include <io/io.h>
#include <io/state.h>
#include <io/types.h>
#include <io/util.h>
#include <io/vfs.h>

#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <mem/ptr.h>
#include <rtc/rtc.h>
#include <util/log.h>
#include <util/preprocessor.h>
#include <util/string_utils.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <system_error>

#if defined(__aarch64__) && defined(__APPLE__)
#define stat64 stat
#endif

// ****************************
// * Utility functions *
// ****************************

static int io_error_impl(const int retval, const char *export_name, const char *func_name) {
    LOG_WARN("{} ({}) returned {}", func_name, export_name, log_hex(retval));
    return retval;
}

#define IO_ERROR(retval) io_error_impl(retval, export_name, __func__)
#define IO_ERROR_UNK() IO_ERROR(-1)

constexpr bool log_file_op = true;
constexpr bool log_file_read = false;
constexpr bool log_file_seek = false;
constexpr bool log_file_stat = false;

AsyncIoRequest::~AsyncIoRequest() {
    if (worker.joinable())
        worker.join();
    if (cleanup)
        cleanup();
}

static fs::path expand_unresolved_path(IOState &io, const char *path, const fs::path &vita_fs_path);
static fs::path find_host_path(IOState &io, const fs::path &path);

// The namespace is process-wide and is deliberately never reset on deinit.
static std::atomic_uint64_t appmeta_sequence{ 1 };

static bool is_mount_name(const std::string &name, std::string_view prefix) {
    if (name.size() != 15 || !name.starts_with(prefix) || name.back() != ':')
        return false;
    return std::all_of(name.begin() + 2, name.end() - 1, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool is_appmeta_mount_name(const std::string &name) {
    return is_mount_name(name, "am");
}

static bool is_appmeta_path(std::string_view path) {
    const auto colon = path.find(':');
    return colon != std::string_view::npos && colon >= 2 && (path[0] == 'a' || path[0] == 'A' || path[0] == 'g' || path[0] == 'G') && (path[1] == 'm' || path[1] == 'M');
}

static bool is_plain_component(const fs::path &path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status) || fs::is_symlink(status))
        return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
#endif
    return true;
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
    auto id = appmeta_sequence.load(std::memory_order_relaxed);
    do {
        if (id > 0xffffffffffffULL)
            return false;
    } while (!appmeta_sequence.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
    char name[16];
    std::snprintf(name, sizeof(name), "am%012llx:", static_cast<unsigned long long>(id));
    std::lock_guard lock(io.appmeta_mutex);
    io.appmeta_mounts.emplace(name, IOState::AppmetaMount{ fs::absolute(source), canonical_source });
    mount_point = name;
    return true;
}

bool unmount_appmeta(IOState &io, const std::string &mount_point) {
    if (!is_appmeta_mount_name(mount_point))
        return false;
    std::lock_guard lock(io.appmeta_mutex);
    return io.appmeta_mounts.erase(mount_point) != 0;
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
    auto id = appmeta_sequence.load(std::memory_order_relaxed);
    do {
        if (id > 0xffffffffffffULL)
            return false;
    } while (!appmeta_sequence.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
    char name[16];
    std::snprintf(name, sizeof(name), "gm%012llx:", static_cast<unsigned long long>(id));
    std::lock_guard lock(io.appmeta_mutex);
    io.appmeta_mounts.emplace(name, IOState::AppmetaMount{ fs::absolute(source), canonical_source, patch_exists ? fs::absolute(patch) : fs::path{}, canonical_patch });
    mount_point = name;
    return true;
}

bool unmount_game_data(IOState &io, const std::string &mount_point) {
    if (!is_mount_name(mount_point, "gm"))
        return false;
    std::lock_guard lock(io.appmeta_mutex);
    return io.appmeta_mounts.erase(mount_point) != 0;
}

static std::vector<fs::path> appmeta_host_paths(IOState &io, const char *path) {
    const std::string input(path);
    const auto colon = input.find(':');
    if (colon == std::string::npos)
        return {};
    const auto name = input.substr(0, colon + 1);
    if (!is_appmeta_mount_name(name) && !is_mount_name(name, "gm"))
        return {};
    auto relative = input.substr(colon + 1);
    if (relative.find_first_of("\\:") != std::string::npos)
        return {};
    if (!relative.empty() && relative.front() == '/')
        relative.erase(0, 1);
    if (!relative.empty() && relative.front() == '/')
        return {};
    if (!relative.empty() && relative.back() == '/')
        relative.pop_back();
    if (relative.find("//") != std::string::npos)
        return {};
    std::lock_guard lock(io.appmeta_mutex);
    const auto it = io.appmeta_mounts.find(name);
    if (it == io.appmeta_mounts.end())
        return {};
    const auto &mount = it->second;
    std::error_code ec;
    if (!is_plain_component(mount.source.parent_path().parent_path()) || !is_plain_component(mount.source.parent_path()) || !is_plain_component(mount.source) || fs::canonical(mount.source, ec) != mount.canonical_source || ec)
        return {};
    if (!mount.patch.empty() && (!is_plain_component(mount.patch.parent_path()) || !is_plain_component(mount.patch) || fs::canonical(mount.patch, ec) != mount.canonical_patch || ec))
        return {};
    std::vector<fs::path> results;
    for (const auto &root : { mount.patch, mount.source }) {
        if (root.empty())
            continue;
        auto result = root;
        bool missing = false;
        for (const auto &part : fs::path(relative)) {
            const auto component = fs_utils::path_to_utf8(part);
            if (component.empty() || component.back() == '.' || component.back() == ' ' || std::any_of(component.begin(), component.end(), [](unsigned char c) { return c < 0x20; }))
                return {};
            result /= part;
            const auto status = fs::symlink_status(result, ec);
            if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status))) {
                ec.clear();
                missing = true;
                break;
            }
            if (ec || !is_plain_component(result))
                return {};
        }
        if (!missing)
            results.push_back(result);
    }
    return results;
}

static fs::path appmeta_host_path(IOState &io, const char *path) {
    const auto paths = appmeta_host_paths(io, path);
    return paths.empty() ? fs::path{} : paths.front();
}

namespace vfs {

bool read_file(const VitaIoDevice device, FileBuffer &buf, const fs::path &vita_fs_path, const fs::path &vfs_file_path) {
    const auto host_file_path = device::construct_emulated_path(device, vfs_file_path, vita_fs_path);
    return fs_utils::read_data(host_file_path, buf);
}

fs::path get_app_base_path(const fs::path &vita_fs_path, const std::string &app_path) {
    IOState io{};
    io.app_path = app_path;
    init_device_paths(io, vita_fs_path);
    if (io.device_paths.app0_device == VitaIoDevice::_INVALID || io.device_paths.app0.empty()) {
        LOG_ERROR("Cannot resolve application directory for '{}'.", app_path);
        return {};
    }
    return device::construct_emulated_path(io.device_paths.app0_device, io.device_paths.app0, vita_fs_path);
}

fs::path resolve_app_path(const fs::path &vita_fs_path, const std::string &app_path, const fs::path &vfs_file_path, const std::string &title_id) {
    IOState io{};
    io.app_path = app_path;
    io.title_id = title_id.empty() ? fs::path(app_path).filename().string() : title_id;
#ifndef _WIN32
    io.case_isens_find_enabled = true;
#endif
    init_device_paths(io, vita_fs_path);
    const auto path = "app0:/" + vfs_file_path.generic_string();
    return expand_path(io, path.c_str(), vita_fs_path);
}

bool read_app_file(FileBuffer &buf, const fs::path &vita_fs_path, const std::string &app_path, const fs::path &vfs_file_path, const std::string &title_id) {
    return fs_utils::read_data(resolve_app_path(vita_fs_path, app_path, vfs_file_path, title_id), buf);
}

bool app_dir_has_entries(const fs::path &vita_fs_path, const std::string &app_path, const fs::path &vfs_dir_path, const std::string &title_id) {
    IOState io{};
    io.app_path = app_path;
    io.title_id = title_id.empty() ? fs::path(app_path).filename().string() : title_id;
#ifndef _WIN32
    io.case_isens_find_enabled = true;
#endif
    init_device_paths(io, vita_fs_path);
    const auto path = "app0:/" + vfs_dir_path.generic_string();
    for (const auto &candidate : resolve_overlay_paths(io, path.c_str(), vita_fs_path)) {
        const auto host = find_host_path(io, expand_unresolved_path(io, candidate.c_str(), vita_fs_path));
        if (!fs::exists(host))
            continue;
        if (!fs::is_directory(host))
            return false;
        if (!fs::is_empty(host))
            return true;
    }
    return false;
}

SceSize get_directory_used_size(const VitaIoDevice device, const std::string &vfs_path, const fs::path &vita_fs_path) {
    const auto emuenv_path = device::construct_emulated_path(device, vfs_path, vita_fs_path);

    SceSize total_size = 0;
    for (const auto &entry : fs::recursive_directory_iterator(emuenv_path)) {
        if (fs::is_regular_file(entry.path()))
            total_size += fs::file_size(entry.path());
    }

    return total_size;
}

} // namespace vfs

// ****************************
// * End utility functions *
// ****************************

static bool is_valid_output_path(const VitaIoDevice device) {
    return !(device == VitaIoDevice::savedata0 || device == VitaIoDevice::savedata1 || device == VitaIoDevice::app0
        || device == VitaIoDevice::_INVALID || device == VitaIoDevice::addcont0 || device == VitaIoDevice::cache0 || device == VitaIoDevice::tty0
        || device == VitaIoDevice::tty1 || device == VitaIoDevice::tty2 || device == VitaIoDevice::tty3
        || device == VitaIoDevice::music0 || device == VitaIoDevice::photo0 || device == VitaIoDevice::video0
        || device == VitaIoDevice::memory || device == VitaIoDevice::attach0 || device == VitaIoDevice::checkout0
        || device == VitaIoDevice::friend0 || device == VitaIoDevice::near0 || device == VitaIoDevice::psnmsg0
        || device == VitaIoDevice::sqlite0 || device == VitaIoDevice::webbrowser0);
}

bool init(IOState &io, const fs::path &cache_path, const fs::path &log_path, const fs::path &vita_fs_path, bool redirect_stdio) {
    {
        const std::lock_guard lock(io.async_mutex);
        io.async_stopping = false;
    }
    // Iterate through the entire list of devices and create the subdirectories if they do not exist
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<VitaIoDevice>>([&vita_fs_path](auto i) {
        if (is_valid_output_path(i.value))
            fs::create_directories(vita_fs_path / i.name);
    });

    const fs::path ux0{ vita_fs_path / "ux0" };
    const fs::path uma0{ vita_fs_path / "uma0" };
    const fs::path vd0{ vita_fs_path / "vd0" };

    fs::create_directories(ux0 / "data");
    // The memory: work directory is volatile across emulator sessions.
    fs::remove_all(ux0 / "data" / "memory");
    fs::create_directories(ux0 / "data" / "memory");
    fs::create_directories(ux0 / "app");
    fs::create_directories(ux0 / "music");
    fs::create_directories(ux0 / "picture");
    fs::create_directories(ux0 / "theme");
    fs::create_directories(ux0 / "video");
    fs::create_directories(ux0 / "user");
    fs::create_directories(uma0 / "data");
    fs::create_directories(vd0 / "registry");
    fs::create_directories(vd0 / "network");

    fs::create_directories(cache_path / "shaders");
    fs::create_directory(log_path / "shaderlog");
    fs::create_directory(log_path / "texturelog");

    io.redirect_stdio = redirect_stdio;

#ifndef _WIN32
    io.case_isens_find_enabled = true;
#endif

    return true;
}

void io_deinit(IOState &io) {
    decltype(io.async_requests) pending;
    {
        const std::lock_guard lock(io.async_mutex);
        io.async_stopping = true;
        pending.swap(io.async_requests);
    }
    pending.clear();

    {
        const std::lock_guard files_lock(io.files_mutex);
        io.std_files.clear();
        io.dir_entries.clear();
        io.tty_files.clear();
    }

    io.next_fd = 0;

    io.device_paths = {};
    io.addcont.clear();
    io.content_id.clear();
    io.savedata.clear();
    io.title_id.clear();
    io.app_path.clear();

    io.cachemap.clear();

    {
        std::lock_guard lock(io.appmeta_mutex);
        io.appmeta_mounts.clear();
    }

    {
        std::lock_guard<std::mutex> lock(io.overlay_mutex);
        io.overlays.clear();
        io.next_overlay_id = 1;
        io.app_overlay_id = -1;
    }
}

void init_device_paths(IOState &io, const fs::path &vita_fs_path) {
    io.device_paths.user_id = io.user_id;
    io.device_paths.savedata0 = "user/" + io.user_id + "/savedata/" + io.savedata;
    const auto app_device = device::get_device(io.app_path);
    if (!io.app_path.empty() && app_device != VitaIoDevice::_INVALID && app_device != VitaIoDevice::app0) {
        io.device_paths.app0_device = app_device;
        io.device_paths.app0 = device::remove_device_from_path(io.app_path, app_device);
    } else if (!io.app_path.empty() && app_device == VitaIoDevice::_INVALID && io.app_path.find(':') == std::string::npos) {
        io.device_paths.app0_device = VitaIoDevice::ux0;
        io.device_paths.app0 = "app/" + io.app_path;
    } else {
        LOG_ERROR("Cannot mount app0 for invalid application path '{}'.", io.app_path);
        io.device_paths.app0_device = VitaIoDevice::_INVALID;
        io.device_paths.app0.clear();
    }
    io.device_paths.addcont0 = "addcont/" + io.addcont;
    io.device_paths.cache0.clear();
    {
        std::lock_guard<std::mutex> lock(io.overlay_mutex);
        std::erase_if(io.overlays, [&](const FiosOverlay &overlay) { return overlay.id == io.app_overlay_id; });
        io.app_overlay_id = -1;
    }
    if (!io.title_id.empty() && fs::is_directory(vita_fs_path / "ux0" / "patch" / io.title_id)) {
        SceFiosProcessOverlay overlay{};
        overlay.type = SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT;
        overlay.order = 0x82;
        const auto src = "ux0:/patch/" + io.title_id + "/";
        if (src.size() >= sizeof(overlay.src)) {
            LOG_ERROR("Cannot mount update for '{}': path is too long", io.title_id);
            return;
        }
        std::strcpy(overlay.dst, "app0:/");
        std::memcpy(overlay.src, src.c_str(), src.size() + 1);
        io.app_overlay_id = create_overlay(io, &overlay);
    }
}

bool init_savedata_app_path(IOState &io, const fs::path &vita_fs_path) {
    const fs::path user_id_path{ vita_fs_path / "ux0" / "user" / io.user_id };
    const fs::path savedata_path{ user_id_path / "savedata" };
    const fs::path savedata_game_path{ savedata_path / io.savedata };

    fs::create_directories(user_id_path);
    fs::create_directories(savedata_path);
    fs::create_directories(savedata_game_path);

    return true;
}

bool find_case_isens_path(IOState &io, VitaIoDevice &device, const fs::path &translated_path, const fs::path &system_path) {
    std::string final_path{};

    switch (device) {
    case VitaIoDevice::app0: {
        std::string app_id = translated_path.string().substr(0, 14);
        final_path = system_path.string().substr(0, system_path.string().find(app_id)) + app_id;
        break;
    }
    case VitaIoDevice::addcont0: {
        std::string addcont_id = translated_path.string().substr(0, 18);
        final_path = system_path.string().substr(0, system_path.string().find(addcont_id)) + addcont_id;
        break;
    }
    case VitaIoDevice::vs0: {
        // This only works if ALL the parent folders of the path are the correct case or are in a case insensitive fs
        // Only the file's name is searched for, not the parent folders
        final_path = system_path.string().substr(0, system_path.string().find_last_of('/'));
        break;
    }
    default: {
        return false;
    }
    }

    if (!fs::exists(final_path))
        return false;

    for (const auto &file : fs::recursive_directory_iterator(final_path)) {
        io.cachemap.emplace(string_utils::tolower(file.path().string()), file.path().string());
    }

    return true;
}

fs::path find_in_cache(IOState &io, const std::string &system_path) {
    const auto find_path = io.cachemap.find(system_path);

    if (find_path != io.cachemap.end()) {
        return fs::path{ find_path->second.c_str() };
    } else {
        return fs::path{};
    }
}

std::string translate_path(const char *path, VitaIoDevice &device, const IOState::DevicePaths &device_paths) {
    auto relative_path = device::remove_duplicate_device(path, device);

    // replace invalid slashes with proper forward slash
    string_utils::replace(relative_path, "\\", "/");
    string_utils::replace(relative_path, "/./", "/");
    string_utils::replace(relative_path, "//", "/");
    // TODO: Handle dot-dot paths

    switch (device) {
    case VitaIoDevice::savedata0: // Redirect savedata0: to ux0:user/00/savedata/<title_id>
    case VitaIoDevice::savedata1: {
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.savedata0);
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::app0: {
        if (device_paths.app0_device == VitaIoDevice::_INVALID || device_paths.app0.empty())
            return {};
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.app0);
        device = device_paths.app0_device;
        break;
    }
    case VitaIoDevice::addcont0: { // Redirect addcont0: to ux0:addcont/<title_id>
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.addcont0);
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::cache0: { // Redirect cache0: to ux0:cache/<title_id>
        if (device_paths.cache0.empty())
            return std::string{};
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.cache0);
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::friend0:
    case VitaIoDevice::psnmsg0:
    case VitaIoDevice::near0: {
        const char *directory = device == VitaIoDevice::friend0 ? "psnfriend" : device == VitaIoDevice::psnmsg0 ? "psnmsg"
                                                                                                                : "near";
        relative_path = device::remove_device_from_path(relative_path, device, "user/" + device_paths.user_id + "/" + directory);
        device = VitaIoDevice::ur0;
        break;
    }
    case VitaIoDevice::sqlite0:
    case VitaIoDevice::attach0:
    case VitaIoDevice::checkout0:
    case VitaIoDevice::webbrowser0: {
        const char *directory = device == VitaIoDevice::sqlite0 ? "sqlite" : device == VitaIoDevice::attach0 ? "attach"
            : device == VitaIoDevice::checkout0                                                              ? "checkout"
                                                                                                             : "webbrowser";
        relative_path = device::remove_device_from_path(relative_path, device, std::string("temp/") + directory);
        device = VitaIoDevice::ur0;
        break;
    }
    case VitaIoDevice::music0: { // Redirect music0: to ux0:music
        relative_path = device::remove_device_from_path(relative_path, device, "music");
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::photo0: { // Redirect photo0: to ux0:picture
        relative_path = device::remove_device_from_path(relative_path, device, "picture");
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::memory: { // Redirect memory: to ux0:data/memory
        relative_path = device::remove_device_from_path(relative_path, device, "data/memory");
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::video0: { // Redirect video0: to ux0:video
        relative_path = device::remove_device_from_path(relative_path, device, "video");
        device = VitaIoDevice::ux0;
        break;
    }

    case VitaIoDevice::host0:
    case VitaIoDevice::gro0:
    case VitaIoDevice::grw0:
    case VitaIoDevice::imc0:
    case VitaIoDevice::os0:
    case VitaIoDevice::pd0:
    case VitaIoDevice::sa0:
    case VitaIoDevice::sd0:
    case VitaIoDevice::tm0:
    case VitaIoDevice::ud0:
    case VitaIoDevice::uma0:
    case VitaIoDevice::ur0:
    case VitaIoDevice::ux0:
    case VitaIoDevice::vd0:
    case VitaIoDevice::vs0:
    case VitaIoDevice::xmc0: {
        relative_path = device::remove_device_from_path(relative_path, device);
        break;
    }
    case VitaIoDevice::tty0:
    case VitaIoDevice::tty1:
    case VitaIoDevice::tty2:
    case VitaIoDevice::tty3: {
        return std::string{};
    }
    default: {
        LOG_CRITICAL_IF(relative_path.contains(':'), "Unknown device with path {} used. Report this to the developers!", relative_path);
        return std::string{};
    }
    }

    // If the path is empty, the request is the device itself
    if (relative_path.empty())
        return std::string{};

    if (relative_path.front() == '/' || relative_path.front() == '\\')
        relative_path.erase(0, 1);

    return relative_path;
}

static fs::path expand_unresolved_path(IOState &io, const char *path, const fs::path &vita_fs_path) {
    if (is_appmeta_path(path))
        return appmeta_host_path(io, path);
    auto device = device::get_device(path);
    if (device == VitaIoDevice::_INVALID)
        return {};

    const auto translated_path = translate_path(path, device, io.device_paths);
    return device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio);
}

static fs::path find_host_path(IOState &io, const fs::path &path) {
    if (!io.case_isens_find_enabled || fs::exists(path))
        return path;

    auto current = path.root_path();
    for (const auto &part : path.relative_path()) {
        const auto exact = current / part;
        if (fs::exists(exact)) {
            current = exact;
            continue;
        }
        if (!fs::is_directory(current))
            return path;
        const auto wanted = string_utils::tolower(fs_utils::path_to_utf8(part));
        bool found = false;
        for (const auto &entry : fs::directory_iterator(current)) {
            if (string_utils::tolower(fs_utils::path_to_utf8(entry.path().filename())) == wanted) {
                current = entry.path();
                found = true;
                break;
            }
        }
        if (!found)
            return path;
    }
    return current;
}

fs::path expand_path(IOState &io, const char *path, const fs::path &vita_fs_path) {
    if (is_appmeta_path(path))
        return appmeta_host_path(io, path);
    const auto resolved = resolve_path(io, path, vita_fs_path);
    return find_host_path(io, expand_unresolved_path(io, resolved.c_str(), vita_fs_path));
}

SceUID open_file(IOState &io, const char *path, const int flags, const fs::path &vita_fs_path, const char *export_name) {
    if (is_appmeta_path(path)) {
        if (can_write(flags) || (flags & SCE_O_TRUNC))
            return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
        const auto host = appmeta_host_path(io, path);
        std::error_code ec;
        if (host.empty() || !fs::is_regular_file(host, ec) || ec)
            return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
        FileStats file{ path, path, host, flags };
        if (!file.get_file_pointer())
            return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
        const auto fd = io.next_fd++;
        {
            const std::lock_guard lock(io.files_mutex);
            io.std_files.emplace(fd, std::move(file));
        }
        return fd;
    }
    auto device = device::get_device(path);
    auto device_for_icase = device;
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    if ((device == VitaIoDevice::tty0) || (device == VitaIoDevice::tty1) || (device == VitaIoDevice::tty2) || (device == VitaIoDevice::tty3)) {
        assert(flags >= 0);

        auto tty_type = TTY_UNKNOWN;
        if (flags & SCE_O_RDONLY)
            tty_type |= TTY_IN;
        if (flags & SCE_O_WRONLY)
            tty_type |= TTY_OUT;

        const auto fd = io.next_fd++;
        {
            const std::lock_guard lock(io.files_mutex);
            io.tty_files.emplace(fd, tty_type);
        }

        LOG_TRACE_IF(log_file_op, "{}: Opening terminal {}:", export_name, device);
        return fd;
    }

    const auto resolved_path = resolve_path(io, path, vita_fs_path, can_write(flags));
    if (is_appmeta_path(resolved_path))
        return open_file(io, resolved_path.c_str(), flags, vita_fs_path, export_name);
    device = device::get_device(resolved_path);
    const auto translated_path = translate_path(resolved_path.c_str(), device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    auto system_path = find_host_path(io, device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio));
    if (fs::is_directory(system_path)) {
        LOG_ERROR("Cannot open directory: {}", system_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    // Do not allow any new files if they do not have a write flag.
    if (!fs::exists(system_path)) {
        if (!(flags & SCE_O_CREAT)) {
            if (io.case_isens_find_enabled) {
                // Attempt a case-insensitive file search.
                const auto original_system_path = system_path;
                const auto cached_path = find_in_cache(io, string_utils::tolower(system_path.string()));
                if (!cached_path.empty()) {
                    system_path = cached_path;
                    LOG_TRACE("Found cached filepath at {}", system_path);
                } else {
                    const bool path_found = find_case_isens_path(io, device_for_icase, translated_path, system_path);
                    system_path = find_in_cache(io, string_utils::tolower(system_path.string()));
                    if (!system_path.empty() && path_found) {
                        LOG_TRACE("Found file on case-sensitive filesystem at {}", system_path);
                    } else {
                        LOG_ERROR("Missing file at {} (target path: {})", original_system_path, path);
                        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                    }
                }
            } else {
                LOG_ERROR("Missing file at {} (target path: {})", system_path, path);
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
            }
        } else {
            if (!fs::exists(system_path.parent_path())) {
                fs::create_directories(system_path.parent_path());
            }
            std::ofstream file(system_path);
        }
    }

    const auto normalized_path = device::construct_normalized_path(device, translated_path);

    FileStats f{ path, normalized_path, system_path, flags };
    const auto fd = io.next_fd++;
    {
        const std::lock_guard lock(io.files_mutex);
        io.std_files.emplace(fd, f);
    }

    LOG_TRACE_IF(log_file_op, "{}: Opening file {} ({}), fd: {}", export_name, path, normalized_path, log_hex(fd));
    return fd;
}

static std::optional<FileStats> acquire_file(IOState &io, SceUID fd) {
    const std::lock_guard lock(io.files_mutex);
    const auto it = io.std_files.find(fd);
    if (it == io.std_files.end() || !it->second.get_file_pointer())
        return std::nullopt;
    return it->second;
}

int read_file(void *data, IOState &io, const SceUID fd, const SceSize size, const char *export_name) {
    assert(data != nullptr);
    assert(size >= 0);

    const auto file = io.std_files.find(fd);
    if (file != io.std_files.end()) {
        const auto read = file->second.read(data, 1, size);
        LOG_TRACE_IF(log_file_op && log_file_read, "{}: Reading {} bytes of fd {}", export_name, read, log_hex(fd));
        return static_cast<int>(read);
    }

    const auto tty_file = io.tty_files.find(fd);
    if (tty_file != io.tty_files.end()) {
        if (tty_file->second == TTY_IN) {
            std::cin.read(static_cast<char *>(data), size);
            LOG_TRACE_IF(log_file_op && log_file_read, "{}: Reading terminal fd: {}, size: {}", export_name, log_hex(fd), size);
            return size;
        }
        return IO_ERROR_UNK();
    }

    return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
}

namespace {

constexpr int ASYNC_BUSY = static_cast<int>(0x80010010u);
constexpr int ASYNC_READ_ERROR = static_cast<int>(0x80010005u);
constexpr uint32_t ASYNC_RESULT_SIZE = 24;
constexpr uint32_t GUEST_PAGE_SIZE = 4096;

struct GuestPin {
    MemState *mem;
    uint64_t id;

    GuestPin(MemState &state, uint64_t pin)
        : mem(&state)
        , id(pin) {}
    GuestPin(const GuestPin &) = delete;
    GuestPin &operator=(const GuestPin &) = delete;
    GuestPin(GuestPin &&other) noexcept
        : mem(other.mem)
        , id(other.id) { other.id = 0; }
    ~GuestPin() {
        if (id)
            unpin_guest_memory(*mem, id);
    }
};

void copy_guest(MemState &mem, Address address, void *buffer, size_t size, bool to_guest) {
    auto *bytes = static_cast<uint8_t *>(buffer);
    while (size) {
        const size_t count = std::min(size, size_t(GUEST_PAGE_SIZE - (address % GUEST_PAGE_SIZE)));
        auto *guest = Ptr<uint8_t>(address).get(mem);
        if (to_guest)
            std::memcpy(guest, bytes, count);
        else
            std::memcpy(bytes, guest, count);
        address += static_cast<Address>(count);
        bytes += count;
        size -= count;
    }
}

} // namespace

int complete_async_io(IOState &io, SceUID request_id, const char *export_name) {
    std::shared_ptr<AsyncIoRequest> request;
    {
        const std::lock_guard lock(io.async_mutex);
        const auto it = io.async_requests.find(request_id);
        if (it == io.async_requests.end())
            return IO_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
        SceUInt32 pattern = 0;
        const int error = it->second->event->get_pattern(pattern);
        if (error)
            return IO_ERROR(error);
        // A pending operation cannot be destroyed safely by the emulator.
        if (!it->second->finished || pattern != 0)
            return IO_ERROR(ASYNC_BUSY);
        request = std::move(it->second);
        io.async_requests.erase(it);
    }
    request.reset();
    return 0;
}

int read_file_async(IOState &io, KernelState &kernel, MemState &mem, SceUID thread_id, SceUID fd,
    Ptr<uint8_t> data, SceSize size, Ptr<uint8_t> result, const char *export_name) {
    const std::lock_guard submit_lock(io.async_mutex);
    if (io.async_stopping)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    if (size > std::numeric_limits<int32_t>::max())
        return IO_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    auto file = acquire_file(io, fd);
    if (!file || !(file->get_open_mode() & SCE_O_RDONLY))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const std::array ranges{ GuestMemoryRange{ data.address(), size },
        GuestMemoryRange{ result.address(), result ? ASYNC_RESULT_SIZE : 0 } };
    uint64_t pin_id;
    try {
        pin_id = pin_guest_memory(mem, ranges);
    } catch (const std::bad_alloc &) {
        LOG_ERROR("sceIoReadAsync: could not pin guest memory");
        return IO_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    }
    if (!pin_id)
        return IO_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    GuestPin pin(mem, pin_id);
    uint32_t cookie = 0;
    if (result)
        copy_guest(mem, result.address() + 8, &cookie, sizeof(cookie), false);

    try {
        auto event = std::make_shared<SimpleEvent>(SCE_KERNEL_EVENT_ATTR_AUTO_RESET, "SceIoAsyncEvent", 0, thread_id, false);
        auto request = std::make_shared<AsyncIoRequest>();
        const SceUID id = kernel.get_next_uid();
        request->event = event;
        request->cleanup = [&kernel, id, thread_id] {
            kernel.objects.remove<SimpleEvent>(id, thread_id);
        };
        kernel.objects.add(event, id);
        io.async_requests.emplace(id, request);
        try {
            request->worker = std::thread([&io, &mem, request_ptr = request.get(), file = std::move(*file),
                                              event, data, size, result, cookie, pin = std::move(pin)]() mutable {
                int64_t read_result;
                try {
                    std::vector<uint8_t> buffer(std::min<size_t>(size, 1024 * 1024));
                    size_t total = 0;
                    const auto file_lock = file.lock_operations();
                    while (total < size) {
                        const size_t requested = std::min(buffer.size(), size_t(size) - total);
                        const size_t count = std::fread(buffer.data(), 1, requested, file.get_file_pointer());
                        if (count)
                            copy_guest(mem, data.address() + static_cast<Address>(total), buffer.data(), count, true);
                        total += count;
                        if (count < requested)
                            break;
                    }
                    read_result = size && total == 0 && std::ferror(file.get_file_pointer()) ? ASYNC_READ_ERROR : static_cast<int64_t>(total);
                } catch (const std::bad_alloc &) {
                    LOG_ERROR("sceIoReadAsync: cannot allocate {} bytes", size);
                    read_result = static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
                }
                if (result) {
                    uint32_t zero = 0;
                    copy_guest(mem, result.address(), &read_result, sizeof(read_result), true);
                    copy_guest(mem, result.address() + 8, &cookie, sizeof(cookie), true);
                    copy_guest(mem, result.address() + 20, &zero, sizeof(zero), true);
                }
                const std::lock_guard done_lock(io.async_mutex);
                request_ptr->finished = true;
                const SceUInt64 user_data = result ? result.address() : static_cast<SceUInt64>(read_result);
                const int error = event->set_system_event(1, user_data);
                if (error)
                    LOG_ERROR("sceIoReadAsync: failed to signal request {}: {}", event->uid, log_hex(error));
            });
        } catch (const std::system_error &) {
            io.async_requests.erase(id);
            throw;
        } catch (const std::bad_alloc &) {
            io.async_requests.erase(id);
            throw;
        }
        return id;
    } catch (const std::system_error &error) {
        LOG_ERROR("sceIoReadAsync: could not start worker: {}", error.what());
        return IO_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    } catch (const std::bad_alloc &) {
        LOG_ERROR("sceIoReadAsync: could not allocate request");
        return IO_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    }
}

int write_file(SceUID fd, const void *data, const SceSize size, const IOState &io, const char *export_name) {
    assert(data != nullptr);
    assert(size >= 0);

    if (fd < 0) {
        LOG_WARN("Error writing fd: {}, size: {}", log_hex(fd), size);
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    const auto tty_file = io.tty_files.find(fd);
    if (tty_file != io.tty_files.end()) {
        if (tty_file->second & TTY_OUT) {
            std::string s(static_cast<char const *>(data), size);

            // trim newline
            if (io.redirect_stdio) {
                std::cout << s;
            } else {
                if (s.back() == '\n')
                    s.pop_back();
                LOG_TRACE_IF(log_file_op, "*** TTY: {}", s);
            }

            return size;
        }
        return IO_ERROR_UNK();
    }

    const auto file = io.std_files.find(fd);
    if (file == io.std_files.end())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    if (!fs::is_directory(file->second.get_system_location().parent_path())) {
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT); // TODO: Is it the right error code?
    }

    if (file->second.can_write_file()) {
        const auto written = file->second.write(data, 1, size);
        LOG_TRACE_IF(log_file_op, "{}: Writing to fd: {}, size: {}", export_name, log_hex(fd), size);
        return static_cast<int>(written);
    }

    return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
}

int truncate_file(const SceUID fd, unsigned long long length, const IOState &io, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const auto file = io.std_files.find(fd);
    if (file == io.std_files.end())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    if (!file->second.can_write_file())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    auto trunc = file->second.truncate(length);
    LOG_TRACE_IF(log_file_op, "{}: Truncating fd: {}, to size: {}", export_name, log_hex(fd), length);
    return trunc;
}

SceOff seek_file(const SceUID fd, const SceOff offset, const SceIoSeekMode whence, IOState &io, const char *export_name) {
    if (!(whence == SCE_SEEK_SET || whence == SCE_SEEK_CUR || whence == SCE_SEEK_END))
        return IO_ERROR(SCE_ERROR_ERRNO_EOPNOTSUPP);

    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const auto file = io.std_files.find(fd);
    if (file == io.std_files.end())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    if (!file->second.seek(offset, whence))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const auto log_mode = [](const SceIoSeekMode whence) -> const char * {
        switch (whence) {
            STR_CASE(SCE_SEEK_SET);
            STR_CASE(SCE_SEEK_CUR);
            STR_CASE(SCE_SEEK_END);
        default:
            return "INVALID";
        }
    };

    LOG_TRACE_IF(log_file_op && log_file_seek, "{}: Seeking fd: {}, offset: {}, whence: {}", export_name, log_hex(fd), log_hex(offset), log_mode(whence));
    return file->second.tell();
}

SceOff tell_file(IOState &io, const SceUID fd, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);

    const auto std_file = io.std_files.find(fd);

    if (std_file == io.std_files.end()) {
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    return std_file->second.tell();
}

int stat_file(IOState &io, const char *file, SceIoStat *statp, const fs::path &vita_fs_path, const char *export_name, const SceUID fd) {
    assert(statp != nullptr);

    memset(statp, '\0', sizeof(SceIoStat));

    fs::path file_path = "";
    if (fd == invalid_fd) {
        if (is_appmeta_path(file)) {
            file_path = appmeta_host_path(io, file);
            if (file_path.empty())
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
        } else {
            auto device = device::get_device(file);
            auto device_for_icase = device;
            if (device == VitaIoDevice::_INVALID) {
                LOG_ERROR("Cannot find device for path: {}", file);
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
            }

            const auto resolved_path = resolve_path(io, file, vita_fs_path);
            if (is_appmeta_path(resolved_path))
                return stat_file(io, resolved_path.c_str(), statp, vita_fs_path, export_name);
            device = device::get_device(resolved_path);
            const auto translated_path = translate_path(resolved_path.c_str(), device, io.device_paths);
            file_path = find_host_path(io, device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio));

            if (!fs::exists(file_path)) {
                if (io.case_isens_find_enabled) {
                    // Attempt a case-insensitive file search.
                    const auto original_file_path = file_path;
                    const auto cached_path = find_in_cache(io, string_utils::tolower(file_path.string()));
                    if (!cached_path.empty()) {
                        file_path = cached_path;
                        LOG_TRACE("Found cached filepath at {}", file_path);
                    } else {
                        const bool path_found = find_case_isens_path(io, device_for_icase, translated_path, file_path);
                        file_path = find_in_cache(io, string_utils::tolower(file_path.string()));
                        if (!file_path.empty() && path_found) {
                            LOG_TRACE("Found file on case-sensitive filesystem at {}", file_path);
                        } else {
                            LOG_ERROR("Missing file at {} (target path: {})", original_file_path, file);
                            return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                        }
                    }
                } else {
                    LOG_ERROR("Missing file at {} (target path: {})", file_path, file);
                    return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                }
            }
            LOG_TRACE_IF(log_file_op && log_file_stat, "{}: Statting file: {} ({})", export_name, file, device::construct_normalized_path(device, translated_path));
        }
    } else { // We have previously opened and defined the location
        const auto fd_file = io.std_files.find(fd);
        if (fd_file == io.std_files.end())
            return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

        if (is_appmeta_path(fd_file->second.get_vita_loc())) {
            file_path = appmeta_host_path(io, fd_file->second.get_vita_loc());
            if (file_path.empty())
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
        } else {
            file_path = fd_file->second.get_system_location();
        }
        LOG_TRACE_IF(log_file_op && log_file_stat, "{}: Statting fd: {}", export_name, log_hex(fd));

        statp->st_attr = fd_file->second.get_file_mode();
    }

    std::uint64_t last_access_time_ticks;
    std::uint64_t creation_time_ticks;
    std::uint64_t last_modification_time_ticks;

#ifdef _WIN32
    struct _stati64 sb;
    if (_wstati64(file_path.generic_wstring().c_str(), &sb) < 0)
        return IO_ERROR_UNK();
#else
    struct stat64 sb;
    if (stat64(file_path.generic_string().c_str(), &sb) < 0)
        return IO_ERROR_UNK();
#endif

    last_access_time_ticks = RTC_OFFSET + static_cast<uint64_t>(sb.st_atime) * VITA_CLOCKS_PER_SEC;
    creation_time_ticks = RTC_OFFSET + static_cast<uint64_t>(sb.st_ctime) * VITA_CLOCKS_PER_SEC;
    last_modification_time_ticks = RTC_OFFSET + static_cast<uint64_t>(sb.st_mtime) * VITA_CLOCKS_PER_SEC;

#ifndef _WIN32
#undef st_atime
#undef st_mtime
#undef st_ctime
#endif

    // report regular files as readable but not executable
    statp->st_mode = SCE_S_IRUSR | SCE_S_IRGRP | SCE_S_IROTH;

    if (fs::is_regular_file(file_path)) {
        statp->st_size = fs::file_size(file_path);
        statp->st_attr = SCE_SO_IFREG;
        statp->st_mode |= SCE_S_IFREG;
    }
    if (fs::is_directory(file_path)) {
        statp->st_attr = SCE_SO_IFDIR;
        statp->st_mode |= SCE_S_IFDIR | SCE_S_IXUSR | SCE_S_IXGRP | SCE_S_IXOTH;
    }

    __RtcTicksToPspTime(&statp->st_atime, last_access_time_ticks);
    __RtcTicksToPspTime(&statp->st_mtime, last_modification_time_ticks);
    __RtcTicksToPspTime(&statp->st_ctime, creation_time_ticks);

    return 0;
}

int stat_file_by_fd(IOState &io, const SceUID fd, SceIoStat *statp, const fs::path &vita_fs_path, const char *export_name) {
    assert(statp != nullptr);
    memset(statp, '\0', sizeof(SceIoStat));

    const auto std_file = io.std_files.find(fd);
    if (std_file == io.std_files.end()) {
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    return stat_file(io, std_file->second.get_vita_loc(), statp, vita_fs_path, export_name, fd);
}

int close_file(IOState &io, const SceUID fd, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);

    LOG_TRACE_IF(log_file_op, "{}: Closing file fd: {}", export_name, log_hex(fd));

    const std::lock_guard lock(io.files_mutex);
    io.tty_files.erase(fd);
    io.std_files.erase(fd);

    return 0;
}

int remove_file(IOState &io, const char *file, const fs::path &vita_fs_path, const char *export_name) {
    if (is_appmeta_path(file))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    auto device = device::get_device(file);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", file);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto resolved_path = resolve_path(io, file, vita_fs_path, true);
    if (is_appmeta_path(resolved_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    device = device::get_device(resolved_path);
    const auto translated_path = translate_path(resolved_path.c_str(), device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", translated_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_path = find_host_path(io, device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio));
    if (!fs::exists(emulated_path) || fs::is_directory(emulated_path)) {
        LOG_ERROR("File does not exist at path: {} (target path: {})", emulated_path, file);
    }

    LOG_TRACE_IF(log_file_op, "{}: Removing file {} ({})", export_name, file, device::construct_normalized_path(device, translated_path));

    std::error_code error_code{};
    const auto res = fs::remove(emulated_path, error_code);

    if (!res || error_code.value()) {
        LOG_ERROR("Cannot remove file: {} ({})", file, device::construct_normalized_path(device, translated_path));
        LOG_ERROR("Error code: {} ({})", error_code.value(), error_code.message());
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    return 0;
}

int rename(IOState &io, const char *old_name, const char *new_name, const fs::path &vita_fs_path, const char *export_name) {
    if (is_appmeta_path(old_name) || is_appmeta_path(new_name))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    auto device = device::get_device(old_name);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", old_name);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    auto new_device = device::get_device(new_name);
    if (new_device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", new_name);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto resolved_old_path = resolve_path(io, old_name, vita_fs_path, true);
    if (is_appmeta_path(resolved_old_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    device = device::get_device(resolved_old_path);
    const auto translated_old_path = translate_path(resolved_old_path.c_str(), device, io.device_paths);
    if (translated_old_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", translated_old_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto resolved_new_path = resolve_path(io, new_name, vita_fs_path, true);
    if (is_appmeta_path(resolved_new_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    new_device = device::get_device(resolved_new_path);
    const auto translated_new_path = translate_path(resolved_new_path.c_str(), new_device, io.device_paths);
    if (translated_new_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", translated_new_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_old_path = device::construct_emulated_path(device, translated_old_path, vita_fs_path, io.redirect_stdio);
    if (!fs::exists(emulated_old_path)) {
        LOG_ERROR("File does not exist at path: {} (target path: {})", emulated_old_path, old_name);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_new_path = device::construct_emulated_path(new_device, translated_new_path, vita_fs_path, io.redirect_stdio);

    LOG_TRACE_IF(log_file_op, "{}: Renaming file {} to {} ({} to {})", export_name, old_name, new_name, emulated_old_path, emulated_new_path);

    std::error_code error_code{};
    fs::rename(emulated_old_path, emulated_new_path, error_code);

    if (error_code.value()) {
        LOG_ERROR("Cannot rename file: {} to {} ({} to {})", old_name, new_name, emulated_old_path, emulated_new_path);
        LOG_ERROR("Error code: {} ({})", error_code.value(), error_code.message());
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    return 0;
}

SceUID open_dir(IOState &io, const char *path, const fs::path &vita_fs_path, const char *export_name) {
    const auto candidates = is_appmeta_path(path) ? std::vector<std::string>{ path } : resolve_overlay_paths(io, path, vita_fs_path);
    std::vector<DirPtr> opened;
    fs::path dir_path;
    for (const auto &candidate : candidates) {
        std::vector<fs::path> host_paths;
        if (is_appmeta_path(candidate)) {
            host_paths = appmeta_host_paths(io, candidate.c_str());
        } else {
            const auto unresolved = expand_unresolved_path(io, candidate.c_str(), vita_fs_path);
            if (!unresolved.empty())
                host_paths.push_back(find_host_path(io, unresolved));
        }
        for (const auto &candidate_path : host_paths) {
            if (!fs::exists(candidate_path))
                continue;
            if (!fs::is_directory(candidate_path)) {
                if (opened.empty()) {
                    LOG_ERROR("Not a directory: {} (target path: {})", candidate_path, path);
                    return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                }
                break;
            }
            auto dir = create_shared_dir(candidate_path);
            if (!dir) {
                LOG_ERROR("Failed to open directory at: {} (target path: {})", candidate_path, path);
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
            }
            if (opened.empty())
                dir_path = candidate_path;
            opened.push_back(std::move(dir));
        }
    }
    if (opened.empty()) {
        LOG_ERROR("Directory does not exist (target path: {})", path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    std::string normalized = path;
    if (!is_appmeta_path(path)) {
        auto device = device::get_device(path);
        const auto translated_path = translate_path(path, device, io.device_paths);
        normalized = device::construct_normalized_path(device, translated_path);
    }
    const DirStats d{ path, normalized, dir_path, std::move(opened) };
    const auto fd = io.next_fd++;
    io.dir_entries.emplace(fd, d);

    LOG_TRACE_IF(log_file_op, "{}: Opening dir {} ({}), fd: {}", export_name, path, normalized, log_hex(fd));

    return fd;
}

SceUID read_dir(IOState &io, const SceUID fd, SceIoDirent *dent, const fs::path &vita_fs_path, const char *export_name) {
    assert(dent != nullptr);

    memset(dent->d_name, '\0', sizeof(dent->d_name));

    const auto dir = io.dir_entries.find(fd);

    if (dir != io.dir_entries.end()) {
        // Refuse any fd that is not explicitly a directory
        if (!dir->second.is_directory())
            return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

        while (const auto d = dir->second.get_dir_ptr()) {
            const auto d_name_utf8 = get_file_in_dir(d);
            if (d_name_utf8 == "." || d_name_utf8 == ".." || !dir->second.add_entry(string_utils::tolower(d_name_utf8)))
                continue;
            strncpy(dent->d_name, d_name_utf8.c_str(), sizeof(dent->d_name) - 1);
            auto file_path = std::string(dir->second.get_vita_loc());
            if (!file_path.ends_with('/'))
                file_path += '/';
            file_path += d_name_utf8;

            LOG_TRACE_IF(log_file_op, "{}: Reading entry {} of fd: {}", export_name, file_path, log_hex(fd));
            if (is_appmeta_path(file_path) && appmeta_host_path(io, file_path.c_str()).empty())
                continue;
            if (stat_file(io, file_path.c_str(), &dent->d_stat, vita_fs_path, export_name) < 0)
                return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);
            else
                return 1; // move to the next file
        }
        return 0;
    }

    return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
}

int create_dir(IOState &io, const char *dir, int mode, const fs::path &vita_fs_path, const char *export_name, const bool recursive) {
    if (is_appmeta_path(dir))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    auto device = device::get_device(dir);
    const auto resolved_path = resolve_path(io, dir, vita_fs_path, true);
    if (is_appmeta_path(resolved_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    device = device::get_device(resolved_path);
    const auto translated_path = translate_path(resolved_path.c_str(), device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Failed to translate path: {}", dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_path = device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio);
    if (recursive)
        return fs::create_directories(emulated_path);
    if (fs::exists(emulated_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EEXIST);

    auto parent_path = emulated_path.parent_path();
    if (!emulated_path.has_filename())
        parent_path = parent_path.parent_path();
    if (!fs::exists(parent_path)) // Vita cannot recursively create directories
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);

    LOG_TRACE_IF(log_file_op, "{}: Creating new dir {} ({})", export_name, dir, device::construct_normalized_path(device, translated_path));

    if (!fs::create_directory(emulated_path)) {
        LOG_ERROR("Failed to create directory at {} (target path: {})", emulated_path, dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    return 0;
}

int close_dir(IOState &io, const SceUID fd, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);

    const auto erased_entries = io.dir_entries.erase(fd);

    LOG_TRACE_IF(log_file_op, "{}: Closing dir fd: {}", export_name, log_hex(fd));

    if (erased_entries == 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    return 0;
}

int remove_dir(IOState &io, const char *dir, const fs::path &vita_fs_path, const char *export_name) {
    if (is_appmeta_path(dir))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    auto device = device::get_device(dir);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto resolved_path = resolve_path(io, dir, vita_fs_path, true);
    if (is_appmeta_path(resolved_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    device = device::get_device(resolved_path);
    const auto translated_path = translate_path(resolved_path.c_str(), device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    LOG_TRACE_IF(log_file_op, "{}: Removing dir {} ({})", export_name, dir, device::construct_normalized_path(device, translated_path));

    if (!fs::remove_all(device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio))) {
        LOG_ERROR("Cannot remove dir: {} ({})", dir, device::construct_normalized_path(device, translated_path));
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    return 0;
}

static std::string standardize_path(std::string_view path) {
    // replace app0:... by app0:/...
    bool start_with_app0 = path.starts_with("app0:");
    if (start_with_app0 && (path.size() == 5 || path[5] != '/'))
        return "app0:/" + std::string(path.substr(5));
    else
        return std::string(path);
}

SceUID create_overlay(IOState &io, SceFiosProcessOverlay *fios_overlay) {
    std::lock_guard<std::mutex> lock(io.overlay_mutex);

    FiosOverlay overlay{
        .id = io.next_overlay_id++,
        .type = fios_overlay->type,
        .order = fios_overlay->order,
        .process_id = fios_overlay->process_id,
        .dst = standardize_path(fios_overlay->dst),
        .src = standardize_path(fios_overlay->src)
    };

    // find location where to put it
    size_t overlay_index = 0;
    // lower order first and in case of equality, last one inserted first
    while (overlay_index < io.overlays.size() && io.overlays[overlay_index].order < overlay.order)
        overlay_index++;
    auto res = overlay.id;
    io.overlays.insert(io.overlays.begin() + overlay_index, std::move(overlay));

    return res;
}

std::vector<std::string> resolve_overlay_paths(IOState &io, const char *input, const fs::path &vita_fs_path, const bool for_write, const SceUInt32 min_order, const SceUInt32 max_order) {
    if (is_appmeta_path(input))
        return { input };
    std::lock_guard<std::mutex> lock(io.overlay_mutex);

    const auto resolve_from = [&](auto &&self, std::string curr_path, size_t overlay_idx) -> std::vector<std::string> {
        while (overlay_idx < io.overlays.size()) {
            if (is_appmeta_path(curr_path))
                break;
            const FiosOverlay &overlay = io.overlays[overlay_idx++];
            if (overlay.order < min_order)
                continue;
            if (overlay.order > max_order)
                break;

            if (!curr_path.starts_with(overlay.dst)
                || (curr_path.size() > overlay.dst.size() && !overlay.dst.ends_with('/') && curr_path[overlay.dst.size()] != '/'))
                continue;

            const auto candidate = overlay.src + curr_path.substr(overlay.dst.size());
            if (overlay.type != SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT) {
                curr_path = candidate;
            } else if (!for_write) {
                auto candidates = self(self, candidate, overlay_idx);
                auto fallback = self(self, curr_path, overlay_idx);
                for (const auto &path : candidates) {
                    const auto host_path = find_host_path(io, expand_unresolved_path(io, path.c_str(), vita_fs_path));
                    if (fs::exists(host_path))
                        continue;
                    auto parent = host_path.parent_path();
                    while (!parent.empty() && parent != parent.root_path()) {
                        if (fs::is_regular_file(find_host_path(io, parent)))
                            return std::vector<std::string>{ path };
                        parent = parent.parent_path();
                    }
                }
                std::erase_if(candidates, [&](const std::string &path) {
                    return !fs::exists(find_host_path(io, expand_unresolved_path(io, path.c_str(), vita_fs_path)));
                });
                candidates.insert(candidates.end(), fallback.begin(), fallback.end());
                return candidates;
            }
        }
        return { std::move(curr_path) };
    };

    return resolve_from(resolve_from, standardize_path(input), 0);
}

std::string resolve_path(IOState &io, const char *input, const fs::path &vita_fs_path, const bool for_write, const SceUInt32 min_order, const SceUInt32 max_order) {
    return resolve_overlay_paths(io, input, vita_fs_path, for_write, min_order, max_order).front();
}
