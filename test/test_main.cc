#define DROGON_TEST_MAIN
#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <sodium.h>

#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string env(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// Resets the test database to a known empty state by replaying schema.sql
// over a freshly dropped public schema. Shelled out to psql for parity with
// what `docker compose up` does at startup.
bool resetDatabase()
{
    const std::string host = env("TEST_DB_HOST",     "127.0.0.1");
    const std::string port = env("TEST_DB_PORT",     "5432");
    const std::string user = env("TEST_DB_USER",     "blog_user");
    const std::string pass = env("TEST_DB_PASSWORD", "");
    const std::string name = env("TEST_DB_NAME",     "blog_test_db");
    const std::string schemaPath = env("TEST_SCHEMA_PATH", "../schema.sql");

    std::ostringstream cmd;
    cmd << "PGPASSWORD='" << pass << "' psql"
        << " -h " << host
        << " -p " << port
        << " -U " << user
        << " -d " << name
        << " -v ON_ERROR_STOP=1"
        << " -c 'DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public;"
           "    GRANT ALL ON SCHEMA public TO " << user << ";'"
        << " -f " << schemaPath
        << " >/dev/null";

    return std::system(cmd.str().c_str()) == 0;
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

    if (!resetDatabase()) {
        std::cerr << "Failed to reset test database. "
                     "Ensure blog_test_db exists and TEST_DB_* env vars are set.\n";
        return 2;
    }

    app().loadConfigJson(buildConfig());

    std::promise<void> ready;
    auto readyFut = ready.get_future();

    std::thread loop([&]() {
        app().getLoop()->queueInLoop([&ready]() { ready.set_value(); });
        app().run();
    });
    readyFut.get();

    int status = test::run(argc, argv);

    app().getLoop()->queueInLoop([]() { app().quit(); });
    loop.join();
    return status;
}
