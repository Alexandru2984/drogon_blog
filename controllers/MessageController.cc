#include "MessageController.h"
#include "../models/Messages.h"
#include "../models/Users.h"
#include "../helpers/HttpCache.h"
#include "../helpers/Security.h"
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cstdint>

using namespace drogon;
using namespace drogon::orm;

namespace {
// Capped well below PG's 8 KiB pg_notify payload limit. The
// trg_messages_notify trigger additionally truncates to 200 bytes
// before json_build_object so the websocket payload stays under that
// envelope even if this cap is raised later.
constexpr std::size_t kMaxMessageBytes = std::size_t{10} * 1024;

// Pagination caps. Previously these endpoints did an unbounded findBy and
// returned every row a user had ever sent/received — a slow, memory-heavy
// query and JSON serialization that grows without limit per account. We now
// return at most kMaxPageLimit rows (newest first by id, which is monotonic
// with created_at), with an optional `before` cursor for older pages. No
// params => latest kDefaultPageLimit, which is backward-compatible for the
// SPA (it just gets a bounded, most-recent window).
constexpr int kDefaultPageLimit = 50;
constexpr int kMaxPageLimit     = 100;

struct PageParams { int limit; std::int64_t before; };

PageParams parsePage(const HttpRequestPtr& req)
{
    int limit = kDefaultPageLimit;
    const auto l = req->getParameter("limit");
    if (!l.empty()) {
        try { const int v = std::stoi(l); if (v > 0) limit = std::min(v, kMaxPageLimit); }
        catch (...) {}
    }
    std::int64_t before = 0;
    const auto b = req->getParameter("before");
    if (!b.empty()) {
        try { const long long v = std::stoll(b); if (v > 0) before = v; }
        catch (...) {}
    }
    return {limit, before};
}
} // namespace

