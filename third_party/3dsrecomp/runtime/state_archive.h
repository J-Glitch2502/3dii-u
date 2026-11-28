// Bounded logical-state stream. Native pointers and executable callbacks are
// deliberately unsupported; the runtime supplies object/service ID adapters.
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>
#include "cpu.h"
namespace kernel { struct Object; struct Service; }
namespace checkpoint {
struct Archive {
    FILE *file;
    std::vector<std::shared_ptr<std::vector<u8>>> staged;
    bool buffering() const { return !file && !reading; }
    bool reading, ok = true;
    const char *error = nullptr;
    uint64_t bytes = 0, hash = 14695981039346656037ull;
    static constexpr uint64_t limit = 256ull << 20;
    uint64_t payload_limit = limit; // file header bounds reconstruction before commit
    std::function<void(std::shared_ptr<kernel::Object>&)> object;
    std::function<void(std::shared_ptr<kernel::Service>&)> service;
    Archive(FILE *f, bool read) : file(f), reading(read) {}
    void fail(const char *why) { if (ok) error = why; ok = false; }
    void raw(void *p, size_t n) {
        if (!ok) return;
        if (bytes > payload_limit || n > payload_limit - bytes) { fail("state exceeds size limit"); return; }
        if (buffering()) {
            // Immutable chunks are shared with RAM restore callbacks.
            if (bytes+n > (160u<<20)) { fail("background snapshot memory budget exceeded"); return; }
            for (size_t at=0;at<n;) {
                const size_t count=std::min<size_t>(n-at,64u<<10);
                staged.push_back(std::make_shared<std::vector<u8>>((u8*)p+at,(u8*)p+at+count));
                at+=count;
            }
            bytes += n;
            return; // checksum belongs to the background writer
        }
        const size_t done = reading ? fread(p, 1, n, file) : fwrite(p, 1, n, file);
        if (done != n) { fail("state I/O incomplete"); return; }
        for (size_t i = 0; i < n; ++i) hash = (hash ^ static_cast<uint8_t *>(p)[i]) * 1099511628211ull;
        bytes += n;
    }
    uint32_t length(size_t current = 0) {
        uint32_t n = reading ? 0 : static_cast<uint32_t>(current);
        raw(&n, sizeof n);
        if (n > 1u << 20 || n > limit - bytes) fail("invalid state collection length");
        return ok ? n : 0;
    }
    template<class T> void io(T &v) {
        if constexpr (requires { v.state_fields(*this); }) v.state_fields(*this);
        else if constexpr (std::is_pointer_v<T>) {
            using P = std::remove_pointer_t<T>;
            if constexpr (std::is_base_of_v<kernel::Object, P>) {
                std::shared_ptr<kernel::Object> p;
                if (!reading && v) p = v->shared_from_this();
                if (object) object(p); else fail("missing object adapter");
                if (reading) { v = dynamic_cast<P *>(p.get()); if (p && !v) fail("object reference type mismatch"); }
            } else fail("native pointer cannot be persisted");
        } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) raw(&v, sizeof v);
        else fail(__PRETTY_FUNCTION__);
    }
    void io(CPU &v) {
        fields(v.r, v.nf, v.zf, v.cf, v.vf, v.qf, v.ge, v.fpscr, v.pc, v.s, v.tls, v.pad);
        if (reading) v.thread = nullptr; // bound to its newly created Thread by the kernel
    }
    template<class T> void io(std::shared_ptr<T> &v) {
        if constexpr (!requires { sizeof(T); }) fail("incomplete native resource cannot be persisted");
        else if constexpr (std::is_base_of_v<kernel::Object, T>) {
            std::shared_ptr<kernel::Object> p = v;
            if (object) object(p); else fail("missing object adapter");
            if (reading) { v = std::dynamic_pointer_cast<T>(p); if (p && !v) fail("object reference type mismatch"); }
        } else if constexpr (std::is_base_of_v<kernel::Service, T>) {
            std::shared_ptr<kernel::Service> p = v;
            if (service) service(p); else fail("missing service adapter");
            if (reading) { v = std::dynamic_pointer_cast<T>(p); if (p && !v) fail("service reference type mismatch"); }
        } else fail("native resource needs reconstruction");
    }
    void io(std::string &v) {
        auto n = length(v.size());
        if (reading && ok) v.resize(n);
        if (ok) raw(v.data(), n);
    }
    void io(std::filesystem::directory_entry &v) {
        std::string path = reading ? std::string{} : v.path().string(); io(path);
        if (reading && ok) {
            std::error_code ec;
            v = std::filesystem::directory_entry(path, ec);
            if (ec) fail("saved directory entry is no longer available");
        }
    }
    template<class T, size_t N> void io(T (&v)[N]) { for (auto &x : v) io(x); }
    template<class T, size_t N> void io(std::array<T,N> &v) { for (auto &x : v) io(x); }
    template<class T> void io(std::vector<T> &v) { sequence(v); }
    template<class T> void io(std::deque<T> &v) { sequence(v); }
    template<class C> void sequence(C &v) {
        auto n = length(v.size());
        if (n > (limit - bytes) / std::max<size_t>(sizeof(typename C::value_type), 1)) { fail("state collection allocation exceeds limit"); return; }
        if (reading && ok) v.resize(n);
        for (auto &x : v) { if (!ok) break; io(x); }
    }
    template<class T> void io(std::set<T> &v) {
        auto n = length(v.size());
        if (reading) {
            v.clear();
            for (uint32_t i=0; ok && i<n; ++i) { T x{}; io(x); if (!v.insert(x).second) fail("duplicate state set entry"); }
        } else for (auto x : v) io(x);
    }
    template<class A, class B> void io(std::pair<A,B> &v) { fields(v.first, v.second); }
    template<class K, class V> void io(std::map<K,V> &v) {
        auto n = length(v.size());
        if (reading) {
            v.clear();
            for (uint32_t i=0; ok && i<n; ++i) { K k{}; V x{}; io(k); io(x); if (ok && !v.emplace(std::move(k),std::move(x)).second) fail("duplicate state map key"); }
        } else for (auto &[k,x] : v) { auto key=k; io(key); io(x); }
    }
    template<class K, class V> void io(std::multimap<K,V> &v) {
        auto n = length(v.size());
        if (reading) {
            v.clear();
            for (uint32_t i=0; ok && i<n; ++i) { K k{}; V x{}; io(k); io(x); if (ok) v.emplace(std::move(k),std::move(x)); }
        } else for (auto &[k,x] : v) { auto key=k; io(key); io(x); }
    }
    template<class... T> void fields(T &...v) { (io(v), ...); }
};
}
