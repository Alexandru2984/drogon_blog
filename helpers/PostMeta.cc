#include "PostMeta.h"
#include "Security.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_set>

using namespace drogon;
using namespace drogon::orm;

namespace post_meta {
namespace {

inline char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Per-process salt for the anonymous viewer hash. Read from the environment
// so a deployment can pin it (making dedup survive a restart) and generated
// randomly otherwise (so a fresh process cannot be correlated with an old
// one). Either way it never leaves the server.
const std::string& viewSalt()
{
    static const std::string salt = [] {
        if (const char* env = std::getenv("BLOG_VIEW_SALT"); env && *env)
            return std::string(env);
        // No configured salt: derive one from the process start. Views
        // recorded before a restart simply stop deduplicating against those
        // recorded after, which overcounts slightly and leaks nothing.
        return security::sha256Hex(
            std::to_string(reinterpret_cast<std::uintptr_t>(&salt)) + ":" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    }();
    return salt;
}

} // namespace

// ------------------------------------------------------------------ tags

std::string slugify(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    bool lastWasHyphen = true;   // true so a leading hyphen is swallowed

    for (const char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(lower(c));
            lastWasHyphen = false;
        } else if (c == ' ' || c == '-' || c == '_' || c == '.' || c == '/') {
            // Separators collapse into a single hyphen. Everything else
            // (punctuation, emoji, control bytes, the non-ASCII part of a
            // UTF-8 sequence) is dropped: the slug is an identifier, and
            // the label preserves what the author actually wrote.
            if (!lastWasHyphen && !out.empty()) {
                out.push_back('-');
                lastWasHyphen = true;
            }
        }
        if (out.size() >= 39) break;
    }

    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::vector<Tag> parseTags(const Json::Value& value)
{
    std::vector<std::string> rawItems;

    if (value.isArray()) {
        for (const auto& v : value) {
            if (v.isString()) rawItems.push_back(v.asString());
        }
    } else if (value.isString()) {
        // A single comma-separated string. A client that builds the field
        // from a text input will send this, and refusing it would be a
        // failure the user cannot diagnose from the error message.
        std::stringstream ss(value.asString());
        std::string item;
        while (std::getline(ss, item, ',')) rawItems.push_back(item);
    }

    std::vector<Tag> out;
    std::unordered_set<std::string> seen;
    for (const auto& raw : rawItems) {
        const std::string slug = slugify(raw);
        if (slug.empty()) continue;             // punctuation-only: skip, don't fail
        if (!seen.insert(slug).second) continue;  // duplicate after folding

        // Trim the label for display; it is what the reader sees.
        std::string label = raw;
        const auto b = label.find_first_not_of(" \t\r\n");
        const auto e = label.find_last_not_of(" \t\r\n");
        label = (b == std::string::npos) ? slug : label.substr(b, e - b + 1);
        if (label.size() > 40) label.resize(40);

        out.push_back(Tag{slug, label});
        if (out.size() >= kMaxTagsPerPost) break;
    }
    return out;
}

bool syncPostTags(const DbClientPtr& db, int postId, const std::vector<Tag>& tags)
{
    try {
        // Drop associations first, then re-add. Simpler than diffing, and
        // the row count is bounded by kMaxTagsPerPost so there is nothing
        // to gain from being cleverer.
        db->execSqlSync("DELETE FROM post_tags WHERE post_id = $1", postId);

        for (const auto& t : tags) {
            // ON CONFLICT DO UPDATE rather than DO NOTHING: DO NOTHING
            // returns no row, so we would need a second SELECT to learn the
            // id. Updating slug to itself is a no-op that still returns it.
            // The label is deliberately NOT overwritten — the first author
            // to use a tag sets how it is displayed, and letting every
            // subsequent post rewrite it means the tag's name flickers.
            auto r = db->execSqlSync(
                "INSERT INTO tags (slug, label) VALUES ($1, $2) "
                "ON CONFLICT (slug) DO UPDATE SET slug = EXCLUDED.slug "
                "RETURNING id",
                t.slug, t.label);
            if (r.empty()) continue;
            const int tagId = r[0]["id"].as<int>();

            db->execSqlSync(
                "INSERT INTO post_tags (post_id, tag_id) VALUES ($1, $2) "
                "ON CONFLICT DO NOTHING",
                postId, tagId);
        }
        return true;
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (syncPostTags, post " << postId << "): "
                  << e.base().what();
        return false;
    }
}

Json::Value tagsForPost(const DbClientPtr& db, int postId)
{
    Json::Value out(Json::arrayValue);
    try {
        auto r = db->execSqlSync(
            "SELECT t.slug, t.label FROM post_tags pt "
            "JOIN tags t ON t.id = pt.tag_id "
            "WHERE pt.post_id = $1 ORDER BY t.label",
            postId);
        for (const auto& row : r) {
            Json::Value tag;
            tag["slug"]  = row["slug"].as<std::string>();
            tag["label"] = row["label"].as<std::string>();
            out.append(tag);
        }
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (tagsForPost): " << e.base().what();
    }
    return out;
}

const char kTagsJsonColumn[] =
    "COALESCE((SELECT json_agg(json_build_object('slug', tj.slug, 'label', tj.label) "
    "                          ORDER BY tj.label) "
    "            FROM post_tags ptj "
    "            JOIN tags tj ON tj.id = ptj.tag_id "
    "           WHERE ptj.post_id = p.id), '[]'::json)::text AS tags_json";

Json::Value tagsFromJson(const std::string& text)
{
    Json::Value out(Json::arrayValue);
    if (text.empty()) return out;

    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value parsed;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &parsed, &errors)) {
        LOG_ERROR << "tagsFromJson: " << errors;
        return out;
    }
    if (!parsed.isArray()) return out;
    return parsed;
}

