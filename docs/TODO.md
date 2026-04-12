# TODO — Native-Bridge

Tracked from code review findings. `[x]` = done, `[ ]` = open.

---

## ✅ Completed

- [x] Fix MCP `device_screenshot` to return `{"type":"image"}` so AI can view screenshots (v1.1.1)
- [x] Socket `0666` permission — documented with ADR-001 (Accepted)

---

## 🚨 CRITICAL (Security)

- [ ] **S2:** MCP blacklist bypass — replaced naive blacklist with hybrid model (allowlist + denylist + audit logging). Denylist expanded to cover shells, interpreters, wrappers, multi-call binaries. Unknown commands allowed but logged to stderr. **Residual risk:** still bypassable via unknown binaries, but now visible. See `docs/ADR-002-mcp-command-classification.md`.
- [ ] **S3:** Path traversal in `device_file_read` — no validation, can read any root-accessible file (`src/main/mcp.c`)
- [ ] **S4:** Shell injection in `device_input_text` — unescaped `"` breaks out of quoting (`src/main/mcp.c`)
- [ ] **S5:** Shell injection in `device_logcat` filter — same injection vector (`src/main/mcp.c`)

---

## 🔴 HIGH

- [ ] **C1:** `stream_reader` doesn't send `RESP_STREAM_END` on connection drop — client blocks forever (`src/main/server.c`)
- [ ] **C2:** `write_all` return value ignored in `stream_reader` — server keeps writing to dead socket (`src/main/server.c`)
- [ ] **C3:** Thread-unsafe socket lifecycle — race between stream threads and cleanup (`src/main/server.c`)
- [ ] **M1:** Zero tests — no unit tests, no integration tests, no CI for functional testing

---

## 🟡 MEDIUM

- [ ] **Q1:** Magic numbers throughout (`64`, `8192`, `128`, `1048576`, `5242880`) — should be named constants
- [ ] **Q2:** Inconsistent error message formats — fragile client-side parsing
- [ ] **Q3:** `write_event` silently ignores errors — tap/swipe can fail without notice (`src/common/input.c`)
- [ ] **C4:** `strncpy` no explicit null-termination in client socket path (`src/main/client.c`)
- [ ] **C6:** `atoi` with no validation — returns 0 on bad input, no error (`src/main/client.c`, `src/main/mcp.c`)
- [ ] **C7:** `extract_json_string` re-parses entire JSON for every field call (`src/main/mcp.c`)
- [ ] **M2:** `.gitignore` minimal — missing `*.o`, `*.so`, `.env`, editor files
- [ ] **M3:** `VERSION` file exists but not used in code — version hardcoded in `mcp.c`

---

## 🟢 LOW

- [ ] **C8:** `fgets` may not read entire JSON-RPC line if > 64KB (`src/main/mcp.c`)
- [ ] **C9:** `pthread_mutex_destroy` never called (`src/main/server.c`)
- [ ] **S8:** No `SIGPIPE` handling in client (`src/main/client.c`)
- [ ] **M4:** No `make debug` target with ASAN/UBSAN
- [ ] **M5:** No `PROTOCOL.md` documentation
