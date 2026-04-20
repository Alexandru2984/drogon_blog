// ─── State ─────────────────────────────────────────────
let currentUser = null;

// ─── API Client ────────────────────────────────────────
const API = {
    async request(method, url, body) {
        const opts = { method, headers: {}, credentials: 'same-origin' };
        if (body && !(body instanceof FormData)) {
            opts.headers['Content-Type'] = 'application/json';
            opts.body = JSON.stringify(body);
        } else if (body instanceof FormData) {
            opts.body = body;
        }
        const res = await fetch(url, opts);
        const data = await res.json().catch(() => null);
        if (!res.ok) throw { status: res.status, message: data?.error || res.statusText };
        return data;
    },
    get: (u) => API.request('GET', u),
    post: (u, b) => API.request('POST', u, b),
    put: (u, b) => API.request('PUT', u, b),
    del: (u) => API.request('DELETE', u),
};

// ─── Toast ─────────────────────────────────────────────
function toast(msg, type = 'info') {
    const el = document.createElement('div');
    el.className = `toast toast-${type}`;
    el.textContent = msg;
    document.getElementById('toast-container').appendChild(el);
    setTimeout(() => el.remove(), 3500);
}

// ─── Helpers ───────────────────────────────────────────
function esc(str) {
    const d = document.createElement('div');
    d.textContent = str || '';
    return d.innerHTML;
}

function timeAgo(dateStr) {
    if (!dateStr) return '';
    const d = new Date(dateStr.replace(' ', 'T') + 'Z');
    const diff = (Date.now() - d.getTime()) / 1000;
    if (diff < 60) return 'just now';
    if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
    if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
    if (diff < 2592000) return Math.floor(diff / 86400) + 'd ago';
    return d.toLocaleDateString();
}

function avatarHtml(user, size = 36) {
    if (user?.profile_image) {
        return `<img src="${esc(user.profile_image)}" style="width:${size}px;height:${size}px;border-radius:50%;object-fit:cover" alt="">`;
    }
    const letter = (user?.username || '?')[0].toUpperCase();
    return `<span style="width:${size}px;height:${size}px;border-radius:50%;background:var(--surface2);display:inline-flex;align-items:center;justify-content:center;font-size:${size*0.45}px;color:var(--text-muted)">${letter}</span>`;
}

