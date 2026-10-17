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

#include <util/log.h>
#include <util/net_utils.h>

#include <curl/curl.h>

#ifdef __ANDROID__
#include <util/fs.h>
#endif

#ifdef _WIN32
#include <iphlpapi.h>
#include <ipifcons.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#if defined(__linux__)
#include <netpacket/packet.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <net/if_dl.h>
#include <net/if_types.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace net_utils {

std::string url_encode(const std::string &value) {
    if (value.empty())
        return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        LOG_ERROR("URL encoding input exceeds curl's length limit.");
        throw std::length_error("URL encoding input exceeds curl's length limit");
    }

    const std::unique_ptr<char, decltype(&curl_free)> encoded(
        curl_easy_escape(nullptr, value.data(), static_cast<int>(value.size())), curl_free);
    if (!encoded) {
        LOG_ERROR("Failed to URL-encode request value.");
        throw std::runtime_error("Failed to URL-encode request value");
    }
    return std::string(encoded.get());
}

static CURLcode perform_verified_request(CURL *curl) {
    std::string os;
#if defined(_WIN32)
    os = "Windows";
#elif defined(__ANDROID__)
    os = "Android";
#elif defined(__linux__)
    os = "Linux";
#elif defined(__APPLE__)
    os = "macOS";
#else
    os = "Unknown OS";
#endif
    const auto user_agent_result = curl_easy_setopt(curl, CURLOPT_USERAGENT, ("Vita3K Emulator on (" + os + ")").c_str());
    if (user_agent_result != CURLE_OK) {
        LOG_ERROR("Failed to configure HTTP user agent: {}", curl_easy_strerror(user_agent_result));
        return user_agent_result;
    }
    auto result = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef __ANDROID__
    static auto ca_bundle = [] {
        std::vector<char> data;
        if (!fs_utils::read_data("data/cacert.pem", data))
            LOG_ERROR("Failed to load Android TLS CA bundle: data/cacert.pem");
        return data;
    }();
    if (ca_bundle.empty())
        return CURLE_SSL_CACERT_BADFILE;

    curl_blob blob{ ca_bundle.data(), ca_bundle.size(), CURL_BLOB_COPY };
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
#endif
    if (result != CURLE_OK) {
        LOG_ERROR("Failed to configure TLS certificate verification: {}", curl_easy_strerror(result));
        return result;
    }
    return curl_easy_perform(curl);
}

static uint64_t get_current_time_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct DownloadCallbackData {
    ProgressData progress;
    const ProgressCallback *callback;
};

struct DownloadFileData {
    std::string output_file_path;
    FILE *file = nullptr;
};

static size_t write_download_data(void *ptr, size_t size, size_t count, void *stream) {
    auto &data = *static_cast<DownloadFileData *>(stream);
    if (!data.file) {
        data.file = std::fopen(data.output_file_path.c_str(), "ab");
        if (!data.file)
            return 0;
    }
    return std::fwrite(ptr, size, count, data.file);
}

static int download_progress_callback(void *user_data, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t) {
    if (total <= 0)
        return 0;

    auto &data = *static_cast<DownloadCallbackData *>(user_data);
    if (!*data.callback)
        return 0;

    const auto now = get_current_time_ms();
    const auto elapsed = std::max<uint64_t>(now - data.progress.time, 1);
    const auto done = static_cast<uint64_t>(std::max<curl_off_t>(current, 0));
    const auto total_bytes = static_cast<uint64_t>(total);
    const auto all_done = done + data.progress.bytes_already_downloaded;
    const auto all_total = total_bytes + data.progress.bytes_already_downloaded;
    const auto remaining_bytes = total_bytes - std::min(done, total_bytes);
    const auto seconds_left = done > 0 ? static_cast<uint64_t>((static_cast<double>(remaining_bytes) / static_cast<double>(done)) * static_cast<double>(elapsed) / 1000.0) : 0;
    auto *state = (*data.callback)(all_total ? (100.f * static_cast<float>(all_done) / static_cast<float>(all_total)) : 0.f, seconds_left, all_done);
    if (!state)
        return 0;

    std::unique_lock lock(state->mutex);
    const auto pause_started = state->pause ? get_current_time_ms() : 0;
    state->cv.wait(lock, [&]() { return !state->pause; });
    if (pause_started)
        data.progress.time += get_current_time_ms() - pause_started;
    return state->canceled ? 1 : 0;
}

static void cleanup_failed_download(const std::string &output_file_path, uint64_t bytes_already_downloaded, bool succeeded, long http_code) {
    if ((http_code != 404) && (succeeded || (bytes_already_downloaded > 0)))
        return;

    std::error_code error;
    std::filesystem::remove(output_file_path, error);
    if (error)
        LOG_ERROR("Failed to remove incomplete download '{}': {}", output_file_path, error.message());
}

