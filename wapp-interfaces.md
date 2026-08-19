# Wapp Engine Interfaces

This document defines the abstractions a **wapp engine** must
provide to host wapps and the contract a wapp module must respect to
run inside one.

A wapp is a self-contained WASM module shipped as a `.wapp` ZIP
package. The engine is the host-side runtime that loads the module,
exposes the abstractions below as imported functions (the HAL),
delivers messages and events, and surfaces the wapp's UI to the
user. The engine is the only side of the contract that talks to the
real OS — the wapp itself runs in a sandbox and only ever sees the
HAL.

There are two reference implementations of the engine in this
repository:

- `iwi/lib/wapp/wapp_engine.dart` — the prototype host
- `lib/wapp/wapp_engine.dart` — the integrated XPRS host

Both implement the same abstractions. The C-level ABI for the
abstractions lives in `wapps/hal/xprs_wasm_hal.h`.

---

## 1. Module lifecycle

The engine drives the WASM module through four lifecycle hooks the
module must export:

| Export | Called when | Notes |
|---|---|---|
| `module_init()` | Once after WASM instantiation | Wire up state, subscribe to topics, register provider responders |
| `module_tick()` | Every `tick_interval_ms` | Periodic work: poll, refresh, prune |
| `module_handle_event()` | When a message lands in the inbox | Drain `hal_msg_recv` and `hal_event_recv` here |
| `module_destroy()` | On wapp unload | Flush any pending writes; the engine releases everything else |
| `module_tick_interval_ms()` | Once during init | Module-declared default; the manifest's `tick_interval_ms` overrides |

Lifecycle events the engine must publish on the host event bus:
`WappLoadedEvent`, `WappUnloadedEvent`, `WappCrashedEvent` (with a
phase tag: `load`, `init`, `tick`, `handle_event`).

---

## 2. Storage

**Direct disk access is forbidden.** The wapp runs in a WASM sandbox
and has no way to call `open(2)`, mmap, or any host filesystem
primitive. Every byte the wapp reads or writes — KV, files,
package contents, screens, translations — must flow through the
HAL. The engine is the only side that talks to the real
filesystem, and it must do so through a uniform abstraction so the
same wapp works identically when the active profile is:

- a plain folder on disk (filesystem profile)
- an encrypted SQLite archive (encrypted profile)
- an in-memory stub (web / tests)

The reference engines route every storage call through
`ProfileStorage` (see `lib/services/profile_storage.dart`) which
hides the backend behind a single abstract interface. New backends
plug in by implementing that interface — no wapp ever needs to
change.

### 2.1 Key-value store (`hal.kv`)

Every wapp gets a private KV namespace scoped to its wapp ID. Keys
and values are arbitrary byte strings; the engine persists them
across runs.

```c
uint32_t hal_kv_get   (key, key_len, val_buf, val_buf_len);
int32_t  hal_kv_set   (key, key_len, val, val_len);
int32_t  hal_kv_delete(key, key_len);
uint32_t hal_kv_list  (prefix, prefix_len, buf, buf_len);
int32_t  hal_kv_exists(key, key_len);
uint32_t hal_kv_size  (key, key_len);
```

The engine must:
- Hold the KV in memory while the wapp runs (HAL calls are
  synchronous — no `await` allowed inside an import)
- Persist changes to the active profile's storage
  (`<profile>/<wappId>/kv.json` in the integrated host)
- Work transparently on top of an encrypted backend (the parent's
  `EncryptedProfileStorage`) when the active profile is encrypted

### 2.2 Host filesystem I/O (`hal.file`)

Bulk binary I/O against the host filesystem. **Absolute paths only,
no sandbox** — same trust model as `hal.process` (§7.1a). Reads
slurp the whole file at open and serve from a buffer; writes
accumulate in memory and flush at close. Parent directories are
created automatically on write/append open. Handles are opaque
integers; out-of-range reads return `0`, writes on a closed handle
fail with `-1`.

```c
int32_t hal_file_open (path, path_len, mode);   // mode: 0=R 1=W 2=A
int32_t hal_file_read (handle, buf, buf_len);
int32_t hal_file_write(handle, buf, buf_len);
void    hal_file_close(handle);
```

### 2.3 Package read access

The engine exposes the wapp's own package directory (manifest,
screens, media, lang) read-only via the same path conventions the
host uses (`screens/home.ui.json`, `media/icons/icon.svg`,
`lang/en.json`). Wapps don't read these directly — the engine
parses screens, surfaces translations via `hal_i18n_get`, and
renders icons on the host side.

---

## 3. Messaging (host ↔ wapp)

A duplex JSON channel between the wapp's WASM module and the host.
Used for everything the GeoUI renderer can't express on its own.

```c
void     hal_msg_send    (json, json_len);   // wapp → host
uint32_t hal_msg_available(void);            // bytes in next msg
uint32_t hal_msg_recv    (buf, buf_len);     // host → wapp
```

Reserved message types the engine **must** handle:

| Direction | `type` | Purpose |
|---|---|---|
| wapp → host | `notify` | Show a notification (level, title, body, scope) |
| wapp → host | `ui.append` | Append a structured line to a host-rendered output group |
| wapp → host | `ui.log.append` | Append text to a `$type="log"` field |
| wapp → host | `ui.snackbar` | Show a transient floating toast (level: info/success/warn/error) |
| wapp → host | `widget.request` | Call a functionality on another wapp |
| wapp → host | `widget.response` | Reply to a `widget.request` |
| wapp → host | `wapp.fetch_index` | Ask the host to fetch a remote wapp index |
| wapp → host | `wapp.install` | Ask the host to download + extract a `.wapp` |
| wapp → host | `store.sources` | Push the current wapp-store source list |
| host → wapp | `command` | Generic command + scalar fields bag |
| host → wapp | `action` | GeoUI action button fired |
| host → wapp | `wapp.index` | Reply to `wapp.fetch_index` |
| host → wapp | `wapp.installed` | Confirmation for `wapp.install` |
| wapp → host | `profile.read` / `write` / `list` / `exists` / `size` / `mkdir` / `remove` | Permission-gated access to a profile-scoped path — see §15.1 |
| host → wapp | `profile.<op>.response` | Response to a `profile.<op>` request — see §15.1 |
| wapp → host | `identity.get` | Read the active profile's callsign + npub — see §15.1 |
| host → wapp | `identity.get.response` | Reply to `identity.get` — see §15.1 |
| wapp → host | `sign.schnorr` | BIP-340 Schnorr signature over a 32-byte digest — see §15.1 |
| host → wapp | `sign.schnorr.response` | Reply with the 64-byte signature — see §15.1 |
| wapp → host | `tests.run` | Run the test suite of a wapp — see §20 |
| host → wapp | `tests.case` / `tests.complete` | Test runner progress and summary — see §20 |

Anything else is wapp-specific and routed unchanged.

---

## 4. Event bus (cross-wapp pub/sub)

A topic-based broadcast channel separate from the host ↔ wapp
message channel. Lets wapps publish to and subscribe from each
other without knowing names.

```c
int32_t  hal_event_subscribe  (topic, topic_len);
int32_t  hal_event_unsubscribe(topic, topic_len);
int32_t  hal_event_publish    (topic, topic_len, data, data_len);
uint32_t hal_event_available  (void);
uint32_t hal_event_recv       (topic_buf, topic_buf_len,
                                data_buf, data_buf_len);
```

