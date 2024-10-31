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

#include <lang/functions.h>
#include <lang/state.h>

#include <config/state.h>
#include <dialog/state.h>
#include <gui/state.h>
#include <ime/state.h>
#include <util/fs.h>
#include <util/vector_utils.h>

#include <cctype>
#include <pugixml.hpp>

namespace lang {
void set_lang_string() {
    std::vector<std::pair<std::string, std::string>> mapping = {
        { "b036d3df", "activities_of" },
        { "f040449e", "anyone" },
        { "aee0b551", "no_one" },
        { "5b08c8fb", "cannot_display" },
        { "48e362b4", "friends_of_friends" },
        { "73f2af03", "friends_only" },
        { "10b08b96", "share_range" }
    };

    const std::string child_name = "contacts_pa";
    const std::string child_open_tag = "<" + child_name + ">";
    const std::string child_close_tag = "</" + child_name + ">";

    const fs::path system_lang_path{ "I:/Git/Vita3K/shadow/lang/system" };
    // const fs::path ps_common_lang_path{ "D:/Emulateurs/Vita3K/Tools/rco_dump/common_resource/xmls" };
    const fs::path ps_lang_path{ "D:/Emulateurs/Vita3K/Tools/rco_dump/contacts_pa_plugin/xmls" };

    std::map<std::string, std::map<std::string, std::string>> psn_values_by_lang;
    std::vector<std::string> lang;

    for (auto &entry : fs::directory_iterator(ps_lang_path)) {
        if (entry.path().extension() != ".xml") {
            LOG_ERROR("Skipping non-XML file in PSN common lang directory: '{}'", entry.path().string());
            continue;
        }

        std::string raw;
        {
            fs::ifstream f(entry.path(), std::ios::binary);
            raw.assign((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
        }

        const auto fix_placeholders = [](std::string &raw) {
            //
            // ÉTAPE 1 : remplacer %1, %2, %3 → {}
            //
            {
                const std::vector<std::string> bare = { "%1", "%2", "%3" };
                for (const auto &p : bare) {
                    size_t pos = 0;
                    while ((pos = raw.find(p, pos)) != std::string::npos) {
                        raw.replace(pos, p.size(), "{}");
                        pos += 2;
                    }
                }
            }

            {
                const std::string src_pattern = "src=\"";
                const auto is_attr_end_quote = [&](size_t quote_pos) {
                    size_t next = quote_pos + 1;
                    while (next < raw.size() && std::isspace(static_cast<unsigned char>(raw[next])))
                        ++next;

                    if (next >= raw.size())
                        return true;

                    if ((raw[next] == '/') || (raw[next] == '>'))
                        return true;

                    const auto is_name_start_char = [](unsigned char ch) {
                        return std::isalpha(ch) || (ch == '_') || (ch == ':');
                    };

                    const auto is_name_char = [&](unsigned char ch) {
                        return is_name_start_char(ch) || std::isdigit(ch) || (ch == '-') || (ch == '.');
                    };

                    if (!is_name_start_char(static_cast<unsigned char>(raw[next])))
                        return false;

                    size_t name_end = next + 1;
                    while (name_end < raw.size() && is_name_char(static_cast<unsigned char>(raw[name_end])))
                        ++name_end;

                    return (name_end < raw.size()) && (raw[name_end] == '=');
                };

                size_t pos = 0;
                while ((pos = raw.find(src_pattern, pos)) != std::string::npos) {
                    const size_t value_start = pos + src_pattern.size();
                    size_t value_end = value_start;

                    while ((value_end = raw.find('"', value_end)) != std::string::npos) {
                        if (is_attr_end_quote(value_end))
                            break;

                        ++value_end;
                    }

                    if (value_end == std::string::npos)
                        break;

                    std::string src_value = raw.substr(value_start, value_end - value_start);
                    size_t quoted_placeholder_pos = 0;
                    while ((quoted_placeholder_pos = src_value.find("\"{}\"", quoted_placeholder_pos)) != std::string::npos) {
                        src_value.replace(quoted_placeholder_pos, 4, "&quot;{}&quot;");
                        quoted_placeholder_pos += 13;
                    }

                    raw.replace(value_start, value_end - value_start, src_value);
                    pos = value_start + src_value.size();
                }
            }
        };

        fix_placeholders(raw);

        pugi::xml_document doc;
        const auto result = doc.load_string(raw.c_str());
        if (!result) {
            LOG_ERROR("Error parsing lang file xml: {}", entry.path().string());
            LOG_DEBUG("error: {} position: {}", result.description(), result.offset);
            constexpr ptrdiff_t context_window = 20;
            ptrdiff_t offset = static_cast<ptrdiff_t>(result.offset);
            if (offset >= 0 && offset < static_cast<ptrdiff_t>(raw.size())) {
                ptrdiff_t start = std::max<ptrdiff_t>(0, offset - context_window);
                ptrdiff_t end = std::min<ptrdiff_t>(raw.size(), offset + context_window);

                ptrdiff_t error_in_context = offset - start;

                std::string error_context(reinterpret_cast<const char *>(raw.data() + start), end - start);
                LOG_DEBUG("Error preview: [{}|{}]", error_context.substr(0, error_in_context), error_context.substr(error_in_context));
            }
            return;
        }

        const auto lang_file = entry.path().filename().string();
        LOG_DEBUG("Processing PSN common lang file: '{}'", lang_file);

        auto &psn_values = psn_values_by_lang[lang_file];

        auto stringset_node = doc.child("stringset");
        if (!stringset_node) {
            LOG_ERROR("No 'stringset' node found in PSN common lang file '{}'", lang_file);
            stringset_node = doc.first_child();
            if (!stringset_node) {
                LOG_ERROR("No root node found in PSN common lang file '{}'", lang_file);
                continue;
            }
        }

        lang.push_back(lang_file);

        for (const auto &[hash, tag] : mapping) {
            const auto string_node = stringset_node.find_child_by_attribute("string", "id", hash.c_str());
            if (string_node) {
                const auto src = string_node.attribute("src").as_string();
                psn_values[tag] = src;
                LOG_DEBUG("Loaded PSN value for tag '{}': '{}'", tag, src);
            } else
                LOG_ERROR("Tag '{}' with hash '{}' not found in PSN common lang file '{}'", tag, hash, lang_file);
        }
    }

    for (const auto &language : lang) {
        const auto dest_path = system_lang_path / language;
        auto values_it = psn_values_by_lang.find(language);
        if (values_it == psn_values_by_lang.end()) {
            LOG_WARN("No PSN values found for system lang file '{}'", dest_path.string());
            continue;
        }

        const auto &psn_values = values_it->second;

        std::string raw;
        {
            fs::ifstream f(dest_path, std::ios::binary);
            if (!f) {
                LOG_ERROR("Failed to load system lang file: '{}'", dest_path.string());
                continue;
            }

            raw.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }

        const size_t lang_start = raw.find("<lang>");
        if (lang_start == std::string::npos) {
            LOG_ERROR("No 'lang' node found in system lang file: '{}'", dest_path.string());
            continue;
        }

        const size_t lang_content_start = lang_start + 6;
        const size_t lang_close_pos = raw.find("</lang>", lang_content_start);
        if (lang_close_pos == std::string::npos) {
            LOG_ERROR("No closing 'lang' node found in system lang file: '{}'", dest_path.string());
            continue;
        }

        const std::string newline = (raw.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
        const auto trim = [](const std::string &text) {
            const size_t start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return std::string{};

            const size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(start, end - start + 1);
        };

        const auto line_indent_at = [&](size_t pos) {
            size_t line_start = raw.rfind('\n', pos);
            line_start = (line_start == std::string::npos) ? 0 : (line_start + 1);

            if ((line_start < raw.size()) && (raw[line_start] == '\r'))
                ++line_start;

            size_t line_end = line_start;
            while (line_end < raw.size() && ((raw[line_end] == ' ') || (raw[line_end] == '\t')))
                ++line_end;

            return raw.substr(line_start, line_end - line_start);
        };

        std::string child_indent;
        {
            size_t scan = lang_content_start;
            if (raw.compare(scan, 2, "\r\n") == 0)
                scan += 2;
            else if ((scan < raw.size()) && (raw[scan] == '\n'))
                ++scan;

            while (scan < lang_close_pos) {
                size_t line_end = raw.find('\n', scan);
                if ((line_end == std::string::npos) || (line_end > lang_close_pos))
                    line_end = lang_close_pos;

                std::string line = raw.substr(scan, line_end - scan);
                const size_t first_non_ws = line.find_first_not_of(" \t\r");
                if ((first_non_ws != std::string::npos) && (line[first_non_ws] == '<') && (line.compare(first_non_ws, 2, "</") != 0)) {
                    child_indent = line.substr(0, first_non_ws);
                    break;
                }

                if (line_end == lang_close_pos)
                    break;

                scan = line_end + 1;
            }
        }

        if (child_indent.empty())
            child_indent = line_indent_at(lang_close_pos) + "\t";

        const auto compare_tag_names = [](const std::string &lhs, const std::string &rhs) {
            const auto min_size = std::min(lhs.size(), rhs.size());
            for (size_t i = 0; i < min_size; ++i) {
                const auto lhs_char = static_cast<unsigned char>(lhs[i]);
                const auto rhs_char = static_cast<unsigned char>(rhs[i]);
                const auto lhs_lower = static_cast<char>(std::tolower(lhs_char));
                const auto rhs_lower = static_cast<char>(std::tolower(rhs_char));
                if (lhs_lower < rhs_lower)
                    return -1;
                if (lhs_lower > rhs_lower)
                    return 1;
            }

            if (lhs.size() < rhs.size())
                return -1;
            if (lhs.size() > rhs.size())
                return 1;

            return 0;
        };

        bool modified = false;

        const auto get_lang_close_pos = [&]() {
            return raw.find("</lang>", lang_content_start);
        };

        const auto get_lang_close_line_start = [&]() {
            const size_t close_pos = get_lang_close_pos();
            if (close_pos == std::string::npos)
                return std::string::npos;

            const size_t line_start = raw.rfind('\n', close_pos);
            return (line_start == std::string::npos) ? 0 : (line_start + 1);
        };

        const auto get_child_bounds = [&]() {
            const size_t current_lang_close_pos = get_lang_close_pos();
            const size_t current_child_start = raw.find(child_open_tag, lang_content_start);
            if ((current_child_start == std::string::npos) || (current_child_start >= current_lang_close_pos))
                return std::pair<std::size_t, std::size_t>{ std::string::npos, std::string::npos };

            const size_t current_child_content_start = current_child_start + child_open_tag.size();
            const size_t current_child_close_pos = raw.find(child_close_tag, current_child_content_start);
            if ((current_child_close_pos == std::string::npos) || (current_child_close_pos >= current_lang_close_pos))
                return std::pair<std::size_t, std::size_t>{ std::string::npos, std::string::npos };

            return std::pair<std::size_t, std::size_t>{ current_child_content_start, current_child_close_pos };
        };

        const auto get_child_close_line_start = [&]() {
            const auto [_, close_pos] = get_child_bounds();
            if (close_pos == std::string::npos)
                return std::string::npos;

            const size_t line_start = raw.rfind('\n', close_pos);
            return (line_start == std::string::npos) ? 0 : (line_start + 1);
        };

        const auto apply_child_entries = [&](const std::vector<std::pair<std::string, std::string>> &entries) {
            std::string missing_block;

            const auto [child_content_start, child_close_pos] = get_child_bounds();
            if ((child_content_start == std::string::npos) || (child_close_pos == std::string::npos)) {
                LOG_ERROR("No '{}' node found in system lang file: '{}'", child_name, dest_path.string());
                return;
            }

            std::string entry_indent;
            {
                size_t scan = child_content_start;
                if (raw.compare(scan, 2, "\r\n") == 0)
                    scan += 2;
                else if ((scan < raw.size()) && (raw[scan] == '\n'))
                    ++scan;

                while (scan < child_close_pos) {
                    size_t line_end = raw.find('\n', scan);
                    if ((line_end == std::string::npos) || (line_end > child_close_pos))
                        line_end = child_close_pos;

                    std::string line = raw.substr(scan, line_end - scan);
                    const size_t first_non_ws = line.find_first_not_of(" \t\r");
                    if ((first_non_ws != std::string::npos) && (line[first_non_ws] == '<') && (line.compare(first_non_ws, 2, "</") != 0)) {
                        entry_indent = line.substr(0, first_non_ws);
                        break;
                    }

                    if (line_end == child_close_pos)
                        break;

                    scan = line_end + 1;
                }
            }

            if (entry_indent.empty())
                entry_indent = child_indent + "\t";

            for (const auto &[_, tag] : entries) {
                const auto [current_child_content_start, current_child_close_pos] = get_child_bounds();
                if (current_child_close_pos == std::string::npos) {
                    LOG_ERROR("No closing '{}' node found in system lang file: '{}'", child_name, dest_path.string());
                    return;
                }

                auto it = psn_values.find(tag);
                if (it == psn_values.end()) {
                    LOG_WARN("PSN value for tag '{}' not found in system lang file '{}'", tag, dest_path.string());
                    continue;
                }

                const std::string &value = it->second;
                LOG_DEBUG("Setting tag '{}' to value '{}' in system lang file '{}'", tag, value, dest_path.string());

                const std::string open_tag = "<" + tag + ">";
                const std::string close_tag = "</" + tag + ">";
                const std::string self_closing_tag = "<" + tag + " />";
                const std::string self_closing_tag_no_space = "<" + tag + "/>";

                size_t node_pos = raw.find(open_tag, current_child_content_start);
                if ((node_pos != std::string::npos) && (node_pos < current_child_close_pos)) {
                    const size_t value_start = node_pos + open_tag.size();
                    const size_t close_pos = raw.find(close_tag, value_start);
                    if ((close_pos != std::string::npos) && (close_pos < current_child_close_pos)) {
                        if (trim(raw.substr(value_start, close_pos - value_start)).empty()) {
                            raw.replace(node_pos, (close_pos + close_tag.size()) - node_pos, open_tag + value + close_tag);
                            modified = true;
                        } else {
                            LOG_DEBUG("Tag '{}' already has a value in system lang file '{}', skipping update", tag, dest_path.string());
                        }

                        continue;
                    }
                }

                node_pos = raw.find(self_closing_tag, current_child_content_start);
                size_t self_closing_len = self_closing_tag.size();
                if ((node_pos == std::string::npos) || (node_pos >= current_child_close_pos)) {
                    node_pos = raw.find(self_closing_tag_no_space, current_child_content_start);
                    self_closing_len = self_closing_tag_no_space.size();
                }

                if ((node_pos != std::string::npos) && (node_pos < current_child_close_pos)) {
                    raw.replace(node_pos, self_closing_len, open_tag + value + close_tag);
                    modified = true;
                    continue;
                }

                missing_block += entry_indent + open_tag + value + close_tag + newline;
                modified = true;
            }

            if (missing_block.empty())
                return;

            const size_t current_child_close_line_start = get_child_close_line_start();
            if (current_child_close_line_start == std::string::npos) {
                LOG_ERROR("No closing '{}' node found in system lang file: '{}'", child_name, dest_path.string());
                return;
            }

            raw.insert(current_child_close_line_start, missing_block);
        };

        const auto child_bounds = get_child_bounds();
        if (child_bounds.first != std::string::npos) {
            apply_child_entries(mapping);
        } else {
            std::vector<std::pair<std::size_t, std::string>> top_level_children;

            size_t scan = lang_content_start;
            if (raw.compare(scan, 2, "\r\n") == 0)
                scan += 2;
            else if ((scan < raw.size()) && (raw[scan] == '\n'))
                ++scan;

            while (scan < lang_close_pos) {
                size_t line_end = raw.find('\n', scan);
                if ((line_end == std::string::npos) || (line_end > lang_close_pos))
                    line_end = lang_close_pos;

                std::string line = raw.substr(scan, line_end - scan);
                const size_t first_non_ws = line.find_first_not_of(" \t\r");
                if ((first_non_ws != std::string::npos) && (line[first_non_ws] == '<') && (line.compare(first_non_ws, 2, "</") != 0)) {
                    const std::string line_indent = line.substr(0, first_non_ws);
                    if (line_indent == child_indent) {
                        size_t name_end = first_non_ws + 1;
                        while ((name_end < line.size()) && !std::isspace(static_cast<unsigned char>(line[name_end])) && (line[name_end] != '>') && (line[name_end] != '/'))
                            ++name_end;

                        top_level_children.emplace_back(scan, line.substr(first_non_ws + 1, name_end - (first_non_ws + 1)));
                    }
                }

                if (line_end == lang_close_pos)
                    break;

                scan = line_end + 1;
            }

            std::string child_block;
            const std::string entry_indent = child_indent + "\t";
            for (const auto &[_, tag] : mapping) {
                auto it = psn_values.find(tag);
                if (it == psn_values.end()) {
                    LOG_WARN("PSN value for tag '{}' not found in system lang file '{}'", tag, dest_path.string());
                    continue;
                }

                child_block += entry_indent + "<" + tag + ">" + it->second + "</" + tag + ">" + newline;
            }

            if (!child_block.empty()) {
                child_block = child_indent + child_open_tag + newline + child_block + child_indent + child_close_tag + newline;

                size_t insert_pos = get_lang_close_line_start();
                std::string insert_before_name;
                for (const auto &[candidate_pos, candidate_name] : top_level_children) {
                    if (compare_tag_names(candidate_name, child_name) <= 0)
                        continue;

                    if (insert_before_name.empty() || (compare_tag_names(candidate_name, insert_before_name) < 0)) {
                        insert_pos = candidate_pos;
                        insert_before_name = candidate_name;
                    }
                }

                if (!top_level_children.empty() && (insert_pos == get_lang_close_line_start()))
                    child_block = newline + child_block;

                if (insert_pos != get_lang_close_line_start())
                    child_block += newline;

                raw.insert(insert_pos, child_block);
                modified = true;
            }
        }

        if (modified) {
            fs::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
            out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
            LOG_DEBUG("Saved modified system lang file: '{}'", dest_path.string());
        }
    }
}

static const std::vector<std::string> list_user_lang_static = {
    "id", "ms", "ua"
};

void init_lang(LangState &lang, EmuEnvState &emuenv) {
    lang = {};
    emuenv.common_dialog.lang = {};
    emuenv.ime.lang = {};

    const auto set_lang = [&](const std::string &language) {
        lang.user_lang[GUI] = language;
        lang.user_lang[LIVE_AREA] = language;
    };

    const auto sys_lang = static_cast<SceSystemParamLang>(emuenv.cfg.sys_lang);
    switch (sys_lang) {
    case SCE_SYSTEM_PARAM_LANG_JAPANESE: set_lang("ja"); break;
    case SCE_SYSTEM_PARAM_LANG_ENGLISH_US: set_lang("en"); break;
    case SCE_SYSTEM_PARAM_LANG_FRENCH: set_lang("fr"); break;
    case SCE_SYSTEM_PARAM_LANG_SPANISH: set_lang("es"); break;
    case SCE_SYSTEM_PARAM_LANG_GERMAN: set_lang("de"); break;
    case SCE_SYSTEM_PARAM_LANG_ITALIAN: set_lang("it"); break;
    case SCE_SYSTEM_PARAM_LANG_DUTCH: set_lang("nl"); break;
    case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT: set_lang("pt"); break;
    case SCE_SYSTEM_PARAM_LANG_RUSSIAN: set_lang("ru"); break;
    case SCE_SYSTEM_PARAM_LANG_KOREAN: set_lang("ko"); break;
    case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
        lang.user_lang[GUI] = "zh-t";
        lang.user_lang[LIVE_AREA] = "ch";
        break;
    case SCE_SYSTEM_PARAM_LANG_CHINESE_S:
        lang.user_lang[GUI] = "zh-s";
        lang.user_lang[LIVE_AREA] = "zh";
        break;
    case SCE_SYSTEM_PARAM_LANG_FINNISH: set_lang("fi"); break;
    case SCE_SYSTEM_PARAM_LANG_SWEDISH: set_lang("sv"); break;
    case SCE_SYSTEM_PARAM_LANG_DANISH: set_lang("da"); break;
    case SCE_SYSTEM_PARAM_LANG_NORWEGIAN: set_lang("no"); break;
    case SCE_SYSTEM_PARAM_LANG_POLISH: set_lang("pl"); break;
    case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: set_lang("pt-br"); break;
    case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB: set_lang("en-gb"); break;
    case SCE_SYSTEM_PARAM_LANG_TURKISH: set_lang("tr"); break;
    default: break;
    }

    const auto system_lang_path{ emuenv.static_assets_path / "lang/system" };
    const auto user_lang_shared_path{ emuenv.shared_path / "lang/user" };
    const auto user_lang_static_path{ emuenv.static_assets_path / "lang/user" };

    // Create user lang folder if not exists and create a file to indicate where to place the lang files
    if (!fs::exists(user_lang_shared_path)) {
        fs::create_directories(user_lang_shared_path);
        fs::ofstream outfile(user_lang_shared_path / "PLACE USER LANG HERE.txt");
        outfile.close();
    }

    const auto is_user_lang_static = vector_utils::contains(list_user_lang_static, emuenv.cfg.user_lang);

    // Load lang xml
    pugi::xml_document lang_xml;
    const auto lang_xml_path{ (emuenv.cfg.user_lang.empty() ? system_lang_path / lang.user_lang[GUI] : (is_user_lang_static ? user_lang_static_path : user_lang_shared_path) / emuenv.cfg.user_lang).replace_extension("xml") };
    std::vector<uint8_t> lang_content{};
    if (fs_utils::read_data(lang_xml_path, lang_content)) {
        const auto load_xml_res = lang_xml.load_buffer(lang_content.data(), lang_content.size(), pugi::encoding_utf8);
        if (load_xml_res) {
            // Lang
            const auto lang_child = lang_xml.child("lang");
            if (!lang_child.empty()) {
                const auto set_lang_string = [](std::map<std::string, std::string> &lang_dest, const pugi::xml_node child) {
                    if (!child.empty()) {
                        for (auto &dest : lang_dest) {
                            if (dest.first == "title") {
                                if (!child.attribute("name").empty())
                                    dest.second = child.attribute("name").as_string();
                            } else {
                                const auto id = dest.first.c_str();
                                if (!child.child(id).empty())
                                    dest.second = child.child(id).text().as_string();
                            }
                        }
                    }
                };

                // Main Menu Bar
                const auto main_menubar = lang_child.child("main_menubar");
                if (!main_menubar.empty()) {
                    // File Menu
                    set_lang_string(lang.main_menubar.file, main_menubar.child("file"));

                    // Emulation Menu
                    set_lang_string(lang.main_menubar.emulation, main_menubar.child("emulation"));

                    // Debug Menu
                    set_lang_string(lang.main_menubar.debug, main_menubar.child("debug"));

                    // Configuration Menu
                    set_lang_string(lang.main_menubar.configuration, main_menubar.child("configuration"));

                    // Controls Menu
                    set_lang_string(lang.main_menubar.controls, main_menubar.child("controls"));

                    // Help Menu
                    set_lang_string(lang.main_menubar.help, main_menubar.child("help"));
                }

                // About
                set_lang_string(lang.about, lang_child.child("about"));

                // App Context
                const auto app_context = lang_child.child("app_context");
                if (!app_context.empty()) {
                    // Main
                    set_lang_string(lang.app_context.main, app_context);

                    // Compat
                    set_lang_string(lang.app_context.compat, app_context.child("compat"));

                    // Copy App info
                    set_lang_string(lang.app_context.copy_app_info, app_context.child("copy_app_info"));

                    // Custom Config
                    set_lang_string(lang.app_context.custom_config, app_context.child("custom_config"));

                    // Path
                    set_lang_string(lang.app_context.path, app_context.child("path"));

                    // Live Area
                    set_lang_string(lang.app_context.live_area, app_context.child("live_area"));

                    // Other
                    set_lang_string(lang.app_context.other, app_context.child("other"));

                    // Delete
                    set_lang_string(lang.app_context.deleting, app_context.child("delete"));

                    // Information
                    set_lang_string(lang.app_context.info, app_context.child("info"));

                    // Time Used
                    set_lang_string(lang.app_context.time_used, app_context.child("time_used"));
                }

                // Common
                const auto common = lang_child.child("common");
                if (!common.empty()) {
                    set_lang_string(emuenv.common_dialog.lang.common, common);
                    set_lang_string(lang.common.main, common);

                    const auto set_calendar = [](std::vector<std::string> &dest, const pugi::xml_node child) {
                        if (!child.empty()) {
                            dest.clear();
                            for (const auto day : child)
                                dest.emplace_back(day.text().as_string());
                        }
                    };

                    // Day of the week
                    set_calendar(lang.common.wday, common.child("wday"));

                    // Months of the year
                    set_calendar(lang.common.ymonth, common.child("ymonth"));

                    // Small months of the year
                    set_calendar(lang.common.small_ymonth, common.child("small_ymonth"));

                    // Days of the month
                    set_calendar(lang.common.mday, common.child("mday"));

                    // Small days of the month
                    set_calendar(lang.common.small_mday, common.child("small_mday"));
                }

                // Compatibility
                const auto compatibility_child = lang_child.child("compatibility");
                if (!compatibility_child.empty()) {
                    auto &lang_compatibility = lang.compatibility;
                    // Name
                    if (!compatibility_child.attribute("name").empty())
                        lang_compatibility.name = compatibility_child.attribute("name").as_string();

                    // States
                    const auto states = compatibility_child.child("states");
                    if (!states.empty()) {
                        for (const auto state : states) {
                            const auto id = static_cast<compat::CompatibilityState>(state.attribute("id").as_int());
                            lang_compatibility.states[id] = state.text().as_string();
                        }
                    }
                }

                // Compatibility Database
                set_lang_string(lang.compat_db, lang_child.child("compat_db"));

                // Compile Shaders
                set_lang_string(lang.compile_shaders, lang_child.child("compile_shaders"));

                // Contacts Pa
                set_lang_string(lang.contacts_pa, lang_child.child("contacts_pa"));

                // Content Manager
                const auto content_manager = lang_child.child("content_manager");
                if (!content_manager.empty()) {
                    // Main
                    set_lang_string(lang.content_manager.main, content_manager);

                    // Application
                    set_lang_string(lang.content_manager.application, content_manager.child("application"));

                    // Saved Data
                    set_lang_string(lang.content_manager.saved_data, content_manager.child("saved_data"));
                }

                // Controllers
                set_lang_string(lang.controllers, lang_child.child("controllers"));

                // Controls
                set_lang_string(lang.controls, lang_child.child("controls"));

                // Dialog
                const auto dialog = lang_child.child("dialog");
                if (!dialog.empty()) {
                    // Trophy
                    set_lang_string(emuenv.common_dialog.lang.trophy, dialog.child("trophy"));

                    // Save Data
                    const auto save_data = dialog.child("save_data");
                    if (!save_data.empty()) {
                        auto &lang_save_data = emuenv.common_dialog.lang.save_data;
                        // Delete
                        set_lang_string(lang_save_data.deleting, save_data.child("delete"));

                        // Info
                        set_lang_string(lang_save_data.info, save_data.child("info"));

                        // Load
                        set_lang_string(lang_save_data.load, save_data.child("load"));

                        // Save
                        set_lang_string(lang_save_data.save, save_data.child("save"));
                    }
                }

                // Friend
                set_lang_string(lang.friends, lang_child.child("friend"));

                // Friend Profile
                set_lang_string(lang.friend_profile, lang_child.child("friend_profile"));

                // Game Data
                set_lang_string(lang.game_data, lang_child.child("game_data"));

                // Home Screen
                set_lang_string(lang.home_screen, lang_child.child("home_screen"));

                // Indicator
                set_lang_string(lang.indicator, lang_child.child("indicator"));

                // Initial Setup
                if (!emuenv.cfg.initial_setup)
                    set_lang_string(lang.initial_setup, lang_child.child("initial_setup"));

                // Install Dialog
                const auto install_dialog = lang_child.child("install_dialog");
                if (!install_dialog.empty()) {
                    // Firmware Install
                    set_lang_string(lang.install_dialog.firmware_install, install_dialog.child("firmware_install"));

                    // Package Install
                    set_lang_string(lang.install_dialog.pkg_install, install_dialog.child("pkg_install"));

                    // Archive Install
                    set_lang_string(lang.install_dialog.archive_install, install_dialog.child("archive_install"));

                    // License Install
                    set_lang_string(lang.install_dialog.license_install, install_dialog.child("license_install"));

                    // Reinstall
                    set_lang_string(lang.install_dialog.reinstall, install_dialog.child("reinstall"));
                }

                // Live Area
                const auto live_area = lang_child.child("live_area");
                if (!live_area.empty()) {
                    // Main
                    set_lang_string(lang.live_area.main, live_area);

                    // Help
                    set_lang_string(lang.live_area.help, live_area.child("help"));
                }

                // Message
                set_lang_string(emuenv.common_dialog.lang.message, lang_child.child("message"));

                // Online Storage
                set_lang_string(lang.online_storage, lang_child.child("online_storage"));

                // Overlay
                set_lang_string(lang.overlay, lang_child.child("overlay"));

                // Patch Check
                set_lang_string(lang.patch_check, lang_child.child("patch_check"));

                // Performance Overlay
                set_lang_string(lang.performance_overlay, lang_child.child("performance_overlay"));

                // Settings
                const auto settings = lang_child.child("settings");
                if (!settings.empty()) {
                    // Main
                    set_lang_string(lang.settings.main, settings);

                    // Sound & Display
                    set_lang_string(lang.settings.sound_display, settings.child("sound_display"));

                    // Theme & Background
                    const auto theme_background = settings.child("theme_background");
                    if (!theme_background.empty()) {
                        set_lang_string(lang.settings.theme_background.main, theme_background);

                        // Theme
                        const auto theme = theme_background.child("theme");
                        if (!theme.empty()) {
                            set_lang_string(lang.settings.theme_background.theme.main, theme);

                            // Information
                            set_lang_string(lang.settings.theme_background.theme.information, theme.child("information"));
                        }

                        // Start Screen
                        set_lang_string(lang.settings.theme_background.start_screen, theme_background.child("start_screen"));

                        // Home Screen Backgrounds
                        set_lang_string(lang.settings.theme_background.home_screen_backgrounds, theme_background.child("home_screen_backgrounds"));
                    }

                    // Date & Time
                    const auto date_time = settings.child("date_time");
                    if (!date_time.empty()) {
                        // Main
                        set_lang_string(lang.settings.date_time.main, date_time);

                        // Date Format
                        set_lang_string(lang.settings.date_time.date_format, date_time.child("date_format"));

                        // Time Format
                        set_lang_string(lang.settings.date_time.time_format, date_time.child("time_format"));
                    }

                    // Language
                    const auto language = settings.child("language");
                    if (!language.empty()) {
                        // Main
                        auto &lang_settings = lang.settings.language;
                        set_lang_string(lang.settings.language.main, language);

                        // Input Language
                        const auto input_language = language.child("input_language");
                        if (!input_language.empty()) {
                            // Main
                            set_lang_string(lang_settings.input_language, input_language);

                            // Keyboards
                            const auto keyboards = input_language.child("keyboards");
                            if (!keyboards.empty()) {
                                set_lang_string(lang_settings.keyboards, keyboards);
                                auto &lang_ime = emuenv.ime.lang.ime_keyboards;
                                const auto &keyboard_lang_ime = keyboards.child("ime_languages");
                                if (!keyboard_lang_ime.empty()) {
                                    lang_ime.clear();
                                    const auto op = [](const auto &lang) {
                                        return std::make_pair(static_cast<SceImeLanguage>(lang.attribute("id").as_ullong()), lang.text().as_string());
                                    };
                                    std::transform(std::begin(keyboard_lang_ime), std::end(keyboard_lang_ime), std::back_inserter(lang_ime), op);
                                }
                            }
                        }
                    }
                }

                // Settings Dialog
                const auto settings_dialog = lang_child.child("settings_dialog");
                if (!settings_dialog.empty()) {
                    // Main
                    set_lang_string(lang.settings_dialog.main_window, settings_dialog);

                    // Core
                    set_lang_string(lang.settings_dialog.core, settings_dialog.child("core"));

                    // CPU
                    set_lang_string(lang.settings_dialog.cpu, settings_dialog.child("cpu"));

                    // GPU
                    set_lang_string(lang.settings_dialog.gpu, settings_dialog.child("gpu"));

                    // Audio
                    set_lang_string(lang.settings_dialog.audio, settings_dialog.child("audio"));

                    // Camera
                    set_lang_string(lang.settings_dialog.camera, settings_dialog.child("camera"));

                    // System
                    set_lang_string(lang.settings_dialog.system, settings_dialog.child("system"));

                    // Emulator
                    set_lang_string(lang.settings_dialog.emulator, settings_dialog.child("emulator"));

                    // GUI
                    set_lang_string(lang.settings_dialog.gui, settings_dialog.child("gui"));

                    // Network
                    set_lang_string(lang.settings_dialog.network, settings_dialog.child("network"));

                    // Debug
                    set_lang_string(lang.settings_dialog.debug, settings_dialog.child("debug"));
                }

                // System Applications Title
                set_lang_string(lang.sys_apps_title, lang_child.child("sys_apps_title"));

                // Trophy Collection
                set_lang_string(lang.trophy_collection, lang_child.child("trophy_collection"));

                // User Management
                set_lang_string(lang.user_management, lang_child.child("user_management"));

                // Vita3k Update
                set_lang_string(lang.vita3k_update, lang_child.child("vita3k_update"));

                // Welcome
                set_lang_string(lang.welcome, lang_child.child("welcome"));
            }
        } else {
            LOG_ERROR("Error parsing lang file xml: {}", lang_xml_path);
            LOG_DEBUG("error: {} position: {}", load_xml_res.description(), load_xml_res.offset);
            constexpr ptrdiff_t context_window = 20;
            ptrdiff_t offset = static_cast<ptrdiff_t>(load_xml_res.offset);
            if (offset >= 0 && offset < static_cast<ptrdiff_t>(lang_content.size())) {
                ptrdiff_t start = std::max<ptrdiff_t>(0, offset - context_window);
                ptrdiff_t end = std::min<ptrdiff_t>(lang_content.size(), offset + context_window);

                ptrdiff_t error_in_context = offset - start;

                std::string error_context(reinterpret_cast<const char *>(lang_content.data() + start), end - start);

                LOG_DEBUG("Error preview: {}|{}", error_context.substr(0, error_in_context), error_context.substr(error_in_context));
            }
        }
    } else
        LOG_ERROR("Lang file xml not found: {}", lang_xml_path);
}

} // namespace lang
