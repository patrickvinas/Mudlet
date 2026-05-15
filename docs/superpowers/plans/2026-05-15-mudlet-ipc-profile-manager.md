# Mudlet IPC Profile Manager — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a QLocalServer-based IPC layer to Mudlet and a PyQt6 profile launcher that lists, tags, launches, and stops profiles via a tag-filtered UI.

**Architecture:** Two new C++ classes — `ProfileMetadata` (atomic reads/writes of per-profile `tags.json`) and `MudletIPCManager` (owns a `QLocalServer`, speaks newline-delimited JSON) — are added to `src/`. A `--ipc-mode` CLI flag suppresses the built-in profile picker and activates the IPC server. A separate PyQt6 application in `tools/mudlet-launcher/` connects via `QLocalSocket`, auto-launches Mudlet if it isn't running, and shows a tag-filtered profile browser.

**Tech Stack:** C++20, Qt6 (`QLocalServer`, `QLocalSocket`, `QJsonDocument`, `QSaveFile`); Python 3.10+, PyQt6

---

## File Map

**New C++ files:**
- `src/ProfileMetadata.h` — read/write `tags.json` per profile, no UI dependencies
- `src/ProfileMetadata.cpp`
- `src/MudletIPCManager.h` — `QLocalServer`, JSON protocol, event broadcasting
- `src/MudletIPCManager.cpp`

**Modified C++ files:**
- `src/mudlet.h` — add `mIPCMode` bool, `mpIPCManager` pointer, `startIPCMode()` method
- `src/mudlet.cpp` — skip profile dialog when `mIPCMode`, implement `startIPCMode()`
- `src/main.cpp` — parse `--ipc-mode` flag, call `startIPCMode()` before event loop
- `src/CMakeLists.txt` — register new source files

**New Python files:**
- `tools/mudlet-launcher/requirements.txt`
- `tools/mudlet-launcher/mudlet_launcher/__init__.py` (empty)
- `tools/mudlet-launcher/mudlet_launcher/__main__.py` — entry point
- `tools/mudlet-launcher/mudlet_launcher/mudlet_client.py` — `MudletClient` IPC wrapper
- `tools/mudlet-launcher/mudlet_launcher/profile_model.py` — `ProfileModel` + `TagFilterProxyModel`
- `tools/mudlet-launcher/mudlet_launcher/main_window.py` — `ProfileBrowserWindow`

---

## Task 1: ProfileMetadata class

**Files:**
- Create: `src/ProfileMetadata.h`
- Create: `src/ProfileMetadata.cpp`

- [ ] **Step 1: Create `src/ProfileMetadata.h`**

```cpp
#ifndef PROFILEMETADATA_H
#define PROFILEMETADATA_H

class QString;
class QStringList;

class ProfileMetadata {
public:
    static QStringList readTags(const QString& profilePath);
    static bool writeTags(const QString& profilePath, const QStringList& tags);
};

#endif // PROFILEMETADATA_H
```

- [ ] **Step 2: Create `src/ProfileMetadata.cpp`**

`qsl` is defined in `src/utils.h` as `#define qsl(s) QStringLiteral(s)`.

```cpp
#include "ProfileMetadata.h"

#include "utils.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QString>
#include <QStringList>

QStringList ProfileMetadata::readTags(const QString& profilePath)
{
    QFile file(profilePath + qsl("/tags.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QStringList tags;
    for (const QJsonValue& v : doc.array()) {
        if (v.isString()) {
            tags << v.toString();
        }
    }
    return tags;
}

bool ProfileMetadata::writeTags(const QString& profilePath, const QStringList& tags)
{
    QJsonArray arr;
    for (const QString& tag : tags) {
        arr.append(tag);
    }
    QSaveFile file(profilePath + qsl("/tags.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(arr).toJson());
    return file.commit();
}
```

- [ ] **Step 3: Run clang-format**

```bash
clang-format -i src/ProfileMetadata.h src/ProfileMetadata.cpp
```

- [ ] **Step 4: Commit**

```bash
git add src/ProfileMetadata.h src/ProfileMetadata.cpp
git commit -m "feat: add ProfileMetadata class for per-profile tag storage"
```