function renderMarkdown(text) {
    if (!text) return '';
    let html = esc(text);
    // code blocks
    html = html.replace(/```(\w*)\n([\s\S]*?)```/g, '<pre><code>$2</code></pre>');
    // inline code
    html = html.replace(/`([^`]+)`/g, '<code>$1</code>');
    // headers
    html = html.replace(/^### (.+)$/gm, '<h3>$1</h3>');
    html = html.replace(/^## (.+)$/gm, '<h2>$1</h2>');
    html = html.replace(/^# (.+)$/gm, '<h1>$1</h1>');
    // bold/italic
    html = html.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
    html = html.replace(/\*(.+?)\*/g, '<em>$1</em>');
    // blockquote
    html = html.replace(/^&gt; (.+)$/gm, '<blockquote>$1</blockquote>');
    // links
    html = html.replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
    // images
    html = html.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, '<img src="$2" alt="$1">');
    // line breaks into paragraphs
    html = html.replace(/\n\n/g, '</p><p>');
    html = html.replace(/\n/g, '<br>');
    html = '<p>' + html + '</p>';
    // clean up empty paragraphs
    html = html.replace(/<p>\s*<\/p>/g, '');
    html = html.replace(/<p>\s*(<h[1-3]>)/g, '$1');
    html = html.replace(/(<\/h[1-3]>)\s*<\/p>/g, '$1');
    html = html.replace(/<p>\s*(<pre>)/g, '$1');
    html = html.replace(/(<\/pre>)\s*<\/p>/g, '$1');
    html = html.replace(/<p>\s*(<blockquote>)/g, '$1');
    html = html.replace(/(<\/blockquote>)\s*<\/p>/g, '$1');
    return html;
}

function truncate(text, len = 200) {
    if (!text || text.length <= len) return text || '';
    return text.substring(0, len) + '...';
}

// ─── Router ────────────────────────────────────────────
const routes = {};
function route(pattern, handler) { routes[pattern] = handler; }

function navigate(hash) {
    location.hash = hash;
}

function getHashParams() {
    const h = location.hash.slice(1) || '/';
    const qIdx = h.indexOf('?');
    const path = qIdx >= 0 ? h.substring(0, qIdx) : h;
    const params = {};
    if (qIdx >= 0) {
        new URLSearchParams(h.substring(qIdx + 1)).forEach((v, k) => params[k] = v);
    }
    return { path, params };
}

async function handleRoute() {
    const { path, params } = getHashParams();
    
    for (const [pattern, handler] of Object.entries(routes)) {
        const regex = new RegExp('^' + pattern.replace(/:(\w+)/g, '(?<$1>[^/]+)') + '$');
        const match = path.match(regex);
        if (match) {
            try {
                await handler({ ...match.groups, ...params });
            } catch (e) {
                console.error('Route error:', e);
                app().innerHTML = `<div class="empty-state"><p>Something went wrong</p><p class="text-muted">${esc(e.message || '')}</p></div>`;
            }
            return;
        }
    }
    app().innerHTML = '<div class="empty-state"><p>Page not found</p><a href="#/">Go home</a></div>';
}

function app() { return document.getElementById('app'); }

// ─── Nav ───────────────────────────────────────────────
function updateNav() {
    const el = document.getElementById('nav-links');
    if (currentUser) {
        el.innerHTML = `
            <a href="#/write">✍ Write</a>
            <a href="#/messages">✉ Messages</a>
            <a href="#/profile/${currentUser.id}">${avatarHtml(currentUser, 28)}</a>
            <button class="btn btn-ghost btn-sm" onclick="logout()">Logout</button>
        `;
    } else {
        el.innerHTML = `
            <a href="#/login">Login</a>
            <a href="#/register" class="btn btn-sm">Sign Up</a>
        `;
    }
}

// ─── Auth ──────────────────────────────────────────────
async function checkAuth() {
    try {
        currentUser = await API.get('/auth/me');
    } catch {
        currentUser = null;
    }
    updateNav();
}

async function logout() {
    try {
        await API.post('/auth/logout');
    } catch {}
    currentUser = null;
    updateNav();
    navigate('#/');
    toast('Logged out', 'success');
}

// ─── Views ─────────────────────────────────────────────

// HOME
route('/', async () => {
    app().innerHTML = '<div class="skeleton" style="height:200px;margin-bottom:1rem"></div>'.repeat(3);
    try {
        const data = await API.get('/posts');
        const posts = data.posts || [];
        if (posts.length === 0) {
            app().innerHTML = `
                <div class="empty-state">
                    <p>No posts yet</p>
                    <p class="text-muted">Be the first to write something!</p>
                    ${currentUser ? '<a href="#/write" class="btn">Write a post</a>' : '<a href="#/register" class="btn">Sign up to write</a>'}
                </div>`;
            return;
        }
        let html = '<h1 class="page-title">Latest Posts</h1>';
        for (const post of posts) {
            html += `
                <article class="card">
                    <div class="card-title"><a href="#/post/${post.id}">${esc(post.title)}</a></div>
                    <div class="card-meta">
                        ${avatarHtml(post.author, 22)}
                        <a href="#/profile/${post.author?.id || ''}">${esc(post.author?.username || 'Unknown')}</a>
                        <span>·</span>
                        <span>${timeAgo(post.created_at)}</span>
                    </div>
                    <div class="card-body">${esc(truncate(post.content, 250))}</div>
                    <a href="#/post/${post.id}" class="text-muted" style="font-size:0.85rem">Read more →</a>
                </article>`;
        }
        app().innerHTML = html;
    } catch (e) {
        app().innerHTML = `<div class="empty-state"><p>Failed to load posts</p><p class="text-muted">${esc(e.message)}</p></div>`;
    }
});

// SINGLE POST
route('/post/:id', async ({ id }) => {
    app().innerHTML = '<div class="skeleton" style="height:400px"></div>';
    try {
        const post = await API.get(`/posts/${id}`);
        const likesData = await API.get(`/posts/${id}/likes`);
        let commentsData;
        try { commentsData = await API.get(`/posts/${id}/comments`); } catch { commentsData = { comments: [] }; }

        let liked = false;
        if (currentUser) {
            // check if user liked this post by trying to see the likes
            // We don't have a direct endpoint; we'll track it client-side
        }

        const isOwner = currentUser && currentUser.id === post.author?.id;

        let html = `
            <article>
                <h1 style="font-size:2rem;margin-bottom:0.75rem">${esc(post.title)}</h1>
                <div class="card-meta mb-2">
                    ${avatarHtml(post.author, 28)}
                    <a href="#/profile/${post.author?.id || ''}">${esc(post.author?.username || 'Unknown')}</a>
                    <span>·</span>
                    <span>${timeAgo(post.created_at)}</span>
                    ${post.updated_at !== post.created_at ? `<span>· edited</span>` : ''}
                </div>
                ${isOwner ? `<div class="mb-2"><a href="#/edit/${post.id}" class="btn btn-outline btn-sm">Edit</a> <button class="btn btn-danger btn-sm" onclick="deletePost(${post.id})">Delete</button></div>` : ''}
                <div class="post-content">${renderMarkdown(post.content)}</div>
                <div class="post-actions">
                    <span class="like-btn" id="like-btn" onclick="toggleLike(${post.id})">
                        ♥ <span id="likes-count">${likesData.likes_count || 0}</span>
                    </span>
                    <span class="text-muted">${(commentsData.comments || []).length} comments</span>
                </div>
            </article>
            <section class="mt-2">
                <h3 style="margin-bottom:1rem">Comments</h3>`;

        if (currentUser) {
            html += `
                <div class="card" style="margin-bottom:1.5rem">
                    <textarea id="comment-input" placeholder="Write a comment..." rows="3"></textarea>
                    <button class="btn btn-sm mt-1" onclick="postComment(${post.id})">Post Comment</button>
                </div>`;
        }

        for (const c of (commentsData.comments || [])) {
            const cIsOwner = currentUser && currentUser.id === c.author?.id;
            html += `
                <div class="comment" id="comment-${c.id}">
                    <div class="comment-meta">
                        ${avatarHtml(c.author, 20)}
                        <a href="#/profile/${c.author?.id || ''}">${esc(c.author?.username || 'Unknown')}</a> · ${timeAgo(c.created_at)}
                    </div>
                    <div>${esc(c.content)}</div>
                    ${cIsOwner ? `<div class="comment-actions"><button class="btn btn-ghost btn-sm" onclick="deleteComment(${c.id}, ${post.id})">Delete</button></div>` : ''}
                </div>`;
        }
        if ((commentsData.comments || []).length === 0) {
            html += '<p class="text-muted" style="padding:1rem 0">No comments yet</p>';
        }

        html += '</section>';
        app().innerHTML = html;
    } catch (e) {
        app().innerHTML = `<div class="empty-state"><p>Post not found</p><a href="#/">Back to home</a></div>`;
    }
});

// LOGIN
route('/login', async () => {
    if (currentUser) { navigate('#/'); return; }
    app().innerHTML = `
        <div class="auth-container">
            <h2>Login</h2>
            <div class="card">
                <div class="form-group">
                    <label>Username</label>
                    <input type="text" id="login-user" placeholder="Enter username">
                </div>
                <div class="form-group">
                    <label>Password</label>
                    <input type="password" id="login-pass" placeholder="Enter password">
                </div>
                <button class="btn" style="width:100%" onclick="doLogin()">Login</button>
                <p class="text-muted text-center mt-2" style="font-size:0.85rem">
                    Don't have an account? <a href="#/register">Sign up</a>
                </p>
                <p class="text-center mt-1" style="font-size:0.85rem">
                    <a href="#/forgot-password">Forgot password?</a>
                </p>
            </div>
        </div>`;
    document.getElementById('login-pass').addEventListener('keydown', e => { if (e.key === 'Enter') doLogin(); });
});

// REGISTER
route('/register', async () => {
    if (currentUser) { navigate('#/'); return; }
    app().innerHTML = `
        <div class="auth-container">
            <h2>Sign Up</h2>
            <div class="card">
                <div class="form-group">
                    <label>Username</label>
                    <input type="text" id="reg-user" placeholder="Choose a username">
                </div>
                <div class="form-group">
                    <label>Email</label>
                    <input type="email" id="reg-email" placeholder="Enter email">
                </div>
                <div class="form-group">
                    <label>Password</label>
                    <input type="password" id="reg-pass" placeholder="Choose a password">
                </div>
                <button class="btn" style="width:100%" onclick="doRegister()">Create Account</button>
                <p class="text-muted text-center mt-2" style="font-size:0.85rem">
                    Already have an account? <a href="#/login">Login</a>
                </p>
            </div>
        </div>`;
});

// WRITE POST
route('/write', async () => {
    if (!currentUser) { navigate('#/login'); toast('Please login first', 'error'); return; }
    app().innerHTML = `
        <h1 class="page-title">Write a Post</h1>
        <div class="card">
            <div class="form-group">
                <label>Title</label>
                <input type="text" id="post-title" placeholder="Post title">
            </div>
            <div class="form-group">
                <label>Content <span class="text-muted">(Markdown supported)</span></label>
                <textarea id="post-content" rows="12" placeholder="Write your post..."></textarea>
            </div>
            <div class="flex gap-1">
                <button class="btn" onclick="doCreatePost()">Publish</button>
                <button class="btn btn-outline" onclick="previewPost()">Preview</button>
            </div>
            <div id="preview-area" class="mt-2" style="display:none"></div>
        </div>`;
});

// EDIT POST
route('/edit/:id', async ({ id }) => {
    if (!currentUser) { navigate('#/login'); return; }
    try {
        const post = await API.get(`/posts/${id}`);
        if (post.author?.id !== currentUser.id) { toast('Not your post', 'error'); navigate('#/'); return; }
        app().innerHTML = `
            <h1 class="page-title">Edit Post</h1>
            <div class="card">
                <div class="form-group">
                    <label>Title</label>
                    <input type="text" id="post-title" value="${esc(post.title)}">
                </div>
                <div class="form-group">
                    <label>Content</label>
                    <textarea id="post-content" rows="12">${esc(post.content)}</textarea>
                </div>
                <button class="btn" onclick="doUpdatePost(${id})">Update</button>
            </div>`;
    } catch {
        app().innerHTML = '<div class="empty-state"><p>Post not found</p></div>';
    }
});

// PROFILE
route('/profile/:id', async ({ id }) => {
    try {
        const user = await API.get(`/users/${id}`);
        const postsData = await API.get(`/posts/user/${id}`);
        const isMe = currentUser && currentUser.id == id;

        let html = `
            <div class="profile-header">
                <div class="profile-avatar">
                    ${user.profile_image ? `<img src="${esc(user.profile_image)}" alt="">` : esc((user.username || '?')[0].toUpperCase())}
                </div>
                <div>
                    <h2>${esc(user.username)}</h2>
                    <p class="text-muted">${esc(user.bio || 'No bio yet')}</p>
                    <p class="text-muted" style="font-size:0.8rem">Joined ${timeAgo(user.created_at)}</p>
                    ${isMe ? '<a href="#/settings" class="btn btn-outline btn-sm mt-1">Edit Profile</a>' : `<a href="#/messages/${id}" class="btn btn-outline btn-sm mt-1">✉ Message</a>`}
                </div>
            </div>
            <h3 style="margin-bottom:1rem">Posts by ${esc(user.username)}</h3>`;

        const posts = postsData.posts || [];
        if (posts.length === 0) {
            html += '<p class="text-muted">No posts yet</p>';
        }
        for (const post of posts) {
            html += `
                <div class="card">
                    <div class="card-title"><a href="#/post/${post.id}">${esc(post.title)}</a></div>
                    <div class="card-meta"><span>${timeAgo(post.created_at)}</span></div>
                    <div class="card-body">${esc(truncate(post.content, 150))}</div>
                </div>`;
        }
        app().innerHTML = html;
    } catch (e) {
        app().innerHTML = '<div class="empty-state"><p>User not found</p></div>';
    }
});

// SETTINGS
route('/settings', async () => {
    if (!currentUser) { navigate('#/login'); return; }
    const user = await API.get(`/users/${currentUser.id}`);
    app().innerHTML = `
        <h1 class="page-title">Settings</h1>
        <div class="card">
            <h3 style="margin-bottom:1rem">Profile</h3>
            <div class="form-group">
                <label>Email</label>
                <input type="email" id="set-email" value="${esc(user.email)}">
            </div>
            <div class="form-group">
                <label>Bio</label>
                <textarea id="set-bio" rows="3">${esc(user.bio || '')}</textarea>
            </div>
            <button class="btn" onclick="doUpdateProfile()">Save Changes</button>
        </div>
        <div class="card mt-2">
            <h3 style="margin-bottom:1rem">Profile Image</h3>
            <div class="flex gap-1" style="align-items:center">
                ${avatarHtml(user, 50)}
                <input type="file" id="avatar-file" accept="image/*">
            </div>
            <button class="btn mt-1" onclick="doUploadAvatar()">Upload</button>
        </div>`;
});

// MESSAGES LIST
route('/messages', async () => {
    if (!currentUser) { navigate('#/login'); return; }
    app().innerHTML = '<div class="skeleton" style="height:300px"></div>';
    try {
        const received = await API.get('/messages/received');
        const sent = await API.get('/messages/sent');

        // Build conversation list from received+sent
        const convos = {};
        for (const m of (received.messages || [])) {
            const uid = m.sender?.id;
            if (!uid) continue;
            if (!convos[uid]) convos[uid] = { user: m.sender, lastMsg: m, unread: 0 };
            if (!m.is_read) convos[uid].unread++;
            if (new Date(m.created_at) > new Date(convos[uid].lastMsg.created_at)) convos[uid].lastMsg = m;
        }
        for (const m of (sent.messages || [])) {
            const uid = m.receiver?.id;
            if (!uid) continue;
            if (!convos[uid]) convos[uid] = { user: m.receiver, lastMsg: m, unread: 0 };
            if (new Date(m.created_at) > new Date(convos[uid].lastMsg.created_at)) convos[uid].lastMsg = m;
        }

        let html = `<div class="flex-between mb-2"><h1 class="page-title" style="margin:0">Messages</h1><button class="btn btn-sm" onclick="showNewMessageModal()">+ New</button></div>`;

        const sorted = Object.values(convos).sort((a, b) => new Date(b.lastMsg.created_at) - new Date(a.lastMsg.created_at));

        if (sorted.length === 0) {
            html += '<div class="empty-state"><p>No messages yet</p><p class="text-muted">Start a conversation with someone!</p></div>';
        } else {
            html += '<div class="conversation-list card" style="padding:0;overflow:hidden">';
            for (const c of sorted) {
                html += `
                    <div class="conv-item" onclick="navigate('#/messages/${c.user.id}')">
                        ${avatarHtml(c.user, 36)}
                        <div style="flex:1;min-width:0">
                            <div style="display:flex;justify-content:space-between">
                                <strong>${esc(c.user.username)}</strong>
                                <span class="text-muted" style="font-size:0.75rem">${timeAgo(c.lastMsg.created_at)}</span>
                            </div>
                            <div class="text-muted" style="font-size:0.85rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">${esc(truncate(c.lastMsg.content, 60))}</div>
                        </div>
                        ${c.unread > 0 ? `<span class="unread-badge">${c.unread}</span>` : ''}
                    </div>`;
            }
            html += '</div>';
        }
        app().innerHTML = html;
    } catch (e) {
        app().innerHTML = `<div class="empty-state"><p>Failed to load messages</p></div>`;
    }
});

// CONVERSATION
route('/messages/:id', async ({ id }) => {
    if (!currentUser) { navigate('#/login'); return; }
    try {
        const data = await API.get(`/messages/conversation/${id}`);

        // Mark unread messages as read
        for (const m of (data.messages || [])) {
            if (m.receiver_id === currentUser.id && !m.is_read) {
                API.put(`/messages/${m.id}/read`).catch(() => {});
            }
        }

        const other = data.other_user || { username: 'User' };
        let html = `
            <div class="flex-between mb-2">
                <div class="flex gap-1" style="align-items:center">
                    <a href="#/messages" class="btn btn-ghost btn-sm">← Back</a>
                    ${avatarHtml(other, 28)}
                    <strong>${esc(other.username)}</strong>
                </div>
            </div>
            <div class="card" style="padding:0">
                <div class="chat-messages" id="chat-msgs">`;

        for (const m of (data.messages || [])) {
            const isSent = m.sender_id === currentUser.id;
            html += `
                <div class="chat-msg ${isSent ? 'sent' : 'received'}">
                    ${esc(m.content)}
                    <div class="msg-time">${timeAgo(m.created_at)}</div>
                </div>`;
        }

        if ((data.messages || []).length === 0) {
            html += '<p class="text-muted text-center" style="padding:2rem">No messages yet. Say hi!</p>';
        }

        html += `
                </div>
                <div class="chat-input" style="padding:0.75rem;border-top:1px solid var(--border)">
                    <input type="text" id="msg-input" placeholder="Type a message..." onkeydown="if(event.key==='Enter')sendMsg(${id})">
                    <button class="btn" onclick="sendMsg(${id})">Send</button>
                </div>
            </div>`;

        app().innerHTML = html;
        const chatEl = document.getElementById('chat-msgs');
        if (chatEl) chatEl.scrollTop = chatEl.scrollHeight;
    } catch (e) {
        app().innerHTML = '<div class="empty-state"><p>Could not load conversation</p></div>';
    }
});

// VERIFY EMAIL
route('/verify-email', async ({ token }) => {
    if (!token) {
        app().innerHTML = '<div class="empty-state"><p>Invalid verification link</p></div>';
        return;
    }
    try {
        await API.post('/auth/verify-email', { token });
        app().innerHTML = `
            <div class="empty-state">
                <p style="color:var(--success);font-size:1.5rem">✓ Email Verified!</p>
                <p class="text-muted">Your email has been verified successfully.</p>
                <a href="#/login" class="btn mt-2">Login</a>
            </div>`;
    } catch (e) {
        app().innerHTML = `
            <div class="empty-state">
                <p style="color:var(--danger)">Verification Failed</p>
                <p class="text-muted">${esc(e.message)}</p>
                <a href="#/" class="btn mt-2">Go Home</a>
            </div>`;
    }
});

// FORGOT PASSWORD
route('/forgot-password', async () => {
    app().innerHTML = `
        <div class="auth-container">
            <h2>Reset Password</h2>
            <div class="card">
                <p class="text-muted mb-2">Enter your email and we'll send you a reset link.</p>
                <div class="form-group">
                    <label>Email</label>
                    <input type="email" id="reset-email" placeholder="Enter your email">
                </div>
                <button class="btn" style="width:100%" onclick="doRequestReset()">Send Reset Link</button>
                <p class="text-center mt-2" style="font-size:0.85rem"><a href="#/login">Back to login</a></p>
            </div>
        </div>`;
});

// RESET PASSWORD
route('/reset-password', async ({ token }) => {
    if (!token) {
        app().innerHTML = '<div class="empty-state"><p>Invalid reset link</p></div>';
        return;
    }
    app().innerHTML = `
        <div class="auth-container">
            <h2>Set New Password</h2>
            <div class="card">
                <div class="form-group">
                    <label>New Password</label>
                    <input type="password" id="new-pass" placeholder="Enter new password">
                </div>
                <div class="form-group">
                    <label>Confirm Password</label>
                    <input type="password" id="confirm-pass" placeholder="Confirm new password">
                </div>
                <button class="btn" style="width:100%" onclick="doResetPassword('${esc(token)}')">Reset Password</button>
            </div>
        </div>`;
});

// ─── Actions ───────────────────────────────────────────

window.doLogin = async function() {
    const username = document.getElementById('login-user').value.trim();
    const password = document.getElementById('login-pass').value;
    if (!username || !password) { toast('Fill in all fields', 'error'); return; }
    try {
        await API.post('/auth/login', { username, password });
        await checkAuth();
        navigate('#/');
        toast('Welcome back!', 'success');
    } catch (e) {
        toast(e.message || 'Login failed', 'error');
    }
};

window.doRegister = async function() {
    const username = document.getElementById('reg-user').value.trim();
    const email = document.getElementById('reg-email').value.trim();
    const password = document.getElementById('reg-pass').value;
    if (!username || !email || !password) { toast('Fill in all fields', 'error'); return; }
    try {
        await API.post('/auth/register', { username, email, password });
        toast('Account created! Check your email to verify.', 'success');
        navigate('#/login');
    } catch (e) {
        toast(e.message || 'Registration failed', 'error');
    }
};

window.doCreatePost = async function() {
    const title = document.getElementById('post-title').value.trim();
    const content = document.getElementById('post-content').value.trim();
    if (!title || !content) { toast('Title and content required', 'error'); return; }
    try {
        const data = await API.post('/posts', { title, content });
        toast('Post published!', 'success');
        navigate(`#/post/${data.post?.id || ''}`);
    } catch (e) {
        toast(e.message || 'Failed to create post', 'error');
    }
};

