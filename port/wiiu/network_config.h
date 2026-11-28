#pragma once
#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>
#include <string>

namespace wiiu::network {
inline constexpr const char *kDefaultWebApi = "http://88.198.47.46:5000";
struct Config {
    bool enabled = false;
    std::string host, nickname = "auto", console_id = "auto", password;
    unsigned port = 24872;
    std::string web_api_url = kDefaultWebApi, web_api_username, network_token;
    std::string chat_side = "right";
    std::map<std::string, std::string> extra;
};
inline bool port_number(const std::string &text, unsigned &out) {
    unsigned n = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !n || n > 65535) return false;
    out = n;
    return true;
}
inline bool host_valid(const std::string &s) {
    return s.size() <= 253 && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") == std::string::npos;
}
inline bool header_valid(const std::string &s) { return s.size() <= 4096 && s.find_first_of("\r\n") == std::string::npos && s.find('\0') == std::string::npos; }
inline bool api_valid(const std::string &s) {
    if (s.empty()) return true;
    if (!header_valid(s) || s.find_first_of(" \t?#@") != std::string::npos) return false;
    const auto start = s.starts_with("https://") ? 8u : s.starts_with("http://") ? 7u : 0u;
    if (!start || s.size() <= start) return false;
    const auto authority = s.substr(start, s.find('/', start) - start);
    const auto colon = authority.find(':');
    unsigned port;
    return !authority.empty() && !authority.substr(0, colon).empty() && host_valid(authority.substr(0, colon)) &&
           (colon == std::string::npos || port_number(authority.substr(colon + 1), port));
}
inline std::string validate(const Config &c) {
    if (!host_valid(c.host)) return "Enter an IPv4 address or hostname, without a URL or port.";
    if (!c.port || c.port > 65535) return "Port must be between 1 and 65535.";
    if(c.chat_side!="left"&&c.chat_side!="right"&&c.chat_side!="off")return "Chat side must be left, right or off.";
    if (!api_valid(c.web_api_url)) return "Web API must be an http:// or https:// directory URL.";
    for (const auto *s : {&c.nickname, &c.console_id, &c.password, &c.web_api_username, &c.network_token})
        if (!header_valid(*s)) return "A network setting contains invalid characters or is too long.";
    return {};
}
inline bool parse(const std::string &text, Config &out, std::string &error) {
    if (text.size() > 32768) { error = "Network settings file is too large."; return false; }
    Config c;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const auto key = line.substr(0, split), value = line.substr(split + 1);
        if (key == "enabled") {
            if (value != "0" && value != "1") { error = "enabled must be 0 or 1."; return false; }
            c.enabled = value == "1";
        } else if (key == "host") c.host = value;
        else if (key == "port") {
            if (!port_number(value, c.port)) { error = "Invalid room port."; return false; }
        } else if (key == "nickname") c.nickname = value;
        else if (key == "console_id") c.console_id = value;
        else if (key == "password") c.password = value;
        else if (key == "web_api_url") c.web_api_url = value;
        else if (key == "web_api_username") c.web_api_username = value;
        else if (key == "network_token") c.network_token = value;
        else if (key == "chat_side") c.chat_side = value;
        else c.extra[key] = value;
    }
    error = validate(c);
    if (!error.empty()) return false;
    out = std::move(c);
    return true;
}
inline std::string serialize(const Config &c) {
    std::ostringstream s;
    s << "# Shared 3Dii U network settings. Keep account tokens private.\n"
      << "enabled=" << (c.enabled ? 1 : 0) << "\nhost=" << c.host << "\nport=" << c.port
      << "\nnickname=" << c.nickname << "\nconsole_id=" << c.console_id << "\npassword=" << c.password
      << "\nchat_side=" << c.chat_side << "\nweb_api_url=" << c.web_api_url << "\nweb_api_username=" << c.web_api_username << "\nnetwork_token=" << c.network_token << '\n';
    for (const auto &[k, v] : c.extra) s << k << '=' << v << '\n';
    return s.str();
}
} // namespace wiiu::network
