#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace direct_source {
struct Source { std::string path; uint64_t offset = 0, length = 0, backing_size = 0; };
inline bool parse(const uint8_t *data, size_t size, Source &out) {
    out = {};
    if (size < 34 || size > 672 || std::memcmp(data, "DROMFS01", 8) || data[size - 1]) return false;
    auto be64 = [&](size_t at) { uint64_t n = 0; for (size_t i = 0; i < 8; ++i) n = (n << 8) | data[at + i]; return n; };
    const uint64_t offset = be64(8), length = be64(16), backing = be64(24);
    if (!length || !backing || backing > UINT32_MAX || offset > backing || length > backing - offset) return false;
    std::string path(reinterpret_cast<const char *>(data + 32), size - 33);
    if (!path.starts_with("/vol/external01/wiiu/apps/") || !path.ends_with(".wuhb")) return false;
    size_t begin = 1;
    while (begin < path.size()) {
        size_t end = path.find('/', begin);
        if (end == std::string::npos) end = path.size();
        const auto part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) if (c < 32 || c == 127 || c == ':' || c == '\\') return false;
        begin = end + 1;
    }
    out = {path, offset, length, backing};
    return true;
}
inline bool fits(uint64_t total, uint64_t at, uint64_t length) {
    return at <= total && length <= total - at;
}
} // namespace direct_source