bool download_file(const std::string &url, const std::string &output_file_path, const ProgressCallback &progress_callback) {
    auto *curl = curl_easy_init();
    if (!curl)
        return false;

    const auto bytes_already_downloaded = std::filesystem::exists(output_file_path) ? std::filesystem::file_size(output_file_path) : 0;
    DownloadCallbackData callback_data{ { get_current_time_ms(), bytes_already_downloaded }, &progress_callback };
    DownloadFileData file_data{ output_file_path };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file_data);
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(bytes_already_downloaded));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &callback_data);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress_callback);

    const auto result = perform_verified_request(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (file_data.file)
        std::fclose(file_data.file);
    curl_easy_cleanup(curl);
    bool succeeded = result == CURLE_OK && (response_code == 200 || response_code == 206);
    if (succeeded && bytes_already_downloaded > 0 && response_code != 206) {
        LOG_ERROR("Expected HTTP 206 for resumed download, got {}.", response_code);
        std::error_code error;
        std::filesystem::resize_file(output_file_path, bytes_already_downloaded, error);
        if (error)
            LOG_ERROR("Failed to restore partial download size: {}.", error.message());
        succeeded = false;
    }
    cleanup_failed_download(output_file_path, bytes_already_downloaded, succeeded, response_code);
    return succeeded;
}

