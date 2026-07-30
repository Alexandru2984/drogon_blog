cmake_minimum_required(VERSION 3.10)
project(blog_test CXX)

set(TEST_SOURCES
    test_main.cc
    test_auth.cc
    test_posts.cc
    test_password_hashing.cc
    test_search.cc
    test_security.cc
    test_image_pipeline.cc
    test_markdown.cc
    test_2fa.cc
    test_caching.cc
    test_workers.cc
    test_sessions.cc
    test_password_policy.cc
    test_moderation.cc
    test_likes.cc
    test_post_meta.cc
    test_social.cc
    test_editor.cc
    test_privacy.cc)

add_executable(${PROJECT_NAME} ${TEST_SOURCES})

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

ParseAndAddDrogonTests(${PROJECT_NAME})
