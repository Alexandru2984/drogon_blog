#include "PostController.h"
#include "../models/Posts.h"
#include "../models/Users.h"
#include "../models/Likes.h"
#include "../helpers/AuditLog.h"
#include "../helpers/HttpCache.h"
#include "../helpers/Markdown.h"
#include "../helpers/Notifications.h"
#include "../helpers/PostMeta.h"
#include "../helpers/Security.h"
#include "../helpers/ImageProcessor.h"
#include "../helpers/Workers.h"

#include <filesystem>
#include <system_error>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

constexpr int kDefaultPageSize = 20;
constexpr int kMaxPageSize     = 50;

// Title bound is generous (5 KiB) — enough for legitimate long headers
// without giving an adversary room to chain arbitrary markdown into it.
// Content bound matches the parser cap in helpers/Markdown.h so we
// reject at the API boundary with a clear 413 instead of returning an
// empty render later.
constexpr std::size_t kMaxTitleBytes   = std::size_t{5} * 1024;
constexpr std::size_t kMaxContentBytes = std::size_t{100} * 1024;

int clampLimit(const std::string& raw)
{
    if (raw.empty()) return kDefaultPageSize;
    try {
        int v = std::stoi(raw);
        if (v < 1) return kDefaultPageSize;
        return std::min(v, kMaxPageSize);
    } catch (...) { return kDefaultPageSize; }
}

int parseCursor(const std::string& raw)
{
    if (raw.empty()) return 0;
    try {
        int v = std::stoi(raw);
        return v > 0 ? v : 0;
    } catch (...) { return 0; }
}

// The column list every feed-shaped listing selects, tags included, up to
// and including the FROM/JOIN clauses — the caller appends its own WHERE
// and ORDER BY. Sharing it keeps the four listings emitting the same post
// shape, and keeps the tags in the same round trip as the rows: see the
// deadlock note on post_meta::kTagsJsonColumn for why a listing driven by
// execSqlAsync must not follow up with a second query.
const std::string& kFeedSelect()
{
    static const std::string sql =
        "SELECT p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
        "       p.published_at, p.reading_minutes, p.excerpt, p.view_count, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image, " +
        std::string(post_meta::kTagsJsonColumn) + " "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id ";
    return sql;
}

} // namespace

void PostController::getAllPosts(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto dbClient = drogon::app().getDbClient();

    // Cursor-based pagination. `before` is the id of the oldest post the
    // client already has — we return rows strictly older than it. IDs are
    // monotonic (IDENTITY) so ordering by id DESC matches created_at DESC
    // and gives a stable cursor.
    const int  limit  = clampLimit(req->getParameter("limit"));
    const int  cursor = parseCursor(req->getParameter("before"));

    // Moderator-hidden posts leave the feed. Every read path needs this
    // predicate; one that misses it makes hiding cosmetic.
    //
    // published_at IS NOT NULL is the draft filter, and it belongs here for
    // the same reason: a draft that leaks into the public feed is the
    // failure mode drafts exist to prevent.
    static const std::string kSqlWithCursor =
        kFeedSelect() +
        "WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL AND p.id < $1 "
        "ORDER BY p.id DESC "
        "LIMIT $2";
    static const std::string kSqlFirstPage =
        kFeedSelect() +
        "WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        "ORDER BY p.id DESC "
        "LIMIT $1";

    auto onOk = [callback, req, limit, cursor](const Result& r) {
        // ETag from the page contents: max(updated_at) inside this page
        // + row count + the pagination keys that defined the page. Any
        // INSERT/UPDATE that lands inside the cursor window will bump
        // max(updated_at); rows leaving / entering due to creation will
        // bump the count or the max id; cache-bypassing parameter
        // changes get distinct tags. We don't include the JOIN'd author
        // fields — those don't change without bumping posts.updated_at
        // because the JOIN is read-only.
        std::int64_t maxTs = 0;
        int64_t      maxId = 0;
        int64_t      minId = 0;
        for (const auto& row : r) {
            const auto id  = row["id"].as<int64_t>();
            const auto ts  = http_cache::parseTimestampMicros(
                                 row["updated_at"].as<std::string>());
            if (ts > maxTs)   maxTs = ts;
            if (id > maxId)   maxId = id;
            if (minId == 0 || id < minId) minId = id;
        }
        const std::string etag = http_cache::makeWeakEtag({
            "posts",
            std::to_string(maxTs),
            std::to_string(maxId),
            std::to_string(static_cast<int>(r.size())),
            std::to_string(cursor),
            std::to_string(limit),
        });
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag));
            return;
        }

        Json::Value ret;
        ret["posts"] = Json::Value(Json::arrayValue);

        for (const auto& row : r) {
            Json::Value post;
            const auto id      = row["id"].as<int64_t>();
            post["id"]         = id;
            post["title"]      = row["title"].as<std::string>();
            post["content"]    = row["content"].as<std::string>();
            if (!row["content_html"].isNull())
                post["content_html"] = row["content_html"].as<std::string>();
            post["created_at"] = row["created_at"].as<std::string>();
            post["updated_at"] = row["updated_at"].as<std::string>();
            if (!row["published_at"].isNull())
                post["published_at"] = row["published_at"].as<std::string>();
            post["reading_minutes"] = row["reading_minutes"].as<int>();
            post["view_count"]      = row["view_count"].as<int64_t>();
            if (!row["excerpt"].isNull())
                post["excerpt"] = row["excerpt"].as<std::string>();

            post["tags"] = post_meta::tagsFromJson(
                row["tags_json"].as<std::string>());

            if (!row["author_id"].isNull()) {
                post["author"]["id"]       = row["author_id"].as<int64_t>();
                post["author"]["username"] = row["author_username"].as<std::string>();
                if (!row["author_profile_image"].isNull()) {
                    auto img = row["author_profile_image"].as<std::string>();
                    if (!img.empty()) post["author"]["profile_image"] = img;
                }
            }
            ret["posts"].append(post);
        }

        // Only emit a cursor when this page filled the limit; otherwise the
        // client knows there's nothing more to fetch.
        std::int64_t nextCursor = 0;
        if (static_cast<int>(r.size()) == limit && minId > 0) {
            nextCursor = minId;
            ret["next_cursor"] = static_cast<Json::Int64>(minId);
        } else {
            ret["next_cursor"] = Json::nullValue;
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag);
        // RFC 5988 Link header for cursor pagination — gives an
        // HTTP-aware client (curl --get, a generic API explorer, a
        // Cloudflare Worker) the next-page URL without making them
        // parse the JSON body. We only emit `next` because the
        // pagination is one-way (newest → oldest by id); a reverse
        // would need to remember the head cursor we started from.
        if (nextCursor > 0) {
            char link[128];
            std::snprintf(link, sizeof(link),
                "</posts?cursor=%lld&limit=%d>; rel=\"next\"",
                static_cast<long long>(nextCursor), limit);
            resp->addHeader("Link", link);
        }
        callback(resp);
    };
    auto onErr = [callback](const DrogonDbException& e) {
        LOG_ERROR << "DB Error (getAllPosts): " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch posts";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    };

    // PG infers parameter types from the prepared statement context: cursor
    // is compared against posts.id (int4) so it must bind as int32; LIMIT is
    // bigint internally so it must bind as int64. Mixing them up triggers
    // "insufficient data left in message" at parse time.
    const int64_t limit64 = limit;
    if (cursor > 0) {
        dbClient->execSqlAsync(kSqlWithCursor, onOk, onErr, cursor, limit64);
    } else {
        dbClient->execSqlAsync(kSqlFirstPage,  onOk, onErr, limit64);
    }
}