// 0 is ok, negative is bad
SceHttpErrorCode parse_url(const std::string &url, parsedUrl &out) {
    out.scheme = url.substr(0, url.find(':'));

    if (out.scheme != "http" && out.scheme != "https")
        return SCE_HTTP_ERROR_UNKNOWN_SCHEME;

    // Check if URL is opaque, if it is opaque then its invalid
    // http://
    //+    012
    //      ^^ Check these 2
    char url1Slash = *(url.c_str() + (out.scheme.length() + 1)); // get uint8 value
    char url2Slash = *(url.c_str() + (out.scheme.length() + 2)); // get next uint8 value
    // Compare the 2 characters that are supposed to be slahses
    if (url1Slash != '/' || url2Slash != '/') {
        out.invalid = true;
        return (SceHttpErrorCode)0;
    }

    auto end_scheme_pos = url.find(':');
    auto has_path = url.find('/', end_scheme_pos + 3) != std::string::npos;

    auto full_wo_scheme = url.substr(end_scheme_pos + 3);

    if (has_path) {
        // username:password@lttstore.com:727/wysi/cookie.php?pog=gers#extremeexploit

        {
            auto path_pos = std::string(full_wo_scheme).find('/');
            auto full_no_scheme_path = full_wo_scheme.substr(0, path_pos);
            // full_no_scheme_path = username:password@lttstore.com:727
            auto c = full_no_scheme_path.find('@');
            if (c != std::string::npos) { // we have credentials
                auto credentials = full_no_scheme_path.substr(0, c);
                // credentials = username:password
                auto semicolon_pos = credentials.find(':');

                if (semicolon_pos != std::string::npos) { // we have password?
                    auto username = credentials.substr(0, semicolon_pos);
                    auto password = credentials.substr(semicolon_pos + 1);

                    if (username.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                        return SCE_HTTP_ERROR_OUT_OF_SIZE;
                    if (password.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                        return SCE_HTTP_ERROR_OUT_OF_SIZE;

                    out.username = username;
                    out.password = password;
                } else { // we don't have password
                    out.username = credentials;
                }
            } else { // no credentials
                // lttstore.com:727
                auto semicolon_pos = full_no_scheme_path.find(':');

                if (semicolon_pos != std::string::npos) { // we have port
                    auto hostname = full_no_scheme_path.substr(0, semicolon_pos);
                    auto port = full_no_scheme_path.substr(semicolon_pos + 1);

                    out.hostname = hostname;
                    out.port = port;
                } else { // no port
                    out.hostname = full_no_scheme_path;
                }
            }
        }
        {
            auto path_pos = std::string(full_wo_scheme).find('/');
            auto full_no_scheme_hostname = full_wo_scheme.substr(path_pos);
            // /wysi/cookie.php?pog=gers#extremeexploit

            auto query_pos = full_no_scheme_hostname.find('?');
            if (query_pos != std::string::npos) { // we have query
                auto path = full_no_scheme_hostname.substr(0, query_pos);
                out.path = path;

                auto query_frag = full_no_scheme_hostname.substr(query_pos);

                auto frag_pos = query_frag.find('#');
                if (frag_pos != std::string::npos) {
                    // query and fragment

                    auto query = query_frag.substr(0, frag_pos);
                    auto fragment = query_frag.substr(frag_pos);

                    out.query = query;
                    out.fragment = fragment;
                } else { // no fragment
                    out.query = query_frag;
                }

            } else { // no query, dunno about fragment
                auto frag_pos = full_no_scheme_hostname.find('#');

                if (frag_pos != std::string::npos) { // we have fragment
                    auto path = full_no_scheme_hostname.substr(0, frag_pos);
                    auto fragment = full_no_scheme_hostname.substr(frag_pos);

                    out.path = path;
                    out.fragment = fragment;
                } else { // no fragment, only path
                    out.path = full_no_scheme_hostname;
                }
            }
        }
    } else {
        // username:password@lttstore.com:727
        auto c = full_wo_scheme.find('@');
        if (c != std::string::npos) { // we have credentials
            auto credentials = full_wo_scheme.substr(0, c);
            // credentials = username:password
            auto semicolon_pos = credentials.find(':');

            if (semicolon_pos != std::string::npos) { // we have password?
                auto username = credentials.substr(0, semicolon_pos);
                auto password = credentials.substr(semicolon_pos + 1);

                if (username.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                    return SCE_HTTP_ERROR_OUT_OF_SIZE;
                if (password.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                    return SCE_HTTP_ERROR_OUT_OF_SIZE;

                out.username = username;
                out.password = password;
            } else { // we don't have password
                out.username = credentials;
            }
        } else { // no credentials
            // lttstore.com:727
            auto semicolon_pos = full_wo_scheme.find(':');

            if (semicolon_pos != std::string::npos) { // we have port
                auto hostname = full_wo_scheme.substr(0, semicolon_pos);
                auto port = full_wo_scheme.substr(semicolon_pos + 1);

                out.hostname = hostname;
                out.port = port;
            } else { // no port
                out.hostname = full_wo_scheme;
            }
        }
    }

    return (SceHttpErrorCode)0;
}

int char_method_to_int(const char *method) {
    if (strcmp(method, "GET") == 0) {
        return SCE_HTTP_METHOD_GET;
    } else if (strcmp(method, "POST") == 0) {
        return SCE_HTTP_METHOD_POST;
    } else if (strcmp(method, "HEAD") == 0) {
        return SCE_HTTP_METHOD_HEAD;
    } else if (strcmp(method, "OPTIONS") == 0) {
        return SCE_HTTP_METHOD_OPTIONS;
    } else if (strcmp(method, "PUT") == 0) {
        return SCE_HTTP_METHOD_PUT;
    } else if (strcmp(method, "DELETE") == 0) {
        return SCE_HTTP_METHOD_DELETE;
    } else if (strcmp(method, "TRACE") == 0) {
        return SCE_HTTP_METHOD_TRACE;
    } else if (strcmp(method, "CONNECT") == 0) {
        return SCE_HTTP_METHOD_CONNECT;
    } else {
        return -1;
    }
}

const char *int_method_to_char(const int n) {
    switch (n) {
    case 0: return "GET";
    case 1: return "POST";
    case 2: return "HEAD";
    case 3: return "OPTIONS";
    case 4: return "PUT";
    case 5: return "DELETE";
    case 6: return "TRACE";
    case 7: return "CONNECT";

    default:
        return "INVALID";
        break;
    }
}

std::string constructHeaders(const HeadersMapType &headers) {
    std::string headersString;
    for (const auto &head : headers) {
        headersString.append(head.first);
        headersString.append(": ");
        headersString.append(head.second);
        headersString.append("\r\n");
    }

    return headersString;
}

bool parseStatusLine(const std::string &line, std::string &httpVer, int &statusCode, std::string &reason) {
    auto lineClean = line.substr(0, line.find("\r\n"));

    // do this check just in case the server is drunk or retarded, would be nice to do more checks with some regex
    if (!lineClean.starts_with("HTTP/"))
        return false; // what

    const auto firstSpace = lineClean.find(' ');
    if (firstSpace == std::string::npos)
        return false;

    const std::string fullHttpVerStr = lineClean.substr(0, firstSpace);
    const std::string httpVerStr = fullHttpVerStr.substr(strlen("HTTP/"));

    if (!std::isdigit(httpVerStr[0]))
        return false;

    if (lineClean.length() < fullHttpVerStr.length() + strlen(" XXX"))
        return false; // the rest of the line is less than 3 characters in length, what the fuck happened also abort

    const auto codeAndReason = lineClean.substr(firstSpace + 1);
    const auto statusCodeStr = codeAndReason.substr(0, 3);
    if (!std::isdigit(statusCodeStr[0]) || !std::isdigit(statusCodeStr[1]) || !std::isdigit(statusCodeStr[2]))
        return false; // status code contains non digit characters, abort

    const int statusCodeInt = std::stoi(statusCodeStr);

    std::string reasonStr = "";
    bool hasReason = codeAndReason.find(' ') != std::string::npos;
    if (hasReason) // standard says that reasons CAN be empty, we have to take this edge case into account
        reasonStr = codeAndReason.substr(4);

    httpVer = httpVerStr;
    statusCode = statusCodeInt;
    reason = reasonStr;

    return true;
}

/*
    CANNOT have ANYTHING after the last \r\n or \r\n\r\n else it will be treated as a header
*/
bool parseHeaders(std::string &headersRaw, HeadersMapType &headersOut) {
    char *ptr = strtok(headersRaw.data(), "\r\n");
    // use while loop to check ptr is not null
    while (ptr != nullptr) {
        auto line = std::string_view(ptr);

        if (line.find(':') == std::string::npos)
            return false; // separator is missing, the header is invalid

        auto name = line.substr(0, line.find(':'));
        int valueStart = name.length() + 1;
        if (line.find(": ") != std::string_view::npos)
            // Theres a space between semicolon and value, trim it
            valueStart++;

        auto value = line.substr(valueStart);

        headersOut.emplace(std::string(name), std::string(value));
        ptr = strtok(nullptr, "\r\n");
    }
    return true;
}

bool parseResponse(const std::string &res, SceRequestResponse &reqres) {
    auto statusLine = res.substr(0, res.find("\r\n"));
    if (!parseStatusLine(statusLine, reqres.httpVer, reqres.statusCode, reqres.reasonPhrase))
        return false;

    auto headersRaw = res.substr(res.find("\r\n") + strlen("\r\n"), res.find("\r\n\r\n"));

    if (!parseHeaders(headersRaw, reqres.headers))
        return false;

    auto contLenIt = reqres.headers.find("Content-Length");
    if (contLenIt == reqres.headers.end()) {
        reqres.contentLength = 0;
    } else {
        reqres.contentLength = std::stoi(contLenIt->second);
    }

    return true;
}

bool socketSetBlocking(int sockfd, bool blocking) {
#ifdef _WIN32
    u_long blocking_tmp = blocking;
    ioctlsocket(sockfd, FIONBIO, &blocking_tmp);
#else
    if (blocking) { // Blocking
        int flags = fcntl(sockfd, F_GETFL); // Get flags
        fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK); // Set NONBLOCK flag off
    } else { // Non blocking
        int flags = fcntl(sockfd, F_GETFL); // Get flags
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK); // Set NONBLOCK flag on
    }
#endif
    return true;
}

static void apply_session_keep_alive(const CurlSession &session) {
    if (!session.is_keep_alive)
        return;
    auto *curl = static_cast<CURL *>(session.handle);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
}

static int cancel_request_callback(void *user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto *cancel_flag = static_cast<const std::atomic<bool> *>(user_data);
    return cancel_flag && cancel_flag->load() ? 1 : 0;
}

WebResponse get_web_response_ex(const std::string &url, const std::string &token, const std::string &post_data, CurlSession *session, const std::atomic<bool> *cancel_flag) {
    WebResponse response{};
    CurlSession local_session;
    const bool owns_session = session == nullptr;
    if (owns_session) {
        local_session = init_curl_download_session(token, false);
        session = &local_session;
    }

    if (!session->handle) {
        response.curl_res = CURLE_FAILED_INIT;
        return response;
    }

    auto *curl = static_cast<CURL *>(session->handle);
    curl_easy_reset(curl);
    apply_session_keep_alive(*session);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, cancel_flag ? 0L : 1L);
    if (cancel_flag) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_request_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel_flag);
    }
    if (!post_data.empty()) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_data.size()));
    }
    const auto write_response = +[](void *data, size_t size, size_t count, std::string *body) {
        body->append(static_cast<char *>(data), size * count);
        return size * count;
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    if (session->headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(session->headers));
    response.curl_res = perform_verified_request(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_code);

    if (owns_session)
        cleanup_curl_session(local_session);
    if (!response.is_success())
        response.body.clear();
    return response;
}