---

## Task 2: MudletIPCManager class

**Files:**
- Create: `src/MudletIPCManager.h`
- Create: `src/MudletIPCManager.cpp`

**Before writing:** verify two APIs by reading their headers:
- `HostManager::getHost(const QString&)` — confirmed at `src/HostManager.h:54`; returns `Host*`
- `mudlet::getMudletPath(enums::mudletPathType, ...)` — confirmed at `src/mudlet.h:111`; enum values `enums::profilesPath` and `enums::profileHomePath` are in `src/enums.h:63,67`

- [ ] **Step 1: Create `src/MudletIPCManager.h`**

```cpp
#ifndef MUDLETIPCMANAGER_H
#define MUDLETIPCMANAGER_H

#include <QHash>
#include <QList>
#include <QObject>

class mudlet;
class Host;
class QLocalServer;
class QLocalSocket;

class MudletIPCManager : public QObject {
    Q_OBJECT

public:
    explicit MudletIPCManager(mudlet* pMudlet, QObject* parent = nullptr);
    bool start();

public slots:
    void onHostCreated(Host* pHost, quint8);
    void onHostDestroyed(Host* pHost, quint8);

private slots:
    void onNewConnection();
    void onClientDataReady();
    void onClientDisconnected();

private:
    void handleCommand(QLocalSocket* client, const QJsonObject& cmd);
    void sendToClient(QLocalSocket* client, const QJsonObject& msg);
    void broadcastEvent(const QJsonObject& event);

    mudlet* mpMudlet;
    QLocalServer* mpServer;
    QList<QLocalSocket*> mClients;
    QHash<QLocalSocket*, QByteArray> mBuffers;
};

#endif // MUDLETIPCMANAGER_H
```

- [ ] **Step 2: Create `src/MudletIPCManager.cpp`**

