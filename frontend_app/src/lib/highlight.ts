// Syntax highlighting for rendered post bodies.
//
// Three decisions worth stating:
//
// 1. Client-side, not server-side. cmark-gfm emits `<pre lang="x"><code>`
//    and knows nothing about tokens; highlighting on the server would mean
//    storing themed markup in content_html, which then has to be
//    re-rendered whenever the theme changes and cannot follow the reader's
//    light/dark preference at all.
//
// 2. `hljs/lib/core` plus an explicit language list, not the default bundle.
//    The full build registers ~190 languages and weighs about 900 KB; this
//    is the set a programming blog actually posts in, and anything outside
//    it renders as plain monospace rather than failing.
//
// 3. Loaded lazily by the caller (`await import(...)`), so a reader who
//    never opens a post with code never downloads it.

import hljs from 'highlight.js/lib/core'

import bash       from 'highlight.js/lib/languages/bash'
import c          from 'highlight.js/lib/languages/c'
import cpp        from 'highlight.js/lib/languages/cpp'
import csharp     from 'highlight.js/lib/languages/csharp'
import css        from 'highlight.js/lib/languages/css'
import diff       from 'highlight.js/lib/languages/diff'
import dockerfile from 'highlight.js/lib/languages/dockerfile'
import go         from 'highlight.js/lib/languages/go'
import ini        from 'highlight.js/lib/languages/ini'
import java       from 'highlight.js/lib/languages/java'
import javascript from 'highlight.js/lib/languages/javascript'
import json       from 'highlight.js/lib/languages/json'
import markdown   from 'highlight.js/lib/languages/markdown'
import nginx      from 'highlight.js/lib/languages/nginx'
import php        from 'highlight.js/lib/languages/php'
import python     from 'highlight.js/lib/languages/python'
import ruby       from 'highlight.js/lib/languages/ruby'
import rust       from 'highlight.js/lib/languages/rust'
import sql        from 'highlight.js/lib/languages/sql'
import typescript from 'highlight.js/lib/languages/typescript'
import xml        from 'highlight.js/lib/languages/xml'
import yaml       from 'highlight.js/lib/languages/yaml'

let registered = false

function register() {
  if (registered) return
  const langs: Record<string, any> = {
    bash, c, cpp, csharp, css, diff, dockerfile, go, ini, java, javascript,
    json, markdown, nginx, php, python, ruby, rust, sql, typescript, xml, yaml,
  }
  for (const [name, def] of Object.entries(langs)) hljs.registerLanguage(name, def)

  // Aliases people actually write in a fence. hljs resolves several of these
  // itself, but not all, and an unrecognised name silently means no
  // highlighting — so the common spellings are pinned explicitly.
  const aliases: Record<string, string> = {
    sh: 'bash', shell: 'bash', zsh: 'bash', console: 'bash',
    'c++': 'cpp', cxx: 'cpp', h: 'cpp', hpp: 'cpp',
    js: 'javascript', jsx: 'javascript', mjs: 'javascript',
    ts: 'typescript', tsx: 'typescript',
    py: 'python', rb: 'ruby', rs: 'rust',
    html: 'xml', vue: 'xml', svg: 'xml',
    yml: 'yaml', toml: 'ini', conf: 'ini',
    postgres: 'sql', psql: 'sql', 'c#': 'csharp', cs: 'csharp',
    patch: 'diff', md: 'markdown',
  }
  for (const [from, to] of Object.entries(aliases)) {
    const def = langs[to]
    if (def) hljs.registerLanguage(from, def)
  }
  registered = true
}

// Highlight every code block inside `root`.
//
// The language comes from `data-lang` on <pre>, not from a `language-x`
// class on <code>. cmark-gfm writes the fence's info string to `lang`,
// which is the HTML human-language attribute and invalid for "sql" or
// "cpp"; sanitizePostHtml moves it to `data-lang` on the way in, and this
// reads it there. An unknown or absent language is left alone deliberately:
// hljs's auto-detection guesses wrong often enough on short snippets that
// plain monospace is the better default.
export function highlightWithin(root: HTMLElement | null) {
  if (!root) return
  register()

  for (const pre of Array.from(root.querySelectorAll('pre'))) {
    const code = pre.querySelector('code')
    if (!code || code.dataset.highlighted === 'yes') continue

    const lang = (pre.dataset.lang || '').trim().toLowerCase()
    if (!lang || !hljs.getLanguage(lang)) continue

    try {
      // `ignoreIllegals` so a snippet that is deliberately invalid — an
      // error message being explained, a half-written line — still gets
      // highlighted instead of throwing and losing the whole block.
      const result = hljs.highlight(code.textContent ?? '', {
        language: lang,
        ignoreIllegals: true,
      })
      code.innerHTML = result.value
      code.dataset.highlighted = 'yes'
      code.classList.add('hljs')
    } catch {
      // Leave the block as plain text. A failed highlight must never cost
      // the reader the code itself.
    }
  }
}
