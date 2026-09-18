// json.h — minimal, dependency-free JSON helpers for the IPC control protocol.
//   Writer: JsonW builds one object/array incrementally with correct escaping.
//   Reader: flat-object field extraction (get_str/get_num/get_bool) — the request side of the
//   protocol is deliberately flat ({"cmd":"x","key":"y","value":1}), so no tree parser is needed.
// Shared by the DLL (server) and the C++ clients (inject/CLI/GUI).
#pragma once
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>

namespace json {

inline void escape_into(std::string& o, const char* s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
}

// Incremental writer. Usage: JsonW w; w.obj(); w.kv("a", 1); w.kv("b", "x"); w.end();
// Nesting is tracked with a small stack so commas are placed automatically.
struct JsonW {
    std::string out;
    char  stack[32]; int depth = 0;      // 'o' object / 'a' array
    bool  first[32];
    void sep() { if (depth > 0) { if (!first[depth-1]) out += ','; first[depth-1] = false; } }
    JsonW& obj()  { sep(); out += '{'; stack[depth] = 'o'; first[depth] = true; depth++; return *this; }
    JsonW& arr()  { sep(); out += '['; stack[depth] = 'a'; first[depth] = true; depth++; return *this; }
    JsonW& end()  { depth--; out += stack[depth] == 'o' ? '}' : ']'; return *this; }
    // key() writes "key": and suppresses the next separator (the value follows directly).
    JsonW& key(const char* k) { sep(); out += '"'; escape_into(out, k); out += "\":"; first[depth-1] = true; return *this; }
    JsonW& str(const char* s) { sep(); out += '"'; escape_into(out, s ? s : ""); out += '"'; return *this; }
    JsonW& str(const std::string& s) { return str(s.c_str()); }
    JsonW& num(double v) {
        sep(); char b[64];
        if (std::isnan(v) || std::isinf(v)) strcpy(b, "null");
        else if (v == std::floor(v) && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%.0f", v);
        else snprintf(b, sizeof b, "%.6g", v);
        out += b; return *this;
    }
    JsonW& num(int v)      { sep(); out += std::to_string(v); return *this; }
    JsonW& num(long v)     { sep(); out += std::to_string(v); return *this; }
    JsonW& num(unsigned v) { sep(); out += std::to_string(v); return *this; }
    JsonW& num(uint64_t v) { sep(); out += std::to_string(v); return *this; }
    JsonW& num(int64_t v)  { sep(); out += std::to_string(v); return *this; }
    JsonW& boolean(bool v) { sep(); out += v ? "true" : "false"; return *this; }
    JsonW& null()          { sep(); out += "null"; return *this; }
    // Convenience: "key": value
    JsonW& kv(const char* k, const char* v)        { return key(k).str(v); }
    JsonW& kv(const char* k, const std::string& v) { return key(k).str(v); }
    JsonW& kv(const char* k, double v)   { return key(k).num(v); }
    JsonW& kv(const char* k, int v)      { return key(k).num(v); }
    JsonW& kv(const char* k, unsigned v) { return key(k).num(v); }
    JsonW& kv(const char* k, uint64_t v) { return key(k).num(v); }
    JsonW& kv(const char* k, int64_t v)  { return key(k).num(v); }
    JsonW& kvb(const char* k, bool v)    { return key(k).boolean(v); }
};

// ---- flat reader ----
// Finds `"key"` at the top level of a flat object and returns a pointer to the first non-space
// character of its value, or nullptr. Does not descend into nested containers (values may be
// nested; we just skip them when scanning for the next key).
inline const char* find_value(const char* s, const char* key) {
    if (!s) return nullptr;
    const char* p = strchr(s, '{'); if (!p) return nullptr; p++;
    size_t kl = strlen(key);
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p != '"') return nullptr;
        const char* ks = ++p;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (!*p) return nullptr;
        size_t l = (size_t)(p - ks);
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') return nullptr;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (l == kl && strncmp(ks, key, kl) == 0) return p;
        // skip the value
        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } if (*p) p++; }
        else if (*p == '{' || *p == '[') {
            int d = 0;
            do {
                if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } if (!*p) return nullptr; }
                else if (*p == '{' || *p == '[') d++;
                else if (*p == '}' || *p == ']') d--;
                p++;
            } while (*p && d > 0);
        } else { while (*p && *p != ',' && *p != '}') p++; }
        if (*p == '}') return nullptr;
    }
}

// Decoded string value (handles \" \\ \n \t \r and \uXXXX for the ASCII range).
inline bool get_str(const char* s, const char* key, std::string& out) {
    const char* p = find_value(s, key);
    if (!p || *p != '"') return false;
    p++; out.clear();
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': { unsigned v = 0; if (sscanf(p + 1, "%4x", &v) == 1) { out += (v < 0x80) ? (char)v : '?'; p += 4; } break; }
                case 0: return false;
                default: out += *p;
            }
            p++;
        } else out += *p++;
    }
    return *p == '"';
}
inline bool get_num(const char* s, const char* key, double& out) {
    const char* p = find_value(s, key);
    if (!p) return false;
    if (*p == 't') { out = 1; return true; }
    if (*p == 'f') { out = 0; return true; }
    if (*p == '"') p++;          // tolerate quoted numbers ("value":"12")
    char* e = nullptr; double v = strtod(p, &e);
    if (e == p) return false;
    out = v; return true;
}
// 64-bit ids (SteamID64 exceeds double's 2^53 exact range — never read those via get_num).
inline bool get_u64(const char* s, const char* key, uint64_t& out) {
    const char* p = find_value(s, key);
    if (!p) return false;
    if (*p == '"') p++;
    char* e = nullptr; unsigned long long v = strtoull(p, &e, 10);
    if (e == p) return false;
    out = v; return true;
}
inline bool get_bool(const char* s, const char* key, bool& out) {
    double v; if (!get_num(s, key, v)) return false; out = v != 0; return true;
}

} // namespace json

// ---- array iteration (for clients rendering peers[] / vehicles[]) ----
// `p` points at '[' (e.g. from find_value). Calls fn(elemStart) for each top-level element; elemStart
// points at the element's first char and the element runs to the next top-level ',' or ']'.
// Elements that are objects can be handed straight back to get_str/get_num.
namespace json {
template <class F> inline void for_each_elem(const char* p, F fn) {
    if (!p || *p != '[') return;
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']' || !*p) return;
        const char* start = p;
        int d = 0;
        do {
            if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } if (!*p) return; }
            else if (*p == '{' || *p == '[') d++;
            else if (*p == '}' || *p == ']') { if (d == 0) break; d--; }
            else if (*p == ',' && d == 0) break;
            p++;
        } while (*p);
        fn(start);
    }
}
// Convenience: count elements / read a numeric element of a flat number array like [1,2,3].
inline int arr_nums(const char* p, double* out, int max) {
    int n = 0;
    for_each_elem(p, [&](const char* e) { if (n < max) out[n++] = strtod(e, nullptr); });
    return n;
}
} // namespace json
