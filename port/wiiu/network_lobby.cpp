#include "network_lobby.h"
#include <curl/curl.h>
#include <mutex>
#include "../../third_party/nlohmann/json.hpp"
namespace wiiu::network {
namespace {
constexpr size_t response_limit = 1 << 20;
#include "network_ca.inc"
struct Response { std::string body; bool too_large = false; };
size_t receive(char *p, size_t size, size_t n, void *opaque) {
    auto &r = *static_cast<Response *>(opaque);
    if (size && n > response_limit / size) { r.too_large = true; return 0; }
    size_t bytes = size * n;
    if (bytes > response_limit - r.body.size()) { r.too_large = true; return 0; }
    r.body.append(p, bytes); return bytes;
}
int progress(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool> *>(p)->load() ? 1 : 0;
}
bool shallow_json(const std::string &text) {
    int depth = 0; bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 24) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return !quoted && depth == 0;
}
std::string text(const nlohmann::json &j, const char *key, size_t max = 256) {
    auto i = j.find(key); if (i == j.end() || !i->is_string()) return {};
    std::string s = i->get<std::string>();
    if (s.size() > max || s.find('\0') != std::string::npos) return {};
    for (char &c : s) if (static_cast<unsigned char>(c) < 32) c = ' ';
    return s;
}
unsigned number(const nlohmann::json &j, const char *key) {
    auto i = j.find(key); if (i == j.end() || !i->is_number_unsigned()) return 0;
    auto n = i->get<uint64_t>(); return n <= 65535 ? unsigned(n) : 0;
}
}
HttpResult web_request(const Config &config, const std::string &path, const char *method,
                       const std::atomic<bool> &cancel, const std::string &bearer, bool credentials) {
    HttpResult result;
    if (config.web_api_url.empty()) { result.error = "Set a community Web API URL first. Azahar has no official room directory."; return result; }
    if (!api_valid(config.web_api_url) || !header_valid(bearer) || !validate(config).empty()) {
        result.error = "Invalid Web API settings."; return result;
    }
    static std::once_flag init; static CURLcode initialized = CURLE_FAILED_INIT;
    std::call_once(init, [] { initialized = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialized != CURLE_OK) { result.error = "Could not initialize HTTP."; return result; }
    CURL *curl = curl_easy_init(); if (!curl) { result.error = "Not enough memory for HTTP."; return result; }
    std::string url = config.web_api_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += path;
    Response response;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "api-version: 1");
    if (!bearer.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    else if (credentials && !config.web_api_username.empty() && !config.network_token.empty()) {
        headers = curl_slist_append(headers, ("x-username: " + config.web_api_username).c_str());
        headers = curl_slist_append(headers, ("x-token: " + config.network_token).c_str());
    }
    curl_blob ca{const_cast<char *>(certificate_authorities), sizeof(certificate_authorities) - 1, CURL_BLOB_NOCOPY};
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // Never forward a token to a redirected host.
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "3Dii-U/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (std::string(method) == "POST") { curl_easy_setopt(curl, CURLOPT_POST, 1L); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ""); }
    auto code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.ok = code == CURLE_OK && result.status >= 200 && result.status < 300;
    if (result.ok) result.body = std::move(response.body);
    else if (cancel.load()) result.error = "Request cancelled.";
    else if (response.too_large) result.error = "Room directory response exceeds 1 MB.";
    else if (code != CURLE_OK) result.error = std::string("Directory request failed: ") + curl_easy_strerror(code);
    else result.error = "Directory returned HTTP " + std::to_string(result.status) + ". Check the URL and account settings.";
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return result;
}
bool decode_rooms(const std::string &body, std::vector<Room> &out, std::string &error) {
    if (body.size() > response_limit || !shallow_json(body)) { error = "Invalid or excessively nested directory response."; return false; }
    auto root = nlohmann::json::parse(body, nullptr, false);
    if (!root.is_object() || !root.contains("rooms") || !root["rooms"].is_array()) { error = "This URL is not an Azahar-compatible room directory."; return false; }
    std::vector<Room> rooms;
    for (const auto &j : root["rooms"]) {
        if (rooms.size() >= 1024) break;
        if (!j.is_object()) continue;
        Room r;
        r.name = text(j, "name"); r.host = text(j, "address"); r.game = text(j, "preferredGameName");
        r.verify_uid = text(j, "externalGuid", 128); r.port = number(j, "port");
        if(j.contains("preferredGameId")&&j["preferredGameId"].is_number_unsigned())r.game_id=j["preferredGameId"].get<unsigned long long>();
        r.capacity = number(j, "maxPlayers"); r.version = number(j, "netVersion");
        r.password = j.contains("hasPassword") && j["hasPassword"].is_boolean() && j["hasPassword"].get<bool>();
        if (j.contains("players") && j["players"].is_array()) r.players = std::min<size_t>(65535, j["players"].size());
        if (r.name.empty() || r.host.empty() || !host_valid(r.host) || !r.port || !r.capacity) continue;
        rooms.push_back(std::move(r));
    }
    out = std::move(rooms); error.clear(); return true;
}
std::string room_authentication(const Config &c, const Room &r, const std::atomic<bool> &cancel, std::string &error) {
    error.clear();
    if (c.network_token.empty() || c.web_api_username.empty() || r.verify_uid.empty()) return {};
    if (r.verify_uid.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
        error = "Directory supplied an invalid room verification ID."; return {};
    }
    auto internal = web_request(c, "/jwt/internal", "POST", cancel, {}, true);
    if (!internal.ok || !header_valid(internal.body) || internal.body.size() > 16384) { error = internal.ok ? "Invalid authentication response." : internal.error; return {}; }
    auto external = web_request(c, "/jwt/external/" + r.verify_uid, "POST", cancel, internal.body);
    if (!external.ok || !header_valid(external.body) || external.body.size() > 16384) { error = external.ok ? "Invalid room authentication response." : external.error; return {}; }
    return external.body;
}
} // namespace wiiu::network
