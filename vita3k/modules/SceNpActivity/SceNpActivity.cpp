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

#include <module/module.h>

#include <io/state.h>

#include <v3kn/account.h>
#include <v3kn/activity.h>
#include <v3kn/state.h>

enum SceNpActivityError : uint32_t {
    SCE_NP_ACTIVITY_ERROR_ALREADY_INITIALIZED = 0x80552301,
    SCE_NP_ACTIVITY_ERROR_NOT_INITIALIZED = 0x80552302,
    SCE_NP_ACTIVITY_ERROR_INVALID_ARGUMENT = 0x80552303,
    SCE_NP_ACTIVITY_ERROR_INVALID_URI_SCHEME = 0x80552304,
    SCE_NP_ACTIVITY_ERROR_INVALID_STATUS_MESSAGE = 0x80552305,
    SCE_NP_ACTIVITY_ERROR_SIGNOUT = 0x80552306,
    SCE_NP_ACTIVITY_ERROR_INTERNAL = 0x80552307,
    SCE_NP_ACTIVITY_ERROR_INVALID_APP_VER = 0x80552308,
    SCE_NP_ACTIVITY_ERROR_INVALID_APP_PARAM = 0x80552309,
};

// Maximum number of characters of the status message (UTF-8), this is not the number of bytes and the null-terminator is not included.
#define SCE_NP_ACTIVITY_STATUS_MESSAGE_MAX_NUM_CHARACTERS 256

// Maximum number of linefeeds of the status message.
#define SCE_NP_ACTIVITY_STATUS_MESSAGE_MAX_NUM_LINEFEEDS 8

// Maximum number of bytes of the status message (including the null-terminator)
// A UTF8 character uses 1 to 4 bytes.
// If the number of characters exceeds the maximum number of characters,
// an error occurs even if it is within the following number of bytes.
#define SCE_NP_ACTIVITY_STATUS_MESSAGE_MAX_SIZE 1025

// Maximum number of action URI of status (including the null-terminator)
// The action URI is in the ASCII characters.
#define SCE_NP_ACTIVITY_STATUS_ACTION_URI_MAX_SIZE 1025

/* NP Activity status message */
typedef struct SceNpActivityStatusMessage {
    // Status message (UTF-8, including the null-terminator).
    // The maximum number of characters (UTF-8) is 256. The maximum number of bytes is 1025 bytes including the null-terminator.
    char data[SCE_NP_ACTIVITY_STATUS_MESSAGE_MAX_SIZE];
} SceNpActivityStatusMessage;

/* NP Activity status action URI */
typedef struct SceNpActivityStatusActionUri {
    /*E Action URI of the status in the ASCII characters (including the NUL terminator) */
    char data[SCE_NP_ACTIVITY_STATUS_ACTION_URI_MAX_SIZE];
} SceNpActivityStatusActionUri;

/* NP Activity status with an action URI for starting up an application */
#define SCE_NP_ACTIVITY_APP_PARAM_MAX_SIZE 897
#define SCE_NP_ACTIVITY_APP_VER_NOT_REQUIRED 0

typedef struct SceNpActivityStatusAppStartupParam {
    // The required application version that is in the format "xx.yy".
    // Please specify this value as a 4-digit number, with the period removed.
    // When SCE_NP_ACTIVITY_APP_VER_NOT_REQUIRED(0) is set, the required application version is not set.
    SceUInt32 appVersion;
    // The argument for starting up application(including the null-terminator)
    // Please perform URI Escape processing by sceHttpUriEscape().
    // When empty string is set, an action link without the parameter is created.
    char escapedParam[SCE_NP_ACTIVITY_APP_PARAM_MAX_SIZE];
    SceUInt8 reserved[3]; // Reserved range
} SceNpActivityStatusAppStartupParam;

static bool initialized = false;

EXPORT(int, sceNpActivityInit, void *option) {
    LOG_INFO("sceNpActivityInit called", option);
    if (initialized)
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_ALREADY_INITIALIZED);

    if (option)
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_INVALID_ARGUMENT);

    initialized = true;
    return 0;
}

EXPORT(int, sceNpActivityPostAppStartupStatus, const SceNpActivityStatusMessage *message,
    const SceNpActivityStatusAppStartupParam *param,
    void *option) {
    LOG_DEBUG("sceNpActivityPostAppStartupStatus called with message: {}, appVersion: {}, escapedParam: {}",
        message ? message->data : "null",
        param ? param->appVersion : 0,
        param ? param->escapedParam : "null");
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpActivityPostStatus, const SceNpActivityStatusMessage *message, const SceNpActivityStatusActionUri *actionUri, void *option) {
    LOG_DEBUG("sceNpActivityPostStatus called with message: {}, actionUri: {}",
        message ? message->data : "null",
        actionUri ? actionUri->data : "null");

    // Library not initialized
    if (!initialized)
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_NOT_INITIALIZED);

    // Invalid arguments
    if (!message || !message->data)
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_INVALID_ARGUMENT);

    // option must ALWAYS be NULL
    if (option)
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_INVALID_ARGUMENT);

    // Validate message length (max 256 chars, max 9 lines)
    {
        const char *txt = message->data;
        size_t len = strlen(txt);

        if (len == 0 || len > 256) {
            return SCE_NP_ACTIVITY_ERROR_INVALID_STATUS_MESSAGE;
        }

        int line_count = 1;
        for (size_t i = 0; i < len; i++) {
            if (txt[i] == '\n')
                line_count++;
        }

        if (line_count > 9) {
            return SCE_NP_ACTIVITY_ERROR_INVALID_STATUS_MESSAGE;
        }
    }

    // Validate actionUri (if provided)
    if (actionUri && actionUri->data) {
        const char *uri = actionUri->data;

        // Must be ASCII only
        for (size_t i = 0; uri[i]; i++) {
            if ((unsigned char)uri[i] > 0x7F)
                RET_ERROR(SCE_NP_ACTIVITY_ERROR_INVALID_URI_SCHEME);
        }

        // Must start with http://, https://, psts:, or psgm:
        if (!(strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0 || strncmp(uri, "psts:", 5) == 0 || strncmp(uri, "psgm:", 5) == 0)) {
            RET_ERROR(SCE_NP_ACTIVITY_ERROR_INVALID_URI_SCHEME);
        }
    }

    // Check sign-in state
    if (emuenv.v3kn.account_state.user_info.online_id.empty())
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_SIGNOUT);

    ActivityPostStatus status{
        .type = "game",
        .title_id = emuenv.io.title_id,
        .message = message->data,
        .created_at = v3kn::get_current_activity_time_ms(emuenv),
        .game_activity = {
            .content_id = emuenv.io.content_id,
            .action_uri = actionUri ? actionUri->data : "" }
    };
    v3kn::create_and_post_activity_status(emuenv, status);

    return 0; // success
}

EXPORT(int, sceNpActivityTerm) {
    LOG_INFO("sceNpActivityTerm called");
    if (!initialized)
        RET_ERROR(SCE_NP_ACTIVITY_ERROR_NOT_INITIALIZED);

    initialized = false;
    return 0;
}