window.doUpdatePost = async function(id) {
    const title = document.getElementById('post-title').value.trim();
    const content = document.getElementById('post-content').value.trim();
    if (!title || !content) { toast('Title and content required', 'error'); return; }
    try {
        await API.put(`/posts/${id}`, { title, content });
        toast('Post updated!', 'success');
        navigate(`#/post/${id}`);
    } catch (e) {
        toast(e.message || 'Failed to update post', 'error');
    }
};

window.deletePost = async function(id) {
    if (!confirm('Delete this post?')) return;
    try {
        await API.del(`/posts/${id}`);
        toast('Post deleted', 'success');
        navigate('#/');
    } catch (e) {
        toast(e.message || 'Failed to delete', 'error');
    }
};

window.toggleLike = async function(postId) {
    if (!currentUser) { toast('Please login to like', 'error'); return; }
    const btn = document.getElementById('like-btn');
    const countEl = document.getElementById('likes-count');
    try {
        if (btn.classList.contains('liked')) {
            await API.del(`/posts/${postId}/like`);
            btn.classList.remove('liked');
            countEl.textContent = Math.max(0, parseInt(countEl.textContent) - 1);
        } else {
            await API.post(`/posts/${postId}/like`);
            btn.classList.add('liked');
            countEl.textContent = parseInt(countEl.textContent) + 1;
        }
    } catch (e) {
        // If already liked and we tried to like, toggle the other way
        if (e.status === 409) {
            try {
                await API.del(`/posts/${postId}/like`);
                btn.classList.remove('liked');
                countEl.textContent = Math.max(0, parseInt(countEl.textContent) - 1);
            } catch {}
        } else {
            toast(e.message || 'Failed', 'error');
        }
    }
};

