#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <string>
#include <vector>

// Post metadata: tags, reading time, excerpts and view counting.
//
// These four are grouped because they share one property — each is derived
// from a post at write time and read back many times, so each is computed
// once and stored rather than recomputed per request.
namespace post_meta {

// ------------------------------------------------------------------ tags

struct Tag {
    std::string slug;    // normalised, unique
    std::string label;   // what the author typed, for display
};

// Fold a user-supplied tag to its canonical slug: lowercase, spaces and
// underscores to hyphens, everything outside [a-z0-9-] dropped, runs of
// hyphens collapsed, trimmed, truncated to 39 characters.
//
// Returns "" for input that has no usable characters at all ("!!!", "  "),
// which the caller must treat as "skip this tag" rather than as an error —
// rejecting the whole post because one tag was punctuation is a worse
// outcome than silently dropping it.
std::string slugify(const std::string& raw);

// Parse the `tags` field of a post payload. Accepts a JSON array of strings
// or a single comma-separated string, because both are things a client will
// plausibly send and rejecting one of them is a pointless failure mode.
//
// Deduplicates by slug, preserves author order, drops empties, and caps the
// result at kMaxTagsPerPost. Never throws.
std::vector<Tag> parseTags(const Json::Value& value);

inline constexpr std::size_t kMaxTagsPerPost = 8;

// Replace a post's tag set. Creates any tag that does not exist yet
// (keeping the first author's spelling as the label) and removes
// associations that are no longer present. Runs inside a transaction
// supplied by the caller when there is one.
//
// Returns false and logs on DB error; the caller decides whether a failed
// tag sync should fail the whole write. It should not: a post that saved
// but lost its tags is recoverable by editing, a post that refused to save
// is lost.
bool syncPostTags(const drogon::orm::DbClientPtr& db,
                  int postId,
                  const std::vector<Tag>& tags);

// Tags of one post, as a JSON array of {slug, label}.
Json::Value tagsForPost(const drogon::orm::DbClientPtr& db, int postId);

// Tags for many posts in one round trip, keyed by post id. The feed renders
// up to 50 posts; asking per post would be 50 queries for a page.
Json::Value tagsForPosts(const drogon::orm::DbClientPtr& db,
                         const std::vector<int>& postIds);

// Whether a post is currently unpublished. Read back from the database
// rather than tracked in the caller, so an edit that did not touch the
// draft flag still reports the true state.
bool isDraft(const drogon::orm::DbClientPtr& db, int postId);

// ---------------------------------------------------------- reading time

// Minutes to read, at 200 words per minute — the middle of the range the
// readability literature reports for adult silent reading of prose, and the
// figure Medium popularised, so it matches what readers expect a "5 min
// read" to mean. Always at least 1: "0 min read" reads as an error.
int estimateReadingMinutes(const std::string& markdown);

// A plain-text summary for feed cards, meta description tags and RSS.
// Strips the markdown syntax that would otherwise show up as literal
// punctuation in a preview (fences, heading hashes, link brackets, emphasis
// markers) and cuts on a word boundary.
std::string makeExcerpt(const std::string& markdown, std::size_t maxChars = 200);

// ----------------------------------------------------------------- views

// A stable-per-day identity for view deduplication.
//
// Signed in: "u:<id>". Anonymous: "a:" plus a truncated keyed hash of the
// client IP and User-Agent. The key is per-deployment and the day is part
// of the primary key, so the value cannot be used to follow a reader across
// days or to recover the address — it exists only to stop one person's
// refresh from counting twice.
std::string viewerKey(const drogon::HttpRequestPtr& req, int userIdOrZero);

// Record a view and return the post's new total. Insert-if-absent and the
// counter bump happen in one statement so the denormalised total cannot
// drift from the dedup table. A repeat view the same day is a no-op that
// still returns the current count.
//
// Best-effort: on DB error this returns the fallback and logs. A view
// counter is not worth failing a page render over.
long long recordView(const drogon::orm::DbClientPtr& db,
                     int postId,
                     const std::string& viewer,
                     long long fallback);

} // namespace post_meta
