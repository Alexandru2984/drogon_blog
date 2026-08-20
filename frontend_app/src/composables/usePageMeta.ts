import { onBeforeUnmount, readonly, shallowRef } from 'vue'

export interface PageMetaOverride {
  title: string
  description?: string
}

interface OwnedPageMeta extends PageMetaOverride {
  owner: number
}

const current = shallowRef<OwnedPageMeta | null>(null)
let nextOwner = 0

// Read by the app shell; views only receive a scoped setter. Ownership keeps
// an old async view from clearing metadata that a newly mounted view supplied.
export const pageMetaOverride = readonly(current)

export function clearPageMeta(): void {
  current.value = null
}

export function usePageMeta() {
  const owner = ++nextOwner

  function setPageMeta(meta: PageMetaOverride): void {
    current.value = { ...meta, owner }
  }

  onBeforeUnmount(() => {
    if (current.value?.owner === owner) current.value = null
  })

  return { setPageMeta }
}
