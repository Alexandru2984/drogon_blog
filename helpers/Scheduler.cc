#include "Scheduler.h"

#include "Notifications.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace scheduler {

namespace {

std::thread       g_thread;
std::atomic<bool> g_running{false};

} // namespace

std::size_t publishDueNow()
{
    auto db = drogon::app().getDbClient();
    if (!db) return 0;   // pool not up yet (e.g. a sweep racing startup)

    try {
        // One statement flips every due post atomically. scheduled_at is
        // cleared in the same UPDATE so a post can never be published twice,
        // even if two sweeps overlap: the second sees published_at already set
        // and the WHERE excludes it.
        const auto r = db->execSqlSync(
            "UPDATE posts "
            "   SET published_at = now(), scheduled_at = NULL "
            " WHERE published_at IS NULL "
            "   AND scheduled_at IS NOT NULL "
            "   AND scheduled_at <= now() "
            "RETURNING id, user_id");

        for (const auto& row : r) {
            // Followers hear about it now, at publish time — the notification
            // createPost deliberately withheld while it was scheduled.
            notifications::emitNewPostToFollowers(
                db, row["user_id"].as<int>(), row["id"].as<int>());
            LOG_INFO << "scheduler: published scheduled post "
                     << row["id"].as<int>();
        }
        return r.size();
    } catch (const std::exception& e) {
        // A failed sweep is not fatal: the rows stay scheduled and the next
        // sweep retries. Logged so a persistent failure is visible.
        LOG_ERROR << "scheduler: publish sweep failed: " << e.what();
        return 0;
    }
}

namespace {

void loop()
{
    using namespace std::chrono_literals;
    while (g_running.load()) {
        // Sleep in one-second slices so stop() is responsive rather than
        // blocking a shutdown for up to a full minute.
        for (int i = 0; i < 60 && g_running.load(); ++i)
            std::this_thread::sleep_for(1s);
        if (!g_running.load()) break;
        publishDueNow();
    }
}

} // namespace

void start()
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    g_thread = std::thread(loop);
    LOG_INFO << "scheduler: started (scheduled-post sweep every 60s)";
}

void stop()
{
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

} // namespace scheduler
