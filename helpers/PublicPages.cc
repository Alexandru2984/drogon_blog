#include "PublicPages.h"
#include "PostMeta.h"

#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace public_pages {

namespace {

// XML/HTML entity-escape. Defensive against angle brackets and quote chars
// that could otherwise break out of an attribute / text node.
std::string xmlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out.push_back(c);
        }
    }
    return out;
}

// CDATA can't contain "]]>". Replace the closing brace with a split that
// keeps the visual sequence intact for the consumer (rare in real content
// but trivial to defuse).
std::string cdataSafe(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() && s[i] == ']' && s[i+1] == ']' && s[i+2] == '>') {
            out += "]]]]><![CDATA[>";
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Serialise JSON-LD cleanly for inline <script type="application/ld+json">
// blocks: escaping `<`, `>`, and `&` prevents closing the script tag or
// breaking HTML parsing without altering decoded JSON strings.
std::string jsonScriptSafe(const Json::Value& v)
{
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    const std::string raw = Json::writeString(w, v);
    std::string out;
    out.reserve(raw.size() + 16);
    for (char c : raw) {
        if (c == '<')      out += "\\u003c";
        else if (c == '>') out += "\\u003e";
        else if (c == '&') out += "\\u0026";
        else               out.push_back(c);
    }
    return out;
}

// PG `2026-05-20 10:33:41.123456` -> ISO `2026-05-20T10:33:41Z`. Atom and
// crawlers want RFC 3339; this is the cheapest conversion that still
// produces valid timestamps.
std::string toIso8601(const std::string& pgTs)
{
    std::string out = pgTs;
    auto space = out.find(' ');
    if (space != std::string::npos) out[space] = 'T';
    auto dot = out.find('.');
    if (dot != std::string::npos) out.resize(dot);
    out += 'Z';
    return out;
}

} // namespace

