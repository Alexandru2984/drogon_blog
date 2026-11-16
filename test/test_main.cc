#define DROGON_TEST_MAIN
#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <sodium.h>

#include "../controllers/MessageWebSocket.h"
#include "../helpers/AccessLog.h"
#include "../helpers/Ops.h"
#include "../helpers/PublicPages.h"
#include "../helpers/Security.h"
#include "../helpers/Roles.h"
#include "../helpers/Sessions.h"
#include "../helpers/Workers.h"

#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>

namespace {

std::string env(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// Called before any worker threads exist. Pass arguments directly to exec,
// never through a shell: DB passwords and filesystem paths are opaque data.
bool runProcess(const std::vector<std::string>& args,
                const std::vector<std::pair<std::string, std::string>>& vars)
{
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        for (const auto& item : vars) setenv(item.first.c_str(), item.second.c_str(), 1);
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool resetDatabase()
{
    const std::string host = env("TEST_DB_HOST", "127.0.0.1");
    const std::string port = env("TEST_DB_PORT", "5432");
    const std::string user = env("TEST_DB_USER", "blog_user");
    const std::string pass = env("TEST_DB_PASSWORD", "");
    const std::string name = env("TEST_DB_NAME", "blog_test_db");
    const std::string migrations = env("TEST_MIGRATIONS_DIR", "../migrations");

    // No override escape hatch: this binary always destroys its target schema.
    const std::string suffix = "_test_db";
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        std::cerr << "Refusing schema reset: TEST_DB_NAME must end in _test_db\n";
        return false;
    }
    if (!runProcess({"psql", "-X", "-h", host, "-p", port, "-U", user,
                     "-d", name, "-v", "ON_ERROR_STOP=1", "-q", "-c",
                     "DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public;"},
                    {{"PGPASSWORD", pass}})) return false;

    return runProcess({"sh", migrations + "/apply.sh"},
                      {{"DB_HOST", host}, {"DB_PORT", port}, {"DB_NAME", name},
                       {"DB_USER", user}, {"DB_PASSWORD", pass}});
}

Json::Value buildConfig()
{
    Json::Value cfg;

    Json::Value listener;
    listener["address"] = "127.0.0.1";
    listener["port"]    = std::stoi(env("TEST_PORT", "18092"));
    listener["https"]   = false;
    cfg["listeners"].append(listener);

    Json::Value dbc;
    dbc["name"]    = "default";
    dbc["rdbms"]   = "postgresql";
    dbc["host"]    = env("TEST_DB_HOST",     "127.0.0.1");
    dbc["port"]    = std::stoi(env("TEST_DB_PORT", "5432"));
    dbc["dbname"]  = env("TEST_DB_NAME",     "blog_test_db");
    dbc["user"]    = env("TEST_DB_USER",     "blog_user");
    dbc["passwd"]  = env("TEST_DB_PASSWORD", "");
    dbc["is_fast"] = false;
    dbc["number_of_connections"] = 4;
    cfg["db_clients"].append(dbc);

    Json::Value app;
    app["number_of_threads"]  = 2;
    app["document_root"] = BLOG_TEST_SOURCE_DIR "/fixtures";
    app["enable_session"]     = true;
    app["session_same_site"]  = "Lax";
    app["session_cookie_key"] = "JSESSIONID";
    app["log"]["log_level"]   = "WARN";
    cfg["app"] = app;

    return cfg;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace drogon;

    if (sodium_init() < 0) {
        std::cerr << "libsodium init failed\n";
        return 1;
    }

    // Install one immutable process-wide key before Drogon starts its worker
    // threads. Individual tests must not mutate the environment concurrently
    // while HTTP handlers are encrypting or decrypting TOTP secrets.
    setenv("BLOG_TOTP_KEY",
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef",
           0);
    security::validateTotpKeyConfiguration();

    // Disable per-IP rate limiting for the test suite — every test originates
    // from 127.0.0.1, so the production limits would trip mid-run.
    setenv("BLOG_DISABLE_RATE_LIMIT", "1", 1);

    if (!resetDatabase()) {
        std::cerr << "Failed to reset test database. "
                     "Ensure blog_test_db exists and TEST_DB_* env vars are set.\n";
        return 2;
    }

    app().loadConfigJson(buildConfig());

    // Mirror production's security wiring (rate limit + CSRF + headers).
    // BLOG_DISABLE_RATE_LIMIT=1 (set above) keeps the limiter out of the way.
    security::registerAdvices();

    // Observability: structured access log + metrics ingestion + ops routes.
    access_log::install();
    ops::install();

    // /feed.xml + /preview/posts/{id}. These are read surfaces over the
    // same rows the REST API serves, so leaving them out of the harness
    // meant nothing checked that they honour the same filters — the
    // moderation tests need them present to be meaningful.
    public_pages::install("http://127.0.0.1");

    // Handlers that block (Argon2id, libvips, the synchronous 2FA queries)
    // hand their work to these pools instead of running it on an IO loop.
    // Without them every such handler sheds its request with a 503, so the
    // harness must start them exactly as main() does.
    workers::start();

    // Session registry + the advice that drops a revoked session before any
    // handler runs. main() installs this after security::registerAdvices();
    // without it here, revocation silently does nothing in tests.
    sessions::install();

    // …and the hook that carries a revocation out to the WebSocket the
    // session opened. Same line as main(), for the same reason it is a hook
    // at all: a revoked session that keeps its socket keeps receiving the
    // account's private messages. This one is easy to forget here because
    // main.cc is not part of the test binary, so production wiring only
    // exists in the harness if it is written out twice.
    sessions::setRevocationObserver(&MessageWebSocket::closeForSession);

    roles::install();

    std::promise<void> ready;
    auto readyFut = ready.get_future();

    std::thread loop([&]() {
        app().getLoop()->queueInLoop([&ready]() { ready.set_value(); });
        app().run();
    });
    readyFut.get();

    int status = test::run(argc, argv);

    // Hang up any open socket, then stop — on the loop, in that order,
    // exactly as main()'s signal handler does. Doing the shutdown from this
    // thread instead would only queue the closes and let quit() race past
    // them, leaving a live connection pinned to a loop that is trying to
    // stop.
    app().getLoop()->queueInLoop([]() {
        MessageWebSocket::shutdownAll();
        app().quit();
    });
    loop.join();
    workers::stop();
    return status;
}