Engine duties:
- One private inbox per loaded wapp; deliver each event to every
  subscriber of an exact-match topic
- Mirror every publish on the host event bus as a
  `WappEventBridgeEvent` so Dart-side observers can debug or bridge
- Drop events for unloaded wapps cleanly

---

## 5. Functionalities (provider/consumer)

Wapps may declare functionalities they provide (in
`provides.functionalities` in `manifest.json`) and call
functionalities provided by other wapps via `widget.request`.

Lookup is by exact ID (e.g. `text.greet`, `tile_provider.osm`).
When multiple wapps provide the same ID, the user picks one default
through a host-managed preference.

The engine's broker must:
- Maintain a registry: `functionalityId → [providerWapp]`
- Resolve the request to one provider (preference > first
  registered)
- Spin up a **headless** instance of the provider (its own engine
  with no UI) when needed
- Inject the request as `widget.request` into the provider, run
  `module_handle_event`
- Scrape the outbox for a matching `widget.response` and deliver
  it back to the caller's inbox
- Surface errors (no provider, response timeout, exception) as
  `widget.response` with an `error` field

---

## 6. Identity-addressed messaging (transport-agnostic)

Wapps must never pick the transport. They address peers by
**callsign** or **npub** and let the engine route the message
through whichever physical channel is available — LAN, mesh
(LoRa, BLE), station relay, or NOSTR relay. If multiple paths
exist the engine picks the cheapest reachable one and falls back
silently when one drops.

Reserved host ↔ wapp message types:

| Direction | `type` | Purpose |
|---|---|---|
| wapp → host | `peer.send` | Send opaque bytes/JSON to a peer by identity |
| wapp → host | `peer.subscribe` | Subscribe to inbound messages from a peer or topic |
| wapp → host | `peer.unsubscribe` | Drop a previous subscription |
| wapp → host | `peer.who` | Ask the engine to resolve a callsign → npub |
| host → wapp | `peer.recv` | Inbound message from a peer (after subscribe) |
| host → wapp | `peer.status` | Peer reachability changed (online, offline, channel) |
| host → wapp | `peer.who.result` | Reply to `peer.who` |

Outbound shape (`peer.send`):

```json
{
  "type": "peer.send",
  "to":   "npub1…"                        | "X1ABCD",
  "tag":  "<wapp-defined channel name>",
  "data": "<opaque string or JSON object>",
  "qos":  "best-effort" | "reliable",      // optional
  "ttl_ms": 30000                          // optional
}
```

Inbound shape (`peer.recv`):

```json
{
  "type": "peer.recv",
  "from": "npub1…",
  "tag":  "...",
  "data": "...",
  "via":  "lan" | "lora" | "ble" | "relay" | "nostr"
}
```

Engine duties:
- Maintain a routing table mapping `npub` (canonical) and the
  short `callsign` form to the set of active transports
- Pick the cheapest viable transport per message, retry on
  failure if `qos == "reliable"`, drop silently otherwise
- Sign outbound messages with the active profile's `nsec` so
  the receiver can authenticate the `from` field
- Expose the chosen transport in `via` for debugging only — the
  wapp must not branch on it
- Buffer inbound messages until the wapp `peer.subscribe`s; drop
  messages for unloaded wapps cleanly

Wapps that need **broadcast to anyone listening** use the
cross-wapp event bus (Section 4) instead — that channel is
local-only by design.

---

## 7. Triggers and timers

There is no general scheduling primitive in the HAL. The engine
provides:

- A periodic **tick timer** at `tick_interval_ms` (per manifest)
  that calls `module_tick()`
- An **event-driven trigger** that calls `module_handle_event()`
  whenever a host→wapp message lands in the inbox

If a wapp needs delayed work it must self-schedule using
`hal_time_ms()` and check inside `module_tick()`.

---

## 8. Background processes and the task monitor

Wapps cannot spawn OS processes, threads, or isolates from inside
the WASM sandbox. When a wapp needs work that runs **outside the
foreground UI thread** — long-running pollers, sync loops, batch
fetches, sensor watchers — it asks the engine to register a
**managed task**. The engine spins it off, schedules it, and
exposes it through the host's task monitor so the user (or the
host itself) can pause, resume, throttle, or kill it.

Reserved host ↔ wapp message types:

| Direction | `type` | Purpose |
|---|---|---|
| wapp → host | `task.register` | Declare a background task and have the engine start scheduling it |
| wapp → host | `task.unregister` | Drop a previously-registered task |
| wapp → host | `task.update` | Mutate task description / priority / interval at runtime |
| wapp → host | `task.heartbeat` | Report progress + a CPU intensity hint |
| wapp → host | `task.list` | Ask the host for the current snapshot |
| host → wapp | `task.tick` | Fire of a periodic task — the wapp does its work and replies via outbox messages |
| host → wapp | `task.paused` | The user (or host) paused this task — stop emitting work |
| host → wapp | `task.resumed` | Resume normal scheduling |
| host → wapp | `task.throttled` | The host is rate-limiting this task; tighten the next tick |
| host → wapp | `task.killed` | The host gave up on this task; do final cleanup |

Registration shape (`task.register`):

```json
{
  "type": "task.register",
  "id":          "<wapp-scoped slug>",
  "name":        "Tile prefetch",
  "description": "Fetching tiles for visible viewport",
  "priority":    "critical" | "normal" | "low",
  "type":        "periodic" | "isolate" | "oneshot",
  "interval_ms": 5000,
  "boot":        "always" | "on-demand" | "lazy",
  "boot_order":  10,
  "max_cpu_pct": 25,
  "max_runtime_ms": 30000
}
```

The reference engine maps these onto `MonitoredTask` registered
with `TaskMonitorService` (see
`lib/services/task_monitor_service.dart` and
`lib/models/monitored_task.dart`). The host UI lists every task
with status (`idle`, `running`, `paused`, `error`), last duration,
run count, success/fail counters, and last error message.

### 8.1 Lifecycle

A managed task progresses through:

```
idle ── tick ──▶ running ──▶ idle (success)
                              │
                              └──▶ error  ── fixed ──▶ idle
                                          │
                                          └─ exhausted ──▶ killed
   ▲                                                        │
   │                                                        │
   pause/resume ◀──── any state ────────────────────────────┘
```

Engine duties:
- Drive periodic tasks at their declared `interval_ms` by sending
  `task.tick` to the wapp's inbox
- Time each tick; record `lastDuration` and update `runCount`
- Stop firing ticks for paused tasks; resume cleanly on resume
- Mirror status changes on the host event bus
  (`TaskStatusChangedEvent`) so the UI updates without polling
- Catch and record exceptions as `task.error` then transition the
  task to `error` state

### 8.2 Startup tasks (boot orchestration)

A wapp can mark a task as **startup-critical**. The engine runs
those during host boot before the launcher grid renders.

| `boot` | Behaviour |
|---|---|
| `always` | Run on every host start; failures are surfaced but don't block boot |
| `on-demand` | Run only when the wapp's UI is opened |
| `lazy` | Run after the launcher is interactive (idle queue) |

`boot_order` is a small integer; lower = earlier. Tasks at the
same order start in parallel (see iwi's `BootOrchestrator` with
`BootStart.sequential | BootStart.parallel`).

