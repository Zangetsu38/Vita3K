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

/**
 * @file pup.cpp
 * @brief PlayStation Update Package (`.pup`) handling
 *
 * On the PlayStation Vita and just like any other PlayStation console, PUP packages
 * contain firmware updates
 */

#include <openssl/evp.h>
#include <packages/exfat.h>
#include <packages/sce_types.h>
#include <util/fs.h>

#include <algorithm>
#include <fstream>
#include <map>

// Credits to TeamMolecule for their original work on this https://github.com/TeamMolecule/sceutils

static const std::map<int, std::string> PUP_TYPES = {
    { 0x100, "version.txt" },
    { 0x101, "license.xml" },
    { 0x200, "psp2swu.self" },
    { 0x204, "cui_setupper.self" },
    { 0x400, "package_scewm.wm" },
    { 0x401, "package_sceas.as" },
    { 0x2005, "UpdaterES1.CpUp" },
    { 0x2006, "UpdaterES2.CpUp" },
};

static const char *FSTYPE[] = {
    "unknown0",
    "os0",
    "unknown2",
    "unknown3",
    "vs0_chmod",
    "unknown5",
    "unknown6",
    "unknown7",
    "pervasive8",
    "boot_slb2",
    "vs0",
    "devkit_cp",
    "motionC",
    "bbmc",
    "unknownE",
    "motionF",
    "touch10",
    "touch11",
    "syscon12",
    "syscon13",
    "pervasive14",
    "unknown15",
    "vs0_tarpatch",
    "sa0",
    "pd0",
    "pervasive19",
    "unknown1A",
    "psp_emulist",
};

static std::string make_filename(unsigned char *hdr, int64_t filetype) {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t flags = 0;
    uint32_t moffs = 0;
    uint64_t metaoffs = 0;
    memcpy(&magic, &hdr[0], 4);
    memcpy(&version, &hdr[4], 4);
    memcpy(&flags, &hdr[8], 4);
    memcpy(&moffs, &hdr[12], 4);
    memcpy(&metaoffs, &hdr[16], 8);

    if (magic == SCE_MAGIC && version == 3 && flags == 0x30040) {
        std::vector meta = std::vector<unsigned char>(&hdr[0] + metaoffs, &hdr[0] + (HEADER_LENGTH - metaoffs));
        unsigned char t = 0;
        memcpy(&t, &meta[4], 1);

        static int typecount = 0;

        if (t < 0x1C) { // 0x1C is the file separator
            std::string name = fmt::format("{}-{:0>2}.pkg", FSTYPE[t], typecount);
            typecount++;
            return name;
        }
    }
    return fmt::format("unknown-0x{:X}.pkg", filetype);
}

