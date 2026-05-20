#pragma once

#include <string>

namespace markdown {

// Convert a CommonMark / GitHub-flavored Markdown string into safe HTML.
//
// "Safe" means:
//   * raw HTML in the input is escaped (CMARK_OPT_SAFE), so a user can't
//     inject <script>, <iframe>, on*-handlers, or style attributes via the
//     post body;
//   * `javascript:`, `data:` and `vbscript:` URLs in links/images are
//     filtered by the GFM safe mode;
//   * GFM extensions enabled: tables, strikethrough, autolinks, task lists.
//
// Output is intended to be served straight to the browser via `v-html` in
// Vue — every piece of HTML is renderer-emitted, never user-controlled.
std::string renderToSafeHtml(const std::string& src);

// Initialise the cmark-gfm core extensions registry. Call once at startup.
void initOnce();

} // namespace markdown