Engine duties:
- Sort startup tasks by `(priority desc, boot_order asc)` and run
  them in that order during host init
- Block the launcher's first paint only on `priority: critical`
  startup tasks; everything else runs after first paint
- Surface startup failures as `WappCrashedEvent` with
  `phase: "startup"` so the launcher can show a recovery banner
- Persist the result of the last boot (success/failed/skipped)
  so the user can inspect why a wapp didn't come up

### 8.3 Resource tracking and throttling

The task monitor records, per task:

- **Last duration** — wall time of the most recent tick
- **CPU intensity** — declared `max_cpu_pct` capped against
  observed `lastDuration / interval_ms` ratio
- **Memory growth** — delta in `hal_heap_free()` between ticks
  (engine-side approximation, not perfect)
- **Outbox volume** — messages emitted per tick, throttled when
  excessive

The engine **throttles a misbehaving wapp** when any of:

- `lastDuration > interval_ms` for N consecutive ticks (the wapp
  can't keep up — back off)
- Cumulative CPU% over a sliding window exceeds `max_cpu_pct`
- Outbox flood (> 500 messages/sec sustained)
- Memory growth > 64 KB/tick for N ticks

Throttling actions, in escalation order:
1. Increase the effective tick interval (additive backoff)
2. Send `task.throttled` so the wapp can shed work voluntarily
3. Pause the task entirely (`task.paused`)
4. Kill the task (`task.killed`) — only `priority: low` tasks
   reach this step automatically; `normal` and `critical` need
   user confirmation

The user can also **manually pause non-critical work** through the
task monitor UI. `pauseAllNonCritical()` is the canonical hook —
it skips tasks with `priority: critical` and pauses everything
else, useful when battery is low or a user is recording video on
mobile.

### 8.4 Task monitor surface

The host exposes the task list as a `$type="tasks"` GeoUI group
that any wapp can render (typically a system-level "Tasks" wapp).
Engine duties for that group:

- List every task (across all loaded wapps + host services)
  grouped by `serviceName`
- Show status pill, last run, last duration, success/fail counts
- Per-row actions: Pause, Resume, Stop, Restart, View errors
- Bulk actions: Pause all non-critical, Resume all
- Cross-wapp event on every status change so the rendering UI
  updates live

Reference: iwi's tasks wapp + `_buildTasksScreen` in the
prototype renderer; the integrated host stubs the group for now
and will land the same renderer in a follow-up stage.

---

## 9. Connections

External transports are exposed as polling APIs because the WASM
side cannot block. The engine handles the real I/O and queues
results.

### 7.1 HTTP (`hal.http`)

```c
int32_t hal_http_request      (method, url, url_len, body, body_len);
int32_t hal_http_poll         (request_id);   // 0=pending 1=done -1=err
int32_t hal_http_read_response(request_id, buf, buf_len);
int32_t hal_http_status       (request_id);
void    hal_http_free         (request_id);
```

### 7.1a Host subprocess (`hal.process`)

```c
int32_t  hal_process_exec        (argv_json, argv_len, cwd, cwd_len);
int32_t  hal_process_poll        (handle);   // 0=running 1=exited -1=err
int32_t  hal_process_exit_code   (handle);
uint32_t hal_process_read_stdout (handle, buf, buf_len);
uint32_t hal_process_read_stderr (handle, buf, buf_len);
void     hal_process_free        (handle);
```

Generic primitive for spawning a host process. `argv_json` is a JSON
array of strings (e.g. `["/usr/bin/clang","-O2","-o","/tmp/a.wasm",
"/tmp/main.c"]`). `cwd` is an absolute working directory or empty
for the host's CWD. argv\[0\] must be an absolute path — the host's
PATH is **not** searched. There is **no sandbox and no allow list**;
wapps run with the same privileges as the XPRS process.

Lifecycle mirrors `hal.http`: `exec` returns a handle immediately
while the process spins up in the background; the wapp polls each
tick, drains stdout/stderr buffers as they fill, and calls
`process_free` when finished. Calling `process_free` on a still
running process best-effort kills it and discards any unread output.

Stub on platforms where `dart:io.Process` is unavailable (web):
`hal_process_exec` returns `-1` and the wapp's poll loop never sees
a handle to advance.

### 7.2 LoRa (`hal.lora`)

```c
int32_t  hal_lora_available_hw(void);
int32_t  hal_lora_send        (data, data_len);
uint32_t hal_lora_available   (void);
uint32_t hal_lora_recv        (buf, buf_len);
```

Stub on platforms without hardware — `hal_lora_available_hw`
returns `0`, all other calls return zero or `-1`.

### 7.3 Bluetooth LE (`hal.ble`)

```c
int32_t  hal_ble_scan_start    (void);
void     hal_ble_scan_stop     (void);
uint32_t hal_ble_scan_read     (buf, buf_len);
int32_t  hal_ble_advertise     (data, data_len);
void     hal_ble_advertise_stop(void);
```

### 7.4 Cross-module RPC (`hal.lib`)

```c
int32_t hal_lib_call(lib_id, lib_id_len, fn_name, fn_name_len,
                     args, args_len, result, result_len);
```

A synchronous, return-value-by-value variant of the functionality
broker. Used when a caller wants a strict request/response with
named arguments, not a fire-and-forget event.

---

## 10. Notifications

Wapps surface user-visible notifications by sending a `notify`
message to the host:

```json
{
  "type": "notify",
  "level": "info|success|warning|error",
  "title": "...",
  "body":  "...",
  "scope": "app|system|both",
  "tag":   "<optional dedup key>"
}
```

The engine must:
- Route to the host's notification service (in-app banner, system
  tray, OS toast — depending on `scope`)
- Tag every notification with the source wapp's name so the user
  can identify it
- De-duplicate by `tag` when present

---

## 11. Sensors and hardware

Read-only access to platform sensors. All return `INT32_MIN` when
the sensor isn't available so wapps can degrade gracefully.

```c
int32_t hal_sensor_temperature(void);   // centidegrees C
int32_t hal_sensor_humidity   (void);   // centipercent
int32_t hal_sensor_battery    (void);   // millivolts
int32_t hal_sensor_gps_lat    (void);   // latitude × 1e7
int32_t hal_sensor_gps_lon    (void);   // longitude × 1e7
```

GPIO and display APIs follow the same pattern (no-op stubs on
desktop, real on ESP32).

---

## 12. Location

The HAL pair `hal_sensor_gps_lat()` / `hal_sensor_gps_lon()`
gives a **fast, cached** read of the device's last known fix
(latitude/longitude scaled by 1e7). Both return `INT32_MIN` when
no fix is available so callers can degrade gracefully.

```c
int32_t lat_e7 = hal_sensor_gps_lat();   // 377749000 → 37.7749
int32_t lon_e7 = hal_sensor_gps_lon();   // -122419400 → -122.4194
if (lat_e7 == INT32_MIN || lon_e7 == INT32_MIN) {
    // No fix available — fall back or request one (see below).
}
```

Use these for **passive readouts**: rendering "where am I" on a
map screen, stamping an entry, deciding whether the user is at
home. They never wake hardware, never spin the GPS chip, and
never block.

### 12.1 Requesting a fix with an accuracy hint

When a wapp needs **a fresh fix at a specific quality**, it goes
through the message channel so the host can pick the right power
mode (battery-saver Wi-Fi/cell triangulation vs full GPS) and
prompt the user when permissions need to be granted.