static void extract_pup_files(const fs::path &pup, const fs::path &output) {
    constexpr int SCEUF_HEADER_SIZE = 0x80;
    constexpr int SCEUF_FILEREC_SIZE = 0x20;
    FILE *infile = FOPEN(pup.c_str(), "rb");
    if (!infile) {
        LOG_CRITICAL("Failed to load pup file in path: {}", fs_utils::path_to_utf8(pup));
        return;
    }

    try {
        fseek(infile, 0, SEEK_END);
        const auto pup_size_raw = ftell(infile);
        if (pup_size_raw < SCEUF_HEADER_SIZE) {
            LOG_ERROR("PUP file is too small: {}", fs_utils::path_to_utf8(pup));
            fclose(infile);
            return;
        }
        const uint64_t pup_size = static_cast<uint64_t>(pup_size_raw);
        fseek(infile, 0, SEEK_SET);

        char header[SCEUF_HEADER_SIZE];
        if (fread(header, SCEUF_HEADER_SIZE, 1, infile) != 1) {
            LOG_ERROR("Failed to read PUP header: {}", fs_utils::path_to_utf8(pup));
            fclose(infile);
            return;
        }

        if (strncmp(header, "SCEUF", 5) != 0) {
            LOG_ERROR("Invalid PUP");
            fclose(infile);
            return;
        }

        uint32_t cnt = 0;
        uint32_t pup_version = 0;
        uint32_t firmware_version = 0;
        uint32_t build_number = 0;
        memcpy(&cnt, &header[0x18], 4);
        memcpy(&pup_version, &header[8], 4);
        memcpy(&firmware_version, &header[0x10], 4);
        memcpy(&build_number, &header[0x14], 4);

        LOG_INFO("PUP Version: 0x{:0}", pup_version);
        LOG_INFO("Firmware Version: 0x{:X}", firmware_version);
        LOG_INFO("Build Number: {:0}", build_number);
        LOG_INFO("Number Of Files: {}", cnt);

        const uint64_t records_end = SCEUF_HEADER_SIZE + (static_cast<uint64_t>(cnt) * SCEUF_FILEREC_SIZE);
        if (records_end > pup_size) {
            LOG_ERROR("Invalid PUP record table: cnt={}, records_end={}, file size={}", cnt, records_end, pup_size);
            fclose(infile);
            return;
        }

        for (uint32_t x = 0; x < cnt; x++) {
            fseek(infile, SCEUF_HEADER_SIZE + x * SCEUF_FILEREC_SIZE, SEEK_SET);
            char rec[SCEUF_FILEREC_SIZE];
            if (fread(rec, SCEUF_FILEREC_SIZE, 1, infile) != 1) {
                LOG_ERROR("Failed to read PUP file record {}", x);
                break;
            }

            uint64_t filetype = 0;
            uint64_t offset = 0;
            uint64_t length = 0;
            uint64_t flags = 0;

            memcpy(&filetype, &rec[0], 8);
            memcpy(&offset, &rec[8], 8);
            memcpy(&length, &rec[16], 8);
            memcpy(&flags, &rec[24], 8);

            if ((offset > pup_size) || (length > (pup_size - offset))) {
                LOG_ERROR("Invalid PUP entry {}: offset={}, length={}, file size={}", x, offset, length, pup_size);
                continue;
            }

            std::string filename = "";
            if (PUP_TYPES.contains(filetype)) {
                filename = PUP_TYPES.at(filetype);
            } else {
                if (HEADER_LENGTH <= length) {
                    fseek(infile, static_cast<long>(offset), SEEK_SET);
                    char hdr[HEADER_LENGTH];
                    if (fread(hdr, HEADER_LENGTH, 1, infile) == 1)
                        filename = make_filename(reinterpret_cast<unsigned char *>(hdr), filetype);
                }

                if (filename.empty())
                    filename = fmt::format("unknown-0x{:X}.pkg", filetype);
            }

            fs::ofstream outfile(output / filename, std::ios::binary);
            if (!outfile.is_open()) {
                LOG_ERROR("Failed to create extracted PUP file: {}", fs_utils::path_to_utf8(output / filename));
                continue;
            }

            fseek(infile, static_cast<long>(offset), SEEK_SET);
            std::vector<char> buffer(1024 * 1024);
            uint64_t remaining = length;
            uint64_t copied = 0;
            uint32_t chunk_index = 0;
            while (remaining > 0) {
                ++chunk_index;
                const auto chunk_size = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                const auto read_count = fread(buffer.data(), 1, chunk_size, infile);
                if (read_count != chunk_size) {
                    LOG_ERROR("Failed to extract PUP entry {} ('{}'): expected {} bytes, got {}", x, filename, chunk_size, read_count);
                    break;
                }

                outfile.write(buffer.data(), static_cast<std::streamsize>(read_count));
                if (!outfile) {
                    LOG_ERROR("Failed writing extracted PUP entry {} to {}", x, fs_utils::path_to_utf8(output / filename));
                    break;
                }

                remaining -= read_count;
            }
            outfile.close();
        }
    } catch (const fs::filesystem_error &e) {
        LOG_ERROR("extract_pup_files filesystem exception: {}", e.what());
    } catch (const std::exception &e) {
        LOG_ERROR("extract_pup_files exception: {}", e.what());
    }

    fclose(infile);
}

static void decrypt_segments(std::ifstream &infile, const fs::path &outdir, const fs::path &filename, KeyStore &SCE_KEYS) {
    char sceheaderbuffer[SceHeader::Size];
    infile.read(sceheaderbuffer, SceHeader::Size);
    const SceHeader sce_hdr = SceHeader(sceheaderbuffer);

    const auto sysver = std::get<0>(get_key_type(infile, sce_hdr));
    const SelfType selftype = std::get<1>(get_key_type(infile, sce_hdr));

    EVP_CIPHER_CTX *cipher_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, "AES-128-CTR", nullptr);
    int dec_len = 0;

    // Reset the offset to the beginning of the file
    infile.seekg(0, std::ios::beg);

    // Read the entire file into a buffer and get the segments
    const auto input = std::vector<uint8_t>(std::istreambuf_iterator<char>(infile), std::istreambuf_iterator<char>());
    const auto scesegs = get_segments(input.data(), sce_hdr, SCE_KEYS, sysver, selftype);
    for (const auto &sceseg : scesegs) {
        fs::ofstream outfile(outdir / fs_utils::path_concat(filename, ".seg02"), std::ios::binary);
        infile.seekg(sceseg.offset);
        std::vector<unsigned char> encrypted_data(sceseg.size);
        infile.read((char *)encrypted_data.data(), sceseg.size);

        std::vector<unsigned char> decrypted_data(sceseg.size);
        EVP_DecryptInit_ex(cipher_ctx, cipher, nullptr, reinterpret_cast<const unsigned char *>(sceseg.key.c_str()), reinterpret_cast<const unsigned char *>(sceseg.iv.c_str()));
        EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);
        EVP_DecryptUpdate(cipher_ctx, decrypted_data.data(), &dec_len, encrypted_data.data(), sceseg.size);
        EVP_DecryptFinal_ex(cipher_ctx, decrypted_data.data() + dec_len, &dec_len);

        if (sceseg.compressed) {
            const std::string decompressed_data = decompress_segments(decrypted_data, sceseg.size);
            outfile.write(decompressed_data.c_str(), decompressed_data.size());
        } else {
            outfile.write((char *)decrypted_data.data(), sceseg.size);
        }
        outfile.close();
    }

    EVP_CIPHER_CTX_free(cipher_ctx);
    EVP_CIPHER_free(cipher);
}