void MessageController::getReceivedMessages(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Messages> messageMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        const auto page = parsePage(req);
        using Cols = drogon_model::blog_db::Messages::Cols;
        auto crit = Criteria(Cols::_receiver_id, CompareOperator::EQ, userIdOpt.value());
        if (page.before > 0)
            crit = crit && Criteria(Cols::_id, CompareOperator::LT, page.before);
        auto messages = messageMapper.orderBy(Cols::_id, SortOrder::DESC)
                                     .limit(static_cast<std::size_t>(page.limit))
                                     .findBy(crit);

        Json::Value ret;
        ret["messages"] = Json::Value(Json::arrayValue);
        std::int64_t minId = 0;

        for (const auto &message : messages) {
            const auto id = message.getValueOfId();
            if (minId == 0 || id < minId) minId = id;
            Json::Value msgJson;
            msgJson["id"] = id;
            msgJson["content"] = message.getValueOfContent();
            msgJson["is_read"] = message.getValueOfIsRead();
            msgJson["created_at"] = message.getValueOfCreatedAt().toDbStringLocal();

            // Best-effort sender enrichment. The author row may have been
            // deleted between the message SELECT and now (ON DELETE CASCADE
            // wipes their messages but the in-flight read can still race);
            // returning the message without sender info beats failing the list.
            try {
                auto sender = userMapper.findByPrimaryKey(message.getValueOfSenderId());
                msgJson["sender"]["id"] = sender.getValueOfId();
                msgJson["sender"]["username"] = sender.getValueOfUsername();
                if (!sender.getValueOfProfileImage().empty()) {
                    msgJson["sender"]["profile_image"] = sender.getValueOfProfileImage();
                }
            } catch (const DrogonDbException& e) {
                LOG_DEBUG << "sender lookup for message "
                          << message.getValueOfId() << " failed: "
                          << e.base().what();
            }

            ret["messages"].append(msgJson);
        }

        // Cursor for the next (older) page: only when this page filled the
        // limit, otherwise the client knows it has reached the end.
        ret["next_cursor"] = (static_cast<int>(messages.size()) == page.limit && minId > 0)
            ? Json::Value(static_cast<Json::Int64>(minId)) : Json::nullValue;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch messages";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::getSentMessages(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Messages> messageMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        const auto page = parsePage(req);
        using Cols = drogon_model::blog_db::Messages::Cols;
        auto crit = Criteria(Cols::_sender_id, CompareOperator::EQ, userIdOpt.value());
        if (page.before > 0)
            crit = crit && Criteria(Cols::_id, CompareOperator::LT, page.before);
        auto messages = messageMapper.orderBy(Cols::_id, SortOrder::DESC)
                                     .limit(static_cast<std::size_t>(page.limit))
                                     .findBy(crit);

        Json::Value ret;
        ret["messages"] = Json::Value(Json::arrayValue);
        std::int64_t minId = 0;

        for (const auto &message : messages) {
            const auto id = message.getValueOfId();
            if (minId == 0 || id < minId) minId = id;
            Json::Value msgJson;
            msgJson["id"] = id;
            msgJson["content"] = message.getValueOfContent();
            msgJson["is_read"] = message.getValueOfIsRead();
            msgJson["created_at"] = message.getValueOfCreatedAt().toDbStringLocal();

            // Best-effort receiver enrichment; see getReceivedMessages.
            try {
                auto receiver = userMapper.findByPrimaryKey(message.getValueOfReceiverId());
                msgJson["receiver"]["id"] = receiver.getValueOfId();
                msgJson["receiver"]["username"] = receiver.getValueOfUsername();
                if (!receiver.getValueOfProfileImage().empty()) {
                    msgJson["receiver"]["profile_image"] = receiver.getValueOfProfileImage();
                }
            } catch (const DrogonDbException& e) {
                LOG_DEBUG << "receiver lookup for message "
                          << message.getValueOfId() << " failed: "
                          << e.base().what();
            }

            ret["messages"].append(msgJson);
        }

        ret["next_cursor"] = (static_cast<int>(messages.size()) == page.limit && minId > 0)
            ? Json::Value(static_cast<Json::Int64>(minId)) : Json::nullValue;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch messages";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::getConversation(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback,
                                       int otherUserId)
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
    Mapper<drogon_model::blog_db::Messages> messageMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        // Messages between the two users. Fetch the newest kMaxPageLimit by id
        // DESC (bounded — a long-running conversation must not pull every row),
        // then reverse to chronological order for display. `before` pages
        // further back. The pair-scoping criteria is unchanged.
        const auto page = parsePage(req);
        using Cols = drogon_model::blog_db::Messages::Cols;
        auto pairCrit =
            (Criteria(Cols::_sender_id,   CompareOperator::EQ, userIdOpt.value()) &&
             Criteria(Cols::_receiver_id, CompareOperator::EQ, otherUserId)) ||
            (Criteria(Cols::_sender_id,   CompareOperator::EQ, otherUserId) &&
             Criteria(Cols::_receiver_id, CompareOperator::EQ, userIdOpt.value()));
        if (page.before > 0)
            pairCrit = pairCrit && Criteria(Cols::_id, CompareOperator::LT, page.before);
        auto messages = messageMapper.orderBy(Cols::_id, SortOrder::DESC)
                                     .limit(static_cast<std::size_t>(page.limit))
                                     .findBy(pairCrit);
        // Reverse the newest-first page into chronological (ascending) order so
        // the rest of the handler (ETag + render loop) sees oldest→newest.
        std::reverse(messages.begin(), messages.end());

        // ETag derives from (peer_id, count, max(created_at) + max(updated_at-equivalent)).
        // Messages don't track updated_at — the only mutation post-insert is
        // marking a message read (is_read flips 0→1 via PUT /messages/{id}/read).
        // Folding the sum of is_read into the tag catches that mutation without
        // a schema change. Vary: Cookie + Cache-Control: private — the payload
        // is scoped to the session pair, never cacheable by a shared proxy.
        std::int64_t maxTs = 0;
        std::int64_t readSum = 0;
        for (const auto& m : messages) {
            const auto ts = http_cache::parseTimestampMicros(
                m.getValueOfCreatedAt().toDbStringLocal());
            if (ts > maxTs) maxTs = ts;
            readSum += m.getValueOfIsRead() ? 1 : 0;
        }
        const std::string etag = http_cache::makeWeakEtag({
            "conv",
            std::to_string(userIdOpt.value()),
            std::to_string(otherUserId),
            std::to_string(static_cast<int>(messages.size())),
            std::to_string(maxTs),
            std::to_string(readSum),
        });
        constexpr std::string_view kVary = "Cookie";
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag, 0, kVary));
            return;
        }

        Json::Value ret;
        ret["messages"] = Json::Value(Json::arrayValue);

        for (const auto &message : messages) {
            Json::Value msgJson;
            msgJson["id"] = message.getValueOfId();
            msgJson["content"] = message.getValueOfContent();
            msgJson["is_read"] = message.getValueOfIsRead();
            msgJson["created_at"] = message.getValueOfCreatedAt().toDbStringLocal();
            msgJson["sender_id"] = message.getValueOfSenderId();
            msgJson["receiver_id"] = message.getValueOfReceiverId();

            ret["messages"].append(msgJson);
        }

        // Best-effort peer enrichment; the conversation rows still come back
        // even if the other user was deleted concurrently.
        try {
            auto otherUser = userMapper.findByPrimaryKey(otherUserId);
            ret["other_user"]["id"] = otherUser.getValueOfId();
            ret["other_user"]["username"] = otherUser.getValueOfUsername();
            if (!otherUser.getValueOfProfileImage().empty()) {
                ret["other_user"]["profile_image"] = otherUser.getValueOfProfileImage();
            }
        } catch (const DrogonDbException& e) {
            LOG_DEBUG << "other_user lookup for " << otherUserId
                      << " failed: " << e.base().what();
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag, 0, kVary);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch conversation";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::sendMessage(const HttpRequestPtr &req,
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

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Per-user send cap: 30 burst, 30/min. Stops a single authenticated
    // account from flooding messages (DB bloat + notification spam) even if
    // it rotates source IPs.
    if (auto rl = security::rateLimitOr429(
            "msg_send", "uid:" + std::to_string(userIdOpt.value()),
            30.0, 30.0 / 60.0)) {
        callback(rl);
        return;
    }

    int receiverId = (*json)["receiver_id"].asInt();
    std::string content = (*json)["content"].asString();

    if (receiverId <= 0 || content.empty()) {
        Json::Value ret;
        ret["error"] = "Receiver ID and content are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }
    if (content.size() > kMaxMessageBytes) {
        Json::Value ret;
        ret["error"] = "Message too long";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k413RequestEntityTooLarge);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Messages> mapper(dbClient);

    drogon_model::blog_db::Messages newMessage;
    newMessage.setSenderId(userIdOpt.value());
    newMessage.setReceiverId(receiverId);
    newMessage.setContent(content);
    newMessage.setIsRead(0);

    try {
        mapper.insert(newMessage);

        Json::Value msgJson;
        msgJson["id"]          = newMessage.getValueOfId();
        msgJson["sender_id"]   = newMessage.getValueOfSenderId();
        msgJson["receiver_id"] = newMessage.getValueOfReceiverId();
        msgJson["content"]     = newMessage.getValueOfContent();
        msgJson["is_read"]     = newMessage.getValueOfIsRead();
        msgJson["created_at"]  = newMessage.getValueOfCreatedAt().toDbStringLocal();

        // No in-process WS push here on purpose: the trg_messages_notify
        // trigger fires pg_notify on `blog_event`, the PgListener picks it
        // up and fans out to MessageWebSocket subscribers. Same path on
        // every node in a multi-instance deployment.

        Json::Value ret;
        ret["message"] = "Message sent successfully";
        ret["msg"]     = msgJson;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to send message";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::markAsRead(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback,
                                  int messageId)
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
    Mapper<drogon_model::blog_db::Messages> mapper(dbClient);

    try {
        auto message = mapper.findByPrimaryKey(messageId);

        // Check if user is the receiver
        if (message.getValueOfReceiverId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        message.setIsRead(1);
        mapper.update(message);

        Json::Value ret;
        ret["message"] = "Message marked as read";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Message not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to mark message as read";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::deleteMessage(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int messageId)
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
    Mapper<drogon_model::blog_db::Messages> mapper(dbClient);

    try {
        auto message = mapper.findByPrimaryKey(messageId);

        // Check if user is sender or receiver
        if (message.getValueOfSenderId() != userIdOpt.value() &&
            message.getValueOfReceiverId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(messageId);

        Json::Value ret;
        ret["message"] = "Message deleted successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Message not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to delete message";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
