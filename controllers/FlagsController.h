#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

// Read-only HTTP surface over the feature-flags helper.
//
// No admin endpoints by design — mutations go through the database
// directly (a migration, a manual psql, your config-as-code repo).
// The audit trail of "who flipped what" belongs in the deploy
// pipeline, not in HTTP request logs.
class FlagsController : public drogon::HttpController<FlagsController>
{
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(FlagsController::list,    "/flags",     Get);
        ADD_METHOD_TO(FlagsController::get,     "/flags/{1}", Get);
    METHOD_LIST_END

    void list(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& cb);

    void get(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb,
             std::string key);
};
