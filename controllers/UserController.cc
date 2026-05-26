#include "UserController.h"
#include "../models/Users.h"
#include "../helpers/AuditLog.h"
#include "../helpers/EmailHelper.h"
#include "../helpers/HttpCache.h"
#include "../helpers/ImageProcessor.h"
#include "../helpers/Presence.h"
#include "../helpers/Security.h"
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <drogon/MultiPart.h>
#include <trantor/utils/Logger.h>
#include <filesystem>
#include <fstream>

using namespace drogon;
using namespace drogon::orm;

void UserController::getUserProfile(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback,
                                   int userId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userId);

        // Public profile — same data for every viewer, so ETag is keyed
        // only on (id, updated_at) and no Vary is needed. updated_at
        // moves on every profile mutation via the BEFORE-UPDATE trigger.
        const std::string etag = http_cache::makeWeakEtag({
            std::to_string(user.getValueOfId()),
            std::to_string(http_cache::parseTimestampMicros(
                user.getValueOfUpdatedAt().toDbStringLocal())),
        });
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag));
            return;
        }

        Json::Value ret;
        ret["id"] = user.getValueOfId();
        ret["username"] = user.getValueOfUsername();
        // NB: email is deliberately NOT exposed here. /users/{id} is a public,
        // unauthenticated endpoint — returning the address leaked PII and aided
        // account enumeration. The owner sees their own email via /auth/me
        // (session-scoped) and /users/profile.
        ret["bio"] = user.getValueOfBio();
        ret["created_at"] = user.getValueOfCreatedAt().toDbStringLocal();

        if (!user.getValueOfProfileImage().empty()) {
            ret["profile_image"] = user.getValueOfProfileImage();
        }
        // Online flag is read AFTER ETag derivation on purpose:
        // presence is volatile (5-30 s TTL window), and including
        // it in the cache key would invalidate the entry every time
        // a heartbeat ticks. We accept that a 304 may show a stale
        // online flag — clients that care can drop their cache and
        // re-fetch.
        if (presence::isOnline(user.getValueOfId())) {
            ret["online"] = true;
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag);
        callback(resp);
    } catch (const DrogonDbException &e) {
        Json::Value ret;
        ret["error"] = "User not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    }
}