Outbound (`location.request`):

```json
{
  "type":      "location.request",
  "req_id":    "<opaque>",
  "accuracy":  "coarse" | "balanced" | "fine" | "best",
  "max_age_ms": 60000,
  "timeout_ms": 15000,
  "allow_cached": true
}
```

Accuracy levels — the engine maps each to a concrete platform
mode:

| Hint | Typical source | Power | Expected uncertainty |
|---|---|---|---|
| `coarse` | Cell / IP geolocation | minimal | 1–10 km |
| `balanced` | Wi-Fi + cell | low | 50–500 m |
| `fine` | GPS, fused with Wi-Fi | medium | 5–50 m |
| `best` | GPS only, wait for SBAS | high | < 5 m |

`max_age_ms` lets the engine return a cached fix if it's recent
enough, skipping a fresh acquisition. `timeout_ms` caps the wait;
when it expires the engine answers with whatever fix it managed
to obtain, or an error.

Reply (`location.response`):

```json
{
  "type":            "location.response",
  "req_id":          "<opaque>",
  "lat":             37.7749,
  "lon":             -122.4194,
  "altitude_m":      52.3,
  "accuracy_m":      8.5,
  "altitude_accuracy_m": 12.0,
  "speed_mps":       1.2,
  "heading_deg":     90.0,
  "fix_at":          1714370012,
  "source":          "gps" | "wifi" | "cell" | "ip" | "cached",
  "cached":          false
}
```

When the engine can't satisfy the request, it returns an error
form so callers don't have to special-case missing fields:

```json
{
  "type":   "location.response",
  "req_id": "<opaque>",
  "error":  "permission_denied" | "no_provider" | "timeout" | "disabled"
}
```

### 12.2 Continuous updates

For navigation, tracking, and other use-cases that need a live
stream rather than a one-shot fix, wapps subscribe to position
events:

```json
{
  "type":         "location.subscribe",
  "req_id":       "<wapp-scoped slug>",
  "accuracy":     "balanced",
  "min_interval_ms": 5000,
  "min_distance_m":  10
}
```

Updates arrive as `location.update` messages with the same shape
as `location.response`. The engine batches updates so a wapp
that asks for `min_interval_ms: 5000` won't receive faster ones
even when a more demanding wapp is also subscribed.

Unsubscribe with `{ "type": "location.unsubscribe", "req_id": "..." }`.

The engine **shares a single hardware fix across all subscribed
wapps** — it never wakes the GPS twice for the same instant.
Wapps with a tighter accuracy hint cause the engine to upgrade
the active mode; when no wapp needs `fine` or `best` anymore the
engine downgrades to save power.

### 12.3 Permissions and privacy

`location.request` is the first wapp action that can fail with
`permission_denied`. Engine duties:

- Check the host's location permission state before talking to
  hardware. If unset, prompt the user with the requesting wapp's
  name and the requested accuracy level.
- Persist the user's choice per wapp; never re-prompt once
  granted or denied (the user can flip it from settings).
- Honour an "all wapps off" master switch independent of
  per-wapp grants.
- Refuse to elevate a wapp's accuracy mid-stream — if it
  subscribed at `coarse`, a later `fine` request must trigger a
  re-prompt.
- Stamp every emitted `location.update` with the source mode so
  a wapp can decide whether to trust the fix for, e.g., legal
  purposes.

### 12.4 Manifest declaration

A wapp that uses location declares it under `requires.hal`:

```json
"requires": {
  "hal": ["log", "msg", "sensor.location"],
  "events": []
}
```

The engine must reject loading a wapp that calls
`location.request` without `sensor.location` in `requires.hal` —
this gives the user a chance to see, in the install dialog,
exactly which wapps will be allowed near their position before
they grant permission.

---

## 13. Time and platform

```c
uint64_t hal_time_ms   (void);   // monotonic ms since host start
uint64_t hal_time_epoch(void);   // unix seconds (0 if no RTC)
void     hal_yield     (void);   // cooperative multitasking
uint32_t hal_platform  (buf, buf_len);   // "esp32"|"linux-desktop"|...
uint32_t hal_heap_free (void);   // free heap bytes available to module
uint32_t hal_i18n_get  (key, key_len, out, out_cap);  // lang/<locale>.json
```

The engine must clamp `hal_time_ms` to the wapp's lifetime (not
absolute) so wapps that persist KV across runs see monotonic
behavior.

---

## 14. UI surface

GeoUI is the wapp's UI dialect — JSON files under `screens/` that
describe screens, fields, actions, and groups. The engine parses
them through a renderer (see `lib/geoui/`) that produces native
widgets. Wapps don't paint pixels; they declare structure and
react to messages.

Group `$type` values reserved for host-side rendering:

| `$type` | Host renderer |
|---|---|
| `map` | Tiled map (FlutterMap or equivalent) |
| `menu` | Popup menu (icon button → list of action children) |
| `header-actions` | Hoists icon-actions / menus into the host AppBar |
| `cards` | Generic data-driven list/grid of cards (icon + text + actions) — the wapp pushes rows via `ui.data` |
| `output` | Wapp store catalog list (deprecated — use `cards` instead) |
| `sources` | Wapp store repository manager |
| `projects` | App Creator project picker |
| `tasks` | Task monitor |
| `ui-editor` | App Creator visual UI editor |
| `translations` | App Creator translations editor |
| `functionalities` | Functionality registry browser |
| `log` | Append-only log view (also as a field type) |
| `code` | Code editor field |
| `icon` | Icon picker field |

The engine treats unknown types as opaque containers and renders
their children as standard fields/actions.

### 14.1 `$type="menu"`

A `<group $type="menu">` collapses into a single icon button. When
the user taps the button, a popup opens with one entry per
`action` child of the group. Selecting an entry dispatches the
standard `{"type":"action","action":"<name>"}` outbox message —
the wapp handles those today, so adopting a menu requires no
message-handling changes.

Decls (all optional):

| decl   | default | meaning                                                |
|--------|---------|--------------------------------------------------------|
| `icon` | `menu`  | Material icon name for the trigger button              |
| `tip`  | `Menu`  | Tooltip on the trigger button (i18n-resolved)          |
| `name` | (none)  | Optional label rendered next to the icon               |

Engines should accept a small whitelist of icon names (`menu`,
`more_vert`, `more_horiz`, `settings`, `add`, `edit`, `tune`,
`filter_list`, …) and fall back to `menu` for unknown values, so
wapps can't reach into arbitrary Material icons by surprise.

Position is the host's concern, not the menu's: when the menu is
declared as a child of another host-rendered group (e.g. inside a
`$type="video"` block), the host renderer for that parent decides
where to place the trigger (typically a corner overlay). When
declared at screen / inline level, the menu flows like any other
group. To pin the trigger into the host AppBar instead, wrap the
menu in a `$type="header-actions"` group (§14.2).

### 14.2 `$type="header-actions"`

A `<group $type="header-actions">` declared at the top level of a
screen is **not rendered inside the screen body**. Its direct
children are hoisted into the host's AppBar `actions` slot — i.e.
they appear on the same line as the wapp's title, right-aligned,
in the order declared. This is the wapp's "title bar action area"
for screen-scoped icons (open file, refresh, share, …).

Two child kinds are supported, and a wapp can mix many of either:

