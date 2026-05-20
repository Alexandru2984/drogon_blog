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
    test_markdown.cc)

add_executable(${PROJECT_NAME} ${TEST_SOURCES})

# Bring in the project's own sources so tests link against the same controllers,
# helpers and ORM models that the production binary uses.
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/../controllers TEST_CTL_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/../helpers     TEST_HELPER_SRC)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/../models      TEST_MODEL_SRC)

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
    ${LIBPQ_INCLUDE_DIRS})

target_link_directories(${PROJECT_NAME} PRIVATE
    ${SODIUM_LIBRARY_DIRS}
    ${VIPS_CPP_LIBRARY_DIRS}
    ${CMARK_GFM_LIBRARY_DIRS}
    ${LIBPQ_LIBRARY_DIRS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Drogon::Drogon
    CURL::libcurl
    OpenSSL::SSL OpenSSL::Crypto
    ${SODIUM_LIBRARIES}
    ${VIPS_CPP_LIBRARIES}
    ${CMARK_GFM_LIBRARIES}
    cmark-gfm-extensions
    ${LIBPQ_LIBRARIES}
    brotlidec brotlienc)

ParseAndAddDrogonTests(${PROJECT_NAME})