```cpp
#include "MudletIPCManager.h"

#include "Host.h"
#include "ProfileMetadata.h"
#include "enums.h"
#include "mudlet.h"
#include "utils.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

MudletIPCManager::MudletIPCManager(mudlet* pMudlet, QObject* parent)
    : QObject(parent)
    , mpMudlet(pMudlet)
    , mpServer(new QLocalServer(this))
{
}

bool MudletIPCManager::start()
{
    QLocalServer::removeServer(qsl("mudlet-ipc"));
    if (!mpServer->listen(qsl("mudlet-ipc"))) {
        qWarning() << "MudletIPCManager: failed to start:" << mpServer->errorString();
        return false;
    }
    connect(mpServer, &QLocalServer::newConnection, this, &MudletIPCManager::onNewConnection);
    return true;
}

void MudletIPCManager::onHostCreated(Host* pHost, quint8)
{
    broadcastEvent(QJsonObject{
        {qsl("event"), qsl("profileStarted")},
        {qsl("name"), pHost->getName()}
    });
}

void MudletIPCManager::onHostDestroyed(Host* pHost, quint8)
{
    broadcastEvent(QJsonObject{
        {qsl("event"), qsl("profileStopped")},
        {qsl("name"), pHost->getName()}
    });
}

void MudletIPCManager::onNewConnection()
{
    QLocalSocket* client = mpServer->nextPendingConnection();
    mClients.append(client);
    mBuffers[client] = QByteArray();
    connect(client, &QLocalSocket::readyRead, this, &MudletIPCManager::onClientDataReady);
    connect(client, &QLocalSocket::disconnected, this, &MudletIPCManager::onClientDisconnected);
}

void MudletIPCManager::onClientDataReady()
{
    auto* client = qobject_cast<QLocalSocket*>(sender());
    if (!client) {
        return;
    }
    mBuffers[client] += client->readAll();
    while (mBuffers[client].contains('\n')) {
        const int idx = mBuffers[client].indexOf('\n');
        const QByteArray line = mBuffers[client].left(idx);
        mBuffers[client] = mBuffers[client].mid(idx + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            handleCommand(client, doc.object());
        }
    }
}

void MudletIPCManager::onClientDisconnected()
{
    auto* client = qobject_cast<QLocalSocket*>(sender());
    if (!client) {
        return;
    }
    mClients.removeAll(client);
    mBuffers.remove(client);
    client->deleteLater();
}

void MudletIPCManager::handleCommand(QLocalSocket* client, const QJsonObject& cmd)
{
    const QString id = cmd.value(qsl("id")).toString();
    const QString command = cmd.value(qsl("cmd")).toString();

    if (command == qsl("ping")) {
        sendToClient(client, QJsonObject{{qsl("id"), id}, {qsl("ok"), true}});

    } else if (command == qsl("listProfiles")) {
        const QString profilesRoot = mudlet::getMudletPath(enums::profilesPath);
        QJsonArray profiles;
        for (const QString& name : QDir(profilesRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString profilePath = mudlet::getMudletPath(enums::profileHomePath, name);
            const bool running = mpMudlet->getHostManager().getHost(name) != nullptr;
            QJsonArray tags;
            for (const QString& tag : ProfileMetadata::readTags(profilePath)) {
                tags.append(tag);
            }
            profiles.append(QJsonObject{
                {qsl("name"), name},
                {qsl("tags"), tags},
                {qsl("running"), running}
            });
        }
        sendToClient(client, QJsonObject{
            {qsl("id"), id},
            {qsl("ok"), true},
            {qsl("profiles"), profiles}
        });

    } else if (command == qsl("launchProfile")) {
        const QString name = cmd.value(qsl("name")).toString();
        if (name.isEmpty()) {
            sendToClient(client, QJsonObject{
                {qsl("id"), id}, {qsl("ok"), false}, {qsl("error"), qsl("missing name")}
            });
            return;
        }
        // slot_connectionDialogueFinished(profileName, autoConnect=true) loads and connects
        mpMudlet->slot_connectionDialogueFinished(name, true);
        sendToClient(client, QJsonObject{{qsl("id"), id}, {qsl("ok"), true}});

    } else if (command == qsl("closeProfile")) {
        const QString name = cmd.value(qsl("name")).toString();
        if (mpMudlet->getHostManager().getHost(name) == nullptr) {
            sendToClient(client, QJsonObject{
                {qsl("id"), id}, {qsl("ok"), false}, {qsl("error"), qsl("Profile not running")}
            });
            return;
        }
        mpMudlet->slot_closeProfileByName(name);
        sendToClient(client, QJsonObject{{qsl("id"), id}, {qsl("ok"), true}});

    } else if (command == qsl("setTags")) {
        const QString name = cmd.value(qsl("name")).toString();
        QStringList tags;
        for (const QJsonValue& v : cmd.value(qsl("tags")).toArray()) {
            tags << v.toString();
        }
        const QString profilePath = mudlet::getMudletPath(enums::profileHomePath, name);
        const bool ok = ProfileMetadata::writeTags(profilePath, tags);
        sendToClient(client, QJsonObject{{qsl("id"), id}, {qsl("ok"), ok}});

    } else {
        sendToClient(client, QJsonObject{
            {qsl("id"), id}, {qsl("ok"), false}, {qsl("error"), qsl("Unknown command")}
        });
    }
}

void MudletIPCManager::sendToClient(QLocalSocket* client, const QJsonObject& msg)
{
    client->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
}

void MudletIPCManager::broadcastEvent(const QJsonObject& event)
{
    const QByteArray data = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    for (QLocalSocket* client : mClients) {
        client->write(data);
    }
}
```

- [ ] **Step 3: Run clang-format**

```bash
clang-format -i src/MudletIPCManager.h src/MudletIPCManager.cpp
```

- [ ] **Step 4: Commit**

```bash
git add src/MudletIPCManager.h src/MudletIPCManager.cpp
git commit -m "feat: add MudletIPCManager QLocalServer IPC layer"
```

---

## Task 3: Wire `--ipc-mode` into Mudlet startup