bool isDraft(const DbClientPtr& db, int postId)
{
    try {
        auto r = db->execSqlSync(
            "SELECT published_at IS NULL AS draft FROM posts WHERE id = $1", postId);
        return !r.empty() && r[0]["draft"].as<bool>();
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (isDraft): " << e.base().what();
        // Fail towards "published": reporting a live post as a draft would
        // have the UI offer to publish something already public.
        return false;
    }
}

// ---------------------------------------------------------- reading time

int estimateReadingMinutes(const std::string& markdown)
{
    // Word count on whitespace. Not exact for CJK (which has no spaces) or
    // for code blocks (which are read more slowly than prose), but the
    // number is displayed as "4 min read" — a rounded estimate whose job is
    // to distinguish a two-minute note from a twenty-minute essay.
    std::size_t words = 0;
    bool inWord = false;
    for (const char c : markdown) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!space && !inWord) { ++words; inWord = true; }
        else if (space)        { inWord = false; }
    }

    constexpr std::size_t kWordsPerMinute = 200;
    const auto minutes = (words + kWordsPerMinute - 1) / kWordsPerMinute;
    return static_cast<int>(std::max<std::size_t>(1, minutes));
}

std::string makeExcerpt(const std::string& markdown, std::size_t maxChars)
{
    std::string text;
    text.reserve(std::min(markdown.size(), maxChars * 3));

    bool inFence = false;
    std::istringstream lines(markdown);
    std::string line;

    while (std::getline(lines, line) && text.size() < maxChars * 2) {
        // Fenced code is not prose and reads as noise in a preview.
        if (line.rfind("```", 0) == 0 || line.rfind("~~~", 0) == 0) {
            inFence = !inFence;
            continue;
        }
        if (inFence) continue;

        std::string clean;
        clean.reserve(line.size());
        bool skipLeading = true;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            // Leading heading hashes, quote markers and list bullets.
            if (skipLeading && (c == '#' || c == '>' || c == '-' ||
                                c == '*' || c == '+' || c == ' ')) {
                continue;
            }
            skipLeading = false;
            // Emphasis and inline-code markers, and the bracket syntax of a
            // link — the link text survives, the URL does not, because
            // "[docs](https://…)" in a card preview is unreadable.
            if (c == '*' || c == '_' || c == '`' || c == '[' || c == ']') continue;
            if (c == '(') {
                const auto close = line.find(')', i);
                if (close != std::string::npos) { i = close; continue; }
            }
            clean.push_back(c);
        }

        // Trim, then join paragraphs with a single space.
        const auto b = clean.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        const auto e = clean.find_last_not_of(" \t\r");
        clean = clean.substr(b, e - b + 1);
        if (clean.empty()) continue;

        if (!text.empty()) text.push_back(' ');
        text += clean;
    }

    if (text.size() <= maxChars) return text;

    // Cut on a word boundary so the ellipsis does not land mid-word.
    auto cut = text.rfind(' ', maxChars);
    if (cut == std::string::npos || cut < maxChars / 2) cut = maxChars;
    text.resize(cut);
    while (!text.empty() && (text.back() == ' ' || text.back() == ',')) text.pop_back();
    return text + "…";
}