| child kind                | renders as                              |
|---------------------------|-----------------------------------------|
| `<action icon="…">`       | Single `IconButton`, fires that action  |
| `<group $type="menu">`    | Popup menu (same as §14.1)              |

Action decls inside `header-actions`:

| decl   | required | meaning                                                  |
|--------|----------|----------------------------------------------------------|
| `name` | yes      | Action name dispatched on press                          |
| `icon` | yes      | Material icon name (engine whitelist)                    |
| `tip`  | no       | Tooltip; falls back to `label`, then to `name`           |
| `label`| no       | Used as tooltip when `tip` is absent (no inline label)   |

Example — three icons in the title bar, the rightmost being a
popup with two extra entries:

```json
{
  "$": "screen",
  "name": "Player",
  "children": [
    {
      "$": "group",
      "$type": "header-actions",
      "children": [
        { "$": "action", "name": "refresh", "icon": "refresh", "tip": "Refresh" },
        { "$": "action", "name": "share",   "icon": "share",   "tip": "Share" },
        {
          "$": "group", "$type": "menu", "icon": "more_vert", "tip": "More",
          "children": [
            { "$": "action", "name": "pick_video",    "label": "Open file…" },
            { "$": "action", "name": "pick_subtitle", "label": "Subtitle…" }
          ]
        }
      ]
    },
    { "$": "group", "$type": "video", "fit": "contain" }
  ]
}
```

Multiple screens can each declare their own `header-actions` —
the host swaps the AppBar contents on tab change so each screen
gets its own action set. A screen with no `header-actions` group
shows just the title (plus engine-injected affordances like the
dev-mode Reload button).

Icon names follow the same whitelist as §14.1; unknown values
fall back to `Icons.menu`. The `<action>` elements inside
`header-actions` are icon-only — `label` is used as the fallback
tooltip but is not rendered next to the icon, since AppBar real
estate is tight.

### 14.3 `$type="cards"`

A generic data-driven list/grid of cards. The wapp pushes rows
via `ui.data`; the host renders each row as a card with icon,
title, subtitle, description, chips and action buttons. The host
knows nothing about what the rows mean — wapps own the data and
the visual hints.

UI declaration:

```json
{
  "$": "group",
  "name": "catalog",
  "$type": "cards",
  "layout": "list",
  "empty": "No items yet."
}
```

`name` (required) — the data target. The wapp sends `ui.data`
messages with `target` matching this name to populate the group.

`layout` (optional, default `"list"`) — `"list"` for a vertical
ListView, `"grid"` for a responsive GridView (2/3/4 columns by
viewport width). The wapp can flip the value at runtime via
`ui.attr`:
```
{"type":"ui.attr","target":"<name>","attr":"layout","value":"grid"}
```

`empty` (optional) — text shown when no items have been pushed
yet.

Wapp-pushed item schema (every field optional):

```json
{
  "type": "ui.data",
  "target": "<group-name>",
  "items": [
    {
      "id":          "<stable identifier>",
      "icon_path":   "wapp:<slug>"   ⏐ "/abs/path.svg" ⏐ "rel/path.png",
      "title":       "<heading>",
      "subtitle":    "<small line under title>",
      "description": "<body, max ~2 lines>",
      "chips":       [{"label":"…","icon":"folder|cloud|…"}],
      "actions":     [
        {"name":"<action-id>", "label":"<button>", "icon":"<icon>",
         "disabled": false}
      ]
    }
  ]
}
```

`icon_path` resolution:
- `wapp:<slug>` — host resolves to the named installed wapp's
  `manifest.icon` SVG. Useful for catalogs of other wapps.
- Anything else — taken as a filesystem path. Absolute paths are
  used as-is; relative paths are resolved against the wapp's
  package base.

Tapping an action button emits the standard
`{"type":"action","action":"<name>"}` message the wapp already
handles. Action names can use any prefix scheme the wapp likes
(e.g. `"install:maps"`, `"delete:43"`); the wapp dispatches in
`module_handle_event`.

---

## 15. Identity and signing

The engine reads the active profile's NOSTR identity (`npub`,
`nsec`) from the host's profile service and uses it to:
- Sign every wapp the user installs (write a NIP-78 `kind:30078`
  event into `signature.json` next to the package)
- Authenticate `widget.request` calls when the target functionality
  requires it
- Stamp social entries (reactions, comments) inside the wapp's
  `social.sqlite3`

A wapp never sees the `nsec` directly. It asks the host to sign on
its behalf via the `sign.schnorr` outbox message (see §15.1 below).

### 15.1 Outbox protocol — profile, identity, sign

Wapps that need encrypted-profile-aware file access, the active
identity, or NOSTR signing do not get a new HAL function — they
send an outbox message and receive the response on `module_handle_event`.
Going through the outbox keeps the WASM/Rust ABI synchronous while
the host side stays free to await `ProfileStorage`, prompt for
credentials, etc.

Permission tokens declared in `manifest.permissions` gate every
sensitive call:

| Token                    | Unlocks                                     |
|--------------------------|---------------------------------------------|
| `collection.<id>.read`   | `profile.read/list/exists/size` under `collections/<id>/...` |
| `collection.<id>.write`  | adds `profile.write/mkdir/remove` under that root            |
| `identity.read`          | `identity.get` (callsign + npub)            |
| `sign`                   | `sign.schnorr` over a 32-byte digest        |

Path scoping: `path` is relative to the granted root. The host
prepends `collections/<id>/`. Anything that escapes the root (`..`,
absolute paths) is rejected with status `-1`. Reads and writes flow
through `AppService().profileStorage`, so encrypted profiles work
identically.

Every message carries a wapp-allocated `req_id`; the host echoes it
in the response so the wapp can correlate outstanding requests.

#### Request shapes (wapp → host)

```json
{ "type": "profile.read",   "req_id": 1, "scope": "collection.forum",
  "path": "<section>/thread-foo.txt" }

{ "type": "profile.write",  "req_id": 2, "scope": "collection.forum",
  "path": "<section>/thread-foo.txt", "mode": "write|append",
  "data": "<utf-8 contents>" }

{ "type": "profile.list",   "req_id": 3, "scope": "collection.forum",
  "path": "<section>" }

{ "type": "profile.exists", "req_id": 4, "scope": "collection.forum",
  "path": "<section>/thread-foo.txt" }

{ "type": "profile.size",   "req_id": 5, "scope": "collection.forum",
  "path": "<section>/thread-foo.txt" }

{ "type": "profile.mkdir",  "req_id": 6, "scope": "collection.forum",
  "path": "<section>" }

{ "type": "profile.remove", "req_id": 7, "scope": "collection.forum",
  "path": "<section>/thread-foo.txt" }

{ "type": "identity.get",   "req_id": 8 }

{ "type": "sign.schnorr",   "req_id": 9,
  "message_hex": "<64 hex chars = SHA-256 digest>" }
```

#### Response shapes (host → wapp)

Each response uses `<request-type>.response` and carries `req_id` +
`status`. `status: 0` is success; non-zero is failure with a human
`error` string.

| status | Meaning                                                  |
|--------|----------------------------------------------------------|
| 0      | OK                                                       |
| -1     | Permission denied / missing token / invalid scope        |
| -2     | Path not found                                           |
| -3     | Other failure (storage error, bad input, no profile yet) |

