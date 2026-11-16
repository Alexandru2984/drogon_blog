#include "PostController.h"
#include "../helpers/PostMeta.h"

#include <algorithm>
#include <cstdint>
#include <string>

using namespace drogon;

namespace {
bool boundedInteger(const std::string& raw, int fallback, int maximum, int& out)
{
    out = fallback;
    if (raw.empty()) return true;
    if (raw.size() > 6 || raw.find_first_not_of("0123456789") != std::string::npos)
        return false;
    out = std::stoi(raw);
    return out <= maximum;
}
}

void PostController::discoverPosts(const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    const auto badRequest = [&callback] {
        Json::Value body;
        body["error"] = "Invalid discovery filters";
        auto resp = HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
    };
    auto sort = req->getParameter("sort");
    if (sort.empty()) sort = "latest";
    std::string order;
    if (sort == "latest") order = "p.id DESC";
    else if (sort == "popular") order = "likes_count DESC, comment_count DESC, p.id DESC";
    else if (sort == "discussed") order = "comment_count DESC, likes_count DESC, p.id DESC";
    else { badRequest(); return; }

    const auto period = req->getParameter("period");
    int days = 0;
    if (period == "week") days = 7;
    else if (period == "month") days = 30;
    else if (!period.empty() && period != "all") { badRequest(); return; }

    const auto rawTag = req->getParameter("tag");
    if (rawTag.size() > 80) { badRequest(); return; }
    const auto tag = post_meta::slugify(rawTag);
    if (!rawTag.empty() && tag.empty()) { badRequest(); return; }

    int limit = 12, offset = 0, minutes = 0;
    if (!boundedInteger(req->getParameter("limit"), 12, 24, limit) || limit < 1 ||
        !boundedInteger(req->getParameter("offset"), 0, 2000, offset) ||
        !boundedInteger(req->getParameter("max_minutes"), 0, 10, minutes) ||
        (minutes != 0 && minutes != 5 && minutes != 10)) {
        badRequest(); return;
    }

    // Only the allowlisted ORDER BY is interpolated. All request values are
    // bound. Return summaries, not entire article bodies, and read one extra
    // row to distinguish a full final page from a page that has a successor.
    const std::string sql =
        "SELECT p.id, p.title, ''::text AS content, NULL::text AS content_html, "
        "p.created_at, p.updated_at, p.published_at, p.reading_minutes, "
        "p.excerpt, p.view_count, u.id AS author_id, u.username AS author_username, "
        "u.profile_image AS author_profile_image, " + std::string(post_meta::kTagsJsonColumn) +
        ", (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS likes_count, "
        "(SELECT count(*) FROM comments c WHERE c.post_id = p.id "
        " AND c.hidden_at IS NULL AND c.deleted_at IS NULL) AS comment_count "
        "FROM posts p LEFT JOIN users u ON u.id = p.user_id "
        "WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        "AND ($1::int = 0 OR p.published_at >= NOW() - $1::int * INTERVAL '1 day') "
        "AND ($2 = '' OR EXISTS (SELECT 1 FROM post_tags pt JOIN tags t ON t.id = pt.tag_id "
        " WHERE pt.post_id = p.id AND t.slug = $2)) "
        "AND ($3::int = 0 OR p.reading_minutes <= $3::int) "
        "ORDER BY " + order + " LIMIT $4 OFFSET $5";

    app().getDbClient()->execSqlAsync(sql,
        [callback, limit, offset](const orm::Result& rows) {
            Json::Value body;
            body["posts"] = Json::Value(Json::arrayValue);
            const auto count = std::min(rows.size(), static_cast<std::size_t>(limit));
            for (std::size_t i = 0; i < count; ++i) {
                auto post = post_meta::feedRowJson(rows[i]);
                post["published_at"] = rows[i]["published_at"].as<std::string>();
                post["likes_count"] = rows[i]["likes_count"].as<int64_t>();
                post["comment_count"] = rows[i]["comment_count"].as<int64_t>();
                body["posts"].append(post);
            }
            body["next_offset"] = rows.size() > static_cast<std::size_t>(limit) && offset + limit <= 2000
                ? Json::Value(offset + limit) : Json::nullValue;
            auto resp = HttpResponse::newHttpJsonResponse(body);
            // Engagement changes independently of the post's updated_at.
            // Do not reuse the legacy feed's timestamp-only ETag here.
            resp->addHeader("Cache-Control", "no-store");
            callback(resp);
        },
        [callback](const orm::DrogonDbException& e) {
            LOG_ERROR << "Discovery query failed: " << e.base().what();
            Json::Value body;
            body["error"] = "Could not load discovery feed";
            auto resp = HttpResponse::newHttpJsonResponse(body);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        }, days, tag, minutes, static_cast<int64_t>(limit + 1), static_cast<int64_t>(offset));
}