// ----------------------------------------------------------------- views

std::string viewerKey(const HttpRequestPtr& req, int userIdOrZero)
{
    if (userIdOrZero > 0) return "u:" + std::to_string(userIdOrZero);

    const std::string ip = security::resolveClientIp(
        req->getPeerAddr().toIp(),
        req->getHeader(security::clientIpHeader()));
    const std::string ua = req->getHeader("User-Agent");

    // Truncated to 24 hex characters (96 bits): far past collision concerns
    // at this scale, and short enough that the stored value is obviously not
    // a full hash someone might try to reverse offline.
    return "a:" + security::sha256Hex(viewSalt() + "|" + ip + "|" + ua).substr(0, 24);
}

namespace {

// One statement: the CTE inserts the dedup row (or does nothing if this
// viewer already looked today), and the UPDATE runs only when the insert
// produced a row. Two statements would let a crash between them leave the
// counter and the dedup table disagreeing.
//
// Shared by the sync and async entry points so the two cannot drift.
const char kRecordViewSql[] =
    "WITH ins AS ("
    "  INSERT INTO post_views (post_id, viewer) VALUES ($1, $2) "
    "  ON CONFLICT DO NOTHING RETURNING 1"
    "), bump AS ("
    "  UPDATE posts SET view_count = view_count + 1 "
    "   WHERE id = $1 AND EXISTS (SELECT 1 FROM ins) "
    "  RETURNING view_count"
    ") "
    "SELECT COALESCE((SELECT view_count FROM bump), "
    "                (SELECT view_count FROM posts WHERE id = $1)) AS n";

} // namespace

long long recordView(const DbClientPtr& db,
                     int postId,
                     const std::string& viewer,
                     long long fallback)
{
    try {
        auto r = db->execSqlSync(kRecordViewSql, postId, viewer);
        if (!r.empty() && !r[0]["n"].isNull())
            return r[0]["n"].as<long long>();
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (recordView, post " << postId << "): "
                  << e.base().what();
    }
    return fallback;
}

void recordViewAsync(const DbClientPtr& db,
                     int postId,
                     const std::string& viewer,
                     long long fallback,
                     std::function<void(long long)>&& cb)
{
    auto once = std::make_shared<std::function<void(long long)>>(std::move(cb));
    db->execSqlAsync(
        kRecordViewSql,
        [once, fallback](const Result& r) {
            if (!r.empty() && !r[0]["n"].isNull())
                (*once)(r[0]["n"].as<long long>());
            else
                (*once)(fallback);
        },
        [once, fallback, postId](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (recordViewAsync, post " << postId << "): "
                      << e.base().what();
            (*once)(fallback);
        },
        postId, viewer);
}

} // namespace post_meta
