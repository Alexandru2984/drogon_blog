#include "Markdown.h"

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>

#include <atomic>
#include <cstdlib>
#include <string>

namespace markdown {

namespace {

std::atomic<bool> g_initDone{false};

cmark_parser* makeParser()
{
    // CMARK_OPT_SAFE escapes raw HTML and filters dangerous URL schemes.
    // CMARK_OPT_GITHUB_PRE_LANG emits <pre lang="…"> instead of the older
    // <pre><code class="language-…"> form for nicer downstream styling.
    const int opts = CMARK_OPT_SAFE
                   | CMARK_OPT_GITHUB_PRE_LANG
                   | CMARK_OPT_HARDBREAKS;     // newline -> <br>
    cmark_parser* p = cmark_parser_new(opts);
    if (!p) return nullptr;

    // GFM extensions we whitelist. Tables + strikethrough + autolinks +
    // task-lists cover what most blog posts want; we deliberately skip
    // the `tagfilter` extension because CMARK_OPT_SAFE already escapes raw HTML.
    static const char* const kExts[] = {"table", "strikethrough",
                                        "autolink", "tasklist"};
    for (const char* name : kExts) {
        if (auto* ext = cmark_find_syntax_extension(name)) {
            cmark_parser_attach_syntax_extension(p, ext);
        }
    }
    return p;
}

} // namespace

void initOnce()
{
    bool expected = false;
    if (!g_initDone.compare_exchange_strong(expected, true)) return;
    cmark_gfm_core_extensions_ensure_registered();
}

std::string renderToSafeHtml(const std::string& src)
{
    // Bound the parser; pathological inputs are the documented Markdown
    // DoS shape. We don't truncate-and-render — silently rendering half
    // a document would obscure that the caller pushed something out of
    // policy. Returning empty makes the failure visible.
    if (src.size() > kMaxMarkdownBytes) return {};

    initOnce();

    cmark_parser* parser = makeParser();
    if (!parser) return {};

    cmark_parser_feed(parser, src.data(), src.size());
    cmark_node* doc = cmark_parser_finish(parser);

    // Render options mirror parse-time. SAFE in particular has to be set on
    // both sides for raw-HTML escaping and URL-scheme filtering to apply.
    const int opts = CMARK_OPT_SAFE
                   | CMARK_OPT_GITHUB_PRE_LANG
                   | CMARK_OPT_HARDBREAKS;
    char* html = cmark_render_html(doc, opts, cmark_parser_get_syntax_extensions(parser));

    std::string out = html ? std::string(html) : std::string{};
    if (html)   std::free(html);
    cmark_node_free(doc);
    cmark_parser_free(parser);
    return out;
}

} // namespace markdown