struct TransferProgressData {
    const ProgressCallback *callback;
    uint64_t start_time;
    uint64_t bytes_already_downloaded;
};

static int transfer_progress_callback(void *user_data, curl_off_t download_total, curl_off_t download_now, curl_off_t upload_total, curl_off_t upload_now) {
    auto &data = *static_cast<TransferProgressData *>(user_data);
    if (!*data.callback)
        return 0;

    const bool downloading = download_total > 0;
    const auto done = static_cast<uint64_t>(std::max<curl_off_t>(downloading ? download_now : upload_now, 0));
    const auto total = static_cast<uint64_t>(std::max<curl_off_t>(downloading ? download_total : upload_total, 0));
    if (!total)
        return 0;

    const auto elapsed = std::max<uint64_t>(get_current_time_ms() - data.start_time, 1);
    const auto completed = done + (downloading ? data.bytes_already_downloaded : 0);
    const auto all_total = total + (downloading ? data.bytes_already_downloaded : 0);
    const auto remaining = total - std::min(done, total);
    const auto seconds_left = done ? static_cast<uint64_t>((static_cast<double>(remaining) / done) * elapsed / 1000.0) : 0;
    auto *state = (*data.callback)(all_total ? 100.f * static_cast<float>(completed) / static_cast<float>(all_total) : 0.f, seconds_left, completed);
    if (!state)
        return 0;

    std::unique_lock lock(state->mutex);
    state->cv.wait(lock, [&]() { return !state->pause; });
    return state->canceled ? 1 : 0;
}

