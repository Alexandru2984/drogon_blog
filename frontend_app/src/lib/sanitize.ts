import DOMPurify, { type Config } from 'dompurify'

const PURIFY_CONFIG: Config = {
  USE_PROFILES: { html: true },
  FORBID_TAGS: [
    'style', 'form', 'input', 'button', 'textarea',
    'select', 'option', 'iframe', 'object', 'embed',
  ],
  ADD_ATTR: ['data-lang', 'tabindex', 'role', 'aria-label', 'target', 'rel'],
}

// Defense-in-depth for the two v-html sinks (post body + search snippet).
// The backend is already the primary wall: post HTML is produced by cmark-gfm
// in SAFE mode (raw HTML escaped, dangerous URL schemes filtered) and search
// snippets are HTML-escaped before ts_headline wraps matches in <mark>. This
// adds a second, client-side wall so a regression in either server path can't
// turn into stored XSS.
export function sanitizeHtml(dirty: string | null | undefined): string {
  if (!dirty) return ''
  return DOMPurify.sanitize(dirty, PURIFY_CONFIG)
}

function normalizePostDom(doc: Document) {
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

  for (const pre of Array.from(doc.body.querySelectorAll('pre'))) {
    const lang = pre.getAttribute('lang')
    if (lang) {
      pre.removeAttribute('lang')
      pre.setAttribute('data-lang', lang.trim().toLowerCase())
    }
    if (!pre.hasAttribute('tabindex')) pre.setAttribute('tabindex', '0')
  }

  // Harden external links in rendered Markdown.
  for (const a of Array.from(doc.body.querySelectorAll('a[href]'))) {
    const href = a.getAttribute('href') ?? ''
    if (/^https?:\/\//i.test(href)) {
      a.setAttribute('rel', 'noopener noreferrer')
      if (!a.hasAttribute('target')) a.setAttribute('target', '_blank')
    }
  }
}

export function sanitizePostHtml(dirty: string | null | undefined): string {
  const firstPass = sanitizeHtml(dirty)
  if (!firstPass) return ''
  if (!firstPass.includes('<table') && !firstPass.includes('<pre') && !firstPass.includes('<a')) {
    return firstPass
  }

  const doc = new DOMParser().parseFromString(firstPass, 'text/html')
  normalizePostDom(doc)
  // Run DOMPurify as the final step so no DOMParser re-serialization quirk
  // can ever emit unsanitized markup.
  return DOMPurify.sanitize(doc.body.innerHTML, PURIFY_CONFIG)
}
