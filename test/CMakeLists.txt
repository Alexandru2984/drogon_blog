cmake_minimum_required(VERSION 3.10)
project(blog_test CXX)

set(TEST_SOURCES
    test_main.cc
    test_auth.cc
    test_posts.cc
    test_password_hashing.cc
    test_search.cc
    test_security.cc
    test_log_safety.cc
    test_image_pipeline.cc
    test_markdown.cc
    test_2fa.cc
    test_2fa_management.cc
    test_caching.cc
    test_workers.cc
    test_sessions.cc
    test_password_policy.cc
    test_moderation.cc
    test_likes.cc
    test_post_meta.cc
    test_social.cc
    test_editor.cc
    test_privacy.cc
    test_draft_visibility.cc)

add_executable(${PROJECT_NAME} ${TEST_SOURCES})
target_compile_definitions(${PROJECT_NAME} PRIVATE BLOG_TEST_BUILD=1)

# Bring in the project's own sources so tests link against the same controllers,
# helpers and ORM models that the production binary uses.
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/../controllers TEST_CTL_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/../helpers     TEST_HELPER_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/../models      TEST_MODEL_SRC)

# Strip the gRPC bridge sources from the test build — the tests don't
# exercise the gRPC surface (the REST controllers cover the same DB
# code paths) and pulling them in would require teaching this CMake
# about the protoc codegen step. Easier to keep the test target
# pure-REST.
list(FILTER TEST_HELPER_SRC EXCLUDE REGEX "/GrpcServer\\.cc$")

target_sources(${PROJECT_NAME} PRIVATE
    ${TEST_CTL_SRC}
    ${TEST_HELPER_SRC}
    ${TEST_MODEL_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../models
    ${SODIUM_INCLUDE_DIRS}
    ${VIPS_CPP_INCLUDE_DIRS}
    ${CMARK_GFM_INCLUDE_DIRS}
    ${LIBPQ_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS})

target_link_directories(${PROJECT_NAME} PRIVATE
    ${SODIUM_LIBRARY_DIRS}
    ${VIPS_CPP_LIBRARY_DIRS}
    ${CMARK_GFM_LIBRARY_DIRS}
    ${LIBPQ_LIBRARY_DIRS}
    ${HIREDIS_LIBRARY_DIRS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Drogon::Drogon
    CURL::libcurl
    OpenSSL::SSL OpenSSL::Crypto
    ${SODIUM_LIBRARIES}
    ${VIPS_CPP_LIBRARIES}
    ${CMARK_GFM_LIBRARIES}
    cmark-gfm-extensions
    ${LIBPQ_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    brotlidec brotlienc)

# Exercise the optional gRPC reader whenever the toolchain is present. Reuse
# the protobuf outputs generated for the production build and compile the
# exact service implementation that ships, instead of testing a SQL
# look-alike.
if (BLOG_GRPC_AVAILABLE)
    set(TEST_GRPC_GENERATED_SOURCES
        ${CMAKE_BINARY_DIR}/proto/blog.pb.cc
        ${CMAKE_BINARY_DIR}/proto/blog.grpc.pb.cc)
    # GENERATED was directory-scoped before CMake 3.20, so repeat the property
    # here where the test target consumes the parent's custom-command outputs.
    set_source_files_properties(${TEST_GRPC_GENERATED_SOURCES}
        PROPERTIES GENERATED TRUE)
    target_sources(${PROJECT_NAME} PRIVATE
        test_grpc.cc
        ${TEST_GRPC_GENERATED_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../controllers/grpc/BlogReaderService.cc)
    add_dependencies(${PROJECT_NAME} blog_grpc_codegen)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}/proto
        ${GRPCPP_INCLUDE_DIRS}
        ${PROTOBUF_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE
        ${GRPCPP_LIBRARY_DIRS}
        ${PROTOBUF_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${GRPCPP_LIBRARIES}
        ${PROTOBUF_LIBRARIES})
endif ()

ParseAndAddDrogonTests(${PROJECT_NAME})