```json
{ "type": "profile.read.response",   "req_id": 1, "status": 0,
  "data": "<file contents>" }

{ "type": "profile.write.response",  "req_id": 2, "status": 0 }

{ "type": "profile.list.response",   "req_id": 3, "status": 0,
  "entries": [ {"name":"foo","is_dir":true},
               {"name":"thread-x.txt","is_dir":false,"size":1234} ] }

{ "type": "profile.exists.response", "req_id": 4, "status": 0,
  "exists": true,  "is_dir": false }

{ "type": "profile.size.response",   "req_id": 5, "status": 0,
  "size":   1234,  "exists": true }

{ "type": "profile.mkdir.response",  "req_id": 6, "status": 0 }
{ "type": "profile.remove.response", "req_id": 7, "status": 0 }

{ "type": "identity.get.response",   "req_id": 8, "status": 0,
  "callsign": "X1SU86", "npub": "npub1..." }

{ "type": "sign.schnorr.response",   "req_id": 9, "status": 0,
  "signature_hex": "<128 hex chars = 64-byte BIP-340 sig>" }
```

#### Engine notes

- The host validates `manifest.permissions` *before* dispatching the
  request. A missing token short-circuits to `status: -1` without
  touching storage.
- `sign.schnorr` requires the active profile to carry an `nsec`. The
  nsec never leaves the host; the wapp only ever supplies a 32-byte
  digest and receives a 64-byte signature.
- `profile.write` defaults to `mode: "write"` (truncate). Use
  `"append"` for race-free reply writes when two posts could land in
  the same tick.
- Engines that want a generic viewer wapp can omit
  `collection.<id>.write`; the read-only handlers (`profile.read`,
  `list`, `exists`, `size`) still work.

---

## 16. Manifest contract

Every `.wapp` must contain a top-level `manifest.json`:

```json
{
  "id":              "tools.xprs.maps",
  "version":         "1.0.0",
  "kind":            "app|system|addon",
  "title":           "Maps",
  "description":     "One-line explanation, used in list views.",
  "summary":         "Paragraph-long explanation for detail / about views.",
  "icon":            "media/icons/<name>.svg",
  "entry_ui":        "screens/home.ui.json",
  "tick_interval_ms": 5000,
  "permissions":     ["network", "storage",
                      "collection.<id>.read", "collection.<id>.write",
                      "identity.read", "sign", "tests.invoke"],
  "provides": {
    "functionalities": [
      "text.greet",
      { "id": "tile_provider.osm",
        "endpoints": [{ "name": "..." }] }
    ]
  },
  "requires": {
    "hal":    ["log", "kv", "msg", "i18n_get"],
    "events": ["geo.position"],
    "libraries": []
  }
}
```

The engine must:
- Reject a wapp whose `requires.hal` contains capabilities the
  current platform can't satisfy
- Surface `requires.events` so the host knows what topics to
  pre-subscribe the wapp to
- Carry `permissions` through to the user-facing install dialog
  (Stage 3)
- Accept the scoped permission tokens defined in §15.1
  (`collection.<id>.read`, `collection.<id>.write`, `identity.read`,
  `sign`) and `tests.invoke` (§20). Unrecognised tokens are kept
  verbatim — newer engines may add new tokens without breaking
  older catalogs.

### 16.1 Display fields: title vs description vs summary

Three text fields describe the wapp at different verbosity levels.
They are independent — pick the right field for the right slot.

| Field         | Length        | Where it's used                                                        |
|---------------|---------------|------------------------------------------------------------------------|
| `title`       | 1–3 words     | Home grid label, AppBar title, "Open with…" picker primary line.       |
| `description` | one line      | Catalog row subtitle, file-handler subtitles, list-view explanation.   |
| `summary`     | one paragraph | Detail / about pages, store cards, long-form previews. Markdown ok.    |

**Legacy migration.** Older manifests had only `description`
(holding the launcher label) and `summary` (the long form). The
engine treats the absence of `title` as a legacy schema and uses
`description` as the title with no separate one-liner. The catalog
emitter (`build-archive.sh` → `index.json`) applies the same rule
when packaging old wapps.

---

## 17. Package layout

```
my-wapp.wapp (zip)
├── manifest.json
├── app.wasm                   ← compiled WASM module
├── tests.wasm                 ← optional, runnable test suite (see §20)
├── signature.json             ← optional NIP-78 NostrEvent
├── permissions.json           ← optional NDF access control
├── social.sqlite3             ← optional reactions+comments DB
├── screens/
│   └── home.ui.json
├── media/
│   └── icons/<name>.svg
├── lang/
│   ├── en.json
│   └── <locale>.json
├── tests/                     ← optional, source for tests.wasm (folder mode)
│   └── test_*.c
└── store/
    ├── description.json       ← multi-lingual store metadata
    └── screenshots/           ← bundled images
```

`app.wasm` is mandatory; everything else is optional. The engine
must extract the package atomically — partial extracts must leave
no `manifest.json` so the launcher's idempotent install flow
treats the wapp as not yet installed.

### 17.1 Folder vs ZIP

A wapp can be hosted in either form, and the engine treats both
identically once installed:

- **Open folder** — the same tree shown above, on disk, with no
  ZIP wrapper. This is the friendly form for human authors during
  development: edit `main.c`, `screens/home.ui.json`, or any other
  file in place; no repackaging step.
- **`.wapp` ZIP** — the same tree compressed into a single
  archive. This is the distribution form: a wapp store catalog
  serves it from a URL, an "Open with…" picker hands it over as
  bytes, etc.

The runtime archive at `<baseDir>/wapps/<wappId>/` is *always* an
open folder regardless of where the wapp came from. ZIP origins are
expanded once at install time and the runtime never reads the ZIP
again — the open folder is the single source of truth the engine
loads from on every boot.

### 17.2 `source.json`

Every installed wapp carries a `source.json` next to its
`manifest.json` recording where the install came from:

```json
{
  "version": 1,
  "type":    "path" | "file" | "url" | "asset",
  "value":   "<absolute path | http URL | flutter asset path>"
}
```

| `type`  | `value` is…                       | Install action                  |
|---------|-----------------------------------|---------------------------------|
| `path`  | absolute folder path              | recursively copy folder verbatim |
| `file`  | absolute path to a `.wapp` ZIP    | re-read ZIP, overwrite archive  |
| `url`   | `http(s)` URL of a `.wapp` ZIP    | re-download, overwrite archive  |
| `asset` | Flutter asset path (e.g. `assets/install.wapp`) | re-extract from bundle, overwrite archive |

The engine writes `source.json` on every successful install — fresh
install, store install, picker install, asset install — and rewrites
it on every successful reinstall. Wapps must not modify it.

### 17.3 Reload

The wapp page exposes a Reload button (visible when the user has
the global Wapp Store debug toggle on). Reload reads `source.json`
and re-executes the matching install action above:

```
folder source  →  copy all folder contents into the archive
ZIP source     →  re-read the .wapp, overwrite the archive
URL source     →  re-fetch the .wapp, overwrite the archive
asset source   →  re-extract the asset, overwrite the archive
```

Then the engine reboots from the refreshed archive. There is no
"sibling source tree" probe, no path guessing, no automatic
repackaging — the recorded source is the only thing consulted.

For an open-folder install this gives the natural human dev loop:

