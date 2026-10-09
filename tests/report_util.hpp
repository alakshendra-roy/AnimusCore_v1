#pragma once
// Small reporting helpers shared by the verification harnesses
// (tests/eval_harness.cpp, tests/soak_shm_mpmc.cpp): platform / compiler
// strings, thousands separators, and JSON string escaping.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace animus_test {

inline const char* os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "unknown";
#endif
}

inline const char* arch_name() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return "unknown";
#endif
}

inline std::string compiler_string() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("g++ ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

inline unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }

inline std::string thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

inline void json_str(FILE* f, const std::string& s) {
    std::fputc('"', f);
    for (char c : s) {
        if (c == '"' || c == '\\') { std::fputc('\\', f); std::fputc(c, f); }
        else if (static_cast<unsigned char>(c) < 0x20) std::fprintf(f, "\\u%04x", static_cast<unsigned>(c));
        else std::fputc(c, f);
    }
    std::fputc('"', f);
}

// fopen that does not trip MSVC's deprecation diagnostic under /W4 /WX.
inline FILE* open_for_write(const std::string& path) {
    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    return f;
}

inline bool parse_u64(const char* s, uint64_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (!end || *end != '\0' || end == s) return false;
    out = v;
    return true;
}

} // namespace animus_test
