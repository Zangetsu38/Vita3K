// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <pfsmgr/functions.h>
#include <pfsmgr/mounts.h>
#include <util/fs.h>

#include <limits>
#include <stdexcept>
#include <system_error>

namespace pfsmgr {

bool has_databases(const std::filesystem::path &root) {
    bool found = false;
    for (const auto *name : { "files.db", "unicv.db" }) {
        std::error_code error;
        const bool exists = std::filesystem::exists(root / "sce_pfs" / name, error);
        if (error)
            throw std::filesystem::filesystem_error("Cannot inspect PFS database", root / "sce_pfs" / name, error);
        found |= exists;
    }
    return found;
}

void Mounts::mount_savedata_directory(const std::string &name, const std::filesystem::path &base, const std::optional<AesKey> &key) {
    if (name.empty())
        throw std::invalid_argument("PFS: empty mount name");
    const auto source = std::filesystem::absolute(base);
    const auto canonical_source = std::filesystem::canonical(source);
    auto layer = key ? std::make_shared<Layer>(source, *key) : std::make_shared<Layer>(source);
    std::lock_guard lock(mutex_);
    if (next_id_ == no_mount)
        throw std::runtime_error("PFS: mount identifiers exhausted");
    const auto id = next_id_++;
    layers_.emplace(id, Entry{ std::move(layer), MountScope::Application, key });
    try {
        MountedDirectory directory{ source, canonical_source, {}, {}, { id, no_mount }, Mode::Mode5,
            Mode::Mode5, MountScope::Application, std::make_shared<int>(0) };
        if (!directories_.emplace(name, std::move(directory)).second)
            throw std::runtime_error("PFS: mount name already exists: " + name);
    } catch (...) {
        layers_.erase(id);
        throw;
    }
}

void Mounts::refresh_savedata_directory(const std::string &name) {
    std::lock_guard lock(mutex_);
    const auto directory = directories_.find(name);
    if (directory == directories_.end() || directory->second.mode != Mode::Mode5)
        throw std::runtime_error("PFS: missing savedata mount");
    const auto entry = layers_.find(directory->second.layers.base);
    if (entry == layers_.end())
        throw std::runtime_error("PFS: missing savedata layer");
    entry->second.layer = entry->second.savedata_key
        ? std::make_shared<Layer>(directory->second.source, *entry->second.savedata_key)
        : std::make_shared<Layer>(directory->second.source);
}

MountedLayers Mounts::mount(const std::filesystem::path &base, const std::filesystem::path &patch,
    const std::filesystem::path &license_directory, const std::string &title_id, Mode mode, const std::string &content_id, MountScope scope) {
    auto base_layer = open_layer(base, license_directory, title_id, mode, content_id);
    auto patch_layer = patch.empty() ? nullptr : open_layer(patch, license_directory, title_id, mode, content_id);

    std::lock_guard lock(mutex_);
    const auto count = static_cast<MountId>(!!base_layer) + static_cast<MountId>(!!patch_layer);
    if (count && (next_id_ == no_mount || count - 1 > std::numeric_limits<MountId>::max() - next_id_))
        throw std::runtime_error("PFS: mount identifiers exhausted");
    MountedLayers result;
    if (base_layer) {
        result.base = next_id_++;
        layers_.emplace(result.base, Entry{ std::move(base_layer), scope });
    }

    try {
        if (patch_layer) {
            result.patch = next_id_++;
            layers_.emplace(result.patch, Entry{ std::move(patch_layer), scope });
        }
    } catch (...) {
        layers_.erase(result.base);
        throw;
    }
    return result;
}

void Mounts::mount_directory(const std::string &name, const std::filesystem::path &base,
    const std::filesystem::path &patch, const std::filesystem::path &license_directory,
    const std::string &title_id, Mode mode, const std::string &content_id, MountScope scope, bool track_open_handles) {
    if (name.empty())
        throw std::invalid_argument("PFS: empty mount name");
    if (mode != Mode::Mode20)
        validate_read_mode(mode);
    const auto source = std::filesystem::absolute(base);
    const auto canonical_source = std::filesystem::canonical(source);
    const auto patch_path = patch.empty() ? std::filesystem::path{} : std::filesystem::absolute(patch);
    const auto canonical_patch = patch.empty() ? std::filesystem::path{} : std::filesystem::canonical(patch_path);
    const auto layers = mode == Mode::Mode20 ? MountedLayers{} : mount(source, patch_path, license_directory, title_id, mode, content_id, scope);
    try {
        std::lock_guard lock(mutex_);
        if ((layers.base != no_mount && !layers_.contains(layers.base)) || (layers.patch != no_mount && !layers_.contains(layers.patch)))
            throw std::runtime_error("PFS: mount cleared before publication");
        const auto base_mode = mode == Mode::GameData && layers.base == no_mount ? Mode::Mode20 : mode;
        const auto patch_mode = mode == Mode::GameData && layers.patch == no_mount ? Mode::Mode20 : mode;
        MountedDirectory directory{ source, canonical_source, patch_path, canonical_patch, layers, base_mode, patch_mode, scope,
            track_open_handles ? std::make_shared<int>(0) : std::shared_ptr<void>{} };
        if (!directories_.emplace(name, std::move(directory)).second)
            throw std::runtime_error("PFS: mount name already exists: " + name);
    } catch (...) {
        unmount(layers);
        throw;
    }
}

std::optional<MountedDirectory> Mounts::get_directory(const std::string &name) const {
    std::lock_guard lock(mutex_);
    const auto it = directories_.find(name);
    if (it == directories_.end())
        return std::nullopt;
    return it->second;
}

bool Mounts::unmount_directory(const std::string &name, bool require_idle) {
    std::lock_guard lock(mutex_);
    const auto it = directories_.find(name);
    if (it == directories_.end() || (require_idle && it->second.lease && it->second.lease.use_count() > 1))
        return false;
    layers_.erase(it->second.layers.base);
    layers_.erase(it->second.layers.patch);
    directories_.erase(it);
    return true;
}

bool Mounts::split_patch(const std::string &base_name, const std::string &patch_name) {
    std::lock_guard lock(mutex_);
    const auto base = directories_.find(base_name);
    if (base == directories_.end() || base->second.patch.empty() || directories_.contains(patch_name))
        return false;
    const auto &entry = base->second;
    MountedDirectory patch{ entry.patch, entry.canonical_patch, {}, {}, { entry.layers.patch, no_mount }, entry.patch_mode, entry.patch_mode, entry.scope, entry.lease };
    directories_.emplace(patch_name, std::move(patch));
    // Insertion can rehash the table, so reacquire the base entry.
    auto &updated_base = directories_.at(base_name);
    updated_base.patch.clear();
    updated_base.canonical_patch.clear();
    updated_base.layers.patch = no_mount;
    return true;
}

std::shared_ptr<const Layer> Mounts::get(MountId id) const {
    std::lock_guard lock(mutex_);
    const auto it = layers_.find(id);
    return it == layers_.end() ? nullptr : it->second.layer;
}

void Mounts::unmount(MountedLayers layers) {
    std::lock_guard lock(mutex_);
    layers_.erase(layers.base);
    layers_.erase(layers.patch);
}

void Mounts::clear(MountScope scope) {
    std::lock_guard lock(mutex_);
    std::erase_if(directories_, [scope](const auto &entry) { return entry.second.scope == scope; });
    std::erase_if(layers_, [scope](const auto &entry) { return entry.second.scope == scope; });
}

size_t Mounts::size() const {
    std::lock_guard lock(mutex_);
    return layers_.size();
}

} // namespace pfsmgr