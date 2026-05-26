#include "Flags.h"
#include "Security.h"        // for sha256Hex

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace flags {

namespace {

// Snapshot of the table. shared_ptr<const T> + atomic swap pattern:
// reads take a shared_ptr copy (cheap, lock-free) and read the const
// map without contending with writers. Writers build a fresh map
// under g_swap, then publish via std::atomic_store. This is the
// standard "publish a new snapshot" idiom for read-mostly caches.
using FlagMap = std::unordered_map<std::string, Flag>;

std::mutex                      g_swap;
std::shared_ptr<const FlagMap>  g_snapshot;

std::shared_ptr<const FlagMap> snapshot()
{
    auto s = std::atomic_load(&g_snapshot);
    if (s) return s;
    // Pre-install state — empty map, fail-closed.
    return std::make_shared<FlagMap>();
}

// 64-bit truncation of sha256(key || ":" || userId) interpreted as
// a hex string. We only need the low 32 bits for the modulus, but
// sha256Hex already lives in helpers/Security.cc and the cost of the
// hash dominates anything we'd save by switching algorithms. Using a
// dedicated salt-style separator (":") makes (key="a", uid="11") and
// (key="a1", uid="1") hash distinctly.
int bucketOf(const std::string& key, int userId)
{
    const std::string input = key + ":" + std::to_string(userId);
    const std::string hex   = security::sha256Hex(input);
    // First 8 hex chars = 4 high bytes = 32 bits. Modulus 100 is
    // well-distributed at this width.
    if (hex.size() < 8) return 0;
    const std::string prefix = hex.substr(0, 8);
    unsigned long n = std::stoul(prefix, nullptr, 16);
    return static_cast<int>(n % 100);
}

bool evaluateLocked(const Flag& f, int userId)
{
    if (!f.enabled) return false;
    if (f.rollout_percent <= 0)   return false;
    if (f.rollout_percent >= 100) return true;
    return bucketOf(f.key, userId) < f.rollout_percent;
}

void doLoad()
{
    auto db = drogon::app().getDbClient();
    if (!db) {
        LOG_WARN << "flags: no db client; cache stays empty.";
        return;
    }
    try {
        const auto r = db->execSqlSync(
            "SELECT key, description, enabled, rollout_percent "
            "FROM feature_flags");
        auto fresh = std::make_shared<FlagMap>();
        fresh->reserve(r.size());
        for (const auto& row : r) {
            Flag f;
            f.key             = row["key"].as<std::string>();
            f.description     = row["description"].as<std::string>();
            f.enabled         = row["enabled"].as<bool>();
            f.rollout_percent = row["rollout_percent"].as<int>();
            (*fresh)[f.key] = std::move(f);
        }
        std::atomic_store(&g_snapshot,
                          std::shared_ptr<const FlagMap>(std::move(fresh)));
        LOG_INFO << "flags: loaded " << r.size() << " entries.";
    } catch (const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "flags: load failed: " << e.base().what();
    }
}

} // namespace

bool install()
{
    // Drogon initializes its DB client pool inside run(), not at
    // config-load time. Calling getDbClient() now would assert
    // (dbClientsMap_ is empty). Schedule the first load onto the
    // main event loop so it fires the moment the loop starts pumping,
    // by which point the pool is ready.
    drogon::app().getLoop()->queueInLoop([] {
        std::lock_guard<std::mutex> lk(g_swap);
        doLoad();
    });
    return true;
}

void reload()
{
    std::lock_guard<std::mutex> lk(g_swap);
    doLoad();
}

bool isEnabled(const std::string& key, int userId)
{
    auto snap = snapshot();
    auto it = snap->find(key);
    if (it == snap->end()) return false;
    return evaluateLocked(it->second, userId);
}

std::optional<bool> lookup(const std::string& key, int userId)
{
    auto snap = snapshot();
    auto it = snap->find(key);
    if (it == snap->end()) return std::nullopt;
    return evaluateLocked(it->second, userId);
}

std::vector<EvalResult> evaluateAll(int userId)
{
    auto snap = snapshot();
    std::vector<EvalResult> out;
    out.reserve(snap->size());
    // entry.second is the Flag; the key lives in flag.key already so
    // the structured binding's first element would be redundant —
    // avoid it because older cppcheck flags `[_, f]` as unused-variable.
    for (const auto& entry : *snap) {
        const auto& f = entry.second;
        out.push_back({f.key, evaluateLocked(f, userId)});
    }
    return out;
}

} // namespace flags