```
edit /path/to/wapps/<wappId>/<any file>
   ↓
press Reload in XPRS
   ↓
see the change
```

No build step is required between editing and Reload as long as
`app.wasm` is up to date — typically `make` in the wapp folder
when `main.c` changed; for pure JSON / media edits, nothing.

---

## 18. File associations

A wapp can register itself as a candidate handler for one or more
file types so the host's "Open with…" picker, the file manager,
and OS-level URL/MIME associations can route documents to it. The
mechanism mirrors the desktop convention: a wapp declares the
extensions / MIME types it accepts; the host indexes those
declarations across all installed wapps; when the user opens a
file, the host shows the matching list and remembers their
choice.

### 18.1 Manifest declaration

Handlers live under `provides.file_handlers` — an array, so a
single wapp can register for multiple unrelated file types (e.g.
an audio player for `mp3`/`ogg` *and* `m3u` playlists):

```json
"provides": {
  "functionalities": [],
  "file_handlers": [
    {
      "extensions": ["mp3", "ogg", "wav", "flac"],
      "mime":       ["audio/mpeg", "audio/ogg", "audio/wav"],
      "title":      "Play",
      "modes":      ["view"]
    },
    {
      "extensions": ["m3u", "m3u8"],
      "mime":       ["audio/x-mpegurl"],
      "title":      "Open playlist",
      "modes":      ["view", "edit"]
    }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `extensions` | one of ext/mime | Lowercased list, no leading dot. The literal `"*"` is a catch-all (lowest priority). |
| `mime` | one of ext/mime | MIME types. Wildcards at the type level are honoured (e.g. `"audio/*"`). `"*/*"` is a catch-all. |
| `title` | optional | Verb shown in the picker — "Play", "Edit", "Preview". Falls back to the wapp's display name. |
| `modes` | optional | Reserved values: `view` (default), `edit`. Other strings pass through unchanged. |

### 18.2 Lookup contract (host side)

The host runs a registry that scans installed manifests and
answers two questions:

- *"Which wapps can open `*.mp3`?"* — returns the list, ordered
  exact-extension first, then MIME-only, then catch-all. Empty
  when no handler claims the type.
- *"Which wapp did the user pick last time?"* — persisted
  per-extension in the active profile so the picker can default
  to the previous choice.

The registry is best invalidated on `WappLoadedEvent` /
`WappUnloadedEvent`; engines that hot-install wapps must drop the
cache so the next lookup sees the new handlers.

### 18.3 Launch protocol — `file.open`

Once the user has chosen a handler, the host boots the wapp the
usual way (load WASM → `module_init` → drain outbox) **and then**
sends a single `file.open` message to the freshly-booted wapp:

```json
{
  "type":      "file.open",
  "path":      "/storage/.../track.mp3",
  "name":      "track.mp3",
  "extension": "mp3",
  "mime":      "audio/mpeg",
  "mode":      "view",
  "size":      4523289
}
```

The wapp reads it from its inbox during the next
`module_handle_event`, decides what to do (open the audio,
refuse `mode: edit` if it didn't declare it, etc.), and may
respond with normal outbox messages — `notify`, `ui.append`, or
the bytes-delivery round-trip in §18.4.

`size = -1` means "unknown" (e.g. streaming source). The wapp
must not assume the file is reachable via `path` directly — on
encrypted profiles the path is virtual and the wapp must go
through the host file HAL or a `file.read_request` round-trip.

A typical wapp-side handler (C, abridged):

```c
void module_handle_event(void) {
    char buf[1024];
    while (hal_msg_available() > 0) {
        int n = hal_msg_recv(buf, sizeof(buf));
        if (n <= 0) break;
        // Cheap discriminator: the message starts with
        // {"type":"file.open" — full JSON parse only when needed.
        if (strstr(buf, "\"type\":\"file.open\"")) {
            on_file_open(buf, n);
        } else if (strstr(buf, "\"type\":\"file.read_response\"")) {
            on_read_response(buf, n);
        }
    }
}
```

The wapp keeps the `path` from `file.open` opaque and uses it in
subsequent `file.read_request` / `file.save` messages — never
parses it as a filesystem path itself.

### 18.4 Reading and writing bytes

The `file.open` message hands the wapp a *handle* (the `path`
field), not the bytes. To read the contents, the wapp issues a
`file.read_request`; the host streams the file in chunks back as
`file.read_response`. This keeps the wapp inside its sandbox even
for files outside its package or per-profile data folder, and
gives the engine a place to enforce range/quota limits.

Wapp → host:

```json
{
  "type":   "file.read_request",
  "req_id": "<opaque>",
  "path":   "<token from file.open>",
  "offset": 0,
  "length": 65536
}
```

Host → wapp (one or more messages, in order):

```json
{
  "type":   "file.read_response",
  "req_id": "<opaque>",
  "offset": 0,
  "bytes_b64": "...base64...",
  "eof":    false
}
```

The final chunk has `eof: true`. If the request fails (path not
granted, I/O error, quota exceeded), the host returns a single
response with `error` instead of `bytes_b64`:

```json
{ "type": "file.read_response",
  "req_id": "<opaque>",
  "error":  "denied" | "not_found" | "io" | "quota" }
```

For `mode: edit` handlers the symmetric save path is
`file.save` (wapp → host) acknowledged by `file.saved`:

```json
{ "type":   "file.save",
  "req_id": "<opaque>",
  "path":   "<token from file.open>",
  "bytes_b64": "...base64..." }
```

```json
{ "type":   "file.saved",
  "req_id": "<opaque>",
  "size":   <bytes written>,
  "error":  null | "denied" | "io" | "quota" }
```

The host **must not** allow `file.save` to write back to a token
the user opened in `view` mode, even if the wapp asks. The mode
chosen at launch time is binding for the whole session.

### 18.5 Permissions and trust

File associations cross a privilege boundary: opening a file the
user picked is implicit consent to read it, but the wapp does not
get blanket filesystem access. The engine duties:

- Only deliver `file.open` for files the user explicitly chose
  (never inject one off a directory scan or a sibling wapp's
  request).
- Pass `path` as an opaque token; the host file HAL must enforce
  that the wapp can read only that specific path during this
  session.
- Honour the manifest's declared `modes` — refuse to deliver a
  `mode: edit` message to a wapp that only declared `view`.
- Persist the user's "always open with…" choice per extension,
  not per individual file path.

### 18.6 Engine checklist (file associations)

- [ ] Parse `provides.file_handlers` from the manifest at install
      time and reject malformed entries (no extensions and no
      mime).
- [ ] Maintain a lookup index keyed by lowercase extension and
      MIME type; expose at minimum `lookup(extension, mime, mode)`.
- [ ] Persist a per-profile "default handler per extension" map
      and surface it in the picker UI.
- [ ] Boot the chosen wapp and dispatch a single `file.open`
      message after `module_init` finishes.
- [ ] Honour `file.read_request` against the granted token only —
      reject paths the user did not pick this session.
- [ ] Refuse `file.save` for tokens granted in `view` mode.
- [ ] Drop the cache on `WappLoadedEvent` / `WappUnloadedEvent`.

---

## 19. Engine implementation checklist

A minimum-viable engine implementation must:

- [ ] Load and instantiate `app.wasm` with all HAL imports wired
- [ ] Run `module_init` and surface failures as `WappCrashedEvent`
- [ ] Maintain a per-wapp KV scoped to the active profile
- [ ] Drain `hal_msg_send` after every tick + handle_event call
- [ ] Dispatch reserved message types (notify, widget.request,
      wapp.fetch_index, wapp.install)
- [ ] Run a tick timer at `manifest.tick_interval_ms`
- [ ] Render screens via the GeoUI renderer
- [ ] Honour `requires.hal` at load time
- [ ] Fire `WappLoadedEvent` / `WappUnloadedEvent` on the host
      event bus
- [ ] Index `provides.file_handlers` so the host can answer
      "which wapps open this filetype?" and dispatch `file.open`
      messages on the file-association launch path

A full engine additionally provides the functionality broker, the
cross-wapp event bus, signing/verification, and the social store.

---

## 20. Unit tests

A wapp may ship a runnable test suite alongside the production
build. The goal is a tight authoring loop: edit code, hit
**Run tests** in the App Creator wapp, see green or red within
seconds — without leaving the host.

Tests are **opt-in**. A wapp without a `tests/` folder or a
shipped `tests.wasm` is fully valid; the engine simply reports
"no tests" when asked to run them.

### 20.1 Source layout

Tests live in a top-level `tests/` directory next to `main.c`:

```
my-wapp/
├── main.c
├── manifest.json
├── tests/
│   ├── test_parse.c
│   └── test_render.c
└── ...
```

Each `test_*.c` file declares one or more cases using the SDK
header `wapps/sdk/wapp_test.h`:

```c
#include "wapp_test.h"
#include "../main.c"   // pull in the units under test