static void join_files(const fs::path &path, const std::string &filename, const fs::path &output) {
    std::vector<fs::path> files;

    for (auto &p : fs::directory_iterator(path)) {
        if (p.path().filename().string().substr(0, 4) == filename) {
            files.push_back(p.path());
        }
    }

    std::sort(files.begin(), files.end());

    fs::ofstream fileout(output, std::ios::binary);
    for (const auto &file : files) {
        std::vector<char> buffer(0);
        fs_utils::read_data(file, buffer);
        fileout.write(buffer.data(), buffer.size());
        fs::remove(file);
    }
    fileout.close();
}

static void decrypt_pup_packages(const fs::path &src, const fs::path &dest, KeyStore &SCE_KEYS) {
    std::vector<fs::path> pkgfiles;

    for (const auto &p : fs::directory_iterator(src)) {
        if (p.path().filename().extension().string() == ".pkg")
            pkgfiles.push_back(p.path().filename());
    }

    for (const auto &filename : pkgfiles) {
        const fs::path &filepath = src / filename;
        fs::ifstream infile(filepath, std::ios::binary);
        decrypt_segments(infile, dest, filename, SCE_KEYS);
        infile.close();
    }

    join_files(dest, "os0-", dest / "os0.img");
    join_files(dest, "pd0-", dest / "pd0.img");
    join_files(dest, "vs0-", dest / "vs0.img");
    join_files(dest, "sa0-", dest / "sa0.img");
}

std::string install_pup(const fs::path &pref_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback) {
    fs::path pup_dec_root = pref_path / "PUP_DEC";
    if (fs::exists(pup_dec_root)) {
        LOG_WARN("Path already exists, deleting it and reinstalling");
        fs::remove_all(pup_dec_root);
    }

    const auto update_progress = [&](const uint32_t progress) {
        if (progress_callback)
            progress_callback(progress);
    };

    fs::create_directory(pup_dec_root);
    const auto pup_dest = pup_dec_root / "PUP";
    fs::create_directory(pup_dest);
    update_progress(10);

    extract_pup_files(pup_path, pup_dest);
    update_progress(20);

    const auto pup_dec = pup_dec_root / "PUP_dec";
    fs::create_directory(pup_dec);

    KeyStore SCE_KEYS;
    register_keys(SCE_KEYS, 0);

    update_progress(30);
    decrypt_pup_packages(pup_dest, pup_dec, SCE_KEYS);

    update_progress(70);
    const auto has_non_empty_file = [](const fs::path &path) {
        return fs::exists(path) && (fs::file_size(path) > 0);
    };

    if (has_non_empty_file(pup_dec / "os0.img"))
        extract_fat(pup_dec, "os0.img", pref_path);
    if (has_non_empty_file(pup_dec / "pd0.img"))
        exfat::extract_exfat(pup_dec, "pd0.img", pref_path);
    if (has_non_empty_file(pup_dec / "sa0.img"))
        extract_fat(pup_dec, "sa0.img", pref_path);
    if (has_non_empty_file(pup_dec / "vs0.img"))
        extract_fat(pup_dec, "vs0.img", pref_path);
    update_progress(100);

    // get firmware version
    std::string fw_version;
    fs::ifstream versionFile(pup_dest / "version.txt");
    if (versionFile.is_open()) {
        std::getline(versionFile, fw_version);
        versionFile.close();
    } else
        LOG_WARN("Firmware Version file not found!");

    fs::remove_all(pup_dec_root);

    return fw_version;
}
