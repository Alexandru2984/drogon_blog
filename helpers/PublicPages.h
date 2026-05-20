#pragma once

#include <string>

// Wires the public, SEO/social-friendly endpoints:
//
//   GET /feed.xml            — Atom 1.0 feed of the latest 30 posts
//   GET /preview/posts/{id}  — server-rendered HTML carrying OpenGraph /
//                              Twitter Card meta tags and an immediate
//                              meta-refresh to the SPA hash URL, so link-
//                              preview crawlers see rich content while
//                              real users still land in the SPA.
//
// Idempotent; call once at startup.
namespace public_pages {
void install(const std::string& siteOrigin);
} // namespace public_pages
