#pragma once

// Read-only gRPC server bolted alongside Drogon's HTTP listener.
//
// Activation:
//   BLOG_GRPC_PORT=8093   (any non-zero TCP port; common picks: 8093,
//                          50051). When unset, install() is a no-op
//                          and the binary runs HTTP-only as before.
//
// Listens on 0.0.0.0:<port>, plaintext (no TLS). Production should
// front this with a sidecar / envoy / nginx-stream proxy if TLS is
// required; baking TLS into this server would duplicate the cert
// rotation logic that already lives in nginx upstream.
//
// Build-time gate: when libgrpc++ + protobuf + protoc aren't
// available, the helper compiles to a stub (#ifdef BLOG_HAS_GRPC).
// See CMakeLists.txt — same shape as the hiredis gate.
namespace rpc {

// Start the server in a background thread. Idempotent.
bool install();

// Stop + join. Called from runOnQuit alongside other lifecycle hooks.
void stop();

} // namespace rpc
