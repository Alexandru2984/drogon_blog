#include "UserController.h"
#include "../models/Users.h"
#include "../helpers/AuditLog.h"
#include "../helpers/EmailHelper.h"
#include "../helpers/HttpCache.h"
#include "../helpers/ImageProcessor.h"
#include "../helpers/Presence.h"
#include "../helpers/Roles.h"
#include "../helpers/Security.h"
#include "../helpers/Workers.h"
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
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userId] {
        auto dbClient = drogon::app().getDbClient();
        Mapper<drogon_model::blog_db::Users> mapper(dbClient);

        try {
            auto user = mapper.findByPrimaryKey(userId);

            if (roles::isErased(userId)) {
                Json::Value ret;
                ret["error"] = "User not found";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k404NotFound);
                callback(resp);
                return;
            }

            const bool online = presence::isOnline(user.getValueOfId());
            const std::string etag = http_cache::makeWeakEtag({
                std::to_string(user.getValueOfId()),
                std::to_string(http_cache::parseTimestampMicros(
                    user.getValueOfUpdatedAt().toDbStringLocal())),
                online ? "1" : "0",
            });
            if (http_cache::ifNoneMatchHit(req, etag)) {
                callback(http_cache::makeNotModified(etag));
                return;
            }

            Json::Value ret;
            ret["id"] = user.getValueOfId();
            ret["username"] = user.getValueOfUsername();
            ret["bio"] = user.getValueOfBio();
            ret["created_at"] = user.getValueOfCreatedAt().toDbStringLocal();

            if (!user.getValueOfProfileImage().empty()) {
                ret["profile_image"] = user.getValueOfProfileImage();
            }
            if (online) {
                ret["online"] = true;
            }

            auto resp = HttpResponse::newHttpJsonResponse(ret);
            http_cache::applyCacheHeaders(resp, etag);
            callback(resp);
        } catch (const DrogonDbException &) {
            Json::Value ret;
            ret["error"] = "User not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
        }
    });
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

    if (auto rl = security::rateLimitOr429(
            "profile_update", "uid:" + std::to_string(userIdOpt.value()),
            15.0, 15.0 / 300.0)) {
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

    // The email-change branch re-authenticates with Argon2id (~158 ms) and
    // the surrounding ORM calls are synchronous, so the whole body runs on
    // the auth pool rather than an IO loop.
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, json, userIdOpt] {

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());

        // The address to warn if the email changes — captured before the
        // change is applied so the notice reaches the previous owner.
        std::string emailChangedFrom;
        std::string newEmailToVerify;
        std::string verificationToken;

        // Changing the email is privileged: an attacker who hijacks a live
        // session could otherwise pivot the account by retargeting password
        // recovery. Require the current password and re-issue verification.
        if (json->isMember("email")) {
            if (!(*json)["email"].isString()) {
                Json::Value ret;
                ret["error"] = "Invalid email address";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k400BadRequest);
                callback(resp);
                return;
            }
            const std::string newEmail = (*json)["email"].asString();
            if (newEmail != user.getValueOfEmail()) {
                if (!security::emailLooksValid(newEmail)) {
                    Json::Value ret;
                    ret["error"] = "Invalid email address";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k400BadRequest);
                    callback(resp);
                    return;
                }
                if (auto rl = security::rateLimitOr429(
                        "email_change", "uid:" + std::to_string(userIdOpt.value()),
                        3.0, 3.0 / 900.0)) {
                    callback(rl);
                    return;
                }
                const std::string currentPassword =
                    (*json)["current_password"].isString()
                        ? (*json)["current_password"].asString()
                        : std::string{};
                if (currentPassword.empty() ||
                    currentPassword.size() > 256 ||
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

                // Check email uniqueness before mutating or dispatching mail,
                // so a collision does not trigger a 500 or send mail to an
                // address owned by someone else.
                const auto existing = dbClient->execSqlSync(
                    "SELECT 1 FROM users WHERE email = $1 AND id <> $2",
                    newEmail, userIdOpt.value());
                if (!existing.empty()) {
                    Json::Value ret;
                    ret["error"] = "Could not update email address";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k409Conflict);
                    callback(resp);
                    return;
                }

                emailChangedFrom = user.getValueOfEmail();
                newEmailToVerify = newEmail;
                verificationToken = EmailHelper::generateToken();

                user.setEmail(newEmail);
                user.setEmailVerified(0);
                user.setEmailVerificationToken(
                    security::sha256Hex(verificationToken));
                user.setEmailVerificationExpires(
                    trantor::Date::now().after(24 * 3600));
            }
        }
        if (json->isMember("bio")) {
            if (!(*json)["bio"].isString()) {
                Json::Value ret;
                ret["error"] = "Invalid bio";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k400BadRequest);
                callback(resp);
                return;
            }
            const std::string newBio = (*json)["bio"].asString();
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

        // Only after the database update succeeds: record the audit entry and
        // notify both the new address (verification link) and the previous one.
        if (!emailChangedFrom.empty()) {
            Json::Value meta;
            meta["old_email"] = emailChangedFrom;
            meta["new_email"] = newEmailToVerify;
            audit_log::record(req, {"profile.email.change",
                                    userIdOpt,
                                    std::string{"user"},
                                    static_cast<std::int64_t>(userIdOpt.value()),
                                    std::move(meta)});
            EmailHelper::sendVerificationEmail(
                newEmailToVerify, user.getValueOfUsername(), verificationToken);
            EmailHelper::sendEmailChangedNotice(emailChangedFrom,
                                                user.getValueOfUsername());
        }

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

        });
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

    // libvips holds its calling thread for the whole decode/resize/encode
    // pipeline, and this handler runs inline on one of Drogon's IO loops —
    // doing the work here stalls every other connection assigned to that
    // loop for the duration. Hand it to the media pool instead. The ORM
    // update that follows is synchronous too, so it belongs in the same job
    // rather than being bounced back to the loop.
    const bool queued = workers::offload(workers::Pool::Media, callback,
        [callback, tmpPath, finalPath, publicPath, userIdOpt] {
            // Validate + resize + EXIF-strip via libvips.
            const auto result = image::processAvatar(tmpPath, finalPath);

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

            // Persist the public path on the user row.
            auto dbClient = drogon::app().getDbClient();
            Mapper<drogon_model::blog_db::Users> mapper(dbClient);

            try {
                auto user = mapper.findByPrimaryKey(userIdOpt.value());
                const std::string oldImage = user.getValueOfProfileImage();
                user.setProfileImage(publicPath);
                mapper.update(user);

                // Delete the avatar this one replaces. Without it every
                // re-upload left the previous JPEG behind, so uploads/profiles/
                // grew without bound. Confined to the profiles dir and skipped
                // when unchanged, so it can only ever remove a superseded
                // avatar this endpoint wrote.
                if (oldImage != publicPath &&
                    oldImage.rfind("/uploads/profiles/", 0) == 0 &&
                    oldImage.find("..") == std::string::npos)
                {
                    std::error_code oldEc;
                    std::filesystem::remove("." + oldImage, oldEc);
                }

                Json::Value ret;
                ret["message"]       = "Profile image uploaded successfully";
                ret["profile_image"] = publicPath;
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const UnexpectedRows &) {
                // The user row vanished between session creation and now —
                // clean up the orphaned upload before responding.
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
        });

    // Saturated pool: offload() already answered 503, but the job that
    // would have removed the tmp file never ran.
    if (!queued) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
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

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt] {
        auto dbClient = drogon::app().getDbClient();

        try {
            constexpr int kMaxUserPage = 100;
            int limit = 50;
            const auto l = req->getParameter("limit");
            if (!l.empty()) { try { const int v = std::stoi(l); if (v > 0) limit = std::min(v, kMaxUserPage); } catch (...) {} }

            // Case-insensitive username prefix search. LIKE metacharacters in
            // the user's input are escaped so "a_%" matches literally.
            std::string pattern;
            const auto q = req->getParameter("q");
            if (!q.empty() && q.size() <= 64) {
                pattern.reserve(q.size() + 4);
                for (char c : q) {
                    if (c == '\\' || c == '%' || c == '_') pattern.push_back('\\');
                    pattern.push_back(c);
                }
                pattern.push_back('%');
            }
            long long before = 0;
            const auto b = req->getParameter("before");
            if (!b.empty()) { try { before = std::max(0LL, std::stoll(b)); } catch (...) {} }

            // Tombstoned accounts are filtered in SQL. The previous version
            // loaded every erased user id into memory on each request, which
            // grows without bound and turned this endpoint into a cheap way to
            // make the server do O(all deleted users) work.
            const auto rows = dbClient->execSqlSync(
                "SELECT id, username, COALESCE(profile_image, '') AS profile_image, "
                "       COALESCE(bio, '') AS bio "
                "FROM users "
                "WHERE deleted_at IS NULL AND id <> $1 "
                "  AND ($2 = '' OR username ILIKE $2 ESCAPE '\\') "
                "  AND ($3::bigint = 0 OR id < $3::bigint) "
                "ORDER BY id DESC LIMIT $4::int",
                userIdOpt.value(), pattern, before, limit);

            Json::Value ret;
            ret["users"] = Json::Value(Json::arrayValue);
            std::int64_t minId = 0;
            for (const auto& row : rows) {
                const auto uid = row["id"].as<std::int64_t>();
                if (minId == 0 || uid < minId) minId = uid;
                Json::Value userJson;
                userJson["id"] = static_cast<Json::Int64>(uid);
                userJson["username"] = row["username"].as<std::string>();
                const auto img = row["profile_image"].as<std::string>();
                if (!img.empty()) userJson["profile_image"] = img;
                const auto bio = row["bio"].as<std::string>();
                if (!bio.empty()) {
                    if (bio.size() <= 160) {
                        userJson["bio"] = bio;
                    } else {
                        std::size_t end = 160;
                        while (end > 0 && (static_cast<unsigned char>(bio[end]) & 0xC0) == 0x80) {
                            --end;
                        }
                        userJson["bio"] = bio.substr(0, end) + "…";
                    }
                }
                userJson["online"] = presence::isOnline(static_cast<int>(uid));
                ret["users"].append(userJson);
            }

            ret["next_cursor"] = (static_cast<int>(rows.size()) == limit && minId > 0)
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
    });
}
