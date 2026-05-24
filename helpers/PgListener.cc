#include "PgListener.h"

#include <libpq-fe.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/select.h>
#include <thread>

namespace pglisten {

namespace {

std::thread       g_thread;
std::atomic<bool> g_running{false};

std::string envOr(const char* k, const char* fallback)
{
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

std::string buildConnInfo()
{
    // libpq's PQconnectdb tolerates a key=value string. Build it from the
    // same env vars main.cc expands into Drogon's config so the listener
    // and the request handlers always agree on the target DB.
    auto esc = [](const std::string& s) {
        // libpq escapes via backslash inside single quotes; we wrap each
        // value in '…' and replace embedded quotes/backslashes.
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\\' || c == '\'') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('\'');
        return out;
    };

    return "host="     + esc(envOr("DB_HOST",     "127.0.0.1"))
         + " port="    + esc(envOr("DB_PORT",     "5432"))
         + " dbname="  + esc(envOr("DB_NAME",     "blog_db"))
         + " user="    + esc(envOr("DB_USER",     "blog_user"))
         + " password="+ esc(envOr("DB_PASSWORD", ""))
         + " application_name='drogon-blog-listener'";
}

void loop(const std::string& channel, const std::string& connInfo, Callback cb)
{
    using namespace std::chrono_literals;

    while (g_running.load()) {
        // Open / re-open the connection. We do this in a loop so transient
        // PG restarts don't kill the listener — we just reconnect with a
        // backoff and resume.
        PGconn* conn = PQconnectdb(connInfo.c_str());
        if (PQstatus(conn) != CONNECTION_OK) {
            LOG_ERROR << "PgListener connect failed: " << PQerrorMessage(conn);
            PQfinish(conn);
            std::this_thread::sleep_for(2s);
            continue;
        }

        const std::string listenSql = "LISTEN " + channel;
        PGresult* r = PQexec(conn, listenSql.c_str());
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            LOG_ERROR << "PgListener LISTEN failed: " << PQerrorMessage(conn);
            PQclear(r);
            PQfinish(conn);
            std::this_thread::sleep_for(2s);
            continue;
        }
        PQclear(r);
        LOG_INFO << "PgListener listening on channel \"" << channel << "\"";

        const int sock = PQsocket(conn);
        while (g_running.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            timeval tv{ /*sec*/ 1, /*usec*/ 0 };
            int sel = select(sock + 1, &rfds, nullptr, nullptr, &tv);
            if (sel < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR << "PgListener select() failed: " << std::strerror(errno);
                break;
            }
            if (!g_running.load()) break;

            if (PQconsumeInput(conn) == 0) {
                LOG_ERROR << "PgListener PQconsumeInput: " << PQerrorMessage(conn);
                break;
            }
            while (PGnotify* n = PQnotifies(conn)) {
                try {
                    cb(n->relname ? std::string(n->relname) : std::string{},
                       n->extra   ? std::string(n->extra)   : std::string{});
                } catch (const std::exception& e) {
                    LOG_ERROR << "PgListener callback threw: " << e.what();
                }
                PQfreemem(n);
            }
        }

        PQfinish(conn);
        if (g_running.load()) {
            LOG_WARN << "PgListener reconnecting in 2s";
            std::this_thread::sleep_for(2s);
        }
    }
}

} // namespace

void start(const std::string& channel, Callback cb, const std::string& connInfo)
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;

    std::string ci = connInfo.empty() ? buildConnInfo() : connInfo;
    g_thread = std::thread(loop, channel, std::move(ci), std::move(cb));
}

void stop()
{
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

} // namespace pglisten
