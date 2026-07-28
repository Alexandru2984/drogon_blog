#include <drogon/drogon_test.h>

#include "../helpers/Workers.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// The pools exist so that blocking work (Argon2id at ~167 ms a hash,
// libvips on an 8 MB upload) never runs on a Drogon IO loop, where it
// would stall every other connection assigned to that loop. These tests
// cover the properties the handlers depend on: work actually runs, it runs
// off the caller's thread, and a saturated pool refuses new work promptly
// instead of growing an unbounded backlog.
//
// test_main.cc calls workers::start() before the suite runs, matching main().

DROGON_TEST(Workers_PoolsAreRunning)
{
    const auto authStats  = workers::stats(workers::Pool::Auth);
    const auto mediaStats = workers::stats(workers::Pool::Media);

    CHECK(authStats.threads  > 0);
    CHECK(mediaStats.threads > 0);
    CHECK(authStats.capacity  > 0);
    CHECK(mediaStats.capacity > 0);
}

DROGON_TEST(Workers_RunsJobOffTheCallingThread)
{
    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;
    std::thread::id         ranOn{};

    const auto callerThread = std::this_thread::get_id();

    REQUIRE(workers::submit(workers::Pool::Auth, [&] {
        {
            std::lock_guard<std::mutex> lk(mu);
            ranOn = std::this_thread::get_id();
            done  = true;
        }
        cv.notify_one();
    }));

    std::unique_lock<std::mutex> lk(mu);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; }));

    // The entire point: the job did not execute inline on the submitting
    // thread. If it had, offloading would be a no-op and the IO loop would
    // still be blocked for the duration of the work.
    CHECK(ranOn != callerThread);
}

// A pool whose queue is full must refuse immediately. Accepting work into
// an unbounded backlog would convert a fast, honest 503 into a request that
// sits for as long as the queue is deep and then answers anyway — the
// client has usually given up by then, and the memory is held the whole
// time.
DROGON_TEST(Workers_SaturatedPoolRefusesInsteadOfQueueingForever)
{
    const auto before = workers::stats(workers::Pool::Media);

    std::mutex              mu;
    std::condition_variable cv;
    bool                    release = false;

    // Block every media worker, then overfill the backlog. The blockers are
    // released through a condition variable rather than a sleep so the test
    // does not depend on timing.
    auto blocker = [&] {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return release; });
    };

    for (std::size_t i = 0; i < before.threads; ++i) {
        REQUIRE(workers::submit(workers::Pool::Media, blocker));
    }

    // Wait until every blocker has actually been picked up. Submitting them
    // only puts them on the queue; until a worker claims each one the queue
    // is not empty, and the capacity arithmetic below would be off by
    // however many are still waiting.
    for (int i = 0; i < 200; ++i) {
        const auto s = workers::stats(workers::Pool::Media);
        if (s.active == before.threads && s.queued == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(workers::stats(workers::Pool::Media).active == before.threads);
    REQUIRE(workers::stats(workers::Pool::Media).queued == 0);

    // Fill the queue exactly to capacity; each of these must be accepted.
    for (std::size_t i = 0; i < before.capacity; ++i) {
        CHECK(workers::submit(workers::Pool::Media, [] {}));
    }

    // One more than capacity must be refused, and refused synchronously.
    const auto start = std::chrono::steady_clock::now();
    const bool accepted = workers::submit(workers::Pool::Media, [] {});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(!accepted);
    CHECK(elapsed < std::chrono::milliseconds(100));
    CHECK(workers::stats(workers::Pool::Media).rejected > before.rejected);

    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();

    // Let the pool drain so a later test does not inherit a full queue.
    for (int i = 0; i < 100; ++i) {
        const auto s = workers::stats(workers::Pool::Media);
        if (s.queued == 0 && s.active == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(workers::stats(workers::Pool::Media).queued == 0);
}

// A throwing job must not take its worker thread with it. If it did, the
// pool would silently shrink on every unexpected exception until no threads
// remained and every submit() queued work that never ran.
DROGON_TEST(Workers_ThrowingJobDoesNotKillTheWorker)
{
    const auto threadsBefore = workers::stats(workers::Pool::Auth).threads;

    REQUIRE(workers::submit(workers::Pool::Auth,
                            [] { throw std::runtime_error("boom"); }));

    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;

    REQUIRE(workers::submit(workers::Pool::Auth, [&] {
        {
            std::lock_guard<std::mutex> lk(mu);
            done = true;
        }
        cv.notify_one();
    }));

    std::unique_lock<std::mutex> lk(mu);
    CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; }));
    CHECK(workers::stats(workers::Pool::Auth).threads == threadsBefore);
}
