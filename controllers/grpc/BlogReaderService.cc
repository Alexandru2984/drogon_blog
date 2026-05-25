#include "BlogReaderService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <string>

namespace controllers::grpc_svc {

namespace {

// Copy a SQL row into the proto Post. Same shape as the JSON we
// serialise on REST so a future tool dumping both side-by-side has
// nothing to compare across.
void rowToPost(const ::drogon::orm::Row& row, ::blog::v1::Post* p)
{
    p->set_id(row["id"].as<std::int32_t>());
    p->set_title(row["title"].as<std::string>());
    p->set_content(row["content"].as<std::string>());
    if (!row["content_html"].isNull()) {
        p->set_content_html(row["content_html"].as<std::string>());
    }
    p->set_created_at(row["created_at"].as<std::string>());
    p->set_updated_at(row["updated_at"].as<std::string>());
    if (!row["author_id"].isNull()) {
        auto* author = p->mutable_author();
        author->set_id(row["author_id"].as<std::int32_t>());
        author->set_username(row["author_username"].as<std::string>());
        if (!row["author_profile_image"].isNull()) {
            author->set_profile_image(
                row["author_profile_image"].as<std::string>());
        }
    }
}

constexpr int kDefaultLimit = 20;
constexpr int kMaxLimit     = 50;

int clampLimit(int raw)
{
    if (raw <= 0) return kDefaultLimit;
    return std::min(raw, kMaxLimit);
}

} // namespace

::grpc::Status BlogReaderService::GetPost(
    ::grpc::ServerContext* /*ctx*/,
    const ::blog::v1::GetPostRequest* req,
    ::blog::v1::Post*                 resp)
{
    if (req->id() <= 0) {
        return {::grpc::StatusCode::INVALID_ARGUMENT, "id must be > 0"};
    }
    auto db = ::drogon::app().getDbClient();
    if (!db) {
        return {::grpc::StatusCode::UNAVAILABLE, "db client not ready"};
    }
    // execSqlSync from a non-Drogon thread is safe: Drogon's DB
    // client uses its own internal worker pool keyed off the SQL
    // call, not the calling thread.
    try {
        const auto r = db->execSqlSync(
            "SELECT p.id, p.title, p.content, p.content_html, "
            "       p.created_at, p.updated_at, "
            "       u.id AS author_id, u.username AS author_username, "
            "       u.profile_image AS author_profile_image "
            "FROM posts p LEFT JOIN users u ON u.id = p.user_id "
            "WHERE p.id = $1",
            req->id());
        if (r.empty()) {
            return {::grpc::StatusCode::NOT_FOUND, "post not found"};
        }
        rowToPost(r[0], resp);
        return ::grpc::Status::OK;
    } catch (const ::drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "grpc GetPost: " << e.base().what();
        return {::grpc::StatusCode::INTERNAL, "db error"};
    }
}

::grpc::Status BlogReaderService::ListPosts(
    ::grpc::ServerContext* /*ctx*/,
    const ::blog::v1::ListPostsRequest* req,
    ::blog::v1::ListPostsResponse*      resp)
{
    const int limit = clampLimit(req->limit());
    auto db = ::drogon::app().getDbClient();
    if (!db) {
        return {::grpc::StatusCode::UNAVAILABLE, "db client not ready"};
    }
    try {
        // Same shape as PostController::getAllPosts (REST) so the two
        // surfaces stay byte-for-byte equivalent at the row level.
        // cursor=0 means "first page" (no WHERE filter).
        const auto r = (req->cursor() > 0)
            ? db->execSqlSync(
                "SELECT p.id, p.title, p.content, p.content_html, "
                "       p.created_at, p.updated_at, "
                "       u.id AS author_id, u.username AS author_username, "
                "       u.profile_image AS author_profile_image "
                "FROM posts p LEFT JOIN users u ON u.id = p.user_id "
                "WHERE p.id < $1 "
                "ORDER BY p.id DESC LIMIT $2",
                req->cursor(), static_cast<std::int64_t>(limit))
            : db->execSqlSync(
                "SELECT p.id, p.title, p.content, p.content_html, "
                "       p.created_at, p.updated_at, "
                "       u.id AS author_id, u.username AS author_username, "
                "       u.profile_image AS author_profile_image "
                "FROM posts p LEFT JOIN users u ON u.id = p.user_id "
                "ORDER BY p.id DESC LIMIT $1",
                static_cast<std::int64_t>(limit));

        std::int32_t minId = 0;
        for (const auto& row : r) {
            auto* p = resp->add_posts();
            rowToPost(row, p);
            const auto id = row["id"].as<std::int32_t>();
            if (minId == 0 || id < minId) minId = id;
        }
        // Next-cursor signalling: only emit when the page filled the
        // limit. proto3 uses 0 as the "absent" sentinel — clients
        // read 0 the same way REST clients read JSON null.
        if (static_cast<int>(r.size()) == limit && minId > 0) {
            resp->set_next_cursor(minId);
        }
        return ::grpc::Status::OK;
    } catch (const ::drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "grpc ListPosts: " << e.base().what();
        return {::grpc::StatusCode::INTERNAL, "db error"};
    }
}

} // namespace controllers::grpc_svc
