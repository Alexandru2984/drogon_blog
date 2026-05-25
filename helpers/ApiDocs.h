#pragma once

// Registers OpenAPI 3.1 documentation routes:
//   GET /api/openapi.yaml  → serves the spec file from disk with
//                            Content-Type: application/yaml
//   GET /api/docs          → returns an HTML page that renders the
//                            spec via Redoc (single CDN-loaded bundle)
//
// File resolution is at request time, looking up `openapi/blog.openapi.yaml`
// relative to the process CWD. The systemd unit and Dockerfile both set
// the CWD so this single relative path works in dev, prod, and inside
// the container image.
namespace api_docs {
void install();
} // namespace api_docs