void UserController::updateProfile(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());

        // Changing the email is privileged: an attacker who hijacks a live
        // session could otherwise pivot the account by retargeting password
        // recovery. Require the current password and re-issue verification.
        if (json->isMember("email")) {
            const std::string newEmail = (*json)["email"].asString();
            if (newEmail != user.getValueOfEmail()) {
                if (!security::emailLooksValid(newEmail)) {
                    // Same SMTP-header-injection guard as /auth/register:
                    // the new address lands in EmailHelper's To: header
                    // when the verification mail goes out.
                    Json::Value ret;
                    ret["error"] = "Invalid email address";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k400BadRequest);
                    callback(resp);
                    return;
                }
                const std::string currentPassword =
                    (*json)["current_password"].asString();
                if (currentPassword.empty() ||
                    !security::verifyPassword(user.getValueOfPasswordHash(),
                                              currentPassword))
                {
                    Json::Value ret;
                    ret["error"] = "Current password is required to change email";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k403Forbidden);
                    callback(resp);
                    return;
                }
                Json::Value meta;
                meta["old_email"] = user.getValueOfEmail();
                meta["new_email"] = newEmail;
                audit_log::record(req, {"profile.email.change",
                                        userIdOpt,
                                        std::string{"user"},
                                        static_cast<std::int64_t>(userIdOpt.value()),
                                        std::move(meta)});

                user.setEmail(newEmail);
                user.setEmailVerified(0);
                // Hash the verification token at rest, same as registerUser
                // and resendVerification (D-audit fix G+H). The previous
                // version stored the plaintext here.
                const std::string verificationToken = EmailHelper::generateToken();
                user.setEmailVerificationToken(
                    security::sha256Hex(verificationToken));
                user.setEmailVerificationExpires(
                    trantor::Date::now().after(24 * 3600));

                EmailHelper::sendVerificationEmail(
                    newEmail, user.getValueOfUsername(), verificationToken);
            }
        }
        if (json->isMember("bio")) {
            const std::string newBio = (*json)["bio"].asString();
            // 8 KiB is generous for a blog bio; protects the users.bio
            // column from being weaponised as DB-bloat storage by an
            // authenticated client looping PUT /users/profile.
            if (newBio.size() > std::size_t{8} * 1024) {
                Json::Value ret;
                ret["error"] = "Bio too long";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k413RequestEntityTooLarge);
                callback(resp);
                return;
            }
            user.setBio(newBio);
        }

        mapper.update(user);

        Json::Value ret;
        ret["message"]          = "Profile updated successfully";
        ret["user"]["id"]       = user.getValueOfId();
        ret["user"]["username"] = user.getValueOfUsername();
        ret["user"]["email"]    = user.getValueOfEmail();
        ret["user"]["bio"]      = user.getValueOfBio();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "User not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to update profile";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void UserController::uploadProfileImage(const HttpRequestPtr &req,
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

    // Per-user avatar cap: 3 burst, 3/10min — image processing (libvips) is
    // the most expensive authenticated operation; this bounds CPU abuse.
    if (auto rl = security::rateLimitOr429(
            "avatar_upload", "uid:" + std::to_string(userIdOpt.value()),
            3.0, 3.0 / 600.0)) {
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

    auto &files = fileUpload.getFiles();
    if (files.empty()) {
        Json::Value ret;
        ret["error"] = "No file uploaded";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto &file = files[0];

    // Both source and destination directories. The upload first lands in
    // `uploads/tmp/` so it doesn't pollute the served `profiles/` dir when
    // processing fails halfway. The "./" prefix tells Drogon's MultiPartParser
    // to use the path as-is instead of prepending app().getUploadPath() —
    // otherwise saveAs("uploads/tmp/foo") lands in uploads/uploads/tmp/foo.
    const std::string profilesDir = "./uploads/profiles/";
    const std::string tmpDir      = "./uploads/tmp/";
    std::error_code ec;
    std::filesystem::create_directories(profilesDir, ec);
    std::filesystem::create_directories(tmpDir,      ec);
    if (ec) {
        LOG_ERROR << "Failed to create upload dirs: " << ec.message();
        Json::Value ret;
        ret["error"] = "Failed to save upload";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    // Stamp the filename server-side; the user-supplied filename is never
    // trusted for path construction. ".jpg" is the final extension regardless
    // of input format — the image pipeline always emits JPEG.
    //
    // Random suffix (libsodium-backed, 12 bytes ≈ 96 bits) instead of a
    // timestamp: a millisecond counter is trivially guessable, and a
    // racing process could try to interfere with the tmp file between
    // saveAs() and processAvatar() if it could predict the name. With
    // a 96-bit token the race is closed against any practical attacker
    // and the final public URL stops leaking upload time.
    const std::string stem = "profile_" + std::to_string(userIdOpt.value())
                           + "_" + security::randomToken(12);
    const std::string tmpPath   = tmpDir      + stem + ".upload";
    const std::string finalPath = profilesDir + stem + ".jpg";
    // Same on-disk file, addressed via a clean URL path served by Drogon's
    // document_root → public/uploads symlink.
    const std::string publicPath = "/uploads/profiles/" + stem + ".jpg";

    file.saveAs(tmpPath);

    // Validate + resize + EXIF-strip via libvips.
    const auto result = image::processAvatar(tmpPath, finalPath);

    std::error_code rmEc;
    std::filesystem::remove(tmpPath, rmEc);                // best effort

    if (!result.ok) {
        Json::Value ret;
        ret["error"] = result.error;
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(static_cast<HttpStatusCode>(result.status));
        callback(resp);
        return;
    }

    // Persist the public path on the user row.
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());
        user.setProfileImage(publicPath);
        mapper.update(user);

        Json::Value ret;
        ret["message"]       = "Profile image uploaded successfully";
        ret["profile_image"] = publicPath;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        // The user row vanished between session creation and now — clean up
        // the orphaned upload before responding.
        std::filesystem::remove(finalPath, rmEc);
        Json::Value ret;
        ret["error"] = "User not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        std::filesystem::remove(finalPath, rmEc);
        Json::Value ret;
        ret["error"] = "Failed to update profile image";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void UserController::getAllUsers(const HttpRequestPtr &req,
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
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        // Bounded directory listing. findAll() returned every row, which is
        // unbounded enumeration + a DoS vector as the user count grows. Cap at
        // 100 rows ordered by id, with an optional `?q=` username prefix search
        // for the SPA's "start a conversation" picker. `before` pages further.
        using Cols = drogon_model::blog_db::Users::Cols;
        constexpr int kMaxUserPage = 100;
        int limit = kMaxUserPage;
        const auto l = req->getParameter("limit");
        if (!l.empty()) { try { const int v = std::stoi(l); if (v > 0 && v < kMaxUserPage) limit = v; } catch (...) {} }

        Criteria crit(Cols::_id, CompareOperator::GE, 1);   // always-true base
        const auto q = req->getParameter("q");
        if (!q.empty() && q.size() <= 64)
            crit = crit && Criteria(Cols::_username, CompareOperator::Like, q + "%");
        const auto before = req->getParameter("before");
        if (!before.empty()) {
            try { const long long v = std::stoll(before); if (v > 0)
                crit = crit && Criteria(Cols::_id, CompareOperator::LT, v); }
            catch (...) {}
        }

        auto users = mapper.orderBy(Cols::_id, SortOrder::DESC)
                           .limit(static_cast<std::size_t>(limit))
                           .findBy(crit);

        Json::Value ret;
        ret["users"] = Json::Value(Json::arrayValue);
        std::int64_t minId = 0;

        for (const auto &user : users) {
            const auto uid = user.getValueOfId();
            if (minId == 0 || uid < minId) minId = uid;
            // Don't include current user
            if (uid == userIdOpt.value()) {
                continue;
            }

            Json::Value userJson;
            userJson["id"] = user.getValueOfId();
            userJson["username"] = user.getValueOfUsername();
            
            if (!user.getValueOfProfileImage().empty()) {
                userJson["profile_image"] = user.getValueOfProfileImage();
            }

            ret["users"].append(userJson);
        }

        ret["next_cursor"] = (static_cast<int>(users.size()) == limit && minId > 0)
            ? Json::Value(static_cast<Json::Int64>(minId)) : Json::nullValue;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch users";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
