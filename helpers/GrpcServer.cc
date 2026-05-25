#include "GrpcServer.h"

#include <trantor/utils/Logger.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#ifdef BLOG_HAS_GRPC
#include "../controllers/grpc/BlogReaderService.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#endif

namespace rpc {

namespace {

#ifdef BLOG_HAS_GRPC

// Owned by the install/stop pair. The Server object's destructor
// blocks until all RPCs drain, so we keep it alive until stop() runs.
std::unique_ptr<grpc::Server>            g_server;
std::unique_ptr<controllers::grpc_svc::BlogReaderService> g_blogReader;
std::thread                              g_thread;
std::atomic<bool>                        g_running{false};

void serve(const std::string& bindAddr)
{
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(bindAddr, grpc::InsecureServerCredentials());
    g_blogReader = std::make_unique<controllers::grpc_svc::BlogReaderService>();
    builder.RegisterService(g_blogReader.get());
    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG_ERROR << "grpc: BuildAndStart failed for " << bindAddr;
        g_running.store(false, std::memory_order_release);
        return;
    }
    LOG_INFO << "grpc: serving on " << bindAddr;
    // Wait() blocks until Shutdown() is called. The dedicated thread
    // means we never block Drogon's main loop.
    g_server->Wait();
}

#endif // BLOG_HAS_GRPC

} // namespace

bool install()
{
#ifndef BLOG_HAS_GRPC
    LOG_INFO << "grpc: build lacks libgrpc++; helper compiled out.";
    return false;
#else
    const char* env = std::getenv("BLOG_GRPC_PORT");
    if (!env || !*env) {
        LOG_INFO << "grpc: BLOG_GRPC_PORT unset; not starting server.";
        return false;
    }
    int port = 0;
    try { port = std::stoi(env); } catch (...) {
        LOG_ERROR << "grpc: malformed BLOG_GRPC_PORT=" << env;
        return false;
    }
    if (port <= 0 || port >= 65536) {
        LOG_ERROR << "grpc: out-of-range BLOG_GRPC_PORT=" << port;
        return false;
    }
    const std::string bindAddr = "0.0.0.0:" + std::to_string(port);
    g_running.store(true, std::memory_order_release);
    g_thread = std::thread([bindAddr] { serve(bindAddr); });
    return true;
#endif
}

void stop()
{
#ifdef BLOG_HAS_GRPC
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
    if (g_server) {
        // 5 s deadline: in-flight RPCs get that long to finish
        // before the server forces them dead. Matches the systemd /
        // K8s shutdown budget — graceful first, hard timeout next.
        const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        g_server->Shutdown(deadline);
    }
    if (g_thread.joinable()) g_thread.join();
    g_server.reset();
    g_blogReader.reset();
#endif
}

} // namespace rpc
