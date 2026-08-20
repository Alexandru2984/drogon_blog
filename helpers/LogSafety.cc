#include "LogSafety.h"

#include "Security.h"

#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace log_safety {

namespace {

std::string escapeUnbounded(std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0x0f]);
                    out.push_back(kHex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string abbreviation(std::string_view value, std::size_t budget)
{
    const std::string digest =
        security::sha256Hex(std::string(value));
    std::string marker = "[abbreviated bytes=" +
                         std::to_string(value.size()) +
                         " sha256=" + digest + "]";
    if (marker.size() <= budget) return marker;

    marker = "sha256:" + digest;
    if (marker.size() <= budget) return marker;
    return marker.substr(0, budget);
}

} // namespace

EscapedField escapeJsonField(std::string_view value,
                             std::size_t      maxEncodedBytes)
{
    // Escaping never shrinks a value. Avoid allocating an attacker-sized
    // intermediate when its raw representation already exceeds the budget.
    if (value.size() <= maxEncodedBytes) {
        auto escaped = escapeUnbounded(value);
        if (escaped.size() <= maxEncodedBytes) {
            return EscapedField{std::move(escaped), false};
        }
    }
    return EscapedField{abbreviation(value, maxEncodedBytes), true};
}

std::string isoUtcNow()
{
    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const int millis = static_cast<int>(
        duration_cast<milliseconds>(now - secs).count());

    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }

    char date[32];
    const std::size_t dateLength =
        std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
    if (dateLength == 0) return "1970-01-01T00:00:00.000Z";

    std::string out(date, dateLength);
    out.push_back('.');
    out.push_back(static_cast<char>('0' + (millis / 100) % 10));
    out.push_back(static_cast<char>('0' + (millis / 10) % 10));
    out.push_back(static_cast<char>('0' + millis % 10));
    out.push_back('Z');
    return out;
}

} // namespace log_safety