WebResponse download_file_ex(const std::string &url, const std::string &output_file_path, const ProgressCallback &progress_callback, const std::string &token, CurlSession *session) {
    WebResponse response{};
    CurlSession local_session;
    const bool owns_session = session == nullptr;
    if (owns_session) {
        local_session = init_curl_download_session(token, false);
        session = &local_session;
    }
    if (!session->handle) {
        response.curl_res = CURLE_FAILED_INIT;
        return response;
    }

    const auto downloaded = std::filesystem::exists(output_file_path) ? std::filesystem::file_size(output_file_path) : 0;
    DownloadFileData file_data{ output_file_path };

    auto *curl = static_cast<CURL *>(session->handle);
    curl_easy_reset(curl);
    apply_session_keep_alive(*session);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file_data);
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(downloaded));
    TransferProgressData progress_data{ &progress_callback, get_current_time_ms(), downloaded };
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, progress_callback ? 0L : 1L);
    if (progress_callback) {
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress_callback);
    }
    if (session->headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(session->headers));
    response.curl_res = perform_verified_request(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_code);
    if (file_data.file)
        std::fclose(file_data.file);
    if (owns_session)
        cleanup_curl_session(local_session);
    if (response.curl_res == CURLE_OK && downloaded > 0 && response.http_code != 206) {
        LOG_ERROR("Expected HTTP 206 for resumed download, got {}.", response.http_code);
        std::error_code error;
        std::filesystem::resize_file(output_file_path, downloaded, error);
        if (error)
            LOG_ERROR("Failed to restore partial download size: {}.", error.message());
        response.curl_res = CURLE_RANGE_ERROR;
    }
    if (response.curl_res == CURLE_OK && response.http_code / 100 != 2)
        response.curl_res = CURLE_HTTP_RETURNED_ERROR;
    cleanup_failed_download(output_file_path, downloaded, response.is_success(), response.http_code);
    return response;
}

static WebResponse upload_data_impl(const std::string &url, const char *bytes, size_t byte_count, const std::string &filename, const std::string &token, const std::string &metadata, const ProgressCallback &progress_callback, CurlSession *session) {
    WebResponse response{};
    CurlSession local_session;
    const bool owns_session = session == nullptr;
    if (owns_session) {
        local_session = init_curl_upload_session(token, false);
        session = &local_session;
    }
    if (!session->handle) {
        response.curl_res = CURLE_FAILED_INIT;
        return response;
    }

    auto *curl = static_cast<CURL *>(session->handle);
    curl_easy_reset(curl);
    apply_session_keep_alive(*session);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (session->headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(session->headers));

    std::ostringstream body;
    const auto write_response = +[](void *data, size_t size, size_t count, std::ostringstream *output) {
        output->write(static_cast<const char *>(data), static_cast<std::streamsize>(size * count));
        return size * count;
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    TransferProgressData progress_data{ &progress_callback, get_current_time_ms(), 0 };
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, progress_callback ? 0L : 1L);
    if (progress_callback) {
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress_callback);
    }

    auto *mime = curl_mime_init(curl);
    auto *file_part = curl_mime_addpart(mime);
    curl_mime_name(file_part, "file");
    curl_mime_filename(file_part, filename.c_str());
    curl_mime_data(file_part, bytes, byte_count);
    if (!metadata.empty()) {
        auto *metadata_part = curl_mime_addpart(mime);
        curl_mime_name(metadata_part, "xml");
        curl_mime_data(metadata_part, metadata.c_str(), CURL_ZERO_TERMINATED);
    }
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    response.curl_res = perform_verified_request(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_code);
    curl_mime_free(mime);
    response.body = body.str();
    if (owns_session)
        cleanup_curl_session(local_session);
    if (!response.is_success())
        response.body.clear();
    return response;
}

