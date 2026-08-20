cmake_minimum_required(VERSION 3.5)
project(blog CXX)

include(CheckIncludeFileCXX)

check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)
if (NOT "${CMAKE_CXX_STANDARD}" STREQUAL "")
    # Do nothing
elseif (HAS_ANY AND HAS_STRING_VIEW AND HAS_COROUTINE)
    set(CMAKE_CXX_STANDARD 20)
elseif (HAS_ANY AND HAS_STRING_VIEW)
    set(CMAKE_CXX_STANDARD 17)
else ()
    set(CMAKE_CXX_STANDARD 14)
endif ()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clang-tidy / clangd / cppcheck can consume
# the same include paths and macro defines we actually use to build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(${PROJECT_NAME} main.cc)

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon 
# add_subdirectory(drogon) 
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
find_package(Drogon CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM    REQUIRED libsodium)
pkg_check_modules(VIPS_CPP  REQUIRED vips-cpp)
pkg_check_modules(CMARK_GFM REQUIRED libcmark-gfm)
pkg_check_modules(LIBPQ     REQUIRED libpq)
# hiredis is OPTIONAL — when present, helpers/Presence.cc compiles in
# its cross-node online-tracker (SETEX/EXISTS-based). Without it, the
# helper compiles to no-ops and the app runs single-pod as before.
# pkg-config name is `hiredis`; Ubuntu / Debian / Alpine all ship it
# under that name from libhiredis-dev / hiredis-dev.
pkg_check_modules(HIREDIS    QUIET     hiredis)
if (HIREDIS_FOUND)
    add_compile_definitions(BLOG_HAS_REDIS=1)
    message(STATUS "Redis presence tracker: ENABLED (hiredis ${HIREDIS_VERSION})")
else ()
    message(STATUS "Redis presence tracker: disabled (libhiredis-dev not found)")
endif ()

# gRPC — also optional. When grpc++ + protobuf + the codegen tools
# are present, we compile a parallel read-only gRPC server. Off →
# helpers/GrpcServer.cc is a stub and the binary still ships.
pkg_check_modules(GRPCPP     QUIET     grpc++)
pkg_check_modules(PROTOBUF   QUIET     protobuf)
find_program(PROTOC_BIN          NAMES protoc)
find_program(GRPC_CPP_PLUGIN_BIN NAMES grpc_cpp_plugin)
if (GRPCPP_FOUND AND PROTOBUF_FOUND AND PROTOC_BIN AND GRPC_CPP_PLUGIN_BIN)
    add_compile_definitions(BLOG_HAS_GRPC=1)
    message(STATUS "gRPC server: ENABLED (grpc++ ${GRPCPP_VERSION}, protobuf ${PROTOBUF_VERSION})")
    set(BLOG_GRPC_AVAILABLE TRUE)
else ()
    message(STATUS "gRPC server: disabled (need libgrpc++-dev + protobuf-compiler[-grpc])")
    set(BLOG_GRPC_AVAILABLE FALSE)
endif ()
# cmark-gfm-extensions ships without a .pc file in Debian/Ubuntu; we link
# it explicitly so the GFM extensions registry (tables, autolink, …)
# resolves at load time.
target_include_directories(${PROJECT_NAME} PRIVATE
    ${SODIUM_INCLUDE_DIRS}
    ${VIPS_CPP_INCLUDE_DIRS}
    ${CMARK_GFM_INCLUDE_DIRS}
    ${LIBPQ_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}/proto)
target_link_directories(${PROJECT_NAME} PRIVATE
    ${SODIUM_LIBRARY_DIRS}
    ${VIPS_CPP_LIBRARY_DIRS}
    ${CMARK_GFM_LIBRARY_DIRS}
    ${LIBPQ_LIBRARY_DIRS}
    ${HIREDIS_LIBRARY_DIRS}
    ${GRPCPP_LIBRARY_DIRS}
    ${PROTOBUF_LIBRARY_DIRS})
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
    ${GRPCPP_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    brotlidec brotlienc)

# ##############################################################################

if (CMAKE_CXX_STANDARD LESS 17)
    message(FATAL_ERROR "c++17 or higher is required")
elseif (CMAKE_CXX_STANDARD LESS 20)
    message(STATUS "use c++17")
else ()
    message(STATUS "use c++20")
endif ()

aux_source_directory(controllers CTL_SRC)
aux_source_directory(filters FILTER_SRC)
aux_source_directory(plugins PLUGIN_SRC)
aux_source_directory(models MODEL_SRC)
aux_source_directory(helpers HELPER_SRC)

# gRPC stub generation. The .proto file gets compiled into 4 sources
# in the build directory (blog.pb.{h,cc} from protoc, blog.grpc.pb.{h,cc}
# from grpc_cpp_plugin). We then add them to the target's source list
# alongside the hand-written service implementation under
# controllers/grpc/. Skipped silently when BLOG_HAS_GRPC is off.
set(BLOG_GRPC_SRC "")
if (BLOG_GRPC_AVAILABLE)
    set(_proto_in  ${CMAKE_CURRENT_SOURCE_DIR}/proto/blog.proto)
    set(_proto_out ${CMAKE_CURRENT_BINARY_DIR}/proto)
    file(MAKE_DIRECTORY ${_proto_out})
    set(_gen_files
        ${_proto_out}/blog.pb.cc        ${_proto_out}/blog.pb.h
        ${_proto_out}/blog.grpc.pb.cc   ${_proto_out}/blog.grpc.pb.h)
    add_custom_command(
        OUTPUT  ${_gen_files}
        COMMAND ${PROTOC_BIN}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                --cpp_out=${_proto_out}
                --grpc_out=${_proto_out}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_BIN}
                ${_proto_in}
        DEPENDS ${_proto_in}
        COMMENT "protoc + grpc_cpp_plugin -> blog.{pb,grpc.pb}.cc")
    add_custom_target(blog_grpc_codegen DEPENDS ${_gen_files})
    aux_source_directory(controllers/grpc GRPC_SVC_SRC)
    list(APPEND BLOG_GRPC_SRC
        ${_proto_out}/blog.pb.cc
        ${_proto_out}/blog.grpc.pb.cc
        ${GRPC_SVC_SRC})
endif ()

drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
                    ${CMAKE_CURRENT_BINARY_DIR})
# use the following line to create views with namespaces.
# drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
#                     ${CMAKE_CURRENT_BINARY_DIR} TRUE)
# use the following line to create views with namespace CHANGE_ME prefixed
# and path namespaces.
# drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
#                     ${CMAKE_CURRENT_BINARY_DIR} TRUE CHANGE_ME)

target_include_directories(${PROJECT_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                   ${CMAKE_CURRENT_SOURCE_DIR}/models)
target_sources(${PROJECT_NAME}
               PRIVATE
               ${SRC_DIR}
               ${CTL_SRC}
               ${FILTER_SRC}
               ${PLUGIN_SRC}
               ${MODEL_SRC}
               ${HELPER_SRC}
               ${BLOG_GRPC_SRC})
# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)

# ##############################################################################

add_subdirectory(test)
