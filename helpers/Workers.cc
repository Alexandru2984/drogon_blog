#include "Workers.h"

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace workers {

namespace {

int envInt(const char* name, int fallback, int lo, int hi)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::clamp(std::stoi(v), lo, hi);
    } catch (...) {
        LOG_WARN << name << " is not a number; using " << fallback;
        return fallback;
    }
}

struct PoolImpl {
    std::string                       name;
    std::vector<std::thread>          threads;
    std::deque<std::function<void()>> queue;
    std::mutex                        mu;
    std::condition_variable           cv;
    std::size_t                       capacity = 0;
    std::size_t                       active   = 0;
    std::atomic<std::size_t>          rejected{0};
    bool                              stopping = false;

    void spawn(std::size_t threadCount, std::size_t backlog)
    {
        capacity = backlog;
        threads.reserve(threadCount);
        for (std::size_t i = 0; i < threadCount; ++i) {
            threads.emplace_back([this] { workerLoop(); });
        }
        LOG_INFO << "worker pool '" << name << "': " << threadCount
                 << " threads, backlog " << backlog;
    }

    void workerLoop()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [this] { return stopping || !queue.empty(); });
                // Drain before exiting: a job already accepted has a client
                // waiting on a response, and dropping it would hang that
                // request until its own timeout rather than answering it.
                if (queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
                ++active;
            }

            // A throwing job must not take the worker thread down with it,
            // or the pool silently shrinks to zero over time and every
            // subsequent submit() queues forever.
            try {
                job();
            } catch (const std::exception& e) {
                LOG_ERROR << "worker pool '" << name
                          << "' job threw: " << e.what();
            } catch (...) {
                LOG_ERROR << "worker pool '" << name << "' job threw";
            }

            {
                std::lock_guard<std::mutex> lk(mu);
                --active;
            }
        }
    }

    bool push(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopping) return false;
            if (queue.size() >= capacity) {
                rejected.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            queue.push_back(std::move(job));
        }
        cv.notify_one();
        return true;
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopping) return;
            stopping = true;
        }
        cv.notify_all();
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        threads.clear();
    }
};

PoolImpl g_auth;
PoolImpl g_media;
std::atomic<bool> g_started{false};

PoolImpl& impl(Pool p) { return p == Pool::Auth ? g_auth : g_media; }

} // namespace

const char* poolName(Pool pool)
{
    return pool == Pool::Auth ? "auth" : "media";
}

void start()
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    const auto hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    // Auth: 4 concurrent Argon2id hashes is 256 MiB at libsodium's
    // INTERACTIVE memlimit, and ~25 logins/s at the measured 167 ms each —
    // far above what the rate limiter lets through anyway (5 logins per IP
    // per minute). Raising it buys throughput nobody is asking for and
    // costs memory linearly.
    g_auth.name = "auth";
    g_auth.spawn(static_cast<std::size_t>(envInt("BLOG_AUTH_WORKERS",
                                                 std::min(4, hw), 1, 64)),
                 64);

    // Media: libvips parallelises inside a single job, so extra workers
    // mostly contend rather than add throughput. The backlog is short
    // because image jobs are slow — a long queue here just converts a
    // fast 503 into a slow one.
    g_media.name = "media";
    g_media.spawn(static_cast<std::size_t>(envInt("BLOG_MEDIA_WORKERS",
                                                  std::min(2, hw), 1, 32)),
                  16);
}

void stop()
{
    if (!g_started.load()) return;
    g_auth.shutdown();
    g_media.shutdown();
    g_started.store(false);
}

bool submit(Pool pool, std::function<void()> job)
{
    if (!g_started.load()) return false;
    return impl(pool).push(std::move(job));
}

bool offload(Pool pool,
             const std::function<void(const drogon::HttpResponsePtr&)>& callback,
             std::function<void()> job)
{
    if (submit(pool, std::move(job))) return true;

    LOG_WARN << "worker pool '" << poolName(pool)
             << "' saturated; shedding request";

    Json::Value body;
    body["error"] = "Server busy, please retry shortly";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    resp->addHeader("Retry-After", "2");
    callback(resp);
    return false;
}

Stats stats(Pool pool)
{
    auto& p = impl(pool);
    std::lock_guard<std::mutex> lk(p.mu);
    return Stats{p.threads.size(), p.queue.size(), p.active, p.capacity,
                 p.rejected.load(std::memory_order_relaxed)};
}

} // namespace workers