WebResponse upload_file(const std::string &url, const std::string &input_file_path, const std::string &token, const std::string &metadata, const ProgressCallback &progress_callback, CurlSession *session) {
    std::ifstream file(input_file_path, std::ios::binary);
    if (!file)
        return { CURLE_READ_ERROR, {} };
    const std::vector<char> bytes{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    return upload_data_impl(url, bytes.data(), bytes.size(), std::filesystem::path(input_file_path).filename().string(), token, metadata, progress_callback, session);
}

WebResponse upload_data(const std::string &url, const std::vector<unsigned char> &data, const std::string &filename, const std::string &token, CurlSession *session) {
    if (data.empty())
        return { CURLE_READ_ERROR, {} };
    return upload_data_impl(url, reinterpret_cast<const char *>(data.data()), data.size(), filename, token, {}, {}, session);
}

CurlSession init_curl_download_session(const std::string &token, bool is_keep_alive) {
    CurlSession session;
    session.is_keep_alive = is_keep_alive;
    session.handle = curl_easy_init();
    if (!session.handle)
        return session;
    auto *curl = static_cast<CURL *>(session.handle);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    apply_session_keep_alive(session);
    if (!token.empty()) {
        auto *headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
        session.headers = headers;
    }
    return session;
}

CurlSession init_curl_upload_session(const std::string &token, bool is_keep_alive) {
    return init_curl_download_session(token, is_keep_alive);
}

void cleanup_curl_session(CurlSession &session) {
    if (session.headers) {
        curl_slist_free_all(static_cast<curl_slist *>(session.headers));
        session.headers = nullptr;
    }
    if (session.handle) {
        curl_easy_cleanup(static_cast<CURL *>(session.handle));
        session.handle = nullptr;
    }
}

std::string get_web_response(const std::string &url) {
    return get_web_response_ex(url).body;
}

std::string get_web_regex_result(const std::string &url, const std::regex &regex) {
    std::string result;

    // Get the response of the web
    const auto response = get_web_response(url);

    // Check if the response is not empty
    if (!response.empty()) {
        std::smatch match;
        // Check if the response matches the regex
        if (std::regex_search(response, match, regex)) {
            result = match[1];
        } else
            LOG_ERROR("No success found regex: {}", response);
    }

    return result;
}

std::vector<AssignedAddr> get_all_assigned_addrs() {
    std::vector<AssignedAddr> out_addrs;
    const auto ret_addrs = [&out_addrs]() {
        if (out_addrs.empty())
            out_addrs.push_back({ "localhost", "127.0.0.1", "255.255.255.255" });

        return out_addrs;
    };

#ifdef _WIN32
    std::unordered_map<std::string, InterfaceType> adapter_types;
    ULONG adapter_addresses_size = 0;
    const DWORD adapter_addresses_result = GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &adapter_addresses_size);
    if (adapter_addresses_result == ERROR_BUFFER_OVERFLOW && adapter_addresses_size > 0) {
        auto *adapter_addresses = static_cast<IP_ADAPTER_ADDRESSES *>(malloc(adapter_addresses_size));
        if (!adapter_addresses) {
            LOG_ERROR("Failed to allocate memory for Windows network adapter types");
        } else {
            const DWORD result = GetAdaptersAddresses(AF_INET, 0, nullptr, adapter_addresses, &adapter_addresses_size);
            if (result == NO_ERROR) {
                for (auto *adapter = adapter_addresses; adapter; adapter = adapter->Next) {
                    InterfaceType type = InterfaceType::Other;
                    if (adapter->IfType == IF_TYPE_IEEE80211)
                        type = InterfaceType::Wifi;
                    else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD)
                        type = InterfaceType::Ethernet;

                    if (type != InterfaceType::Other && adapter->AdapterName)
                        adapter_types.emplace(adapter->AdapterName, type);
                }
            } else {
                LOG_WARN("GetAdaptersAddresses failed while classifying interfaces: {}", result);
            }
            free(adapter_addresses);
        }
    } else if (adapter_addresses_result != NO_ERROR && adapter_addresses_result != ERROR_NO_DATA) {
        LOG_WARN("GetAdaptersAddresses failed while sizing interface data: {}", adapter_addresses_result);
    }

    PIP_ADAPTER_INFO pAdapterInfo;
    DWORD dwRetVal = 0;
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    pAdapterInfo = (IP_ADAPTER_INFO *)malloc(sizeof(IP_ADAPTER_INFO));
    if (pAdapterInfo == nullptr) {
        LOG_CRITICAL("Error allocating memory needed to call GetAdaptersinfo");
        return ret_addrs();
    }
    // Make an initial call to GetAdaptersInfo to get the necessary size into the ulOutBufLen variable
    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)malloc(ulOutBufLen);
        if (pAdapterInfo == nullptr) {
            LOG_CRITICAL("Error allocating memory needed to call GetAdaptersinfo");
            return ret_addrs();
        }
    }
    if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
        const std::string noAddress = "0.0.0.0";
        std::array<std::string, 2> dns_servers{};
        ULONG fixed_info_length = sizeof(FIXED_INFO);
        auto *fixed_info = static_cast<FIXED_INFO *>(malloc(fixed_info_length));
        if (!fixed_info) {
            LOG_CRITICAL("Error allocating memory needed to call GetNetworkParams");
        } else {
            DWORD network_params_result = GetNetworkParams(fixed_info, &fixed_info_length);
            if (network_params_result == ERROR_BUFFER_OVERFLOW) {
                free(fixed_info);
                fixed_info = static_cast<FIXED_INFO *>(malloc(fixed_info_length));
                if (!fixed_info) {
                    LOG_CRITICAL("Error allocating memory needed to call GetNetworkParams");
                    network_params_result = ERROR_NOT_ENOUGH_MEMORY;
                } else {
                    network_params_result = GetNetworkParams(fixed_info, &fixed_info_length);
                }
            }
            if (network_params_result == NO_ERROR) {
                IP_ADDR_STRING *dns = &fixed_info->DnsServerList;
                for (size_t index = 0; dns && index < dns_servers.size(); dns = dns->Next) {
                    if (noAddress.compare(dns->IpAddress.String) != 0)
                        dns_servers[index++] = dns->IpAddress.String;
                }
            } else {
                LOG_WARN("GetNetworkParams failed with error: {}", network_params_result);
            }
            free(fixed_info);
        }

        PIP_ADAPTER_INFO pAdapter = pAdapterInfo;
        while (pAdapter) {
            std::string gateway;
            if (noAddress.compare(pAdapter->GatewayList.IpAddress.String) != 0)
                gateway = pAdapter->GatewayList.IpAddress.String;

            IP_ADDR_STRING *pIPAddr = &pAdapter->IpAddressList;
            MIB_IFROW interface_row{};
            interface_row.dwIndex = pAdapter->Index;
            const DWORD interface_result = GetIfEntry(&interface_row);
            const bool link_up = interface_result == NO_ERROR && interface_row.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL;
            if (interface_result != NO_ERROR)
                LOG_DEBUG("GetIfEntry failed for network adapter {} with error {}", pAdapter->Description, interface_result);

            const auto adapter_type = adapter_types.find(pAdapter->AdapterName);
            const auto type = adapter_type != adapter_types.end() ? adapter_type->second : InterfaceType::Other;
            std::array<uint8_t, 6> hardware_address{};
            const bool has_hardware_address = pAdapter->AddressLength == hardware_address.size();
            if (has_hardware_address)
                std::copy_n(pAdapter->Address, hardware_address.size(), hardware_address.begin());

            while (pIPAddr) {
                if (noAddress.compare(pIPAddr->IpAddress.String) != 0)
                    out_addrs.push_back({ pAdapter->Description, pIPAddr->IpAddress.String, pIPAddr->IpMask.String,
                        gateway, dns_servers, link_up, type, hardware_address, has_hardware_address });
                pIPAddr = pIPAddr->Next;
            }
            pAdapter = pAdapter->Next;
        }
    } else {
        LOG_CRITICAL("GetAdaptersInfo failed with error: {}", dwRetVal);
    }
    free(pAdapterInfo);
