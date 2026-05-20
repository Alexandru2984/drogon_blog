#include <drogon/drogon_test.h>
#include "../helpers/Markdown.h"

#include <string>

namespace {

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

DROGON_TEST(Markdown_RendersHeadingsAndEmphasis)
{
    const auto h = markdown::renderToSafeHtml("# Hello\n\n**bold** and *em*");
    CHECK(contains(h, "<h1>Hello</h1>"));
    CHECK(contains(h, "<strong>bold</strong>"));
    CHECK(contains(h, "<em>em</em>"));
}

DROGON_TEST(Markdown_StripsRawScript)
{
    // CMARK_OPT_SAFE replaces raw HTML blocks with an HTML comment
    // ("<!-- raw HTML omitted -->") and never lets <script> reach output.
    const auto h = markdown::renderToSafeHtml(
        "Hello\n\n<script>alert('xss')</script>\n\nEnd");
    CHECK(!contains(h, "<script>"));
    CHECK(!contains(h, "alert('xss')"));
}

DROGON_TEST(Markdown_StripsJavascriptUrl)
{
    // The autolink/link href filter must drop `javascript:` URLs even when
    // wrapped in valid markdown syntax.
    const auto h = markdown::renderToSafeHtml("[click](javascript:alert(1))");
    CHECK(!contains(h, "javascript:"));
}

DROGON_TEST(Markdown_RendersFencedCodeBlock)
{
    const auto h = markdown::renderToSafeHtml(
        "```cpp\nint main() { return 0; }\n```");
    CHECK(contains(h, "<pre"));
    CHECK(contains(h, "<code"));
    // Body text is rendered with escaping so `{` etc. survive.
    CHECK(contains(h, "int main"));
}

DROGON_TEST(Markdown_GfmTablesAndStrikethrough)
{
    const auto table = markdown::renderToSafeHtml(
        "| h1 | h2 |\n|---|---|\n| a | b |\n");
    CHECK(contains(table, "<table>"));
    CHECK(contains(table, "<th>h1</th>"));

    const auto strike = markdown::renderToSafeHtml("~~gone~~");
    CHECK(contains(strike, "<del>gone</del>"));
}

DROGON_TEST(Markdown_AutolinksBareUrls)
{
    const auto h = markdown::renderToSafeHtml("see https://example.org for more");
    CHECK(contains(h, "<a href=\"https://example.org\""));
}
