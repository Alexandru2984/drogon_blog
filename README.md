# Micu's Blog

Aplicație de blog full-stack — backend REST API în C++ cu [Drogon](https://github.com/drogonframework/drogon), frontend SPA vanilla JS, bază de date SQLite.

**Live:** [https://blog.micutu.com](https://blog.micutu.com)

## Funcționalități

- **Autentificare** — înregistrare, login, logout, verificare email, reset parolă
- **Postări** — CRUD complet, listare, postări per utilizator, suport Markdown
- **Comentarii** — adăugare, editare, ștergere comentarii la postări
- **Like-uri** — like/unlike pe postări
- **Mesaje private** — trimitere, primire, conversații, marcare ca citit
- **Profil utilizator** — editare profil, upload imagine de profil
- **Email** — verificare cont și reset parolă via SMTP (Brevo)
- **Frontend SPA** — dark mode, responsive, hash routing

## Tehnologii

- **C++17** cu framework-ul Drogon
- **SQLite3** — bază de date
- **libcurl** — trimitere email-uri SMTP
- **OpenSSL** — hash-uri parole
- **CMake** — build system
- **Vanilla JS/CSS** — frontend SPA (fără framework, fără build tools)
- **nginx** — reverse proxy cu SSL (Let's Encrypt)
- **systemd** — management serviciu

## Structura proiectului

```
drogon_blog/
├── main.cc                  # Entry point
├── config.json              # Configurație Drogon (port, DB, sesiuni)
├── schema.sql               # Schema inițială a bazei de date
├── migrations.sql           # Migrări (verificare email, reset parolă)
├── CMakeLists.txt           # Build configuration
├── .env                     # Credențiale SMTP (nu e în git)
├── controllers/             # HTTP Controllers
│   ├── AuthController       # /auth/*
│   ├── PostController       # /posts/*
│   ├── CommentController    # /posts/{id}/comments, /comments/*
│   ├── MessageController    # /messages/*
│   └── UserController       # /users/*
├── models/                  # ORM Models (generate de Drogon)
│   ├── Users, Posts, Comments, Likes, Messages
│   └── PasswordResetTokens
├── helpers/
│   └── EmailHelper.h        # Trimitere email via SMTP
├── public/                  # Document root (servit de Drogon)
│   ├── index.html           # SPA entry point
│   ├── static/css/style.css # Stiluri (dark mode)
│   ├── static/js/app.js     # Aplicația frontend
│   └── uploads -> ../uploads # Symlink pentru fișiere uploadate
├── uploads/                 # Fișiere uploadate
│   └── profiles/            # Imagini de profil
└── test/                    # Teste
```

## API Endpoints

### Autentificare (`/auth`)
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| POST | `/auth/register` | Înregistrare utilizator |
| POST | `/auth/login` | Autentificare |
| POST | `/auth/logout` | Deconectare |
| GET | `/auth/me` | Utilizator curent |
| POST | `/auth/verify-email` | Verificare email |
| POST | `/auth/request-reset` | Cerere reset parolă |
| POST | `/auth/reset-password` | Resetare parolă |
| POST | `/auth/resend-verification` | Retrimitere email verificare |

### Postări (`/posts`)
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/posts` | Toate postările |
| GET | `/posts/{id}` | O singură postare |
| POST | `/posts` | Creare postare (auth) |
| PUT | `/posts/{id}` | Editare postare (owner) |
| DELETE | `/posts/{id}` | Ștergere postare (owner) |
| GET | `/posts/user/{id}` | Postările unui utilizator |
| POST | `/posts/{id}/like` | Like (auth) |
| DELETE | `/posts/{id}/like` | Unlike (auth) |
| GET | `/posts/{id}/likes` | Număr like-uri |

### Comentarii
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/posts/{id}/comments` | Comentariile unei postări |
| POST | `/posts/{id}/comments` | Adăugare comentariu (auth) |
| PUT | `/comments/{id}` | Editare comentariu (owner) |
| DELETE | `/comments/{id}` | Ștergere comentariu (owner) |

### Mesaje (`/messages`)
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/messages/received` | Mesaje primite (auth) |
| GET | `/messages/sent` | Mesaje trimise (auth) |
| GET | `/messages/conversation/{userId}` | Conversație cu un utilizator (auth) |
| POST | `/messages` | Trimitere mesaj (auth) |
| PUT | `/messages/{id}/read` | Marcare ca citit (receiver) |
| DELETE | `/messages/{id}` | Ștergere mesaj (sender/receiver) |

### Utilizatori (`/users`)
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/users` | Toți utilizatorii (auth) |
| GET | `/users/{id}` | Profil utilizator |
| PUT | `/users/profile` | Editare profil (auth) |
| POST | `/users/profile/image` | Upload imagine profil (auth) |

## Prerequisite

- **CMake** >= 3.5
- **Drogon** framework (`apt install libdrogon-dev`)
- **SQLite3** (`apt install libsqlite3-dev`)
- **libcurl** (`apt install libcurl4-openssl-dev`)
- **OpenSSL** (`apt install libssl-dev`)
- Compilator C++ cu suport C++17

## Build & Run

```bash
# Inițializare bază de date
sqlite3 blog.db < schema.sql
sqlite3 blog.db < migrations.sql

# Build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -Wno-dev
make -j$(nproc)

# Rulare (din directorul proiectului)
cd ..
./build/blog
```

Serverul pornește pe **http://localhost:8092** (configurabil în `config.json`).

## Deploy (producție)

Blogul rulează ca serviciu systemd cu nginx reverse proxy:

```bash
# Build
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -Wno-dev && make -j$(nproc)

# Restart serviciu
sudo systemctl restart drogon-blog
```

**Infrastructură:**
- **systemd** — `drogon-blog.service` (auto-start la boot)
- **nginx** — reverse proxy pe `blog.micutu.com` → `127.0.0.1:8092`
- **SSL** — Let's Encrypt via certbot (auto-renew)
- **Cloudflare** — DNS + proxy

## Configurare email

Creează un fișier `.env` în directorul proiectului:

```
SMTP_SERVER=smtp://smtp-relay.brevo.com:587
SMTP_USERNAME=...
SMTP_PASSWORD=...
SMTP_FROM_EMAIL=blog@example.com
SMTP_FROM_NAME=Blog Name
```

## Teste

```bash
cd build/test
./blog_test
```

## Schema bazei de date

- **users** — utilizatori (username, email, parolă hash, imagine profil, bio, verificare email)
- **posts** — postări (titlu, conținut, autor, timestamps)
- **comments** — comentarii la postări
- **likes** — like-uri pe postări (unic per utilizator/postare)
- **messages** — mesaje private între utilizatori
- **password_reset_tokens** — token-uri temporare pentru resetare parolă

## Licență

Proiect personal.