#else
    struct ifaddrs *ifAddrStruct = nullptr;
    struct ifaddrs *ifa = nullptr;
    void *tmpAddrPtr = nullptr;
    std::unordered_map<std::string, InterfaceType> interface_types;
    std::unordered_map<std::string, std::array<uint8_t, 6>> interface_hardware_addresses;

    getifaddrs(&ifAddrStruct);

#if defined(__linux__)
    for (auto *link_ifa = ifAddrStruct; link_ifa != nullptr; link_ifa = link_ifa->ifa_next) {
        if (!link_ifa->ifa_addr || link_ifa->ifa_addr->sa_family != AF_PACKET)
            continue;

        const auto *link_addr = reinterpret_cast<const sockaddr_ll *>(link_ifa->ifa_addr);
        if (link_addr->sll_halen == 6) {
            auto &hardware_address = interface_hardware_addresses[link_ifa->ifa_name];
            std::copy_n(link_addr->sll_addr, hardware_address.size(), hardware_address.begin());
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    for (auto *link_ifa = ifAddrStruct; link_ifa != nullptr; link_ifa = link_ifa->ifa_next) {
        if (!link_ifa->ifa_addr || link_ifa->ifa_addr->sa_family != AF_LINK)
            continue;

        const auto *link_addr = reinterpret_cast<const sockaddr_dl *>(link_ifa->ifa_addr);
        if (link_addr->sdl_alen == 6) {
            auto &hardware_address = interface_hardware_addresses[link_ifa->ifa_name];
            const auto *link_address_data = reinterpret_cast<const uint8_t *>(LLADDR(link_addr));
            std::copy_n(link_address_data, hardware_address.size(), hardware_address.begin());
        }
#if defined(IFT_IEEE80211)
        if (link_addr->sdl_type == IFT_IEEE80211)
            interface_types[link_ifa->ifa_name] = InterfaceType::Wifi;
#else
        if (link_addr->sdl_type == IFT_ETHER)
#endif
        interface_types[link_ifa->ifa_name] = InterfaceType::Ethernet;
    }
#endif

    for (ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;
        if (ifa->ifa_flags)
            if (ifa->ifa_addr->sa_family == AF_INET) { // check it is IP4
                char netMaskAddrStr[INET_ADDRSTRLEN];
                auto netMaskAddr = ((sockaddr_in *)ifa->ifa_netmask)->sin_addr;
                inet_ntop(AF_INET, &netMaskAddr, netMaskAddrStr, INET_ADDRSTRLEN);
                // is a valid IP4 Address
                tmpAddrPtr = &((sockaddr_in *)ifa->ifa_addr)->sin_addr;
                char addressBuffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
                const bool link_up = (ifa->ifa_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING);
                InterfaceType type = InterfaceType::Other;
#if defined(__linux__)
                const auto sysfs_interface = std::filesystem::path("/sys/class/net") / ifa->ifa_name;
                std::error_code error;
                if (std::filesystem::exists(sysfs_interface / "wireless", error)) {
                    type = InterfaceType::Wifi;
                } else {
                    std::ifstream interface_type_file(sysfs_interface / "type");
                    unsigned int hardware_type = 0;
                    if (interface_type_file >> hardware_type && hardware_type == 1)
                        type = InterfaceType::Ethernet;
                }
#elif defined(__APPLE__) || defined(__FreeBSD__)
                if (const auto it = interface_types.find(ifa->ifa_name); it != interface_types.end())
                    type = it->second;
#endif
                const auto hardware_address = interface_hardware_addresses.find(ifa->ifa_name);
                const bool has_hardware_address = hardware_address != interface_hardware_addresses.end();
                out_addrs.push_back({ ifa->ifa_name, addressBuffer, netMaskAddrStr, {}, {}, link_up, type,
                    has_hardware_address ? hardware_address->second : std::array<uint8_t, 6>{}, has_hardware_address });
            }
    }
    if (ifAddrStruct != nullptr)
        freeifaddrs(ifAddrStruct);
#endif
    return ret_addrs();
}

std::vector<AssignedAddr> get_assigned_addrs_by_type(const std::vector<AssignedAddr> &addresses, InterfaceType type) {
    std::vector<AssignedAddr> interfaces;
    std::unordered_set<std::string> names;
    for (const auto &address : addresses) {
        if (address.type == type && names.insert(address.name).second)
            interfaces.push_back(address);
    }
    return interfaces;
}

std::optional<AssignedAddr> get_selected_assigned_addr(int32_t &outIndex, InterfaceType type) {
    const auto interfaces = get_assigned_addrs_by_type(get_all_assigned_addrs(), type);
    const auto is_routable_address = [](const AssignedAddr &address) {
        return address.name != "localhost" && address.addr != "0.0.0.0" && address.addr.rfind("127.", 0) != 0 && address.addr.rfind("169.254.", 0) != 0;
    };

    if (outIndex >= 0 && static_cast<size_t>(outIndex) < interfaces.size() && is_routable_address(interfaces[outIndex]))
        return interfaces[outIndex];

    for (size_t index = 0; index < interfaces.size(); ++index) {
        if (is_routable_address(interfaces[index])) {
            if (outIndex != static_cast<int32_t>(index)) {
                const char *type_name = type == InterfaceType::Wifi ? "Wi-Fi" : "Ethernet";
                LOG_WARN("Selected {} interface index {} is unavailable; using {} ({}) instead",
                    type_name, outIndex, interfaces[index].name, interfaces[index].addr);
                outIndex = static_cast<int32_t>(index);
            }
            return interfaces[index];
        }
    }

    return std::nullopt;
}

AssignedAddr get_selected_assigned_addr(int32_t &outIndex) {
    const auto addrs = get_all_assigned_addrs();
    const auto is_routable_address = [](const AssignedAddr &address) {
        return address.name != "localhost" && address.addr != "0.0.0.0" && address.addr.rfind("127.", 0) != 0 && address.addr.rfind("169.254.", 0) != 0;
    };

    if (outIndex >= 0 && static_cast<size_t>(outIndex) < addrs.size() && is_routable_address(addrs[outIndex]))
        return addrs[outIndex];

    for (size_t index = 0; index < addrs.size(); ++index) {
        if (is_routable_address(addrs[index])) {
            if (outIndex != static_cast<int32_t>(index)) {
                LOG_WARN("Selected network address index {} is unavailable; using {} ({}) instead",
                    outIndex, addrs[index].name, addrs[index].addr);
                outIndex = static_cast<int32_t>(index);
            }
            return addrs[index];
        }
    }

    if (outIndex < 0 || static_cast<size_t>(outIndex) >= addrs.size()) {
        LOG_ERROR("Invalid index {}, returning first address", outIndex);
        outIndex = 0;
    }
    return addrs[outIndex];
}

void init_address(int32_t &outIndex, uint32_t &netAddr, uint32_t &broadcastAddr, uint32_t &netMask) {
    // Initialize the net and broadcast address based on the assigned address and netmask
    const auto addr = get_selected_assigned_addr(outIndex);
    if (inet_pton(AF_INET, addr.addr.c_str(), &netAddr) != 1
        || inet_pton(AF_INET, addr.netMask.c_str(), &netMask) != 1) {
        LOG_ERROR("Invalid IPv4 address or netmask: {} / {}", addr.addr, addr.netMask);
        netAddr = 0;
        broadcastAddr = 0;
        netMask = 0;
        return;
    }
    broadcastAddr = netAddr | ~netMask;
}

} // namespace net_utils
