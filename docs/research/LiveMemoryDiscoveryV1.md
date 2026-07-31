# Live memory discovery v1

The optional GDB endpoint may be used for short, discovery-only coherent memory
reads. Build Flycast with `ENABLE_GDB_SERVER=ON`, then enable
`config:Debug.GDBEnabled=yes`. The endpoint always binds IPv4 loopback
(`127.0.0.1`) on `config:Debug.GDBPort` (default `3263`).

Connecting pauses the current SH-4 executor. Read-memory and read-register
packets are accepted; writes, register mutation, breakpoints, stepping, reset,
kill, and arbitrary monitor commands fail with `E22`. Plain continue and detach
are permitted only to resume the paused session. The executor selection is not
changed. A normal detach or an abrupt client disconnect resumes gameplay, and
only one client may own a snapshot session at a time. Individual protocol reads
are capped at 1 KiB; clients may compose a larger bounded snapshot from ordered
chunks while the same pause remains active.

This endpoint is not an evidence publisher. Data obtained through it is
labelled `discovery-only`; any semantic claim still requires a bounded native
typed capture, independent validation, and the atomic evidence workflow.
Clients that apply game- or build-specific semantics must authenticate the
connected guest while the same snapshot session remains paused. The Lodoss
inventory client hashes its proven 256-byte executable window before reading the
inventory and fails closed on a build mismatch.
