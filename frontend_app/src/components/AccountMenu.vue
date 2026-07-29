<script setup lang="ts">
import { ref, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'
import { useRoute } from 'vue-router'

// The desktop navigation grew past what a single row holds: Feed, Tags, New
// post, Drafts, Saved, Notifications, Messages, username, 2FA, Logout, theme
// and locale is twelve items, and at 1440 px the last of them ran 44 px off
// the right edge. Rather than shrinking everything until it technically
// fits, the account-scoped items move behind the name they all belong to —
// which is also where a reader would look for them.
//
// This is a menu button per the WAI-ARIA pattern: aria-expanded on the
// trigger, aria-controls pointing at the menu, Escape to close, focus
// returned to the trigger, and a click outside dismissing it.

const props = defineProps<{ username: string }>()
const emit = defineEmits<{ (e: 'logout'): void }>()

const open    = ref(false)
const rootEl  = ref<HTMLElement | null>(null)
const menuEl  = ref<HTMLElement | null>(null)
const btnEl   = ref<HTMLButtonElement | null>(null)

const route = useRoute()
// A menu left open across a navigation hangs over the page the reader just
// asked for.
watch(() => route.fullPath, () => { open.value = false })

function onDocPointerDown(e: PointerEvent) {
  if (!open.value) return
  if (rootEl.value && !rootEl.value.contains(e.target as Node)) open.value = false
}

function onKeydown(e: KeyboardEvent) {
  if (!open.value) return
  if (e.key === 'Escape') {
    open.value = false
    btnEl.value?.focus()   // keep the reader's place
    return
  }
  if (e.key !== 'Tab' || !menuEl.value) return
  // Tabbing out of the menu closes it rather than trapping focus: this is a
  // menu, not a modal dialog, and the rest of the page is still usable.
  const items = Array.from(
    menuEl.value.querySelectorAll<HTMLElement>('a[href], button:not([disabled])'))
  if (!items.length) return
  const last = items[items.length - 1]
  const first = items[0]
  const active = document.activeElement
  if (!e.shiftKey && active === last)  open.value = false
  if (e.shiftKey  && active === first) open.value = false
}

onMounted(() => {
  document.addEventListener('pointerdown', onDocPointerDown)
  window.addEventListener('keydown', onKeydown)
})
onBeforeUnmount(() => {
  document.removeEventListener('pointerdown', onDocPointerDown)
  window.removeEventListener('keydown', onKeydown)
})

async function toggle() {
  open.value = !open.value
  if (open.value) {
    await nextTick()
    menuEl.value?.querySelector<HTMLElement>('a[href], button')?.focus()
  }
}
</script>

<template>
  <div ref="rootEl" class="account">
    <button
      ref="btnEl"
      class="quiet account-trigger"
      :aria-expanded="open"
      aria-controls="account-menu"
      aria-haspopup="true"
      @click="toggle"
    >
      <span class="account-name">{{ username }}</span>
      <span class="account-caret" aria-hidden="true">▾</span>
    </button>

    <div v-if="open" id="account-menu" ref="menuEl" class="account-menu" role="menu">
      <slot />
      <hr />
      <button role="menuitem" class="account-logout" @click="emit('logout')">
        <slot name="logout-label">Logout</slot>
      </button>
    </div>
  </div>
</template>

<style scoped>
.account { position: relative; }

.account-trigger {
  display: inline-flex;
  align-items: center;
  gap: 0.35em;
  max-width: 12rem;
  color: var(--accent);
  font-weight: 600;
}
.account-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.account-caret { font-size: 0.7em; opacity: 0.7; }

.account-menu {
  position: absolute;
  top: calc(100% + var(--sp-2));
  right: 0;
  z-index: 60;
  min-width: 12rem;
  padding: var(--sp-2);
  background: var(--bg-elev);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow-lg);
  display: flex;
  flex-direction: column;
  gap: 2px;
}

/* Styling the slotted links from here needs :deep — they are rendered by the
   parent, so they do not carry this component's scope attribute. */
.account-menu :deep(a),
.account-menu button {
  display: flex;
  align-items: center;
  gap: var(--sp-2);
  width: 100%;
  padding: var(--sp-2) var(--sp-3);
  border-radius: var(--radius-sm);
  background: transparent;
  border: 0;
  color: var(--text-dim);
  font: inherit;
  font-size: var(--step--1);
  text-align: left;
  white-space: nowrap;
  cursor: pointer;
  min-height: 2.25rem;
}
.account-menu :deep(a:hover),
.account-menu button:hover {
  background: var(--bg-elev2);
  color: var(--text);
  text-decoration: none;
}
.account-menu :deep(a.router-link-active) {
  background: var(--bg-elev2);
  color: var(--text);
}
.account-menu hr { margin: var(--sp-1) 0; }
.account-logout { color: var(--danger) !important; }
.account-logout:hover { background: var(--danger-soft) !important; }
</style>
