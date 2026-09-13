#pragma once

// Read-only gRPC server bolted alongside Drogon's HTTP listener.
//
// Activation:
//   BLOG_GRPC_PORT=8093   (any non-zero TCP port; common picks: 8093,
//                          50051). When unset, install() is a no-op
//                          and the binary runs HTTP-only as before.
//
// Binds to BLOG_GRPC_ADDR, falling back to BLOG_LISTEN_ADDR, falling
// back to 127.0.0.1 — i.e. it defaults to whatever interface the HTTP
// listener uses, not to every interface. This surface has no
// authentication (see docs/adr/0009-readonly-grpc.md), so exposing it
// beyond loopback needs an explicit BLOG_GRPC_ADDR and a network layer
// (VPN, mTLS sidecar, firewalled service-to-service link) in front of
// it — ufw alone is not that layer.
//
// Plaintext (no TLS) regardless of bind address. Production should
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