void install(const std::string& siteOrigin)
{
    using namespace drogon;
    const std::string origin = siteOrigin.empty()
        ? std::string("https://blog.micutu.com") : siteOrigin;

    // -------- GET /feed.xml --------
    app().registerHandler("/feed.xml",
        [origin](const HttpRequestPtr&,
                 std::function<void(const HttpResponsePtr&)>&& cb) {
            auto db = app().getDbClient();
            static const char* kSql =
                "SELECT p.id, p.title, p.content, p.content_html, "
                "       p.created_at, p.updated_at, p.published_at, "
                "       u.username AS author "
                "  FROM posts p "
                "  LEFT JOIN users u ON u.id = p.user_id "
                // Public discovery must use the same visibility predicate as
                // the REST feed. `hidden_at IS NULL` only covers moderation;
                // without `published_at IS NOT NULL`, Atom becomes an
                // unauthenticated export of every saved draft.
                " WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL "
                " ORDER BY p.id DESC LIMIT 30";

            db->execSqlAsync(kSql,
                [origin, cb](const orm::Result& r) {
                    std::ostringstream out;
                    out << R"(<?xml version="1.0" encoding="utf-8"?>)" "\n"
                        << R"(<feed xmlns="http://www.w3.org/2005/Atom">)" "\n"
                        << "  <title>Micu's Blog</title>\n"
                        << "  <link href=\"" << xmlEscape(origin) << "\"/>\n"
                        << "  <link href=\"" << xmlEscape(origin)
                        <<       "/feed.xml\" rel=\"self\"/>\n"
                        << "  <id>" << xmlEscape(origin) << "/</id>\n";

                    // Editing an older entry also updates the feed.
                    std::string newest = "1970-01-01 00:00:00";
                    for (const auto& row : r)
                        newest = std::max(newest, row["updated_at"].as<std::string>());
                    out << "  <updated>" << xmlEscape(toIso8601(newest))
                        << "</updated>\n";

                    for (const auto& row : r) {
                        const auto id      = row["id"].as<int64_t>();
                        const auto title   = row["title"].as<std::string>();
                        const auto created = row["published_at"].as<std::string>();
                        const auto updated = row["updated_at"].as<std::string>();
                        const std::string content = row["content_html"].isNull()
                            ? std::string{} : row["content_html"].as<std::string>();
                        const std::string author = row["author"].isNull()
                            ? std::string("anonymous")
                            : row["author"].as<std::string>();

                        out << "  <entry>\n"
                            << "    <title>" << xmlEscape(title) << "</title>\n"
                            << "    <link href=\"" << xmlEscape(origin)
                            <<       "/preview/posts/" << id << "\"/>\n"
                            << "    <id>" << xmlEscape(origin)
                            <<       "/posts/" << id << "</id>\n"
                            << "    <published>"
                            << xmlEscape(toIso8601(created)) << "</published>\n"
                            << "    <updated>"
                            << xmlEscape(toIso8601(updated)) << "</updated>\n"
                            << "    <author><name>" << xmlEscape(author)
                            <<       "</name></author>\n"
                            << "    <content type=\"html\"><![CDATA["
                            << cdataSafe(content) << "]]></content>\n"
                            << "  </entry>\n";
                    }
                    out << "</feed>\n";

                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k200OK);
                    resp->setBody(out.str());
                    resp->setContentTypeString("application/atom+xml; charset=utf-8");
                    cb(resp);
                },
                [cb](const orm::DrogonDbException& e) {
                    LOG_ERROR << "feed.xml DB error: " << e.base().what();
                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k500InternalServerError);
                    resp->setBody("feed generation failed");
                    cb(resp);
                });
        },
        {Get});

    // -------- GET /sitemap.xml --------
    // The SPA routes on the hash fragment, which crawlers do not follow —
    // everything after `#` is stripped before a request is ever made. So
    // the only crawlable address for a post is /preview/posts/{id}, and
    // without a sitemap there is nothing linking to those at all: the
    // entire archive is invisible to search engines no matter how long it
    // has been up.
    app().registerHandler("/sitemap.xml",
        [origin](const HttpRequestPtr&,
                 std::function<void(const HttpResponsePtr&)>&& cb) {
            auto db = app().getDbClient();
            static const char* kSql =
                "SELECT id, updated_at FROM posts "
                // Draft IDs are private too. Advertising one in the sitemap
                // leaks its existence and hands crawlers the preview URL.
                " WHERE hidden_at IS NULL AND published_at IS NOT NULL "
                " ORDER BY id DESC LIMIT 5000";

            db->execSqlAsync(kSql,
                [origin, cb](const orm::Result& r) {
                    std::ostringstream out;
                    out << R"(<?xml version="1.0" encoding="UTF-8"?>)" "\n"
                        << R"(<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">)"
                        << "\n";

                    out << "  <url><loc>" << xmlEscape(origin)
                        << "/</loc><changefreq>daily</changefreq>"
                        << "<priority>1.0</priority></url>\n";

                    for (const auto& row : r) {
                        out << "  <url>"
                            << "<loc>" << xmlEscape(origin) << "/preview/posts/"
                            <<     row["id"].as<int64_t>() << "</loc>"
                            << "<lastmod>"
                            <<     xmlEscape(toIso8601(
                                       row["updated_at"].as<std::string>()))
                            << "</lastmod>"
                            << "<changefreq>weekly</changefreq>"
                            << "</url>\n";
                    }
                    out << "</urlset>\n";

                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k200OK);
                    resp->setBody(out.str());
                    resp->setContentTypeString("application/xml; charset=utf-8");
                    // Crawlers refetch this often and it changes at most
                    // as fast as posts are written.
                    resp->addHeader("Cache-Control", "public, max-age=3600");
                    cb(resp);
                },
                [cb](const orm::DrogonDbException& e) {
                    LOG_ERROR << "sitemap DB error: " << e.base().what();
                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k500InternalServerError);
                    resp->setBody("sitemap generation failed");
                    cb(resp);
                });
        },
        {Get});

    // -------- GET /.well-known/security.txt (RFC 9116) --------
    // Gives someone who finds a vulnerability a documented way to report
    // it. Without one, a finder's options are a public tweet, a
    // contact form that may not be read, or nothing — and "nothing" is
    // what usually happens.
    app().registerHandler("/.well-known/security.txt",
        [origin](const HttpRequestPtr&,
                 std::function<void(const HttpResponsePtr&)>&& cb) {
            const char* contact = std::getenv("BLOG_SECURITY_CONTACT");
            const std::string mail =
                (contact && *contact) ? contact : "security@micutu.com";

            // Expires is mandatory in RFC 9116 and must be in the future,
            // so it is computed rather than hardcoded — a stale file is
            // treated as invalid by the tools that read it, and a
            // hardcoded date silently becomes stale.
            const auto expiry = trantor::Date::now().after(365 * 24 * 3600);

            std::ostringstream out;
            out << "Contact: mailto:" << mail << "\n"
                << "Expires: " << expiry.toCustomFormattedString(
                                      "%Y-%m-%dT%H:%M:%S", false) << "Z\n"
                << "Preferred-Languages: en, ro\n"
                << "Canonical: " << origin << "/.well-known/security.txt\n"
                << "Policy: https://github.com/Alexandru2984/drogon_blog/blob/main/SECURITY.md\n";

            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k200OK);
            resp->setBody(out.str());
            resp->setContentTypeString("text/plain; charset=utf-8");
            resp->addHeader("Cache-Control", "public, max-age=86400");
            cb(resp);
        },
        {Get});

    // -------- GET /preview/posts/{id} --------
    // Share-friendly URL: contains the OG/Twitter meta tags crawlers need,
    // and offers a link to the SPA discussion. No automatic redirect: we
    // deliberately do not serve the SPA itself here because hash routes
    // (`/#/...`) are opaque to crawlers.
    app().registerHandler("/preview/posts/{id}",
        [origin](const HttpRequestPtr&,
                 std::function<void(const HttpResponsePtr&)>&& cb,
                 const std::string& idStr) {
            auto badId = [&]() {
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(k400BadRequest);
                resp->setBody("bad id");
                cb(resp);
            };
            if (idStr.empty() ||
                !std::all_of(idStr.begin(), idStr.end(), [](unsigned char c) {
                    return c >= '0' && c <= '9';
                })) {
                badId();
                return;
            }
            int64_t postId = 0;
            try {
                postId = std::stoll(idStr);
            } catch (...) {
                badId();
                return;
            }
            if (postId < 1 || postId > std::numeric_limits<int>::max()) {
                badId();
                return;
            }

            auto db = app().getDbClient();
            static const char* kSql =
                "SELECT p.title, p.content, p.content_html, "
                "       p.created_at, p.updated_at, p.published_at, "
                "       u.username AS author "
                "  FROM posts p "
                "  LEFT JOIN users u ON u.id = p.user_id "
                // A guessed id must not turn the social-card preview into a
                // public draft reader. Keep this predicate aligned with the
                // REST `getPost` rule for anonymous viewers.
                " WHERE p.hidden_at IS NULL "
                "   AND p.published_at IS NOT NULL AND p.id = $1";

            db->execSqlAsync(kSql,
                [origin, postId, cb](const orm::Result& r) {
                    if (r.empty()) {
                        auto resp = HttpResponse::newHttpResponse();
                        resp->setStatusCode(k404NotFound);
                        resp->setBody("Post not found");
                        cb(resp);
                        return;
                    }
                    const auto title   = r[0]["title"].as<std::string>();
                    const auto content = r[0]["content"].as<std::string>();
                    const std::string contentHtml = r[0]["content_html"].isNull()
                        ? std::string{} : r[0]["content_html"].as<std::string>();
                    const std::string author = r[0]["author"].isNull()
                        ? std::string{} : r[0]["author"].as<std::string>();
                    const std::string publishedRaw = r[0]["published_at"].isNull()
                        ? r[0]["created_at"].as<std::string>()
                        : r[0]["published_at"].as<std::string>();
                    const std::string updatedRaw = r[0]["updated_at"].isNull()
                        ? publishedRaw
                        : r[0]["updated_at"].as<std::string>();
                    const std::string publishedIso = toIso8601(publishedRaw);
                    const std::string updatedIso   = toIso8601(updatedRaw);

                    const std::string previewUrl = origin + "/preview/posts/" + std::to_string(postId);
                    const std::string spaUrl     = origin + "/#/posts/" + std::to_string(postId);
                    const std::string desc       = post_meta::makeExcerpt(content);

                    Json::Value ld(Json::objectValue);
                    ld["@context"] = "https://schema.org";
                    ld["@type"] = "BlogPosting";
                    ld["headline"] = title;
                    ld["description"] = desc;
                    ld["url"] = previewUrl;
                    ld["mainEntityOfPage"] = previewUrl;
                    ld["datePublished"] = publishedIso;
                    ld["dateModified"] = updatedIso;
                    if (!author.empty()) {
                        ld["author"]["@type"] = "Person";
                        ld["author"]["name"] = author;
                    }

                    std::ostringstream h;
                    h << "<!DOCTYPE html>\n"
                      << "<html lang=\"en\">\n"
                      << "<head>\n"
                      << "  <meta charset=\"utf-8\">\n"
                      << "  <title>" << xmlEscape(title) << " — Micu's Blog</title>\n"
                      << "  <link rel=\"canonical\" href=\"" << xmlEscape(previewUrl) << "\">\n"
                      << "  <link rel=\"alternate\" type=\"application/atom+xml\" "
                         "title=\"Micu's Blog — Atom Feed\" href=\""
                      <<      xmlEscape(origin) << "/feed.xml\">\n"
                      << "  <meta name=\"description\" content=\""
                      <<      xmlEscape(desc) << "\">\n"
                      << "  <meta property=\"og:type\" content=\"article\">\n"
                      << "  <meta property=\"og:site_name\" content=\"Micu's Blog\">\n"
                      << "  <meta property=\"og:title\" content=\""
                      <<      xmlEscape(title) << "\">\n"
                      << "  <meta property=\"og:description\" content=\""
                      <<      xmlEscape(desc) << "\">\n"
                      << "  <meta property=\"og:url\" content=\""
                      <<      xmlEscape(previewUrl) << "\">\n"
                      << "  <meta property=\"article:published_time\" content=\""
                      <<      xmlEscape(publishedIso) << "\">\n"
                      << "  <meta property=\"article:modified_time\" content=\""
                      <<      xmlEscape(updatedIso) << "\">\n";
                    if (!author.empty()) {
                        h << "  <meta property=\"article:author\" content=\""
                          <<      xmlEscape(author) << "\">\n";
                    }
                    h << "  <meta name=\"twitter:card\" content=\"summary\">\n"
                      << "  <meta name=\"twitter:title\" content=\""
                      <<      xmlEscape(title) << "\">\n"
                      << "  <meta name=\"twitter:description\" content=\""
                      <<      xmlEscape(desc) << "\">\n"
                      << "  <script type=\"application/ld+json\">"
                      <<      jsonScriptSafe(ld) << "</script>\n"
                      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                      << "  <meta name=\"color-scheme\" content=\"light dark\">\n"
                      << "  <link rel=\"stylesheet\" href=\"/article.css?v=20261116\">\n"
                      << "</head>\n"
                      << "<body>\n"
                      << "  <a class=\"skip-link\" href=\"#main\">Skip to article</a>\n"
                      << "  <header class=\"site-head\"><a href=\"/\">✦ Micu's Blog</a>"
                      << "<a class=\"discussion-link\" href=\"" << xmlEscape(spaUrl)
                      << "\">Join the discussion ↗</a></header>\n"
                      << "  <main id=\"main\">\n"
                      << "    <article>\n"
                      << "      <h1>" << xmlEscape(title) << "</h1>\n";
                    if (!author.empty()) {
                        h << "      <p class=\"byline\">by " << xmlEscape(author)
                          << " · <time datetime=\"" << xmlEscape(publishedIso) << "\">"
                          << xmlEscape(publishedIso.substr(0, 10)) << "</time></p>\n";
                    }
                    if (!contentHtml.empty()) {
                        h << "      <div class=\"article-body\">" << contentHtml << "</div>\n";
                    } else {
                        h << "      <p class=\"plain-source\">" << xmlEscape(content) << "</p>\n";
                    }
                    h << "    </article>\n"
                      << "    <footer><p>Keep the conversation going.</p><a class=\"discussion-link\" href=\""
                      <<      xmlEscape(spaUrl) << "\">Comment, save and share this article →</a></footer>\n"
                      << "  </main>\n"
                      << "</body></html>\n";

                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k200OK);
                    resp->setBody(h.str());
                    resp->setContentTypeCode(CT_TEXT_HTML);
                    cb(resp);
                },
                [cb](const orm::DrogonDbException& e) {
                    LOG_ERROR << "preview DB error: " << e.base().what();
                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k500InternalServerError);
                    resp->setBody("preview generation failed");
                    cb(resp);
                },
                static_cast<int>(postId));
        },
        {Get});
}

} // namespace public_pages