// Every tag that is actually attached to something published, with how
// many posts carry it. Tags with no visible posts are omitted rather than
// listed with a zero: a tag whose only post was deleted or hidden is not a
// thing a reader can browse to, and offering it is a dead end.
void PostController::listTags(const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto dbClient = drogon::app().getDbClient();

    static const char* kSql =
        "SELECT t.slug, t.label, count(*) AS n "
        "FROM tags t "
        "JOIN post_tags pt ON pt.tag_id = t.id "
        "JOIN posts p ON p.id = pt.post_id "
        "WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        "GROUP BY t.slug, t.label "
        "ORDER BY n DESC, t.label ASC "
        "LIMIT 200";

    dbClient->execSqlAsync(
        kSql,
        [callback, req](const Result& r) {
            Json::Value ret;
            ret["tags"] = Json::Value(Json::arrayValue);
            std::string fingerprint;
            for (const auto& row : r) {
                Json::Value tag;
                tag["slug"]  = row["slug"].as<std::string>();
                tag["label"] = row["label"].as<std::string>();
                tag["count"] = row["n"].as<int64_t>();
                ret["tags"].append(tag);
                fingerprint += row["slug"].as<std::string>();
                fingerprint += ':';
                fingerprint += std::to_string(row["n"].as<int64_t>());
                fingerprint += ';';
            }
            // The whole list is the payload, so hash it: any tag appearing,
            // disappearing or changing count yields a different tag.
            const std::string etag = http_cache::makeWeakEtag({
                "tags", security::sha256Hex(fingerprint).substr(0, 24),
            });
            if (http_cache::ifNoneMatchHit(req, etag)) {
                callback(http_cache::makeNotModified(etag));
                return;
            }
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            http_cache::applyCacheHeaders(resp, etag);
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (listTags): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Failed to fetch tags";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        });
}

void PostController::getPostsByTag(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback,
                                   const std::string &slug)
{
    // Normalise the path segment the same way a tag is normalised on write,
    // so /tags/C++/posts and /tags/c/posts reach the same place a reader
    // typing either would expect. An empty result after folding means the
    // segment contained nothing tag-shaped.
    const std::string wanted = post_meta::slugify(slug);
    if (wanted.empty()) {
        Json::Value ret;
        ret["error"] = "Unknown tag";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    const int limit = clampLimit(req->getParameter("limit"));

    // The tag filter joins post_tags/tags under their own aliases; the tag
    // list in the SELECT brings its own (ptj/tj) so the two do not collide.
    static const std::string kSql =
        kFeedSelect() +
        "JOIN post_tags pt ON pt.post_id = p.id "
        "JOIN tags t ON t.id = pt.tag_id "
        "WHERE t.slug = $1 AND p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        "ORDER BY p.id DESC "
        "LIMIT $2";

    const int64_t limit64 = limit;
    dbClient->execSqlAsync(
        kSql,
        [callback, req, wanted](const Result& r) {
            Json::Value ret;
            ret["tag"]   = wanted;
            ret["count"] = static_cast<int>(r.size());
            ret["posts"] = Json::Value(Json::arrayValue);

            for (const auto& row : r) {
                Json::Value post;
                const auto id      = row["id"].as<int64_t>();
                post["id"]         = id;
                post["title"]      = row["title"].as<std::string>();
                post["content"]    = row["content"].as<std::string>();
                if (!row["content_html"].isNull())
                    post["content_html"] = row["content_html"].as<std::string>();
                post["created_at"]      = row["created_at"].as<std::string>();
                post["updated_at"]      = row["updated_at"].as<std::string>();
                post["reading_minutes"] = row["reading_minutes"].as<int>();
                post["view_count"]      = row["view_count"].as<int64_t>();
                if (!row["excerpt"].isNull())
                    post["excerpt"] = row["excerpt"].as<std::string>();

                post["tags"] = post_meta::tagsFromJson(
                    row["tags_json"].as<std::string>());

                if (!row["author_id"].isNull()) {
                    post["author"]["id"]       = row["author_id"].as<int64_t>();
                    post["author"]["username"] = row["author_username"].as<std::string>();
                    if (!row["author_profile_image"].isNull()) {
                        auto img = row["author_profile_image"].as<std::string>();
                        if (!img.empty()) post["author"]["profile_image"] = img;
                    }
                }
                ret["posts"].append(post);
            }
            callback(HttpResponse::newHttpJsonResponse(ret));
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (getPostsByTag): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Failed to fetch posts";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        },
        wanted, limit64);
}

void PostController::getMyDrafts(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session ? session->getOptional<int>("user_id")
                             : std::optional<int>{};
    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();

    // Ordered by updated_at, not id: the draft you were last working on is
    // the one you came back for.
    static const std::string kSql =
        std::string(
        "SELECT p.id, p.title, p.content, p.created_at, p.updated_at, "
        "       p.reading_minutes, p.excerpt, ") + post_meta::kTagsJsonColumn + " "
        "FROM posts p "
        "WHERE p.user_id = $1 AND p.published_at IS NULL AND p.hidden_at IS NULL "
        "ORDER BY p.updated_at DESC "
        "LIMIT 100";

    dbClient->execSqlAsync(
        kSql,
        [callback](const Result& r) {
            Json::Value ret;
            ret["posts"] = Json::Value(Json::arrayValue);
            for (const auto& row : r) {
                Json::Value post;
                const auto id           = row["id"].as<int64_t>();
                post["id"]              = id;
                post["title"]           = row["title"].as<std::string>();
                post["content"]         = row["content"].as<std::string>();
                post["created_at"]      = row["created_at"].as<std::string>();
                post["updated_at"]      = row["updated_at"].as<std::string>();
                post["reading_minutes"] = row["reading_minutes"].as<int>();
                post["is_draft"]        = true;
                if (!row["excerpt"].isNull())
                    post["excerpt"] = row["excerpt"].as<std::string>();
                post["tags"] = post_meta::tagsFromJson(
                    row["tags_json"].as<std::string>());
                ret["posts"].append(post);
            }
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            // Never cached anywhere but this browser: a draft is private.
            resp->addHeader("Cache-Control", "private, no-store");
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (getMyDrafts): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Failed to fetch drafts";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        },
        userIdOpt.value());
}

// Render markdown to the same safe HTML publishing would produce, without
// writing anything. Authenticated and rate-limited: it runs the parser on
// caller-supplied input, so it is the one endpoint here that turns a request
// directly into CPU work.
void PostController::previewMarkdown(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session ? session->getOptional<int>("user_id")
                             : std::optional<int>{};
    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    // Generous enough for typing (the client debounces to roughly one call
    // per pause) and low enough that a script cannot use the editor as a
    // markdown-rendering service.
    if (auto rl = security::rateLimitOr429(
            "post_preview", "uid:" + std::to_string(userIdOpt.value()),
            30.0, 2.0)) {
        callback(rl);
        return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        Json::Value ret;
        ret["error"] = "Invalid JSON";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    const std::string content = (*json)["content"].asString();
    if (content.size() > kMaxContentBytes) {
        Json::Value ret;
        ret["error"] = "Content too long";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k413RequestEntityTooLarge);
        callback(resp);
        return;
    }

    // Off the event loop: rendering is unbounded CPU on caller-supplied
    // input, and a large document would otherwise stall every other request
    // on this thread. Answers 503 with Retry-After if the pool is saturated.
    workers::offload(workers::Pool::Media, callback, [content, callback]() {
        Json::Value ret;
        ret["content_html"]    = markdown::renderToSafeHtml(content);
        ret["reading_minutes"] = post_meta::estimateReadingMinutes(content);
        ret["excerpt"]         = post_meta::makeExcerpt(content);
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        // A preview is a function of the request body and nothing else, but
        // it is also the author's unpublished work.
        resp->addHeader("Cache-Control", "private, no-store");
        callback(resp);
    });
}

// Other posts sharing tags with this one, most overlap first.
//
// Tags are the only signal available: there is no click history, no
// embedding, and inventing one from title similarity would produce
// confident nonsense. When a post has no tags the honest answer is an empty
// list rather than "here are the newest posts", which is not relatedness.
void PostController::getRelatedPosts(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int postId)
{
    auto dbClient = drogon::app().getDbClient();

    // The tag list is a scalar subquery on p.id, which is in the GROUP BY,
    // so it survives the aggregation without joining the grouped rows.
    static const std::string kSql =
        std::string(
        "SELECT p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
        "       p.reading_minutes, p.excerpt, p.view_count, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image, "
        "       count(*) AS shared, ") + post_meta::kTagsJsonColumn + " "
        "  FROM post_tags mine "
        "  JOIN post_tags theirs ON theirs.tag_id = mine.tag_id "
        "                       AND theirs.post_id <> mine.post_id "
        "  JOIN posts p ON p.id = theirs.post_id "
        "  LEFT JOIN users u ON u.id = p.user_id "
        " WHERE mine.post_id = $1 "
        "   AND p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        " GROUP BY p.id, p.title, p.content, p.content_html, p.created_at, "
        "          p.updated_at, p.reading_minutes, p.excerpt, p.view_count, "
        "          u.id, u.username, u.profile_image "
        // Most tags in common first; newest breaks the tie, because between
        // two equally related posts the fresher one is the better offer.
        " ORDER BY shared DESC, p.id DESC "
        " LIMIT 5";

    dbClient->execSqlAsync(
        kSql,
        [callback, req](const Result& r) {
            Json::Value ret;
            ret["posts"] = Json::Value(Json::arrayValue);
            for (const auto& row : r) {
                Json::Value post;
                const auto id           = row["id"].as<int64_t>();
                post["id"]              = id;
                post["title"]           = row["title"].as<std::string>();
                post["content"]         = row["content"].as<std::string>();
                post["created_at"]      = row["created_at"].as<std::string>();
                post["updated_at"]      = row["updated_at"].as<std::string>();
                post["reading_minutes"] = row["reading_minutes"].as<int>();
                post["view_count"]      = row["view_count"].as<int64_t>();
                post["shared_tags"]     = row["shared"].as<int64_t>();
                if (!row["excerpt"].isNull())
                    post["excerpt"] = row["excerpt"].as<std::string>();

                post["tags"] = post_meta::tagsFromJson(
                    row["tags_json"].as<std::string>());

                if (!row["author_id"].isNull()) {
                    post["author"]["id"]       = row["author_id"].as<int64_t>();
                    post["author"]["username"] = row["author_username"].as<std::string>();
                    if (!row["author_profile_image"].isNull()) {
                        const auto img = row["author_profile_image"].as<std::string>();
                        if (!img.empty()) post["author"]["profile_image"] = img;
                    }
                }
                ret["posts"].append(post);
            }
            callback(HttpResponse::newHttpJsonResponse(ret));
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (getRelatedPosts): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Failed to fetch related posts";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        },
        postId);
}

void PostController::searchPosts(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    const std::string q = req->getParameter("q");
    if (q.empty() || q.size() > 256) {
        Json::Value ret;
        ret["error"] = "Missing or oversized query parameter `q`";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // websearch_to_tsquery tolerates raw user input (quoted phrases, OR, -negate)
    // without throwing on punctuation. ts_headline returns highlighted snippets;
    // ts_rank orders by relevance, falling back to recency as a tiebreaker.
    //
    // The content is HTML-escaped BEFORE ts_headline. ts_headline's
    // tokenizer strips a hard-coded set of HTML-ish tags (script,
    // iframe, …) but happily preserves attributes on others — e.g.
    // `<img src=x onerror=alert(1)>` survives intact and becomes
    // stored XSS the moment the SPA v-htmls the snippet. Escaping
    // up front converts every angle-bracket in the source to a text
    // entity, so the only real HTML left in the output is the
    // <mark> wrapping ts_headline inserts itself.
    static const char* kSql =
        "WITH q AS (SELECT websearch_to_tsquery('english', $1) AS query) "
        "SELECT p.id, p.title, "
        "       ts_headline('english', "
        "         replace(replace(replace(replace(replace(p.content, "
        "             '&', '&amp;'), "
        "             '<', '&lt;'), "
        "             '>', '&gt;'), "
        "             '\"', '&quot;'), "
        "             '''', '&#39;'), "
        "         q.query, "
        "         'MaxFragments=2,MaxWords=24,MinWords=8,"
        "ShortWord=2,StartSel=<mark>,StopSel=</mark>') AS snippet, "
        "       p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image, "
        "       ts_rank(p.search, q.query) AS rank "
        "FROM posts p "
        "CROSS JOIN q "
        "LEFT JOIN users u ON u.id = p.user_id "
        // published_at IS NOT NULL is not optional here. `search` is a
        // generated tsvector over every row in `posts`, drafts included, and
        // this endpoint answers anonymous callers — without the filter a
        // dictionary walk against /posts/search returns the title, author and
        // a ts_headline fragment of the body of every unpublished draft in
        // the database. Every other read path in this file already carries
        // the pair; this one was written from the tsvector outwards and
        // inherited only the hidden_at half.
        "WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        "  AND p.search @@ q.query "
        "ORDER BY rank DESC, p.created_at DESC "
        "LIMIT 50";

    auto dbClient = drogon::app().getDbClient();
    dbClient->execSqlAsync(
        kSql,
        [callback, req, q](const Result& r) {
            // ETag from (q, max(updated_at over matches), count). Adding
            // or editing a matching post changes one of those. Editing
            // a non-matching post outside the result set has no effect,
            // which is correct.
            std::int64_t maxTs = 0;
            for (const auto& row : r) {
                const auto ts = http_cache::parseTimestampMicros(
                                    row["updated_at"].as<std::string>());
                if (ts > maxTs) maxTs = ts;
            }
            const std::string etag = http_cache::makeWeakEtag({
                "search", q,
                std::to_string(maxTs),
                std::to_string(static_cast<int>(r.size())),
            });
            if (http_cache::ifNoneMatchHit(req, etag)) {
                callback(http_cache::makeNotModified(etag));
                return;
            }

            Json::Value ret;
            ret["query"] = q;
            ret["count"] = static_cast<Json::UInt>(r.size());
            ret["posts"] = Json::Value(Json::arrayValue);

            for (const auto& row : r) {
                Json::Value post;
                post["id"]         = row["id"].as<int64_t>();
                post["title"]      = row["title"].as<std::string>();
                post["snippet"]    = row["snippet"].as<std::string>();
                post["created_at"] = row["created_at"].as<std::string>();
                post["updated_at"] = row["updated_at"].as<std::string>();
                post["rank"]       = row["rank"].as<double>();

                if (!row["author_id"].isNull()) {
                    post["author"]["id"]       = row["author_id"].as<int64_t>();
                    post["author"]["username"] = row["author_username"].as<std::string>();
                    if (!row["author_profile_image"].isNull()) {
                        auto img = row["author_profile_image"].as<std::string>();
                        if (!img.empty()) post["author"]["profile_image"] = img;
                    }
                }
                ret["posts"].append(post);
            }
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            http_cache::applyCacheHeaders(resp, etag);
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (searchPosts): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Search failed";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        },
        q);
}

void PostController::getPost(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback,
                            int postId)
{
    auto dbClient = drogon::app().getDbClient();

    // A draft is visible to its author and to nobody else. Passing the
    // viewer into the query rather than fetching and then filtering keeps
    // "does this post exist" and "may you see it" the same question, so
    // there is no branch where the row is loaded and the check is forgotten.
    auto session = req->session();
    const int viewerId = session ? session->getOptional<int>("user_id").value_or(0) : 0;

    // Tags and the viewer's bookmark state ride along with the row instead
    // of being fetched afterwards. That is one round trip rather than three,
    // and — because this callback runs on a database loop thread — it is the
    // only safe shape: see post_meta::kTagsJsonColumn.
    static const std::string kSql =
        std::string(
        "SELECT p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
        "       p.published_at, p.reading_minutes, p.excerpt, p.view_count, p.user_id, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image, ") + post_meta::kTagsJsonColumn + ", "
        // Whether *this* viewer saved it, so the bookmark control can render
        // its own state rather than guessing. $2 is 0 for an anonymous
        // reader, who matches no bookmark row.
        "       EXISTS (SELECT 1 FROM bookmarks bk "
        "                WHERE bk.user_id = $2::int AND bk.post_id = p.id) AS bookmarked "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id "
        // A hidden post is a 404, not a 403: confirming it exists would
        // tell whoever got it moderated exactly that it worked. An
        // unpublished one is a 404 to everyone but its author, for the
        // same reason — a draft's existence is not public information.
        "WHERE p.hidden_at IS NULL AND p.id = $1 "
        "  AND (p.published_at IS NOT NULL OR p.user_id = $2::int)";

    dbClient->execSqlAsync(
        kSql,
        [callback, req, postId, viewerId](const Result& r) {
            if (r.empty()) {
                Json::Value ret;
                ret["error"] = "Post not found";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k404NotFound);
                callback(resp);
                return;
            }
            const auto& row = r[0];

            // ETag derives from (id, updated_at). Anything that bumps
            // updated_at (UPDATE trigger fires on every row write) gives
            // the resource a new tag; comments/likes don't.
            const std::string updatedAt = row["updated_at"].as<std::string>();

            // Everything past the view count, which the counting statement
            // below may still change. Taking a copy of the row's fields here
            // rather than capturing `row` keeps the continuation valid after
            // the Result goes out of scope.
            auto respond = [callback, req, postId, updatedAt,
                            id            = row["id"].as<int64_t>(),
                            title         = row["title"].as<std::string>(),
                            content       = row["content"].as<std::string>(),
                            contentHtml   = row["content_html"].isNull()
                                              ? std::string{}
                                              : row["content_html"].as<std::string>(),
                            hasHtml       = !row["content_html"].isNull(),
                            createdAt     = row["created_at"].as<std::string>(),
                            publishedAt   = row["published_at"].isNull()
                                              ? std::string{}
                                              : row["published_at"].as<std::string>(),
                            isDraft       = row["published_at"].isNull(),
                            readingMin    = row["reading_minutes"].as<int>(),
                            excerpt       = row["excerpt"].isNull()
                                              ? std::string{}
                                              : row["excerpt"].as<std::string>(),
                            hasExcerpt    = !row["excerpt"].isNull(),
                            tags          = post_meta::tagsFromJson(
                                              row["tags_json"].as<std::string>()),
                            bookmarked    = row["bookmarked"].as<bool>(),
                            authorId      = row["author_id"].isNull()
                                              ? int64_t{0}
                                              : row["author_id"].as<int64_t>(),
                            authorName    = row["author_id"].isNull()
                                              ? std::string{}
                                              : row["author_username"].as<std::string>(),
                            authorImage   = row["author_profile_image"].isNull()
                                              ? std::string{}
                                              : row["author_profile_image"].as<std::string>()
                           ](long long views) {
                // The view count is in the ETag, so a reader whose visit
                // bumped it gets a fresh body rather than a 304 showing the
                // old number.
                const std::string etag = http_cache::makeWeakEtag({
                    "post", std::to_string(postId),
                    std::to_string(http_cache::parseTimestampMicros(updatedAt)),
                    std::to_string(views),
                });
                if (http_cache::ifNoneMatchHit(req, etag)) {
                    callback(http_cache::makeNotModified(etag));
                    return;
                }

                Json::Value ret;
                ret["id"]         = id;
                ret["title"]      = title;
                ret["content"]    = content;
                if (hasHtml) ret["content_html"] = contentHtml;
                ret["created_at"] = createdAt;
                ret["updated_at"] = updatedAt;
                ret["published_at"] = isDraft ? Json::nullValue
                                              : Json::Value(publishedAt);
                ret["is_draft"]        = isDraft;
                ret["reading_minutes"] = readingMin;
                ret["view_count"]      = static_cast<Json::Int64>(views);
                if (hasExcerpt) ret["excerpt"] = excerpt;
                ret["tags"]       = tags;
                ret["bookmarked"] = bookmarked;

                if (authorId > 0) {
                    ret["author"]["id"]       = authorId;
                    ret["author"]["username"] = authorName;
                    if (!authorImage.empty())
                        ret["author"]["profile_image"] = authorImage;
                }
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                http_cache::applyCacheHeaders(resp, etag);
                callback(resp);
            };

            // Count the read before building the response, so the number we
            // report is the one that includes this visit — a reader who
            // refreshes and sees the count unchanged assumes it is broken.
            // Author's own views do not count: an author reloading a draft
            // would otherwise inflate their own numbers.
            const bool isAuthor = viewerId > 0 &&
                                  row["user_id"].as<int>() == viewerId;
            const long long views = row["view_count"].as<long long>();
            if (isAuthor) {
                respond(views);
                return;
            }
            post_meta::recordViewAsync(
                drogon::app().getDbClient(), postId,
                post_meta::viewerKey(req, viewerId), views, respond);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (getPost): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Post not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
        },
        postId, viewerId);
}

void PostController::createPost(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    // Per-user create cap: 10 burst, 10/min — bounds post spam / DB bloat.
    if (auto rl = security::rateLimitOr429(
            "post_create", "uid:" + std::to_string(userIdOpt.value()),
            10.0, 10.0 / 60.0)) {
        callback(rl);
        return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string title = (*json)["title"].asString();
    std::string content = (*json)["content"].asString();

    if (title.empty() || content.empty()) {
        Json::Value ret;
        ret["error"] = "Title and content are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }
    if (title.size() > kMaxTitleBytes || content.size() > kMaxContentBytes) {
        Json::Value ret;
        ret["error"] = "Title or content too long";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k413RequestEntityTooLarge);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();

    // Render markdown once at write-time and store both the raw source (so
    // we can re-render if rendering policy changes) and the resulting safe
    // HTML (so reads stay cheap).
    const std::string contentHtml = markdown::renderToSafeHtml(content);

    // Derived once here rather than per read, for the same reason: both are
    // pure functions of the content.
    const int         readingMinutes = post_meta::estimateReadingMinutes(content);
    const std::string excerpt        = post_meta::makeExcerpt(content);
    const auto        tags           = post_meta::parseTags((*json)["tags"]);

    // Absent means publish — the overwhelming majority of writes, and the
    // behaviour every existing client already depends on. Only an explicit
    // `"draft": true` withholds it.
    const bool asDraft = (*json)["draft"].isBool() && (*json)["draft"].asBool();

    try {
        // Raw SQL rather than the ORM mapper: the generated model predates
        // this migration and has no accessor for published_at, and
        // regenerating every model file to add one column is a much larger
        // diff than the feature warrants.
        auto ins = dbClient->execSqlSync(
            "INSERT INTO posts (user_id, title, content, content_html, "
            "                   reading_minutes, excerpt, published_at) "
            "VALUES ($1, $2, $3, $4, $5::int, $6, "
            "        CASE WHEN $7::bool THEN NULL ELSE now() END) "
            "RETURNING id, created_at, published_at",
            userIdOpt.value(), title, content, contentHtml,
            readingMinutes, excerpt, asDraft);

        const int newId = ins[0]["id"].as<int>();
        post_meta::syncPostTags(dbClient, newId, tags);

        // Followers hear about published posts, not drafts. A draft that
        // notified everyone the moment it was saved would be the single
        // most annoying thing this feature could do.
        if (!asDraft) {
            notifications::emitNewPostToFollowers(dbClient, userIdOpt.value(), newId);
        }

        Json::Value ret;
        ret["message"] = asDraft ? "Draft saved" : "Post created successfully";
        ret["post"]["id"]              = newId;
        ret["post"]["title"]           = title;
        ret["post"]["content"]         = content;
        ret["post"]["content_html"]    = contentHtml;
        ret["post"]["reading_minutes"] = readingMinutes;
        ret["post"]["excerpt"]         = excerpt;
        ret["post"]["is_draft"]        = asDraft;
        ret["post"]["tags"]            = post_meta::tagsForPost(dbClient, newId);

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to create post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::uploadPostImage(const HttpRequestPtr &req,
                                    std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");
    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    // 30 burst, 30/10min — a post may legitimately embed several images, but
    // image processing (libvips) is CPU-heavy so we still bound abuse per user.
    if (auto rl = security::rateLimitOr429(
            "post_image", "uid:" + std::to_string(userIdOpt.value()),
            30.0, 30.0 / 600.0)) {
        callback(rl);
        return;
    }

    MultiPartParser fileUpload;
    if (fileUpload.parse(req) != 0) {
        Json::Value ret;
        ret["error"] = "Invalid file upload";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }
    const auto &files = fileUpload.getFiles();
    if (files.size() != 1) {
        Json::Value ret;
        ret["error"] = "Exactly one image file is required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }
    auto &file = files[0];

    // First land in uploads/tmp, then re-encode into uploads/posts. The
    // filename is stamped server-side with a 96-bit random suffix — the
    // user-supplied name is never trusted for path construction. The image
    // pipeline always emits JPEG, so the served extension is fixed.
    const std::string postsDir = "./uploads/posts/";
    const std::string tmpDir   = "./uploads/tmp/";
    std::error_code ec;
    std::filesystem::create_directories(postsDir, ec);
    std::filesystem::create_directories(tmpDir,   ec);
    if (ec) {
        LOG_ERROR << "Failed to create post-image dirs: " << ec.message();
        Json::Value ret;
        ret["error"] = "Failed to save upload";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    const std::string stem = "post_" + std::to_string(userIdOpt.value())
                           + "_" + security::randomToken(12);
    const std::string tmpPath    = tmpDir   + stem + ".upload";
    const std::string finalPath  = postsDir + stem + ".jpg";
    const std::string publicPath = "/uploads/posts/" + stem + ".jpg";

    file.saveAs(tmpPath);

    // Decode + downscale + re-encode runs on the media pool, not here.
    // libvips holds the calling thread for the whole pipeline, and this
    // handler is executing on one of Drogon's IO loops — every other
    // connection assigned to that loop would wait out the resize. The tmp
    // file is already on disk, so the job needs nothing but paths.
    const bool queued = workers::offload(workers::Pool::Media, callback,
        [req, callback, tmpPath, finalPath, publicPath, userIdOpt] {
            // Validate (magic-byte sniff), downscale preserving aspect,
            // strip EXIF.
            const auto result = image::processPostImage(tmpPath, finalPath);

            std::error_code rmEc;
            std::filesystem::remove(tmpPath, rmEc);        // best effort

            if (!result.ok) {
                Json::Value ret;
                ret["error"] = result.error;
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(static_cast<HttpStatusCode>(result.status));
                callback(resp);
                return;
            }

            audit_log::record(req, {"post.image.upload", userIdOpt,
                                    std::nullopt, std::nullopt,
                                    Json::objectValue});

            // The SPA embeds this as Markdown: ![alt](url). cmark renders it
            // in SAFE mode (no raw HTML) and the CSP img-src 'self' confines
            // display to this origin, so a re-encoded same-origin JPEG
            // carries no active content.
            Json::Value ret;
            ret["url"] = publicPath;
            callback(HttpResponse::newHttpJsonResponse(ret));
        });

    // Saturated pool: offload() has already answered 503, but the job that
    // would have cleaned up never ran, so the tmp file is ours to remove.
    // Skipping this turns every shed upload into a permanent orphan under
    // uploads/tmp.
    if (!queued) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
    }
}

void PostController::updatePost(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback,
                               int postId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    try {
        auto post = mapper.findByPrimaryKey(postId);

        // Check if user owns the post
        if (post.getValueOfUserId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        // Whether anything the generated model knows about changed. The
        // ORM builds "UPDATE posts SET <dirty columns> WHERE id = ?" and
        // emits a syntactically invalid empty SET when nothing is dirty —
        // so a PUT that only flips the draft flag or replaces the tags,
        // both of which live outside the model, must not reach update().
        bool modelChanged = false;

        if (json->isMember("title")) {
            const std::string newTitle = (*json)["title"].asString();
            if (newTitle.size() > kMaxTitleBytes) {
                Json::Value ret;
                ret["error"] = "Title too long";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k413RequestEntityTooLarge);
                callback(resp);
                return;
            }
            post.setTitle(newTitle);
            modelChanged = true;
        }
        if (json->isMember("content")) {
            const std::string newContent = (*json)["content"].asString();
            if (newContent.size() > kMaxContentBytes) {
                Json::Value ret;
                ret["error"] = "Content too long";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k413RequestEntityTooLarge);
                callback(resp);
                return;
            }
            post.setContent(newContent);
            post.setContentHtml(markdown::renderToSafeHtml(newContent));
            modelChanged = true;
        }

        if (modelChanged) mapper.update(post);

        // The columns added in 0013 are not on the generated model, so they
        // are written separately. Reading time and excerpt are recomputed
        // only when the content actually changed — a title-only edit should
        // not rewrite them.
        if (json->isMember("content")) {
            const std::string& c = post.getValueOfContent();
            dbClient->execSqlSync(
                "UPDATE posts SET reading_minutes = $2::int, excerpt = $3 WHERE id = $1",
                postId, post_meta::estimateReadingMinutes(c),
                post_meta::makeExcerpt(c));
        }

        // Publishing a draft stamps published_at once and never again:
        // re-publishing an already-live post must not reset its date and
        // shuffle it back to the top of the feed. Unpublishing (back to
        // draft) is allowed and clears it.
        bool isDraft = post_meta::isDraft(dbClient, postId);
        if (json->isMember("draft")) {
            const bool wantDraft = (*json)["draft"].asBool();
            if (wantDraft && !isDraft) {
                dbClient->execSqlSync(
                    "UPDATE posts SET published_at = NULL WHERE id = $1", postId);
                isDraft = true;
            } else if (!wantDraft && isDraft) {
                dbClient->execSqlSync(
                    "UPDATE posts SET published_at = COALESCE(published_at, now()) "
                    "WHERE id = $1", postId);
                isDraft = false;
                // The draft-to-published transition is the moment followers
                // should hear about it. Guarded by `isDraft` so re-sending
                // draft:false on an already-public post does not notify
                // everyone a second time.
                notifications::emitNewPostToFollowers(
                    dbClient, userIdOpt.value(), postId);
            }
        }

        // Absent `tags` means "leave them alone"; an empty array means
        // "remove them all". Treating absent as empty would silently strip
        // the tags off any post edited by a client that does not know about
        // them yet.
        if (json->isMember("tags")) {
            post_meta::syncPostTags(dbClient, postId,
                                    post_meta::parseTags((*json)["tags"]));
        }

        Json::Value ret;
        ret["message"] = "Post updated successfully";
        ret["post"]["id"]       = post.getValueOfId();
        ret["post"]["title"]    = post.getValueOfTitle();
        ret["post"]["content"]  = post.getValueOfContent();
        ret["post"]["is_draft"] = isDraft;
        ret["post"]["tags"]     = post_meta::tagsForPost(dbClient, postId);

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Post not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to update post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::deletePost(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback,
                               int postId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    try {
        auto post = mapper.findByPrimaryKey(postId);

        // Check if user owns the post
        if (post.getValueOfUserId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(postId);

        Json::Value meta;
        meta["title"] = post.getValueOfTitle();
        audit_log::record(req, {"post.delete", userIdOpt,
                                std::string{"post"},
                                static_cast<std::int64_t>(postId),
                                std::move(meta)});

        Json::Value ret;
        ret["message"] = "Post deleted successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Post not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to delete post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::getUserPosts(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&callback,
                                 int userId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    // Cursor pagination, same shape as getAllPosts: this used to return every
    // post a user had ever written in one unbounded findBy — slow and a memory
    // hog for prolific authors. `before` is the oldest id the client already
    // holds; ids are monotonic so id DESC matches created_at DESC.
    using Cols = drogon_model::blog_db::Posts::Cols;
    const int limit  = clampLimit(req->getParameter("limit"));
    const int cursor = parseCursor(req->getParameter("before"));

    try {
        Criteria crit(Cols::_user_id, CompareOperator::EQ, userId);
        // A profile listing is a read path like any other, so it drops
        // moderator-hidden posts too.
        //
        // The column is named as a string rather than through Cols::, which
        // is generated from the schema by drogon_ctl and therefore does not
        // know about columns added by a later migration. Regenerating the
        // models to gain one constant would rewrite every model file for no
        // other benefit; Criteria accepts a column name directly.
        crit = crit && Criteria(std::string("hidden_at"),
                                CompareOperator::IsNull);
        // …and unpublished ones. A profile is a public listing: it is reached
        // without a session and its response carries the full `content` of
        // every row it returns, so a missing draft filter here hands out
        // unpublished bodies verbatim rather than the fragment /posts/search
        // would leak. The author reads their own drafts through
        // /posts/drafts, which is session-scoped and returns is_draft.
        crit = crit && Criteria(std::string("published_at"),
                                CompareOperator::IsNotNull);
        if (cursor > 0)
            crit = crit && Criteria(Cols::_id, CompareOperator::LT, cursor);
        auto posts = mapper.orderBy(Cols::_id, SortOrder::DESC)
                           .limit(static_cast<std::size_t>(limit))
                           .findBy(crit);

        // ETag tracks (user_id, page keys, count, max(updated_at)). Adding /
        // removing / editing one of this user's posts changes one of those;
        // other users' posts don't affect it.
        std::int64_t maxTs = 0;
        std::int64_t minId = 0;
        for (const auto& p : posts) {
            const auto id = static_cast<std::int64_t>(p.getValueOfId());
            if (minId == 0 || id < minId) minId = id;
            const auto ts = http_cache::parseTimestampMicros(
                                p.getValueOfUpdatedAt().toDbStringLocal());
            if (ts > maxTs) maxTs = ts;
        }
        const std::string etag = http_cache::makeWeakEtag({
            "user-posts", std::to_string(userId),
            std::to_string(maxTs),
            std::to_string(static_cast<int>(posts.size())),
            std::to_string(cursor),
            std::to_string(limit),
        });
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag));
            return;
        }

        Json::Value ret;
        ret["posts"] = Json::Value(Json::arrayValue);

        for (const auto &post : posts) {
            Json::Value postJson;
            postJson["id"] = post.getValueOfId();
            postJson["title"] = post.getValueOfTitle();
            postJson["content"] = post.getValueOfContent();
            postJson["created_at"] = post.getValueOfCreatedAt().toDbStringLocal();
            ret["posts"].append(postJson);
        }

        ret["next_cursor"] = (static_cast<int>(posts.size()) == limit && minId > 0)
            ? Json::Value(static_cast<Json::Int64>(minId)) : Json::nullValue;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch posts";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::likePost(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&callback,
                             int postId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();

    // ON CONFLICT DO NOTHING + RETURNING id avoids the SELECT-then-INSERT
    // TOCTOU race that the ORM mapper would hit when two concurrent
    // /like requests both pass the "already exists?" check and then race
    // to insert. The UNIQUE(post_id, user_id) constraint serialises them
    // at the DB layer; ON CONFLICT lets us turn the second insert into
    // a clean 409 instead of letting libpq throw and degrading to a 500.
    try {
        // Resolve the target first, scoped to what the caller is allowed to
        // see. This used to run after the insert, purely to find the author
        // to notify, and unscoped — so a like landed on any id at all,
        // including someone else's unpublished draft, and the resulting
        // notification told them a stranger had found it. Doing the lookup
        // up front costs nothing (it is the same round trip, moved) and
        // turns an invisible post into a 404 instead of a silent write.
        auto owner = dbClient->execSqlSync(
            "SELECT user_id FROM posts "
            " WHERE id = $1 AND hidden_at IS NULL AND published_at IS NOT NULL",
            postId);
        if (owner.empty()) {
            Json::Value ret;
            ret["error"] = "Post not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }

        auto r = dbClient->execSqlSync(
            "INSERT INTO likes (post_id, user_id) "
            "VALUES ($1, $2) "
            "ON CONFLICT (post_id, user_id) DO NOTHING "
            "RETURNING id",
            postId, userIdOpt.value());
        if (r.empty()) {
            Json::Value ret;
            ret["error"] = "Post already liked";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k409Conflict);
            callback(resp);
            return;
        }
        // Tell the author. Only on the transition — the ON CONFLICT branch
        // above already returned, so reaching here means a like was really
        // created and not re-sent.
        notifications::emit(dbClient, owner[0]["user_id"].as<int>(),
                            userIdOpt.value(), notifications::Kind::Like, postId);

        Json::Value ret;
        ret["message"] = "Post liked successfully";
        callback(HttpResponse::newHttpJsonResponse(ret));
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error (likePost): " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to like post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::unlikePost(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback,
                               int postId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Likes> mapper(dbClient);

    try {
        auto likes = mapper.findBy(
            Criteria(drogon_model::blog_db::Likes::Cols::_post_id, 
                    CompareOperator::EQ, postId) &&
            Criteria(drogon_model::blog_db::Likes::Cols::_user_id, 
                    CompareOperator::EQ, userIdOpt.value())
        );

        if (likes.size() == 0) {
            Json::Value ret;
            ret["error"] = "Like not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(likes[0].getValueOfId());

        Json::Value ret;
        ret["message"] = "Post unliked successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to unlike post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::getLikesCount(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback,
                                  int postId)
{
    // Anonymous readers get the count only. For a signed-in reader we also
    // report whether *they* have liked it, because without that the client
    // cannot render a like control that knows its own state — the UI used to
    // show a Like button beside an Unlike button and guess.
    auto session = req->session();
    const auto userIdOpt = session ? session->getOptional<int>("user_id")
                                   : std::optional<int>{};

    auto dbClient = drogon::app().getDbClient();

    try {
        // count(*) instead of materialising every like row just to call
        // .size() — a viral post would otherwise pull thousands of rows
        // across the wire to produce a single integer.
        //
        // The membership test rides along in the same round trip. $2 is the
        // viewer id, or 0 for an anonymous reader — no user has id 0, so the
        // EXISTS is simply false. The explicit ::int cast pins the parameter
        // type: Postgres otherwise infers it from context, and a mismatch
        // against Drogon's int32 bind fails at the binary protocol level.
        auto r = dbClient->execSqlSync(
            "SELECT count(*) AS n,"
            "       bool_or(user_id = $2::int) AS liked"
            "  FROM likes WHERE post_id = $1",
            postId, userIdOpt.value_or(0));
        const long long count = r[0]["n"].as<long long>();
        // bool_or over an empty set is NULL, not false.
        const bool liked = !r[0]["liked"].isNull() && r[0]["liked"].as<bool>();

        // ETag = (post_id, count, viewer's own like). likes is just a join row
        // that gets created/dropped by like/unlike — count is nearly the
        // entire payload, so any change yields a new ETag without further
        // state. The viewer's own flag has to be in the key as well, or two
        // readers with the same count would share a cache entry that
        // disagrees about which of them pressed the button.
        const std::string etag = http_cache::makeWeakEtag({
            "likes-count", std::to_string(postId),
            std::to_string(count),
            liked ? "1" : "0",
        });
        // The body now varies per viewer, so Vary: Cookie — which also makes
        // applyCacheHeaders downgrade Cache-Control from public to private,
        // keeping shared caches from holding it at all.
        constexpr std::string_view kVary = "Cookie";

        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag, 0, kVary));
            return;
        }

        Json::Value ret;
        ret["post_id"] = postId;
        ret["likes_count"] = static_cast<Json::Int64>(count);
        ret["liked"] = liked;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag, 0, kVary);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to get likes count";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
