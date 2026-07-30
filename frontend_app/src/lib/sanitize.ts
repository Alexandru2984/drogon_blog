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

// Same sanitizing, plus each <table> is wrapped in its own horizontal
// scroller. A five-column markdown table is about 440 px at its narrowest
// and pushed the whole page sideways on a phone — the reader ended up
// scrolling the article to read a table, and every other paragraph went
// off-screen with it.
//
// Done here rather than in CSS because the CSS-only fix is
// `table { display: block; overflow-x: auto }`, and changing a table's
// display type takes it out of the table role in several screen readers.
// Wrapping keeps `display: table` — and the row/column semantics that come
// with it — intact.
//
// The wrapper is tabindex="0" so the scroll region is reachable by keyboard:
// a scrollable box that only responds to a pointer is unusable without one.
// role="region" + aria-label give that tab stop a reason to exist when it is
// announced.
// Code blocks get the same treatment for the same reason, plus one fix that
// is squarely cmark-gfm's doing: it puts the fence's info string on the
// <pre> as `lang="sql"`. `lang` is the HTML *human language* attribute and
// expects a BCP-47 tag, so "sql", "cpp" and "bash" are all invalid values —
// a screen reader asked to switch pronunciation to language "cpp" gets
// nonsense, and axe flags it as `valid-lang`. The value is genuinely useful,
// so it moves to `data-lang`, where the highlighter and the CSS label read
// it, rather than being thrown away.
function normalizeCodeBlocks(doc: Document) {
  for (const pre of Array.from(doc.body.querySelectorAll('pre'))) {
    const lang = pre.getAttribute('lang')
    if (lang) {
      pre.removeAttribute('lang')
      pre.setAttribute('data-lang', lang.trim().toLowerCase())
    }
    // `overflow-x: auto` on the <pre> means a long line scrolls inside the
    // block. Without a tab stop that scrolling is pointer-only.
    if (!pre.hasAttribute('tabindex')) pre.setAttribute('tabindex', '0')
  }
}

export function sanitizePostHtml(dirty: string | null | undefined): string {
  const clean = sanitizeHtml(dirty)
  if (!clean.includes('<table') && !clean.includes('<pre')) return clean

  // Parse rather than regex: the input is already sanitized, so this is a
  // structural transform on trusted-shaped HTML, and the DOM gets the
  // nesting right where a regex would not.
  const doc = new DOMParser().parseFromString(clean, 'text/html')
  for (const table of Array.from(doc.body.querySelectorAll('table'))) {
    if (table.parentElement?.classList.contains('table-scroll')) continue
    const wrap = doc.createElement('div')
    wrap.className = 'table-scroll'
    wrap.setAttribute('tabindex', '0')
    wrap.setAttribute('role', 'region')
    wrap.setAttribute('aria-label', 'Table, scrollable')
    table.replaceWith(wrap)
    wrap.appendChild(table)
  }
  normalizeCodeBlocks(doc)
  return doc.body.innerHTML
}
