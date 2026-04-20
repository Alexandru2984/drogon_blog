# Drogon Blog

O aplicație de blog RESTful construită cu [Drogon](https://github.com/drogonframework/drogon) (C++ HTTP framework) și SQLite.

## Funcționalități

- **Autentificare** — înregistrare, login, logout, verificare email, reset parolă
- **Postări** — CRUD complet, listare, postări per utilizator
- **Comentarii** — adăugare, editare, ștergere comentarii la postări
- **Like-uri** — like/unlike pe postări
- **Mesaje private** — trimitere, primire, conversații, marcare ca citit
- **Profil utilizator** — editare profil, upload imagine de profil
- **Email** — verificare cont și reset parolă via SMTP (Brevo)

## Tehnologii

- **C++17/20** cu framework-ul Drogon
- **SQLite3** — bază de date
- **libcurl** — trimitere email-uri SMTP
- **CMake** — build system

## Structura proiectului

```
blog/
├── main.cc                  # Entry point
├── config.json              # Configurație Drogon (port, DB, sesiuni)
├── schema.sql               # Schema inițială a bazei de date
├── migrations.sql           # Migrări (verificare email, reset parolă)
├── CMakeLists.txt           # Build configuration
├── controllers/             # HTTP Controllers
│   ├── AuthController       # /auth/*
│   ├── PostController       # /posts/*
│   ├── CommentController    # /posts/{id}/comments, /comments/*
│   ├── MessageController    # /messages/*
│   └── UserController       # /users/*
├── models/                  # ORM Models (Drogon)
│   ├── Users, Posts, Comments, Likes, Messages
│   └── PasswordResetTokens
├── helpers/
│   └── EmailHelper.h        # Trimitere email via SMTP
├── views/                   # CSP templates (Drogon views)
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
| POST | `/posts` | Creare postare |
| PUT | `/posts/{id}` | Editare postare |
| DELETE | `/posts/{id}` | Ștergere postare |
| GET | `/posts/user/{id}` | Postările unui utilizator |
| POST | `/posts/{id}/like` | Like |
| DELETE | `/posts/{id}/like` | Unlike |
| GET | `/posts/{id}/likes` | Număr like-uri |

### Comentarii
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/posts/{id}/comments` | Comentariile unei postări |
| POST | `/posts/{id}/comments` | Adăugare comentariu |
| PUT | `/comments/{id}` | Editare comentariu |
| DELETE | `/comments/{id}` | Ștergere comentariu |

### Mesaje (`/messages`)
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/messages/received` | Mesaje primite |
| GET | `/messages/sent` | Mesaje trimise |
| GET | `/messages/conversation/{userId}` | Conversație cu un utilizator |
| POST | `/messages` | Trimitere mesaj |
| PUT | `/messages/{id}/read` | Marcare ca citit |
| DELETE | `/messages/{id}` | Ștergere mesaj |

### Utilizatori (`/users`)
| Metodă | Endpoint | Descriere |
|--------|----------|-----------|
| GET | `/users` | Toți utilizatorii |
| GET | `/users/{id}` | Profil utilizator |
| PUT | `/users/profile` | Editare profil |
| POST | `/users/profile/image` | Upload imagine profil |

## Prerequisite

- **CMake** >= 3.5
- **Drogon** framework ([instalare](https://drogon.docsforge.com/master/installation/))
- **SQLite3**
- **libcurl**
- Compilator C++ cu suport C++17 sau C++20

## Build & Run

```bash
# Creare director build
mkdir build && cd build

# Configurare
cmake ..

# Compilare
make -j$(nproc)

# Inițializare bază de date
sqlite3 ../blog.db < ../schema.sql
sqlite3 ../blog.db < ../migrations.sql

# Rulare server
./blog
```

Serverul pornește pe **http://localhost:8090**.

## Teste

```bash
cd build/test
./blog_test
```

## Schema bazei de date

- **users** — utilizatori (username, email, parolă hash, imagine profil, bio, verificare email)
- **posts** — postări (titlu, conținut, autor)
- **comments** — comentarii la postări
- **likes** — like-uri pe postări (unic per utilizator/postare)
- **messages** — mesaje private între utilizatori
- **password_reset_tokens** — token-uri pentru resetare parolă

## Licență

Proiect personal.
