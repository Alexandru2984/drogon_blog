#pragma once

#include <string>

namespace markdown {

// Hard cap on the input we'll feed cmark-gfm. CommonMark parsers
// historically suffer from pathological-input bugs (deeply nested lists,
// long blockquote chains) that take exponential time to parse and
// occupy an IO thread the entire time. Capping the source at 100 KiB
// keeps any single render bounded; legitimate blog posts are several
// orders of magnitude below this.
inline constexpr std::size_t kMaxMarkdownBytes = std::size_t{100} * 1024;

// Convert a CommonMark / GitHub-flavored Markdown string into safe HTML.
//
// "Safe" means:
//   * raw HTML in the input is escaped (CMARK_OPT_SAFE), so a user can't
//     inject <script>, <iframe>, on*-handlers, or style attributes via the
//     post body;
//   * `javascript:`, `data:` and `vbscript:` URLs in links/images are
//     filtered by the GFM safe mode;
//   * GFM extensions enabled: tables, strikethrough, autolinks, task lists;
//   * inputs longer than kMaxMarkdownBytes return an empty string — the
//     caller is expected to have rejected them at the API boundary with
//     a 413, this is a belt-and-braces fallback for paths that forget.
//
// Output is intended to be served straight to the browser via `v-html` in
// Vue — every piece of HTML is renderer-emitted, never user-controlled.
std::string renderToSafeHtml(const std::string& src);

// Initialise the cmark-gfm core extensions registry. Call once at startup.
void initOnce();

} // namespace markdown
