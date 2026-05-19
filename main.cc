#include <drogon/drogon.h>
#include <json/json.h>
#include <sodium.h>
#include "helpers/AccessLog.h"
#include "helpers/EmailHelper.h"
#include "helpers/Ops.h"
#include "helpers/Security.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <cstdlib>
#include <cctype>

namespace {

void loadEnvFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key   = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        };
        trim(key); trim(value);

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) setenv(key.c_str(), value.c_str(), 1);
    }
}

// Expands ${VAR_NAME} occurrences using the current process env.
std::string expandEnv(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{')
        {
            auto end = s.find('}', i + 2);
            if (end != std::string::npos)
            {
                std::string name = s.substr(i + 2, end - (i + 2));
                const char* v = std::getenv(name.c_str());
                if (v) out.append(v);
                i = end + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

} // namespace

int main()
{
    std::cout << "Starting Drogon blog server..." << std::endl;

    loadEnvFile("./.env");

    std::ifstream cfg("./config.json");
    if (!cfg)
    {
        std::cerr << "Error: config.json not found" << std::endl;
        return 1;
    }
    std::stringstream raw;
    raw << cfg.rdbuf();
    std::string expanded = expandEnv(raw.str());

    Json::CharReaderBuilder builder;
    builder["allowComments"]            = true;
    builder["allowTrailingCommas"]      = true;
    builder["allowSingleQuotes"]        = true;
    builder["allowSpecialFloats"]       = true;
    builder["skipBom"]                  = true;

    Json::Value cfgJson;
    std::string errs;
    std::istringstream iss(expanded);
    if (!Json::parseFromStream(builder, iss, &cfgJson, &errs))
    {
        std::cerr << "Failed to parse config.json: " << errs << std::endl;
        return 1;
    }

    if (sodium_init() < 0)
    {
        std::cerr << "libsodium init failed" << std::endl;
        return 1;
    }

    try
    {
        drogon::app().loadConfigJson(cfgJson);
        std::cout << "Config loaded successfully!" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error loading config: " << e.what() << std::endl;
        return 1;
    }

    EmailHelper::start();
    drogon::app().getLoop()->runOnQuit([] { EmailHelper::stop(); });

    // Rate limiting, CSRF (double-submit), and response security headers.
    security::registerAdvices();

    // Structured JSON access log + request-ID propagation + metrics ingestion.
    access_log::install();

    // /healthz, /readyz, /metrics.
    ops::install();

    std::cout << "Drogon listening (see config for port)..." << std::endl;
    drogon::app().run();
    return 0;
}
