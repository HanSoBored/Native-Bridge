# ADR-001: Socket Permission Model — Accept 0666 with Documented Rationale

## Status
**Accepted**

## Context

The Native-Bridge server (`nativeb_server`) runs as **root** on the Android Host.
Clients (`andro`, `nativeb_mcp`) run as **regular user `han`** inside a chroot.
The Unix socket lives at `/tmp/bridge.sock` (inside chroot) which maps to
`/data/local/rootfs/ubuntu-resolute-26.04/tmp/bridge.sock` on Android.

A code review flagged `0666` (world-writable) as a CRITICAL security issue.
However, `0660` breaks connectivity because `han` is neither the socket owner
(root) nor in the root group — resulting in "Permission denied".

The server and client exist in **different UID namespaces** (Android kernel vs
chroot), making `SO_PEERCRED` unreliable. Group-based access (`chown root:han`)
requires a group that exists on the Android host side, which is fragile and
build-time dependent.

This is a **solo dev tool**, not a multi-tenant production service. The chroot
itself is the primary security boundary.

## Decision

**Keep `0666` socket permissions.** Document the reasoning inline and in this ADR.
No handshake protocol, no shared secrets, no group gymnastics.

### Why

1. **The chroot IS the security boundary.** Any process that can execute code
   inside the chroot already has access to the filesystem, environment variables,
   and all other IPC mechanisms. The socket is the *least* of our concerns.

2. **UID namespace mismatch kills peer credential auth.** `SO_PEERCRED` returns
   the UID from the Android kernel's perspective, not the chroot's. `han` might
   be UID 1000 inside the chroot but a completely different UID on the Android
   host. We'd need a mapping table — over-engineering for a solo tool.

3. **Group-based access is fragile.** `chown root:han` only works if `han` exists
   on the Android host's `/etc/group` — which it doesn't in a standard Android
   environment. The chroot's user database is invisible to the Android kernel.

4. **Shared secrets add complexity without meaningful security.** Storing a token
   in a file that both sides can read is equivalent to `0666` in practice — any
   process inside the chroot can read the token file too. And now we have token
   rotation, expiry, and failure modes to manage.

5. **Simplicity ships.** This is a personal development tool. The socket only
   exposes command execution that the chroot user could theoretically perform
   through other means anyway.

### Risk Model

| Threat | Mitigation |
|--------|------------|
| Malicious process inside chroot connects to socket | **Accepted risk.** If a malicious process is inside the chroot, you already have a bigger problem. |
| Process outside chroot connects to socket | **Not possible.** The socket only exists inside the chroot's filesystem namespace. Android host processes don't see `/tmp/bridge.sock` at the chroot path. |
| Socket symlink attack in `/tmp` | **Mitigated.** `/tmp` inside chroot is isolated. An attacker would need chroot access first. |
| Privilege escalation via server | **Mitigated.** The MCP layer already has a command blacklist. The server runs as root by design (needs system command access). |

## Consequences

### Positive
- ✅ **Works immediately** — no group setup, no token files, no handshake protocol
- ✅ **Zero configuration** — no build-time user/group detection, no runtime env vars
- ✅ **No failure modes** — no "token expired", "group not found", "UID mismatch" bugs
- ✅ **Simple to audit** — the permission model is one line: `chmod(socket_path, 0666)`
- ✅ **Easy to harden later** — if the project scope changes, we can layer on auth without removing the socket

### Negative
- ❌ Static analysis / code reviewers will flag `0666` as a vulnerability
- ❌ If the chroot is compromised, the socket is an additional attack vector (but not a meaningful one)
- ❌ Doesn't satisfy "perfect security" checklists

## Implementation Notes

### Files to modify

1. **`src/main/server.c`** — keep `chmod(socket_path, 0666)` but add a comment
   explaining the rationale (points to this ADR).

2. **`docs/ADR-001-socket-permissions.md`** — this file, stored in the repo.

### Comment to add in server.c

```c
// Socket permissions: 0666 (rw-rw-rw-)
//
// WHY: The server runs as root on Android Host. Clients run as a non-root
// user inside a chroot. The chroot's user database is invisible to the
// Android kernel, so:
//   - 0660 fails: the chroot user isn't root or in root's group on Android
//   - SO_PEERCRED fails: UID namespaces differ between chroot and host
//   - Group chown fails: the group doesn't exist on Android's /etc/group
//
// The chroot IS the security boundary. Any process inside it is already
// trusted. See docs/ADR-001-socket-permissions.md for full analysis.
if (chmod(socket_path, 0666) < 0) {
    perror("Failed to chmod socket");
}
```

### No migration needed
This is a clarification of existing behavior, not a behavioral change.

## Notes

### Future hardening path (if scope changes)

If this project ever becomes a production service with untrusted clients,
the upgrade path is:

1. **Layer 1:** Add a shared-secret handshake as the first packet (new
   `CMD_AUTH` command type). The secret is read from a root-only file on
   the Android host, and an env var inside the chroot.

2. **Layer 2:** Move the socket to a root-owned directory with `0700`
   permissions (e.g., `/run/nativeb/bridge.sock`). The directory is
   bind-mounted into the chroot.

3. **Layer 3:** Use an abstract Unix socket (`sun_path[0] = '\0'`) which
   has no filesystem permissions at all — only processes with the right
   namespace can connect. (Android supports this.)

None of these are needed today. They are documented here so a future
maintainer knows the options exist.

### Related: MCP Command Classification

The MCP layer (`nativeb_mcp`) uses a hybrid command classification model
(allowlist + denylist + audit logging) for `device_exec`. This is tracked
in `docs/ADR-002-mcp-command-classification.md`. Like the socket permissions,
this is **not** a security control — the chroot is the boundary. The hybrid
model provides visibility into AI agent behavior without breaking workflows.