window.postComment = async function(postId) {
    const input = document.getElementById('comment-input');
    const content = input.value.trim();
    if (!content) return;
    try {
        await API.post(`/posts/${postId}/comments`, { content });
        toast('Comment added', 'success');
        handleRoute(); // refresh
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.deleteComment = async function(commentId, postId) {
    if (!confirm('Delete comment?')) return;
    try {
        await API.del(`/comments/${commentId}`);
        toast('Comment deleted', 'success');
        handleRoute();
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.doUpdateProfile = async function() {
    const email = document.getElementById('set-email').value.trim();
    const bio = document.getElementById('set-bio').value.trim();
    try {
        await API.put('/users/profile', { email, bio });
        await checkAuth();
        toast('Profile updated!', 'success');
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.doUploadAvatar = async function() {
    const file = document.getElementById('avatar-file').files[0];
    if (!file) { toast('Select a file first', 'error'); return; }
    const fd = new FormData();
    fd.append('image', file);
    try {
        await API.post('/users/profile/image', fd);
        await checkAuth();
        toast('Avatar updated!', 'success');
        handleRoute();
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.sendMsg = async function(receiverId) {
    const input = document.getElementById('msg-input');
    const content = input.value.trim();
    if (!content) return;
    try {
        await API.post('/messages', { receiver_id: receiverId, content });
        input.value = '';
        handleRoute(); // refresh conversation
    } catch (e) {
        toast(e.message || 'Failed to send', 'error');
    }
};

window.showNewMessageModal = async function() {
    try {
        const data = await API.get('/users');
        const users = data.users || [];
        let opts = users.map(u => `<option value="${u.id}">${esc(u.username)}</option>`).join('');
        app().innerHTML = `
            <h1 class="page-title">New Message</h1>
            <div class="card">
                <div class="form-group">
                    <label>To</label>
                    <select id="msg-to">${opts || '<option disabled>No users found</option>'}</select>
                </div>
                <div class="form-group">
                    <label>Message</label>
                    <textarea id="msg-content" rows="4" placeholder="Write a message..."></textarea>
                </div>
                <div class="flex gap-1">
                    <button class="btn" onclick="doSendNew()">Send</button>
                    <button class="btn btn-outline" onclick="navigate('#/messages')">Cancel</button>
                </div>
            </div>`;
    } catch (e) {
        toast('Failed to load users', 'error');
    }
};

window.doSendNew = async function() {
    const receiverId = parseInt(document.getElementById('msg-to').value);
    const content = document.getElementById('msg-content').value.trim();
    if (!receiverId || !content) { toast('Select user and write a message', 'error'); return; }
    try {
        await API.post('/messages', { receiver_id: receiverId, content });
        toast('Message sent!', 'success');
        navigate(`#/messages/${receiverId}`);
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.doRequestReset = async function() {
    const email = document.getElementById('reset-email').value.trim();
    if (!email) { toast('Enter your email', 'error'); return; }
    try {
        await API.post('/auth/request-reset', { email });
        toast('If an account exists, a reset link has been sent.', 'success');
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.doResetPassword = async function(token) {
    const password = document.getElementById('new-pass').value;
    const confirm = document.getElementById('confirm-pass').value;
    if (!password || password.length < 6) { toast('Password must be at least 6 characters', 'error'); return; }
    if (password !== confirm) { toast('Passwords do not match', 'error'); return; }
    try {
        await API.post('/auth/reset-password', { token, password });
        toast('Password reset successfully!', 'success');
        navigate('#/login');
    } catch (e) {
        toast(e.message || 'Failed', 'error');
    }
};

window.previewPost = function() {
    const area = document.getElementById('preview-area');
    const content = document.getElementById('post-content').value;
    if (area.style.display === 'none') {
        area.style.display = 'block';
        area.innerHTML = `<div class="card"><h3 style="margin-bottom:0.5rem">Preview</h3><div class="post-content">${renderMarkdown(content)}</div></div>`;
    } else {
        area.style.display = 'none';
    }
};

window.navigate = navigate;

// ─── Init ──────────────────────────────────────────────
window.addEventListener('hashchange', handleRoute);

(async () => {
    await checkAuth();
    if (!location.hash || location.hash === '#') location.hash = '#/';
    handleRoute();
})();
