#include "Metrics.h"
#include "EmailHelper.h"
#include "../controllers/MessageWebSocket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace metrics {

namespace {

// Prometheus default-ish latency buckets, in seconds.
constexpr std::array<double, 12> kBuckets{
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};

struct CounterKey {
    std::string route;
    std::string method;
    int         status;
    bool operator==(const CounterKey& o) const noexcept {
        return status == o.status && method == o.method && route == o.route;
    }
};

struct CounterKeyHash {
    std::size_t operator()(const CounterKey& k) const noexcept {
        // FNV-1a 64
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&](const std::string& s) {
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        };
        mix(k.route);
        mix(k.method);
        h ^= static_cast<std::uint64_t>(k.status);
        h *= 1099511628211ull;
        return static_cast<std::size_t>(h);
    }
};

// Global counter map, mutex-protected. Not on the hot path of every CPU cycle;
// one lock per request is fine at the scale this app targets.
std::mutex                                                       g_mu;
std::unordered_map<CounterKey, std::uint64_t, CounterKeyHash>    g_requests;

// Histogram state. Atomics so renderPrometheus() can take a stable-enough
// snapshot without holding g_mu.
std::array<std::atomic<std::uint64_t>, kBuckets.size()>          g_buckets{};
std::atomic<std::uint64_t>                                       g_inf{0};
std::atomic<double>                                              g_sum{0.0};
std::atomic<std::uint64_t>                                       g_count{0};

const auto g_started = std::chrono::steady_clock::now();

void addToAtomicDouble(std::atomic<double>& a, double v)
{
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {}
}

std::string escapeLabel(const std::string& v)
{
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            default:   out.push_back(c);
        }
    }
    return out;
}

std::uint64_t residentBytes()
{
    std::ifstream f("/proc/self/status");
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::uint64_t kb = 0;
            std::sscanf(line.c_str(), "VmRSS: %lu", &kb);
            return kb * 1024ull;
        }
    }
    return 0;
}

} // namespace

void observeRequest(const std::string& route,
                    const std::string& method,
                    int                status,
                    double             latency)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_requests[CounterKey{route, method, status}];
    }

    bool placed = false;
    for (std::size_t i = 0; i < kBuckets.size(); ++i) {
        if (latency <= kBuckets[i]) {
            g_buckets[i].fetch_add(1, std::memory_order_relaxed);
            placed = true;
            break;
        }
    }
    if (!placed) g_inf.fetch_add(1, std::memory_order_relaxed);

    addToAtomicDouble(g_sum, latency);
    g_count.fetch_add(1, std::memory_order_relaxed);
}

std::string renderPrometheus()
{
    std::ostringstream out;

    out << "# HELP blog_http_requests_total Total HTTP requests handled.\n"
        << "# TYPE blog_http_requests_total counter\n";
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& [key, count] : g_requests) {
            out << "blog_http_requests_total{"
                << "route=\"" << escapeLabel(key.route) << "\","
                << "method=\"" << escapeLabel(key.method) << "\","
                << "status=\"" << key.status << "\"} "
                << count << '\n';
        }
    }

    out << "# HELP blog_http_request_duration_seconds Request latency in seconds.\n"
        << "# TYPE blog_http_request_duration_seconds histogram\n";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kBuckets.size(); ++i) {
        cumulative += g_buckets[i].load(std::memory_order_relaxed);
        out << "blog_http_request_duration_seconds_bucket{le=\""
            << kBuckets[i] << "\"} " << cumulative << '\n';
    }
    cumulative += g_inf.load(std::memory_order_relaxed);
    out << "blog_http_request_duration_seconds_bucket{le=\"+Inf\"} " << cumulative << '\n';
    out << "blog_http_request_duration_seconds_sum "   << g_sum.load(std::memory_order_relaxed) << '\n';
    out << "blog_http_request_duration_seconds_count " << g_count.load(std::memory_order_relaxed) << '\n';

    out << "# HELP blog_email_queue_depth Outstanding entries in the email worker queue.\n"
        << "# TYPE blog_email_queue_depth gauge\n"
        << "blog_email_queue_depth " << EmailHelper::queueDepth() << '\n';

    out << "# HELP blog_ws_connections Open WebSocket connections (live message subscribers).\n"
        << "# TYPE blog_ws_connections gauge\n"
        << "blog_ws_connections " << MessageWebSocket::connectionCount() << '\n';

    out << "# HELP blog_process_resident_memory_bytes Resident memory of the process.\n"
        << "# TYPE blog_process_resident_memory_bytes gauge\n"
        << "blog_process_resident_memory_bytes " << residentBytes() << '\n';

    const auto uptime = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_started).count();
    out << "# HELP blog_uptime_seconds Seconds since process start.\n"
        << "# TYPE blog_uptime_seconds gauge\n"
        << "blog_uptime_seconds " << uptime << '\n';

    return out.str();
}

} // namespace metrics
