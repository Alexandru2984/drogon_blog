#include "Metrics.h"
#include "EmailHelper.h"
#include "Security.h"
#include "Workers.h"
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
std::atomic<std::int64_t>                                        g_inFlight{0};

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

void incInFlight() { g_inFlight.fetch_add(1, std::memory_order_relaxed); }
void decInFlight() { g_inFlight.fetch_sub(1, std::memory_order_relaxed); }

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
        << "blog_ws_connections " << MessageWebSocket::connectionCount() << '\n'
        << "# HELP blog_ws_connection_rejected_total WebSocket opens refused by per-session/account limits.\n"
        << "# TYPE blog_ws_connection_rejected_total counter\n"
        << "blog_ws_connection_rejected_total "
        << MessageWebSocket::rejectedConnectionCount() << '\n'
        << "# HELP blog_ws_policy_closure_total WebSockets closed for oversized or excessive control frames.\n"
        << "# TYPE blog_ws_policy_closure_total counter\n"
        << "blog_ws_policy_closure_total "
        << MessageWebSocket::policyClosureCount() << '\n';

    const auto limiter = security::rateLimitStats();
    out << "# HELP blog_rate_limit_buckets In-memory token buckets currently retained.\n"
        << "# TYPE blog_rate_limit_buckets gauge\n"
        << "blog_rate_limit_buckets " << limiter.buckets << '\n'
        << "# HELP blog_rate_limit_bucket_capacity Hard cap on retained token buckets.\n"
        << "# TYPE blog_rate_limit_bucket_capacity gauge\n"
        << "blog_rate_limit_bucket_capacity " << limiter.capacity << '\n'
        << "# HELP blog_rate_limit_capacity_evictions_total LRU buckets evicted at the hard cardinality cap.\n"
        << "# TYPE blog_rate_limit_capacity_evictions_total counter\n"
        << "blog_rate_limit_capacity_evictions_total "
        << limiter.capacityEvictions << '\n';

    out << "# HELP blog_http_requests_in_flight Requests currently being processed.\n"
        << "# TYPE blog_http_requests_in_flight gauge\n"
        << "blog_http_requests_in_flight "
        << g_inFlight.load(std::memory_order_relaxed) << '\n';

    // Blocking-work pools (Argon2id, libvips). These are the saturation
    // signal for the two slowest paths in the app: `queued` climbing means
    // work is arriving faster than the pool retires it, and `rejected`
    // incrementing means requests are being shed with a 503. Alert on the
    // latter — it is the difference between "slow" and "refusing traffic".
    out << "# HELP blog_worker_pool_threads Worker threads in a blocking-work pool.\n"
        << "# TYPE blog_worker_pool_threads gauge\n"
        << "# HELP blog_worker_pool_active Jobs currently executing.\n"
        << "# TYPE blog_worker_pool_active gauge\n"
        << "# HELP blog_worker_pool_queued Jobs waiting for a worker.\n"
        << "# TYPE blog_worker_pool_queued gauge\n"
        << "# HELP blog_worker_pool_capacity Maximum backlog before submissions are refused.\n"
        << "# TYPE blog_worker_pool_capacity gauge\n"
        << "# HELP blog_worker_pool_rejected_total Jobs refused because the backlog was full.\n"
        << "# TYPE blog_worker_pool_rejected_total counter\n";
    for (const auto pool : {workers::Pool::Auth, workers::Pool::Media}) {
        const auto s = workers::stats(pool);
        const std::string label =
            std::string("{pool=\"") + workers::poolName(pool) + "\"} ";
        out << "blog_worker_pool_threads"        << label << s.threads   << '\n'
            << "blog_worker_pool_active"         << label << s.active    << '\n'
            << "blog_worker_pool_queued"         << label << s.queued    << '\n'
            << "blog_worker_pool_capacity"       << label << s.capacity  << '\n'
            << "blog_worker_pool_rejected_total" << label << s.rejected  << '\n';
    }

    // Build info as a constant gauge of value 1 with version labels. Standard
    // Prometheus pattern for static service metadata (lets dashboards switch
    // queries by version, alerts annotate which build started misbehaving).
    const char* ver = std::getenv("BLOG_VERSION");
    const char* rev = std::getenv("BLOG_GIT_REV");
    out << "# HELP blog_build_info Static service build metadata (always 1).\n"
        << "# TYPE blog_build_info gauge\n"
        << "blog_build_info{"
        << "version=\""  << escapeLabel(ver ? ver : "dev") << "\","
        << "git_rev=\""  << escapeLabel(rev ? rev : "unknown") << "\""
        << "} 1\n";

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
