#include <drogon/drogon_test.h>

#include "../helpers/LogSafety.h"
#include "../helpers/Metrics.h"

#include <cctype>
#include <string>

using namespace drogon;

DROGON_TEST(LogSafety_JsonFieldsStayValidAndBounded)
{
    const auto ordinary =
        log_safety::escapeJsonField("quote=\" newline=\n slash=\\", 128);
    CHECK(!ordinary.abbreviated);
    CHECK(ordinary.text == "quote=\\\" newline=\\n slash=\\\\");

    const std::string huge(1024 * 1024, 'x');
    const auto first  = log_safety::escapeJsonField(huge, 128);
    const auto second = log_safety::escapeJsonField(huge, 128);
    CHECK(first.abbreviated);
    CHECK(first.text.size() <= 128);
    CHECK(first.text == second.text);
    CHECK(first.text.find("sha256=") != std::string::npos);

    // Raw bytes fit, but their JSON representation would expand sixfold.
    const auto controls = log_safety::escapeJsonField(
        std::string(100, '\x01'), 128);
    CHECK(controls.abbreviated);
    CHECK(controls.text.size() <= 128);
}

DROGON_TEST(LogSafety_TimestampHasStableUtcShape)
{
    const std::string ts = log_safety::isoUtcNow();
    REQUIRE(ts.size() == 24);
    CHECK(ts[4] == '-');
    CHECK(ts[7] == '-');
    CHECK(ts[10] == 'T');
    CHECK(ts[13] == ':');
    CHECK(ts[16] == ':');
    CHECK(ts[19] == '.');
    CHECK(ts[23] == 'Z');
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u,
                                11u, 12u, 14u, 15u, 17u, 18u,
                                20u, 21u, 22u})
    {
        CHECK(std::isdigit(static_cast<unsigned char>(ts[i])) != 0);
    }
}

DROGON_TEST(LogSafety_MetricRoutesCannotCreateUnboundedSeries)
{
    const std::string attackerPrefix(300, 'r');
    for (int i = 0; i < 256; ++i) {
        metrics::observeRequest(attackerPrefix + std::to_string(i),
                                "GET", 404, 0.001);
    }
    const std::string rendered = metrics::renderPrometheus();
    CHECK(rendered.find(attackerPrefix) == std::string::npos);
    CHECK(rendered.find("route=\"/__unmatched__\"") != std::string::npos);
    CHECK(rendered.find("blog_observability_input_truncated_total") !=
          std::string::npos);

    // Length alone is not a cardinality defence: random short paths are the
    // normal shape of a 404 scanner. The metrics map keeps a final fixed
    // overflow series even if a future caller forgets to collapse them.
    const std::string shortPrefix = "/cardinality-probe-";
    for (int i = 0; i < 2300; ++i) {
        metrics::observeRequest(shortPrefix + std::to_string(i),
                                "GET", 404, 0.001);
    }
    const std::string saturated = metrics::renderPrometheus();
    CHECK(saturated.find(shortPrefix + "2299") == std::string::npos);
    CHECK(saturated.find("route=\"/__overflow__\"") != std::string::npos);
}