**Files:**
- Modify: `src/mudlet.h`
- Modify: `src/mudlet.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add declarations to `src/mudlet.h`**

Add the forward declaration near the other forward declarations at the top of the file:
```cpp
class MudletIPCManager;
```

Add in the `public:` section (near the other public methods, around line 108):
```cpp
void startIPCMode();
```

Add in the private member variables section (near `mpConnectionDialog`, around line 359):
```cpp
bool mIPCMode = false;
MudletIPCManager* mpIPCManager = nullptr;
```

- [ ] **Step 2: Add `startIPCMode()` to `src/mudlet.cpp` and skip dialog in IPC mode**

At the top of `src/mudlet.cpp`, add to the includes:
```cpp
#include "MudletIPCManager.h"
```

Add the `startIPCMode()` implementation at the bottom of `src/mudlet.cpp` (before the closing of any namespace if applicable):
```cpp
void mudlet::startIPCMode()
{
    mIPCMode = true;
    mpIPCManager = new MudletIPCManager(this, this);
    if (!mpIPCManager->start()) {
        qWarning() << "mudlet: IPC mode requested but server failed to start";
        return;
    }
    connect(this, &mudlet::signal_hostCreated, mpIPCManager, &MudletIPCManager::onHostCreated);
    connect(this, &mudlet::signal_hostDestroyed, mpIPCManager, &MudletIPCManager::onHostDestroyed);
}
```

In `mudlet::slot_showConnectionDialog()`, add an early return at the very top of the function body (after the opening brace):
```cpp
void mudlet::slot_showConnectionDialog()
{
    if (mIPCMode) {
        return;
    }
    // Don't show connection dialog if we're processing a telnet:// URI
    // ... rest of the existing function unchanged ...
```

- [ ] **Step 3: Parse `--ipc-mode` in `src/main.cpp`**

After the last `parser.addOption(steamMode)` call (line 407), add:
```cpp
QCommandLineOption ipcModeOption(qsl("ipc-mode"),
    qsl("Start without profile picker and accept IPC connections for external profile management"));
parser.addOption(ipcModeOption);
```

After the block that checks `parser.isSet(startFullscreen)` (around line 1079), add:
```cpp
if (parser.isSet(ipcModeOption)) {
    mudlet::self()->startIPCMode();
}
```

- [ ] **Step 4: Run clang-format**

```bash
clang-format -i src/mudlet.h src/mudlet.cpp src/main.cpp
```

- [ ] **Step 5: Commit**

```bash
git add src/mudlet.h src/mudlet.cpp src/main.cpp
git commit -m "feat: add --ipc-mode flag to suppress profile picker and start IPC server"
```

---

## Task 4: Register new files with CMake and build

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add new source files to `src/CMakeLists.txt`**

Find the `target_sources(mudlet PRIVATE` block (or equivalent `set(SOURCES ...)` listing). Add the four new files alongside other source files:

```cmake
MudletIPCManager.cpp
MudletIPCManager.h
ProfileMetadata.cpp
ProfileMetadata.h
```

- [ ] **Step 2: Build Mudlet**

```bash
cd /path/to/mudlet/build
cmake --build .
```

Expected: clean build with no errors. If you see errors about `getHost`, `getMudletPath`, or `getName`, verify the exact method signatures in `src/HostManager.h`, `src/mudlet.h`, and `src/Host.h` respectively and fix the call sites in `MudletIPCManager.cpp`.

- [ ] **Step 3: Smoke-test the IPC server**

Run Mudlet in IPC mode (it should start without showing the profile picker):
```bash
./src/mudlet --ipc-mode &
sleep 1
```

Verify the socket is reachable:
```bash
python3 -c "
import socket, json, time
# On Linux/macOS, QLocalServer creates a file socket in the system temp dir
import tempfile, os
sock_path = os.path.join(tempfile.gettempdir(), 'mudlet-ipc')
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
s.send(json.dumps({'id': '1', 'cmd': 'ping'}).encode() + b'\n')
print(s.recv(4096).decode())
"
```

Expected output: `{"id":"1","ok":true}`

Also test `listProfiles`:
```bash
python3 -c "
import socket, json, tempfile, os
sock_path = os.path.join(tempfile.gettempdir(), 'mudlet-ipc')
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
s.send(json.dumps({'id': '2', 'cmd': 'listProfiles'}).encode() + b'\n')
print(json.dumps(json.loads(s.recv(4096)), indent=2))
"
```

Expected: JSON object with `ok: true` and a `profiles` array listing your existing Mudlet profiles.

- [ ] **Step 4: Commit**

```bash
git add src/CMakeLists.txt
git commit -m "build: add MudletIPCManager and ProfileMetadata to CMake sources"
```

---

## Task 5: Python project scaffold

**Files:**
- Create: `tools/mudlet-launcher/requirements.txt`
- Create: `tools/mudlet-launcher/mudlet_launcher/__init__.py`
- Create: `tools/mudlet-launcher/mudlet_launcher/__main__.py`

- [ ] **Step 1: Create directories**

```bash
mkdir -p tools/mudlet-launcher/mudlet_launcher
```

- [ ] **Step 2: Create `tools/mudlet-launcher/requirements.txt`**

```
PyQt6>=6.6.0
```

- [ ] **Step 3: Create `tools/mudlet-launcher/mudlet_launcher/__init__.py`**

Empty file.

- [ ] **Step 4: Create `tools/mudlet-launcher/mudlet_launcher/__main__.py`**

```python
import sys
from PyQt6.QtWidgets import QApplication
from .main_window import ProfileBrowserWindow


def main():
    app = QApplication(sys.argv)
    window = ProfileBrowserWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Install dependencies**

```bash
cd tools/mudlet-launcher
pip install -r requirements.txt
```

- [ ] **Step 6: Commit**

```bash
git add tools/mudlet-launcher/
git commit -m "feat: add PyQt6 launcher project scaffold"
```

---

## Task 6: MudletClient IPC wrapper

**Files:**
- Create: `tools/mudlet-launcher/mudlet_launcher/mudlet_client.py`

- [ ] **Step 1: Create `tools/mudlet-launcher/mudlet_launcher/mudlet_client.py`**

```python
import json
import shutil
from typing import Callable, Optional

from PyQt6.QtCore import QObject, QProcess, QTimer, pyqtSignal
from PyQt6.QtNetwork import QLocalSocket


class MudletClient(QObject):
    profile_started = pyqtSignal(str)
    profile_stopped = pyqtSignal(str)
    profile_error = pyqtSignal(str, str)
    connected = pyqtSignal()
    disconnected = pyqtSignal()

    _SOCKET_NAME = "mudlet-ipc"
    _RETRY_INTERVAL_MS = 500
    _CONNECT_TIMEOUT_MS = 10_000
    _COMMAND_TIMEOUT_MS = 5_000

    def __init__(self, mudlet_path: Optional[str] = None, parent=None):
        super().__init__(parent)
        self._mudlet_path = mudlet_path or self._find_mudlet()
        self._socket = QLocalSocket(self)
        self._buffer = b""
        self._pending: dict[str, dict] = {}
        self._next_id = 0
        self._retry_elapsed_ms = 0
        self._mudlet_launched = False

        self._retry_timer = QTimer(self)
        self._retry_timer.setInterval(self._RETRY_INTERVAL_MS)
        self._retry_timer.timeout.connect(self._attempt_connect)

        self._socket.connected.connect(self._on_connected)
        self._socket.disconnected.connect(self._on_disconnected)
        self._socket.readyRead.connect(self._on_data)

    def start(self):
        self._retry_elapsed_ms = 0
        self._mudlet_launched = False
        self._attempt_connect()

    def _find_mudlet(self) -> str:
        import os
        env = os.environ.get("MUDLET_PATH")
        if env:
            return env
        found = shutil.which("mudlet")
        return found if found else "mudlet"

    def _attempt_connect(self):
        if self._socket.state() == QLocalSocket.LocalSocketState.ConnectedState:
            return
        self._socket.connectToServer(self._SOCKET_NAME)
        if self._socket.state() == QLocalSocket.LocalSocketState.ConnectedState:
            return

        self._retry_elapsed_ms += self._RETRY_INTERVAL_MS
        if self._retry_elapsed_ms >= self._CONNECT_TIMEOUT_MS:
            self._retry_timer.stop()
            self.profile_error.emit("", f"Could not connect to Mudlet after {self._CONNECT_TIMEOUT_MS // 1000}s")
            return

        if not self._mudlet_launched:
            self._mudlet_launched = True
            proc = QProcess(self)
            proc.startDetached(self._mudlet_path, ["--ipc-mode"])

        self._retry_timer.start()

    def _on_connected(self):
        self._retry_timer.stop()
        self._retry_elapsed_ms = 0
        self.connected.emit()

    def _on_disconnected(self):
        self._buffer = b""
        for entry in self._pending.values():
            entry["timer"].stop()
        self._pending.clear()
        self.disconnected.emit()
        self._retry_elapsed_ms = 0
        self._mudlet_launched = False
        self._retry_timer.start()

    def _on_data(self):
        self._buffer += bytes(self._socket.readAll())
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "event" in msg:
                self._dispatch_event(msg)
            elif "id" in msg:
                self._dispatch_response(msg)

    def _dispatch_event(self, msg: dict):
        event = msg.get("event", "")
        name = msg.get("name", "")
        if event == "profileStarted":
            self.profile_started.emit(name)
        elif event == "profileStopped":
            self.profile_stopped.emit(name)
        elif event == "profileError":
            self.profile_error.emit(name, msg.get("message", ""))

    def _dispatch_response(self, msg: dict):
        entry = self._pending.pop(str(msg.get("id", "")), None)
        if entry:
            entry["timer"].stop()
            entry["callback"](msg)

    def _send(self, cmd: dict, callback: Callable):
        self._next_id += 1
        msg_id = str(self._next_id)
        cmd["id"] = msg_id
        timer = QTimer(self)
        timer.setSingleShot(True)
        timer.setInterval(self._COMMAND_TIMEOUT_MS)
        timer.timeout.connect(lambda: self._on_timeout(msg_id))
        self._pending[msg_id] = {"callback": callback, "timer": timer}
        timer.start()
        self._socket.write(json.dumps(cmd).encode() + b"\n")

    def _on_timeout(self, msg_id: str):
        entry = self._pending.pop(msg_id, None)
        if entry:
            entry["callback"]({"ok": False, "error": "timeout"})

    def list_profiles(self, callback: Callable):
        self._send({"cmd": "listProfiles"}, callback)

    def launch_profile(self, name: str, callback: Optional[Callable] = None):
        self._send({"cmd": "launchProfile", "name": name}, callback or (lambda _: None))

    def close_profile(self, name: str, callback: Optional[Callable] = None):
        self._send({"cmd": "closeProfile", "name": name}, callback or (lambda _: None))

    def set_tags(self, name: str, tags: list[str], callback: Optional[Callable] = None):
        self._send({"cmd": "setTags", "name": name, "tags": tags}, callback or (lambda _: None))
```

- [ ] **Step 2: Commit**

```bash
git add tools/mudlet-launcher/mudlet_launcher/mudlet_client.py
git commit -m "feat: add MudletClient IPC wrapper"
```

---

## Task 7: ProfileModel and TagFilterProxyModel

**Files:**
- Create: `tools/mudlet-launcher/mudlet_launcher/profile_model.py`

- [ ] **Step 1: Create `tools/mudlet-launcher/mudlet_launcher/profile_model.py`**

```python
from dataclasses import dataclass, field
from typing import Optional

from PyQt6.QtCore import QAbstractListModel, QModelIndex, QSortFilterProxyModel, Qt


@dataclass
class ProfileEntry:
    name: str
    tags: list[str] = field(default_factory=list)
    running: bool = False


class ProfileModel(QAbstractListModel):
    NameRole = Qt.ItemDataRole.UserRole + 1
    TagsRole = Qt.ItemDataRole.UserRole + 2
    RunningRole = Qt.ItemDataRole.UserRole + 3

    def __init__(self, parent=None):
        super().__init__(parent)
        self._profiles: list[ProfileEntry] = []

    def rowCount(self, parent=QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self._profiles)

    def data(self, index: QModelIndex, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid() or index.row() >= len(self._profiles):
            return None
        p = self._profiles[index.row()]
        if role in (Qt.ItemDataRole.DisplayRole, self.NameRole):
            return p.name
        if role == self.TagsRole:
            return p.tags
        if role == self.RunningRole:
            return p.running
        return None

    def load(self, profiles: list[dict]):
        self.beginResetModel()
        self._profiles = [
            ProfileEntry(name=p["name"], tags=p.get("tags", []), running=p.get("running", False))
            for p in profiles
        ]
        self.endResetModel()

    def set_running(self, name: str, running: bool):
        for i, p in enumerate(self._profiles):
            if p.name == name:
                p.running = running
                idx = self.index(i)
                self.dataChanged.emit(idx, idx, [self.RunningRole])
                return

    def set_tags(self, name: str, tags: list[str]):
        for i, p in enumerate(self._profiles):
            if p.name == name:
                p.tags = tags
                idx = self.index(i)
                self.dataChanged.emit(idx, idx, [self.TagsRole])
                return

    def get_profile(self, name: str) -> Optional[ProfileEntry]:
        for p in self._profiles:
            if p.name == name:
                return p
        return None

    def all_tags(self) -> set[str]:
        tags: set[str] = set()
        for p in self._profiles:
            tags.update(p.tags)
        return tags


class TagFilterProxyModel(QSortFilterProxyModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._active_tags: set[str] = set()

    def set_active_tags(self, tags: set[str]):
        self._active_tags = tags
        self.invalidateFilter()

    def filterAcceptsRow(self, source_row: int, source_parent: QModelIndex) -> bool:
        if not self._active_tags:
            return True
        idx = self.sourceModel().index(source_row, 0, source_parent)
        profile_tags: list[str] = self.sourceModel().data(idx, ProfileModel.TagsRole) or []
        return bool(self._active_tags.intersection(profile_tags))
```

- [ ] **Step 2: Commit**

```bash
git add tools/mudlet-launcher/mudlet_launcher/profile_model.py
git commit -m "feat: add ProfileModel and TagFilterProxyModel"
```

---

## Task 8: ProfileBrowserWindow

**Files:**
- Create: `tools/mudlet-launcher/mudlet_launcher/main_window.py`

- [ ] **Step 1: Create `tools/mudlet-launcher/mudlet_launcher/main_window.py`**

```python
from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (QAbstractItemView, QCheckBox, QHBoxLayout, QInputDialog,
                              QLabel, QListView, QMainWindow, QPushButton, QVBoxLayout,
                              QWidget)

from .mudlet_client import MudletClient
from .profile_model import ProfileModel, TagFilterProxyModel


class ProfileBrowserWindow(QMainWindow):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Mudlet Launcher")
        self.resize(600, 500)

        self._model = ProfileModel()
        self._proxy = TagFilterProxyModel()
        self._proxy.setSourceModel(self._model)

        self._client = MudletClient(parent=self)
        self._client.connected.connect(self._on_connected)
        self._client.disconnected.connect(self._on_disconnected)
        self._client.profile_started.connect(lambda name: self._model.set_running(name, True))
        self._client.profile_stopped.connect(lambda name: self._model.set_running(name, False))
        self._client.profile_started.connect(lambda _: self._refresh_buttons())
        self._client.profile_stopped.connect(lambda _: self._refresh_buttons())

        self._tag_checkboxes: dict[str, QCheckBox] = {}

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        self._status_label = QLabel("Connecting to Mudlet...")
        root.addWidget(self._status_label)

        tag_row = QHBoxLayout()
        tag_row.addWidget(QLabel("Filter by tag:"))
        self._tag_row = tag_row
        tag_row.addStretch()
        root.addLayout(tag_row)

        self._list_view = QListView()
        self._list_view.setModel(self._proxy)
        self._list_view.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self._list_view.selectionModel().selectionChanged.connect(self._refresh_buttons)
        root.addWidget(self._list_view)

        btn_row = QHBoxLayout()
        self._launch_btn = QPushButton("Launch")
        self._stop_btn = QPushButton("Stop")
        self._edit_tags_btn = QPushButton("Edit Tags")
        for btn in (self._launch_btn, self._stop_btn, self._edit_tags_btn):
            btn.setEnabled(False)
            btn_row.addWidget(btn)
        btn_row.addStretch()
        root.addLayout(btn_row)

        self._launch_btn.clicked.connect(self._on_launch)
        self._stop_btn.clicked.connect(self._on_stop)
        self._edit_tags_btn.clicked.connect(self._on_edit_tags)

        self._client.start()

    def _on_connected(self):
        self._status_label.setText("Connected")
        self._list_view.setEnabled(True)
        self._client.list_profiles(self._on_profiles_loaded)

    def _on_disconnected(self):
        self._status_label.setText("Reconnecting to Mudlet...")
        self._list_view.setEnabled(False)
        self._launch_btn.setEnabled(False)
        self._stop_btn.setEnabled(False)
        self._edit_tags_btn.setEnabled(False)

    def _on_profiles_loaded(self, response: dict):
        if not response.get("ok"):
            return
        self._model.load(response.get("profiles", []))
        self._rebuild_tag_bar()

    def _rebuild_tag_bar(self):
        for cb in self._tag_checkboxes.values():
            self._tag_row.removeWidget(cb)
            cb.deleteLater()
        self._tag_checkboxes.clear()
        for tag in sorted(self._model.all_tags()):
            cb = QCheckBox(tag)
            cb.stateChanged.connect(self._on_tag_filter_changed)
            self._tag_checkboxes[tag] = cb
            self._tag_row.insertWidget(self._tag_row.count() - 1, cb)

    def _on_tag_filter_changed(self):
        active = {tag for tag, cb in self._tag_checkboxes.items() if cb.isChecked()}
        self._proxy.set_active_tags(active)

    def _selected_name(self) -> str | None:
        indexes = self._list_view.selectionModel().selectedIndexes()
        if not indexes:
            return None
        return self._model.data(self._proxy.mapToSource(indexes[0]), ProfileModel.NameRole)

    def _refresh_buttons(self):
        name = self._selected_name()
        if name is None:
            self._launch_btn.setEnabled(False)
            self._stop_btn.setEnabled(False)
            self._edit_tags_btn.setEnabled(False)
            return
        profile = self._model.get_profile(name)
        running = profile.running if profile else False
        self._launch_btn.setEnabled(not running)
        self._stop_btn.setEnabled(running)
        self._edit_tags_btn.setEnabled(True)

    def _on_launch(self):
        name = self._selected_name()
        if name:
            self._client.launch_profile(name)

    def _on_stop(self):
        name = self._selected_name()
        if name:
            self._client.close_profile(name)

    def _on_edit_tags(self):
        name = self._selected_name()
        if not name:
            return
        profile = self._model.get_profile(name)
        current = ", ".join(profile.tags) if profile else ""
        text, ok = QInputDialog.getText(
            self, "Edit Tags", f"Tags for '{name}' (comma-separated):", text=current
        )
        if not ok:
            return
        tags = [t.strip() for t in text.split(",") if t.strip()]
        self._model.set_tags(name, tags)
        self._rebuild_tag_bar()
        self._client.set_tags(name, tags)
```

- [ ] **Step 2: Verify the launcher runs**

```bash
cd tools/mudlet-launcher
python -m mudlet_launcher
```

Expected: window appears showing "Connecting to Mudlet...", then Mudlet launches automatically in the background and the profile list appears.

- [ ] **Step 3: Commit**

```bash
git add tools/mudlet-launcher/mudlet_launcher/main_window.py
git commit -m "feat: add ProfileBrowserWindow tag-filtered profile launcher UI"
```

---

## Task 9: End-to-end manual verification

- [ ] **Step 1: Build Mudlet**

```bash
cd /path/to/mudlet/build
cmake --build .
```

- [ ] **Step 2: Run through the full use case**

1. Start the launcher: `cd tools/mudlet-launcher && python -m mudlet_launcher`
2. Confirm Mudlet starts automatically with `--ipc-mode` (no profile picker dialog)
3. Confirm the profile list appears in the launcher window
4. Select a profile → click **Launch** → confirm Mudlet opens that profile
5. Confirm the profile shows as running (Stop button enabled, Launch disabled)
6. Click **Edit Tags** → enter `pvp, roleplay` → confirm tag filter checkboxes appear
7. Check the `pvp` checkbox → confirm profiles without that tag are hidden
8. Click **Stop** → confirm the profile closes and the running indicator updates
9. Kill the Mudlet process → confirm the launcher shows "Reconnecting..." and re-launches Mudlet
