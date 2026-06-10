import DOMPurify from 'dompurify'

// Defense-in-depth for the two v-html sinks (post body + search snippet).
// The backend is already the primary wall: post HTML is produced by cmark-gfm
// in SAFE mode (raw HTML escaped, dangerous URL schemes filtered) and search
// snippets are HTML-escaped before ts_headline wraps matches in <mark>. This
// adds a second, client-side wall so a regression in either server path can't
// turn into stored XSS. USE_PROFILES.html keeps the markdown output intact
// (headings, lists, tables, code, links, images, <mark>) while stripping
// scripts, event handlers, and javascript: URLs.
export function sanitizeHtml(dirty: string | null | undefined): string {
  if (!dirty) return ''
  return DOMPurify.sanitize(dirty, { USE_PROFILES: { html: true } })
}
