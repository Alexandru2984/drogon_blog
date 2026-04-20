#include "UserController.h"
#include "../models/Users.h"
#include <drogon/orm/Mapper.h>
#include <drogon/MultiPart.h>
#include <trantor/utils/Logger.h>
#include <fstream>

using namespace drogon;
using namespace drogon::orm;

void UserController::getUserProfile(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback,
                                   int userId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::sqlite3::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userId);

        Json::Value ret;
        ret["id"] = user.getValueOfId();
        ret["username"] = user.getValueOfUsername();
        ret["email"] = user.getValueOfEmail();
        ret["bio"] = user.getValueOfBio();
        ret["created_at"] = user.getValueOfCreatedAt().toDbStringLocal();
        
        if (!user.getValueOfProfileImage().empty()) {
            ret["profile_image"] = user.getValueOfProfileImage();
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
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
    Mapper<drogon_model::sqlite3::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());

        if (json->isMember("email")) {
            user.setEmail((*json)["email"].asString());
        }
        if (json->isMember("bio")) {
            user.setBio((*json)["bio"].asString());
        }

        mapper.update(user);

        Json::Value ret;
        ret["message"] = "Profile updated successfully";
        ret["user"]["id"] = user.getValueOfId();
        ret["user"]["username"] = user.getValueOfUsername();
        ret["user"]["email"] = user.getValueOfEmail();
        ret["user"]["bio"] = user.getValueOfBio();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
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
    
    // Generate unique filename
    auto fileExtension = file.getFileName();
    auto dotPos = fileExtension.find_last_of('.');
    if (dotPos != std::string::npos) {
        fileExtension = fileExtension.substr(dotPos);
    } else {
        fileExtension = ".jpg";
    }
    
    std::string filename = "profile_" + std::to_string(userIdOpt.value()) + 
                          "_" + std::to_string(std::time(nullptr)) + fileExtension;
    std::string uploadPath = "uploads/profiles/";
    
    // Create directory if it doesn't exist
    system(("mkdir -p " + uploadPath).c_str());
    
    std::string fullPath = uploadPath + filename;
    
    // Save file
    file.saveAs(fullPath);

    // Update database
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::sqlite3::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());
        user.setProfileImage("/" + fullPath);
        mapper.update(user);

        Json::Value ret;
        ret["message"] = "Profile image uploaded successfully";
        ret["profile_image"] = "/" + fullPath;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
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
    Mapper<drogon_model::sqlite3::Users> mapper(dbClient);

    try {
        auto users = mapper.findAll();

        Json::Value ret;
        ret["users"] = Json::Value(Json::arrayValue);

        for (const auto &user : users) {
            // Don't include current user
            if (user.getValueOfId() == userIdOpt.value()) {
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
