#pragma once

// Forward-declare the generated service base so this header doesn't
// drag the heavy grpcpp/protobuf includes into the rest of the
// codebase. The .cc owns the full include chain.
#include "blog.grpc.pb.h"

namespace controllers::grpc_svc {

class BlogReaderService final : public blog::v1::BlogReader::Service {
public:
    ::grpc::Status GetPost(
        ::grpc::ServerContext*           ctx,
        const ::blog::v1::GetPostRequest* req,
        ::blog::v1::Post*                 resp) override;

    ::grpc::Status ListPosts(
        ::grpc::ServerContext*               ctx,
        const ::blog::v1::ListPostsRequest*  req,
        ::blog::v1::ListPostsResponse*       resp) override;
};

} // namespace controllers::grpc_svc
