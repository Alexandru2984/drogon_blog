<script setup lang="ts">
import { ref, watch, onBeforeUnmount, nextTick } from 'vue'

// An in-page contents list for long posts.
//
// It reads the *rendered* body rather than the markdown source, so it agrees
// with what is actually on screen — a `##` inside a fenced code block is a
// line of code, not a heading, and cmark-gfm has already made that call.
//
// Two constraints shaped this component:
//
// 1. The app runs on a hash history (`createWebHashHistory`), so a real
//    `href="#some-heading"` would be read as a route change, not an in-page
//    jump. On top of that the router's scrollBehavior returns `{ top: 0 }`
//    unconditionally and ignores `to.hash`. Hence buttons plus an explicit
//    scroll, and focus moved onto the heading so a keyboard user actually
//    lands there instead of only the viewport moving.
//
// 2. The body is injected with v-html after the fetch resolves, so headings
//    do not exist at mount. The scan is driven by `revision` changing.

const props = defineProps<{
  body: HTMLElement | null
  revision: string
}>()

interface Entry {
  id:    string
  text:  string
  level: number    // 2 or 3
}

const entries = ref<Entry[]>([])
const activeId = ref('')

// The reading column is 44rem, so this cannot become a sticky sidebar
// without taking the article below a readable measure. Inline it is — which
// means on a phone a fifteen-entry list would sit between the reader and
// the first paragraph. <details> solves that with no custom keyboard or
// ARIA work: open where there is room, folded where there is not.
const open = ref(
  typeof window !== 'undefined' && window.matchMedia('(min-width: 700px)').matches,
)

// Below this a contents list costs more attention than it saves — the whole
// article is already a couple of screens.
const MIN_HEADINGS = 3

let observer: IntersectionObserver | null = null

function slugify(text: string, used: Set<string>): string {
  const base = text
    .toLowerCase()
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')   // strip the accents NFKD just split off
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 60) || 'section'
  // Two headings with the same words are common ("Notes", "Notes"); an id
  // collision would send both links to the first one.
  let id = base
  let n = 2
  while (used.has(id)) id = `${base}-${n++}`
  used.add(id)
  return id
}

function teardown() {
  observer?.disconnect()
  observer = null
}

function scan() {
  teardown()
  entries.value = []
  activeId.value = ''

  const root = props.body
  if (!root) return

  const found = Array.from(root.querySelectorAll<HTMLElement>('h2, h3'))
  if (found.length < MIN_HEADINGS) return

  const used = new Set<string>()
  const list: Entry[] = []
  for (const el of found) {
    const text = (el.textContent ?? '').trim()
    if (!text) continue
    // Respect an id the author already put there; only mint one when the
    // heading has none, so an existing deep link keeps working.
    if (!el.id) el.id = slugify(text, used)
    else used.add(el.id)
    // -1 rather than 0: the heading should be focusable as a scroll target
    // but must not become a stop in the normal tab order.
    el.tabIndex = -1
    list.push({ id: el.id, text, level: el.tagName === 'H3' ? 3 : 2 })
  }
  entries.value = list
  if (!list.length) return

  // Scroll-spy. rootMargin pulls the detection band up near the top of the
  // viewport, so the highlighted entry is the section being read rather than
  // whichever one happens to be lowest on screen.
  observer = new IntersectionObserver(
    (records) => {
      const visible = records
        .filter(r => r.isIntersecting)
        .sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)
      if (visible.length) activeId.value = (visible[0].target as HTMLElement).id
    },
    { rootMargin: '-80px 0px -70% 0px', threshold: 0 },
  )
  for (const e of list) {
    const el = root.querySelector(`#${CSS.escape(e.id)}`)
    if (el) observer.observe(el)
  }
}

watch(
  () => [props.revision, props.body] as const,
  async () => { await nextTick(); scan() },
  { immediate: true },
)

onBeforeUnmount(teardown)

function jumpTo(id: string) {
  const el = props.body?.querySelector<HTMLElement>(`#${CSS.escape(id)}`)
  if (!el) return
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches
  el.scrollIntoView({ behavior: reduced ? 'auto' : 'smooth', block: 'start' })
  // Moving focus is what makes this work for a keyboard or screen-reader
  // user: without it the page scrolls but the next Tab continues from the
  // contents list, not from the section they asked for.
  el.focus({ preventScroll: true })
  activeId.value = id
}
</script>

<template>
  <nav v-if="entries.length" class="toc" aria-label="Contents">
    <details :open="open" @toggle="open = ($event.target as HTMLDetailsElement).open">
      <summary class="toc-heading">
        Contents <span class="toc-count">({{ entries.length }})</span>
      </summary>
      <ol class="toc-list">
        <li v-for="e in entries" :key="e.id" :class="['toc-item', `lvl-${e.level}`]">
          <button
            type="button"
            class="toc-link"
            :class="{ active: activeId === e.id }"
            :aria-current="activeId === e.id ? 'true' : undefined"
            @click="jumpTo(e.id)"
          >{{ e.text }}</button>
        </li>
      </ol>
    </details>
  </nav>
</template>

<style scoped>
.toc {
  margin: 0 0 var(--sp-5);
  padding: var(--sp-4);
  background: var(--bg-elev2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
}

.toc-heading {
  font-size: var(--step--1);
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: var(--text-dim);
  cursor: pointer;
  /* A summary is the disclosure control, so it has to clear the 24 px
     target minimum on its own. */
  min-height: 1.75rem;
  display: flex;
  align-items: center;
  gap: var(--sp-2);
}
.toc-heading:hover { color: var(--text); }
.toc-count { font-weight: 400; letter-spacing: 0; text-transform: none; }

details[open] .toc-heading { margin-bottom: var(--sp-3); }

.toc-list {
  margin: 0;
  padding: 0;
  list-style: none;
  display: flex;
  flex-direction: column;
  gap: 1px;
  /* A post with thirty headings would otherwise put most of a screen
     between the title and the first paragraph. The entries are buttons, so
     the overflow container is reachable by keyboard through them. */
  max-height: 45vh;
  overflow-y: auto;
}

.toc-item.lvl-3 { padding-left: var(--sp-4); }

.toc-link {
  display: block;
  width: 100%;
  /* 0.45rem top/bottom on a 1.4 line box clears the 24 px minimum target
     size without turning a ten-entry list into its own screenful. */
  padding: 0.45rem var(--sp-2);
  border: 0;
  border-left: 2px solid transparent;
  border-radius: 0 var(--radius-sm) var(--radius-sm) 0;
  background: transparent;
  color: var(--text-dim);
  font: inherit;
  font-size: var(--step--1);
  line-height: 1.4;
  text-align: left;
  cursor: pointer;
  overflow-wrap: anywhere;
}
.toc-link:hover {
  color: var(--text);
  background: var(--bg-elev);
}
/* The active entry is marked by a bar and a colour change, not colour
   alone — 1.4.1 applies to a state indicator the same way it applies to a
   link in a paragraph. */
.toc-link.active {
  color: var(--accent);
  border-left-color: var(--accent);
  background: var(--bg-elev);
}
</style>
