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

#pragma once

#include <v3kn/account.h>
#include <v3kn/activity.h>
#include <v3kn/friend.h>
#include <v3kn/messages.h>
#include <v3kn/storage.h>

#include <util/net_utils.h>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct AccountState {
    UserInfo user_info;
    std::atomic<bool> is_logged_in{ false };
    uint32_t selected_server_index = 0;
};

struct ActivityState {
    std::vector<ActivityPostStatus> cache;
    std::vector<ActivityPostStatus> posted_activities;
    std::atomic<uint64_t> last_created_at_ms{ 0 };
};

struct FriendsState {
    std::atomic<bool> stop_friends_polling{ false };
    std::mutex presence_mutex;

    std::string notice_event_group;

    time_t notice_activity_created_at;

    std::vector<FriendInfo> friends_list;
    std::vector<FriendInfo> requests_sent;
    std::vector<FriendInfo> requests_received;
    std::vector<FriendInfo> blocked_players;
    std::unordered_map<std::string, uint32_t> friends_panel_bg_color;
    std::optional<uint32_t> self_panel_bg_color;

    std::condition_variable presence_cv;
    std::mutex friends_polling_mutex;
    std::condition_variable friends_polling_cv;

    std::atomic<bool> stop_presence{ false };
    FriendInfo self_info;
    std::atomic<PresenceStatus> presence_status{ PresenceStatus::NotAvailable };
    std::atomic<bool> is_loading_friends{ false };
    std::atomic<bool> has_loaded_friends_avatars{ false };
    std::atomic<bool> has_loaded_friends_panels{ false };
    std::atomic<bool> has_loaded_requests_avatars{ false };
    std::atomic<bool> has_loaded_blocked_avatars{ false };
    std::atomic<bool> is_polling_friends{ false };
    std::atomic<bool> is_presence_active{ false };
};

struct MessagesState {
    std::atomic<bool> stop_message_polling{ false };
    std::vector<Conversation> conversations_list;
    std::vector<Message> current_conversation_messages;
    std::string current_conversation_online_id;
    std::vector<std::string> current_group_participants;
    std::string current_group_name;
    bool is_current_user_creator = false;
    std::atomic<bool> is_loading_messages{ false };
    std::atomic<bool> is_polling_messages{ false };
    std::atomic<bool> stop_polling{ false };
    std::mutex messages_polling_mutex;
    std::condition_variable messages_polling_cv;
};

struct StorageState {
    OnlineStorageState online_storage_state = ONLINE_STORAGE_SELECT;
    std::map<SaveDataType, SaveDataInfo> savedata_info;
    std::atomic<float> progress{ 0 };
    std::atomic<uint64_t> remaining{ 0 };
    std::atomic<uint64_t> bytes_done{ 0 };
    net_utils::ProgressState progress_state{};

    uint64_t progress_done_timestamp = 0;
    uint64_t please_wait_timestamp = 0;
    std::atomic<bool> please_wait_done{ false };
    std::mutex mutex_progress;
    std::condition_variable cv_progress;
};

struct ProfileState {
    FriendProfileInfo profile_info;
    std::atomic<bool> is_loading{ false };
    std::atomic<bool> is_action_pending{ false };
    std::string action_result_message;

    std::vector<SearchResultEntry> search_results;
    std::atomic<bool> is_searching{ false };
    std::string last_search_query;
};

struct V3KNState {
    AccountState account_state;
    ActivityState activity_state;
    FriendsState friends_state;
    MessagesState messages_state;
    StorageState storage_state;
    ProfileState profile_state;
};
