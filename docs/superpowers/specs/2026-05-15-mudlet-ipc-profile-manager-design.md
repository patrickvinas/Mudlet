# Mudlet IPC Profile Manager — Design Spec

**Date:** 2026-05-15  
**Status:** Approved

## Goal

Expose Mudlet's profile management through a cross-platform IPC interface so a custom PyQt6 frontend can list, tag, launch, and stop profiles — without maintaining UI patches against Mudlet's codebase.

The PyQt frontend replaces Mudlet's built-in profile picker. The in-game UI (consoles, trigger editor, mapper) remains Mudlet's own Qt widgets.

---

## Architecture

```
┌─────────────────────────────────┐       QLocalSocket (JSON)
│  PyQt6 Frontend                 │ ◄────────────────────────►  ┌──────────────────────┐
│  - Profile browser (tagged)     │                              │  Mudlet C++ Backend  │
│  - Launch / stop controls       │                              │  - QLocalServer      │
│  - MudletClient (IPC wrapper)   │                              │  - MudletIPCManager  │
│  - ProfileModel + filter proxy  │                              │  - ProfileMetadata   │
└─────────────────────────────────┘                              └──────────────────────┘
```

Mudlet gains a `MudletIPCManager` that binds a named local socket (`mudlet-ipc`) at startup when launched with `--ipc-mode`. On Linux/macOS this creates a socket file under the system temp directory; on Windows it becomes a named pipe. The PyQt frontend connects to this socket, sends JSON commands, and receives JSON responses and async event pushes.

Profile tag metadata is stored as a `tags.json` sidecar in each profile's existing directory (`~/.config/mudlet/profiles/<name>/tags.json`). Mudlet owns tag storage; the frontend reads and writes tags via IPC commands.

---

## Message Protocol

All messages are newline-delimited JSON over the local socket.

### Commands (PyQt → Mudlet)

```json
{"id": "1", "cmd": "listProfiles"}
{"id": "2", "cmd": "launchProfile", "name": "MyMUD"}
{"id": "3", "cmd": "closeProfile",  "name": "MyMUD"}
{"id": "4", "cmd": "setTags",       "name": "MyMUD", "tags": ["pvp", "roleplay"]}
{"id": "5", "cmd": "ping"}
```

### Responses (Mudlet → PyQt, matched by `id`)

```json
{"id": "1", "ok": true, "profiles": [{"name": "MyMUD", "tags": ["pvp"], "running": true}]}
{"id": "2", "ok": true}
{"id": "3", "ok": false, "error": "Profile not running"}
```

### Async events (Mudlet → PyQt, no `id`)

```json
{"event": "profileStarted", "name": "MyMUD"}
{"event": "profileStopped", "name": "MyMUD"}
{"event": "profileError",   "name": "MyMUD", "message": "Connection refused"}
```

`ping` responds with `{"id": "5", "ok": true}` and is used by `MudletClient` to confirm the server is alive before sending real commands.

The `id` field distinguishes responses from events. The protocol is additive — new commands and events can be added without breaking existing clients.

---

## C++ Changes (Mudlet Backend)

### New classes

**`MudletIPCManager`** (owned by the `mudlet` application class):
- Owns a `QLocalServer`; binds at startup to a platform-appropriate socket name
- Manages connected client sockets (supports multiple connections)
- Parses incoming JSON, dispatches to command handlers
- Broadcasts async events to all connected clients by connecting to existing profile-lifecycle signals in `mudlet`

**`ProfileMetadata`** (lightweight, no Qt widget dependencies):
- Reads and writes `tags.json` in a profile's directory
- Writes atomically: write to a temp file, then rename
- Used by `MudletIPCManager` for `setTags` and `listProfiles`

### Changes to existing code

| File | Change |
|------|--------|
| `mudlet.cpp` | Instantiate `MudletIPCManager` in `startupAction()` when `--ipc-mode` is passed |
| `mudlet.cpp` | Connect existing profile-lifecycle signals to `MudletIPCManager::onProfileStarted` etc. |
| `mudlet.h` | Add `MudletIPCManager* mpIPCManager` member |
| `main.cpp` | Parse `--ipc-mode` CLI flag; pass to `mudlet` constructor |
| `mudlet.cpp` | Skip showing built-in profile picker dialog when `--ipc-mode` is active |

No changes to `Host`, `cTelnet`, `TConsole`, trigger units, or Lua interpreter.

### `--ipc-mode` flag behaviour

When Mudlet is launched with `--ipc-mode`:
1. `MudletIPCManager` starts and binds the socket
2. The built-in `dlgConnectionProfiles` picker is not shown
3. Mudlet runs its event loop and waits for IPC commands to open profiles

---

## PyQt6 Frontend

### `MudletClient` — IPC wrapper

- Owns a `QLocalSocket`
- On startup: attempts connection; if it fails, launches `mudlet --ipc-mode` via `QProcess`, then retries every 500ms for up to 10 seconds
- If Mudlet does not become available within the timeout, shows an error dialog
- Serializes commands to JSON with auto-incrementing `id`; resolves in-flight `Future`s when matching response arrives
- Each in-flight command times out after 5 seconds to prevent UI hangs
- Emits Qt signals for async events: `profile_started(name)`, `profile_stopped(name)`, `profile_error(name, message)`
- On disconnect: disables UI controls, re-enters retry/relaunch mode

### `ProfileModel` — `QAbstractListModel`

- Holds profile list with tags and running state
- Populated on connect via `listProfiles`; kept in sync by `MudletClient` signals
- Supports tag-based filtering via a `QSortFilterProxyModel`

### `ProfileBrowserWindow` — main UI

- Profile list view driven by `ProfileModel` + filter proxy
- Tag filter bar (checkboxes or chips) at the top
- Per-profile: name, tags, running indicator, Launch / Stop button
- Inline tag editor for adding and removing tags
- Writes tag changes back to Mudlet via `setTags` command

### Mudlet path discovery

The frontend locates the `mudlet` executable via:
1. An environment variable `MUDLET_PATH` if set
2. The system `PATH`
3. A config file preference

---

## Edge Cases

| Scenario | Behaviour |
|----------|-----------|
| Mudlet not running on frontend start | Frontend launches `mudlet --ipc-mode`, retries connection for 10s |
| Mudlet fails to start | Frontend shows error dialog with path hint |
| Connection lost mid-session | UI disables controls, enters reconnect loop |
| Multiple frontend instances | `QLocalServer` accepts all; events broadcast to all clients |
| Concurrent tag writes | `ProfileMetadata` writes atomically (write + rename) |
| Unknown command | Mudlet responds `{"id": "...", "ok": false, "error": "Unknown command"}` |
| Profile launch while already running | Mudlet responds with error; frontend shows inline status |

---

## Out of Scope

- Replacing the in-game UI (consoles, trigger editor, mapper)
- Automated tests (manual testing sufficient for initial implementation)
- HTTP/WebSocket interface
- macOS notarization or Windows installer changes
