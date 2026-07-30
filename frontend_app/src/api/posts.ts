import { api } from './client'

export interface PostAuthor {
  id: number
  username: string
  profile_image?: string
}

export interface Tag {
  slug:  string    // normalised identifier, what the URL carries
  label: string    // what the first author to use it typed, for display
}

export interface Post {
  id: number
  title: string
  content: string             // raw markdown source as authored
  content_html?: string       // sanitized HTML rendered server-side (cmark-gfm SAFE)
  created_at: string
  updated_at: string
  // Null while a post is a draft. `is_draft` is only sent on the single-post
  // and drafts endpoints; the feed never contains drafts, so its absence
  // there is not a missing field.
  published_at?: string | null
  is_draft?: boolean
  reading_minutes?: number
  view_count?: number
  excerpt?: string
  tags?: Tag[]
  // The viewer's own saved state, sent on the single-post endpoint only.
  bookmarked?: boolean
  author?: PostAuthor
}

export interface TagSummary extends Tag {
  count: number
}

// A post surfaced by /posts/{id}/related, plus how many tags it has in
// common with the one being read — the reason it is being suggested.
export interface RelatedPost extends Post {
  shared_tags: number
}

export interface PreviewResult {
  content_html:    string
  reading_minutes: number
  excerpt:         string
}

export interface FeedPage {
  posts:       Post[]
  next_cursor: number | null
}

export interface SearchHit {
  id: number
  title: string
  snippet: string          // HTML; contains <mark>…</mark> highlights
  rank: number
  created_at: string
  updated_at: string
  author?: PostAuthor
}

export interface SearchResponse {
  query: string
  count: number
  posts: SearchHit[]
}

export const postsApi = {
  list(opts: { before?: number; limit?: number } = {}) {
    const params: Record<string, string> = {}
    if (opts.before) params.before = String(opts.before)
    if (opts.limit)  params.limit  = String(opts.limit)
    return api.get<FeedPage>('/posts', { params }).then(r => r.data)
  },
  search(q: string) {
    return api.get<SearchResponse>('/posts/search', { params: { q } }).then(r => r.data)
  },
  byUser(userId: number) {
    return api.get<{ posts: Post[] }>(`/posts/user/${userId}`).then(r => r.data.posts)
  },
  listTags() {
    return api.get<{ tags: TagSummary[] }>('/tags').then(r => r.data.tags)
  },
  byTag(slug: string, limit = 50) {
    return api
      .get<{ tag: string; count: number; posts: Post[] }>(
        `/tags/${encodeURIComponent(slug)}/posts`, { params: { limit } })
      .then(r => r.data)
  },
  myDrafts() {
    return api.get<{ posts: Post[] }>('/posts/drafts').then(r => r.data.posts)
  },
  get(id: number) {
    return api.get<Post>(`/posts/${id}`).then(r => r.data)
  },
  related(id: number) {
    return api.get<{ posts: RelatedPost[] }>(`/posts/${id}/related`).then(r => r.data.posts)
  },
  // Render markdown through the same cmark-gfm pipeline that publishing
  // uses, so the preview is what the post will be rather than a lookalike
  // produced by a second renderer. Server-side and rate-limited; the caller
  // is expected to debounce.
  preview(content: string, signal?: AbortSignal) {
    return api
      .post<PreviewResult>('/posts/preview', { content }, { signal })
      .then(r => r.data)
  },
  create(payload: { title: string; content: string; tags?: string[]; draft?: boolean }) {
    return api.post('/posts', payload).then(r => r.data)
  },
  // Upload an inline image; returns a same-origin URL to embed as Markdown
  // (![alt](url)). The server re-encodes to JPEG, strips EXIF and bounds the
  // dimensions, so the returned asset is a safe same-origin image.
  uploadImage(file: File) {
    const fd = new FormData()
    fd.append('image', file)
    return api
      .post<{ url: string }>('/posts/images', fd, {
        headers: { 'Content-Type': 'multipart/form-data' },
      })
      .then(r => r.data.url)
  },
  // Omitting `tags` leaves them untouched; passing an empty array clears
  // them. Omitting `draft` leaves the published state alone.
  update(id: number, payload: {
    title?: string; content?: string; tags?: string[]; draft?: boolean
  }) {
    return api.put(`/posts/${id}`, payload).then(r => r.data)
  },
  remove(id: number) {
    return api.delete(`/posts/${id}`).then(r => r.data)
  },
  like(id: number) {
    return api.post(`/posts/${id}/like`).then(r => r.data)
  },
  unlike(id: number) {
    return api.delete(`/posts/${id}/like`).then(r => r.data)
  },
  likesCount(id: number) {
    // `liked` is the viewer's own state; always false for anonymous readers.
    return api
      .get<{ post_id: number; likes_count: number; liked: boolean }>(`/posts/${id}/likes`)
      .then(r => r.data)
  },
}
