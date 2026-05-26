# ADR 0008 — Synchronous `write(2)` for access log lines

## Context

Every HTTP response emits one JSON access log line to stdout
(captured by journald in production). The naive code path was:

```cpp
fprintf(stdout, "%s\n", line);
fflush(stdout);
```

That's `fprintf` (libc buffering + global lock) + an explicit
`fflush` (one extra syscall) per request. At benchmark load
(thousands of RPS), the lock + flush became visible as p99
contention.

Alternatives considered:

| Approach | Why not / why |
|----------|---------------|
| `fprintf` + `fflush` per request | Two locks acquired per request (FILE-stream lock + libc internals). Original code; replaced. |
| Buffered async log queue (e.g. spdlog, fmt) | The right industrial answer. Adds a dependency + a worker thread + a flush-on-shutdown contract. The blog's worst-case log volume is small enough that the architecture cost outweighs the gain. |
| `dprintf(STDOUT_FILENO, ...)` | Still does internal locking around the formatter. |
| `snprintf` into a local buffer + a single `write(STDOUT_FILENO, …)` | Zero global locks; one syscall per request. **Chosen.** |

The single `write(2)` is safe because:

- journald reads `STDOUT_FILENO` as a stream socket.
- POSIX guarantees writes ≤ `PIPE_BUF` (4 KiB on Linux) are atomic
  — interleaved writers see no torn lines.
- Our log lines max out around 1.3 KiB even under degenerate input.

## Decision

`helpers/AccessLog.cc` formats each line into a 1280-byte stack
buffer with `snprintf`, then issues exactly one `write(STDOUT_FILENO,
buf, n)`. Return value is intentionally ignored — if journald
disappears, we drop the log line rather than block the request path
(EPIPE → silently discard).

## Consequences

- **No log loss on graceful shutdown**, because the writes go to
  the kernel buffer of stdout, which systemd / journald drains
  during pod teardown. A SIGKILL while a write was queued in
  userspace would still drop it, but at SIGKILL we have bigger
  problems.
- **Atomic-line guarantee depends on staying ≤ `PIPE_BUF`.** We
  defensively cap the formatter at 1280 bytes (well below the
  4096-byte boundary even on systems with a smaller PIPE_BUF). If
  a future field bloats the line past that ceiling, interleaved
  writers from different IO threads could tear it.
- **No log levels.** This is intentional for the request log — every
  request gets the same JSON shape. `LOG_DEBUG` / `LOG_INFO` /
  `LOG_ERROR` for non-request log lines go through Drogon's logger
  (which writes via its own mutex, but is much lower volume).
