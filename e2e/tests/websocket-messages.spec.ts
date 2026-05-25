import { test, expect, BrowserContext, Page } from '@playwright/test'
import { registerAndLogin, userIdByName, uniqueUser } from './_helpers'

// The realtime hub at /ws/messages drives the SPA's conversation list
// via pg_notify-backed events. A successful round-trip proves:
//   - Bob's browser opens the WS on /messages (App.vue auto-connect),
//   - the INSERT into messages triggers trg_messages_notify,
//   - PgListener picks it up server-side and fans it out,
//   - Bob's tab receives a `new_message` frame and renders without reload.

// Send a POST /messages from a logged-in Playwright context. Reads the
// CSRF cookie back out of the context and echoes it in the header,
// mirroring the SPA's axios interceptor exactly.
async function sendMessage(ctx: BrowserContext, baseURL: string,
                           receiverId: number, content: string): Promise<void> {
  const cookies = await ctx.cookies(baseURL)
  const csrf = cookies.find(c => c.name === 'csrf_token')?.value
  expect(csrf).toBeTruthy()
  const res = await ctx.request.post(`${baseURL}/messages`, {
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf! },
    data:    { receiver_id: receiverId, content },
  })
  expect(res.status()).toBe(201)
}

test('a message posted by Alice shows up on Bob\'s open /messages tab',
  async ({ browser, baseURL }) => {
    const aliceCtx = await browser.newContext()
    const bobCtx   = await browser.newContext()
    const alicePage = await aliceCtx.newPage()
    const bobPage   = await bobCtx.newPage()

    try {
      const alice = await registerAndLogin(alicePage, uniqueUser())
      const bob   = await registerAndLogin(bobPage,   uniqueUser())
      const bobId = await userIdByName(bob.username)

      // Bob opens /messages and waits for the WS to be connected. The
      // toolbar reads "● live" once the socket is open.
      await bobPage.goto('/#/messages')
      await expect(bobPage.locator('.mlist')).toContainText(/live/i, { timeout: 5_000 })

      // Alice sends the first message of the conversation via REST. The
      // SPA itself has no "start a new conversation" UI — peers only
      // surface after an exchange exists, which is exactly the path the
      // server-side WS push has to populate for Bob.
      const marker = `ws-marker-${Math.random().toString(36).slice(2, 8)}`
      await sendMessage(aliceCtx, baseURL!, bobId, marker)

      // Bob's tab should pick up the conversation and the bubble
      // without a reload. The conversation list item carries the
      // peer's username — exactly the case the WS fan-out covers
      // (REST POST → trigger → notify → bus → JS frame → Vue ref).
      await expect(bobPage.locator('.mlist'))
        .toContainText(alice.username, { timeout: 8_000 })

      // Click into the conversation. The marker should appear in the
      // chat panel as Alice's incoming bubble (not "mine").
      await bobPage.locator('.mlist-item')
        .filter({ hasText: alice.username }).first().click()
      await expect(bobPage.locator('.mbubble').filter({ hasText: marker }))
        .toBeVisible({ timeout: 5_000 })
    } finally {
      await aliceCtx.close()
      await bobCtx.close()
    }
  })

test('Bob\'s reply (sent via the UI) reaches Alice\'s open /messages tab',
  async ({ browser, baseURL }) => {
    const aliceCtx = await browser.newContext()
    const bobCtx   = await browser.newContext()
    const alicePage = await aliceCtx.newPage()
    const bobPage   = await bobCtx.newPage()

    try {
      const alice = await registerAndLogin(alicePage, uniqueUser())
      const bob   = await registerAndLogin(bobPage,   uniqueUser())
      const bobId = await userIdByName(bob.username)

      // Seed a conversation so Bob's "send" input has a peer to address.
      const seed = `seed-${Math.random().toString(36).slice(2, 8)}`
      await sendMessage(aliceCtx, baseURL!, bobId, seed)

      // Both tabs land on /messages. Wait for the WS to come up on Alice's
      // side, then pick Bob's conversation on Bob's side.
      await alicePage.goto('/#/messages')
      await expect(alicePage.locator('.mlist')).toContainText(/live/i, { timeout: 5_000 })

      await bobPage.goto('/#/messages')
      await bobPage.locator('.mlist-item')
        .filter({ hasText: alice.username }).first().click()

      const reply = `reply-${Math.random().toString(36).slice(2, 8)}`
      await bobPage.getByPlaceholder(/write a message/i).fill(reply)
      await bobPage.getByRole('button', { name: /^send$/i }).click()

      // Alice's conversation list should refresh with Bob's preview
      // text without her reloading. The `last` field is the most
      // recent message body — that's the bubble we want to see.
      await expect(alicePage.locator('.mlist'))
        .toContainText(reply, { timeout: 8_000 })
    } finally {
      await aliceCtx.close()
      await bobCtx.close()
    }
  })
