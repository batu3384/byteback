#pragma once

#include <string>

namespace byteback {

// N-API String::New aborts/throws on illegal UTF-8 (NTFS names). Replace
// broken bytes with '?' so JS callbacks cannot kill the process.
inline std::string utf8ForJs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        int need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else {
            out.push_back('?');
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(need) >= n) {
            out.push_back('?');
            break;
        }
        bool ok = true;
        for (int k = 1; k <= need; ++k) {
            if ((p[i + static_cast<size_t>(k)] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.append(s, i, static_cast<size_t>(need) + 1);
        i += static_cast<size_t>(need) + 1;
    }
    return out;
}

// CA-055: length-preserving strict-validity variant for byte-offset-critical
// strings (content-search snippets). utf8ForJs can SHRINK the string (a
// truncated multi-byte tail collapses into one '?'), and sequences that are
// well-formed but not scalar values (overlong encodings, UTF-16 surrogates,
// > U+10FFFF) pass through untouched — V8 then re-encodes each of those as
// U+FFFD (3 bytes), shifting every byte offset downstream of the fix-up. This
// variant guarantees BOTH invariants the snippet highlight relies on:
//   1. out.size() == in.size() — every invalid BYTE becomes exactly one '?'
//      (never a dropped sequence);
//   2. the output is valid UTF-8 — every kept multi-byte sequence encodes a
//      real scalar value, so the renderer's UTF-8 -> UTF-16 conversion cannot
//      move byte offsets ('?' is ASCII, valid sequences convert 1:1).
inline std::string utf8SanitizeLenientPreserving(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        // Decode a candidate sequence; min2/max2 bound the 2nd byte so
        // overlong encodings, surrogates and > U+10FFFF are rejected as
        // invalid at the LEAD (each of their bytes becomes one '?').
        size_t need = 0;
        unsigned char min2 = 0x80, max2 = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) min2 = 0xA0;          // no overlong 3-byte
            else if (c == 0xED) max2 = 0x9F;     // no surrogates (U+D800..DFFF)
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) min2 = 0x90;          // no overlong 4-byte
            else if (c == 0xF4) max2 = 0x8F;     // no > U+10FFFF
        }
        // else: invalid lead (continuation byte, 0xC0/0xC1 overlong lead,
        // 0xF5..0xFF) — falls through with need == 0.
        bool ok = need != 0 && n - i >= need + 1; // full sequence available
        if (ok && (p[i + 1] < min2 || p[i + 1] > max2)) ok = false;
        for (size_t k = 2; ok && k <= need; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) ok = false;
        }
        if (ok) {
            out.append(s, i, need + 1);
            i += need + 1;
        } else {
            out.push_back('?'); // 1:1: the invalid byte becomes exactly one '?'
            ++i;
        }
    }
    return out;
}

} // namespace byteback