WAPP_TEST(parse_thread_minimal) {
    Thread t;
    int ok = parse_thread("# THREAD: Hi\nbody\n", 19, &t);
    WAPP_EXPECT_INT_EQ(ok, 1);
    WAPP_EXPECT_STR_EQ(t.title, "Hi");
}

WAPP_TEST(sanitise_title_trims) {
    char out[64];
    sanitise_title("  Hello, world!  ", out, sizeof(out));
    WAPP_EXPECT_STR_EQ(out, "hello-world");
}
```

Macros provided by `wapp_test.h`:

| Macro | Behaviour |
|-------|-----------|
| `WAPP_TEST(name) { ... }` | Register a case under the file's suite name |
| `WAPP_EXPECT_TRUE(expr)` | Pass when `expr` is non-zero |
| `WAPP_EXPECT_FALSE(expr)` | Pass when `expr` is zero |
| `WAPP_EXPECT_INT_EQ(a, b)` | Both ints equal |
| `WAPP_EXPECT_STR_EQ(a, b)` | Two `char*` strings equal byte-for-byte |
| `WAPP_EXPECT_MEM_EQ(a, b, n)` | First `n` bytes equal |
| `WAPP_FAIL(msg)` | Force-fail the case with `msg` |

A failing expectation captures `__FILE__` / `__LINE__` and a short
human message, attaches it to the case, and returns from the test
body — siblings continue to run.

### 20.2 Build

The SDK Makefile (`wapps/sdk/Makefile.common`) gains a `tests`
target. From any wapp folder:

```sh
make           # builds app.wasm (production)
make tests     # builds tests.wasm (production sources + tests/* + runner)
```

The test build links:
1. `MODULE_SRCS` (the same source files as production)
2. `tests/test_*.c` (auto-discovered by the makefile)
3. `wapps/sdk/wapp_test.c` (the runner)

…into a single `tests.wasm` that exports `module_run_tests` (in
addition to the usual lifecycle exports). Production `app.wasm`
never links the runner.

### 20.3 Distribution

The packager (`wapps/build-archive.sh`) includes `tests.wasm` and
the `tests/` source folder in the `.wapp` archive **only when they
exist** in the source folder. There is no requirement for
`tests.wasm` to be present in a published wapp — production
distributions can omit it to save bytes; debug/dev builds keep it
so consumers can verify functionality after install.

Both folder mode and ZIP mode follow the same rule: the engine
looks for `tests.wasm` next to `app.wasm` in the runtime archive
(`<baseDir>/wapps/<wappId>/tests.wasm`) and reports "no tests"
when absent.

### 20.4 Invocation protocol

Any wapp can ask the engine to run another wapp's tests by
sending a `tests.run` outbox message:

```json
{ "type":  "tests.run",
  "req_id": 17,
  "target": "tools.xprs.forum"   // optional; defaults to the sender
}
```

When `target` is omitted (or equals the sender's own ID), the
request is a **self-test** and is always allowed. When `target`
points at a different wapp, the requester must declare the
`tests.invoke` permission in its manifest — otherwise the engine
replies with `tests.complete` carrying `status: -1`.

The engine then:
1. Resolves the target wapp's archive folder.
2. Loads `tests.wasm` from that folder. If missing, replies
   `tests.complete` with `status: -2` (no tests).
3. Wires the same HAL surface as `app.wasm` (KV, msg, log, file —
   the test runner needs `hal_msg_send` to emit results).
4. Calls `module_run_tests`. The runner walks the registered
   cases, emits one `tests.case` per case as it finishes, and a
   single `tests.complete` summary at the end.
5. Forwards every `tests.case` and `tests.complete` from the test
   module back to the original requester's inbox, preserving the
   `req_id`.
6. Disposes the test module after `tests.complete` fires.

Streaming results lets the App Creator render a live progress
list rather than waiting for the whole suite to finish.

### 20.5 Result message shapes

`tests.case` — one per case as it completes:

```json
{ "type":     "tests.case",
  "req_id":   17,
  "suite":    "test_parse",
  "name":     "parse_thread_minimal",
  "passed":   true,
  "duration_ms": 0,
  "error":    null
}
```

On failure `passed` is `false` and `error` is a short string
formatted as `<file>:<line>: <message>` (e.g.
`tests/test_parse.c:42: expected 'Hi' got 'Hello'`).

`tests.complete` — single message after the last case:

```json
{ "type":   "tests.complete",
  "req_id": 17,
  "status": 0,
  "passed": 4,
  "failed": 1,
  "duration_ms": 12,
  "error":  null
}
```

`status` codes mirror the §15.1 convention: `0` ran successfully
(may still have failed cases — see `failed`); `-1` permission
denied; `-2` no tests in target; `-3` other failure (load error,
runner crashed). When `status != 0`, `error` carries a human
message and `passed` / `failed` are both 0.

### 20.6 Permissions

| Token          | Required for                                |
|----------------|---------------------------------------------|
| (none)         | Self-test (`target` omitted or = sender)    |
| `tests.invoke` | Running tests on a different wapp           |

The App Creator wapp declares `tests.invoke` so it can run tests
on the wapp the user is editing.

### 20.7 Engine notes

- `tests.wasm` runs in its own isolated WASM instance — separate
  module state from the live `app.wasm` of the same wapp. This
  prevents tests from corrupting production KV.
- The test instance receives a **scratch KV namespace** (e.g.
  `<wappId>::tests`) so production KV stays untouched. The wapp's
  own files (`hal_file_*`) remain isolated to a temp dir for the
  test run.
- Test runs have a hard timeout (default 10 s). On timeout the
  engine kills the test instance and emits `tests.complete` with
  `status: -3` and `error: "timeout"`.
- The runner emits messages synchronously inside `module_run_tests`;
  the engine drains the outbox after the call returns and forwards
  each message in order.
