#include <drogon/drogon.h>
#include <drogon/drogon_test.h>

#include "../controllers/grpc/BlogReaderService.h"

#include <chrono>
#include <string>

using namespace drogon;

DROGON_TEST(GrpcReader_DoesNotExposeDrafts)
{
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    auto db = app().getDbClient();
    REQUIRE(db);

    const auto users = db->execSqlSync(
        "INSERT INTO users (username, email, password_hash) "
        "VALUES ($1, $2, $3) RETURNING id",
        "grpcdraft_" + suffix,
        "grpcdraft_" + suffix + "@example.test",
        "not-a-login-capable-hash");
    REQUIRE(!users.empty());

    const auto drafts = db->execSqlSync(
        "INSERT INTO posts (user_id, title, content, content_html, "
        "                   published_at) "
        "VALUES ($1, $2, $3, $4, NULL) RETURNING id",
        users[0]["id"].as<int>(),
        "Private gRPC draft " + suffix,
        "Confidential body " + suffix,
        "<p>Confidential body</p>");
    REQUIRE(!drafts.empty());
    const int draftId = drafts[0]["id"].as<int>();

    controllers::grpc_svc::BlogReaderService service;
    grpc::ServerContext context;

    blog::v1::GetPostRequest oneRequest;
    oneRequest.set_id(draftId);
    blog::v1::Post oneResponse;
    const auto oneStatus = service.GetPost(&context, &oneRequest, &oneResponse);
    CHECK(oneStatus.error_code() == grpc::StatusCode::NOT_FOUND);

    blog::v1::ListPostsRequest listRequest;
    listRequest.set_limit(50);
    blog::v1::ListPostsResponse listResponse;
    const auto listStatus = service.ListPosts(
        &context, &listRequest, &listResponse);
    REQUIRE(listStatus.ok());
    for (const auto& post : listResponse.posts()) {
        CHECK(post.id() != draftId);
    }
}
