#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace log_safety {

// JSON-string content (without surrounding quotes) that is guaranteed not to
// exceed the caller's encoded-byte budget. Oversized or escape-heavy input is
// represented by a stable SHA-256 marker instead of being cut mid-escape or
// mid-UTF-8 sequence.
struct EscapedField {
    std::string text;
    bool        abbreviated;
};

EscapedField escapeJsonField(std::string_view value,
                             std::size_t      maxEncodedBytes);

// RFC 3339 / ISO-8601 UTC with millisecond precision, always 24 bytes for
// normal civil years: 2026-08-20T21:30:45.123Z.
std::string isoUtcNow();

} // namespace log_safety
