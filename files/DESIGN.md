# Files wapp — decentralized media storage (design)

Status (2026-06-12): **Phases 1–3 implemented; 4 partial.** Builds on
XPRS.md section 16 + the host `MediaArchive` (`aurora/lib/util/media_archive.dart`).

Working and verified on Linux desktop:
- Files wapp 0.1.0 (`wapps/files`): Library browser (archive as a people-list
  with tags), details (token / Edit tags / Delete), Find-by-hash prompt,
  Sharing panel (Blossom + BitTorrent toggles + live status).
- BlossomServer (`aurora/lib/services/blossom_server.dart`): live —
  `curl http://<host>:3457/<sha256-hex>` returns the hosted bytes, hash
  verified; PUT /upload behind a toggle; `fetchFrom` pulls + verifies.
- TorrentService (`aurora/lib/services/torrent_service.dart`): seeds every
  archived file with a DETERMINISTIC infohash (same bytes → same infohash,
  unit-proven); fetch-by-infohash wired.
- Client fetch proven live: a fresh archive holding only a recorded source
  fetched a file from an external Blossom origin, verified it, and became a
  provider (`test/blossom_live_fetch_test.dart`).
- HAL: hal_media_* + hal_share_* in `wapp_engine.dart` / the HAL header.

- **Discovery announce** (§7): WORKING. The aprs wapp (≥0.2.36) broadcasts a
  FILES-group bulletin `HAVE <token> <blossom-url> ih:<infohash>` whenever a
  sent message carries a media token; receivers intercept FILES bulletins
  (never shown as chat), record the source (hal_media_add_source) and fetch
  (hal_media_fetch) so they render + reseed. `WANT <token>` is answered with a
  HAVE. Discovery rides APRS-IS (global, NAT-agnostic). VERIFIED end-to-end
  over real APRS-IS: a station knowing only the token fetched the bytes from
  the host over Blossom (hash-exact) and rendered the thumbnail.

Remaining (Phase 5):
- **NAT traversal**: Blossom HTTP needs a reachable host (public IP / port
  forward / LAN); behind NAT the BitTorrent DHT path is the answer — the
  infohash is already announced in the HAVE and the seed/fetch code is in
  place, but live cross-internet swarm exchange isn't yet demonstrated.
- Policy/caps, public-address (STUN/UPnP) detection, Android background
  seeding, NIP-94 announce on internet NOSTR.

## 1. Goal

XPRS messages mix text with media references (`file:<sha256>.<ext>`). The
token names content; it does not move bytes. The **Files** wapp closes that
gap with decentralized distribution:

- every station that downloads a file can **become a provider** of that file
  to others — storage and bandwidth scale with popularity, like BitTorrent;
- a station can **search for a file by its hash** and fetch it from whoever
  has it;
- a station can deliberately **make local files available** to others.

Two transports, complementary:

1. **Blossom-compatible HTTP endpoint** (server) — simple, direct,
   NOSTR-ecosystem compatible: any Blossom client can `GET /<sha256>` from an
   Aurora device, and Aurora can fetch from any Blossom server.
2. **BitTorrent-compatible client** — swarm download/seed with DHT peer
   discovery, for files too big or too popular for point-to-point HTTP.

## 2. Architecture (three layers)

```
┌──────────────────────────────────────────────────────────────┐
│ Files wapp (wapps/files, C + GeoUI)                          │
│   UI + policy: browse/search archive, add/share files,      │
│   search-by-hash, transfers view, settings (share on/off,   │
│   ports, storage caps)                                       │
├──────────────────────────────────────────────────────────────┤
│ Host services (aurora/lib, generic — no wapp specifics)      │
│   MediaArchive   (exists)  content-addressed sqlite store    │
│   BlossomServer  (new)     HTTP provider endpoint            │
│   TorrentService (new)     dtorrent-based swarm client       │
│   SourcesIndex   (new)     sha256 → {infohash, urls, peers}  │
├──────────────────────────────────────────────────────────────┤
│ HAL (new wapp-facing functions)                              │
│   hal_media_*    archive access (list/meta/put/delete)       │
│   hal_share_*    server + torrent control and status         │
└──────────────────────────────────────────────────────────────┘
```

Heavy lifting (HTTP server, BitTorrent, hashing of large blobs) lives in the
host in Dart — a WASM wapp cannot bind sockets or run a swarm. The wapp is
the control surface and the policy owner, mirroring how `mediapack` gates the
`media.video` capability: the host ships the machinery, the installed wapp
switches it on.

## 3. Identifiers — one digest, several encodings

The canonical identity of a file is its **SHA-256 digest** (32 bytes).

| Context              | Encoding                  | Example length |
|----------------------|---------------------------|----------------|
| XPRS token (section 16)     | base64url, no padding     | 43 chars       |
| Blossom URL / NOSTR  | lowercase hex             | 64 chars       |
| media.sqlite3 key    | base64url (as the token)  | 43 chars       |

These are trivially convertible (decode → bytes → re-encode). The host gains
small helpers (`MediaRef.hexOf` / `fromHex`) and `MediaArchive` lookups accept
either form.

The **BitTorrent infohash is a different value** (the hash of the torrent's
info dictionary), so the mapping `sha256 → infohash` must be either computed
(only possible when you have the bytes) or learned from an announcement
(§7). It is cached in the SourcesIndex.

## 4. Phase 1 — Files wapp + media HAL (local only)

New wapp `wapps/files` (folder name `files`, title "Files").

HAL additions (host: `wapp_engine.dart`; header: `hal/xprs_wasm_hal.h`):

```
hal_media_list(offset, limit, out_json)      // archive entries (meta only)
hal_media_meta(hash, out_json)               // one entry's metadata
hal_media_put_file(path) -> token            // host file → archive
hal_media_set_meta(hash, json)               // name/description/tags
hal_media_delete(hash)
hal_media_stats(out_json)                    // count/bytes for the UI
```

Wapp UI (GeoUI screens):

- **Library** (home): list of archive entries — name, ext, size, tags, age —
  with search box filtering by name / tag / hash prefix. Tapping an entry
  shows details (full hashes, description, screenshot preview) with actions:
  copy token, edit tags/description, delete.
- **Add**: file picker (existing host `file_selector`) → `hal_media_put_file`
  → shows the new `file:` token ready to paste into any chat.
- **Settings**: sharing toggles + ports + caps (used from Phase 2 on).

Deliverable: archive management end-to-end without any networking.

## 5. Phase 2 — Blossom-compatible provider endpoint

Host service `BlossomServer` (`aurora/lib/services/blossom_server.dart`),
modelled on the existing `RemoteApiService` (dart:io `HttpServer`). Default
port **3457** (3456 is the device API), configurable.

Endpoints (Blossom BUDs, serving the shared `MediaArchive`):

| Endpoint                     | BUD    | v1 behaviour                          |
|------------------------------|--------|---------------------------------------|
| `GET /<sha256-hex>[.<ext>]`  | BUD-01 | 200 + blob (+ correct `Content-Type`) or 404 |
| `HEAD /<sha256-hex>[.<ext>]` | BUD-01 | 200/404, `Content-Length`              |
| `PUT /upload`                | BUD-02 | accept blob, verify digest, store; **auth required** |
| `GET /list/<pubkey-hex>`     | BUD-02 | blob descriptors uploaded by that key (needs an `uploader` column) |
| `DELETE /<sha256-hex>`       | BUD-02 | only by the original uploader; optional in v1 |

Auth: the Blossom `Authorization: Nostr <base64-event>` header — a **kind
24242** NOSTR event signed with standard **BIP-340 Schnorr**. Aurora profiles
already hold secp256k1 keys; verification needs a proper BIP-340 verify
(pointycastle primitives; note: XPRS section 14 short-Schnorr is NOT BIP-340 — this
is a separate, standard implementation, also reusable later for NOSTR relay
features). v1 may ship with `PUT /upload` disabled by default (toggle in the
Files wapp) so the read path doesn't wait on auth.

Blob descriptor (returned by PUT/list):
`{ "url": "...", "sha256": "<hex>", "size": n, "type": "image/png", "uploaded": ts }`.

Wapp control: `hal_share_server(start|stop, port)` + `hal_share_status` →
shown in the Files wapp Settings (running, port, bytes served).

Result: two Aurora stations (or any NOSTR/Blossom client) can fetch each
other's referenced media over HTTP/LAN/internet — the simplest "downloader
becomes provider" loop.

## 6. Phase 3 — BitTorrent-compatible client

Host service `TorrentService` behind a capability seam (same pattern as
`MediaCapabilities`), implemented with the pure-Dart **dtorrent** stack
(`dtorrent_task_v2` — BEP-52 capable, plus `bittorrent_dht` for BEP-5 peer
discovery). Pure Dart keeps the APK small; if the stack proves inadequate the
seam allows a native libtorrent backend later without touching callers.

Capabilities:

- **Seed**: for every archive entry marked shared, construct a torrent and
  announce on the DHT; serve peers (uTP/TCP).
- **Fetch**: given an infohash/magnet, download into a staging file, verify
  the **plain SHA-256 equals the requested token hash**, then `putBytes` into
  the archive (which dedups) — and keep seeding it.
- **Deterministic torrents**: every station independently constructs the SAME
  single-file torrent for the same content so infohashes coincide:
  - single file, `name = "<sha256-hex>.<ext>"`;
  - `piece length = clamp(2^ceil(log2(size/1024)), 16 KiB, 4 MiB)`;
  - no comment/date/private/extra keys; bencode canonical ordering.
  Documented here as normative so other implementations interoperate.
- Throttles: max upload rate, max active torrents, storage cap — Files wapp
  settings.

## 7. Phase 4 — discovery: who has hash X?

New `SourcesIndex` (table inside `media.sqlite3`):

```
sources(sha256 TEXT, kind TEXT, value TEXT, last_seen INTEGER,
        PRIMARY KEY (sha256, kind, value))
-- kind = 'infohash' | 'blossom' (server base URL) | 'callsign'
```

Populated from **announcements**:

- **XPRS (offline / radio)**: a bulletin to the reserved `FILES` group:
  `have file:<sha256-b64u>.<ext> ih:<infohash-hex> sz:<bytes>` and/or
  `srv <base-url>` — every word fits the 67-char line limit, multi-line
  word-split applies. Stations cache the mapping even for files they don't
  hold (they may relay the answer to others).
- **NOSTR (internet, later)**: NIP-94 file-metadata events carry the same
  fields (`x` = sha256 hex, `magnet`, `i` = infohash, `url`, `size`) — the
  Files wapp can publish/read them when a relay connection exists.

Resolution pipeline for "find hash X" (search box, or the *Find* action on a
chat "media not available" chip):

1. local `MediaArchive` (instant),
2. known Blossom servers from SourcesIndex (HTTP GET, cheap),
3. BitTorrent via cached `infohash` mapping (swarm),
4. else: optionally broadcast a `want file:<token>` XPRS bulletin to the
   FILES group and wait for a `have` announce.

Every successful fetch ends in `putBytes` → the station automatically becomes
a provider (Blossom immediately; BT re-seed) — the core decentralization
loop. Chat thumbnails light up on the next rebuild since they read the same
archive.

## 8. Phase 5 — policy, safety, platform notes

- **Sharing default**: providing files ON only for content the user added or
  explicitly fetched; a global "share downloads" toggle (default ON, like a
  torrent client) plus per-entry opt-out.
- **Caps**: archive size cap with LRU prune (`MediaArchive.prune` exists);
  upload rate cap; max peers.
- **Android**: seeding/serving only while the app runs (foreground service
  integration exists for background wapps); battery saver pauses the swarm.
- **NAT**: DHT + uTP work outbound-only for downloads; inbound seeding
  benefits from a reachable port — surfaced as a status line, not a blocker.
- Only content already present in the local archive is ever shared; deleting
  an entry stops both transports for it.

## 9. Implementation order & open questions

Suggested order: **Phase 1 → 2** first (immediately useful: add file → token
→ chat thumbnail on the other device fetched over Blossom HTTP), then
**4 (SourcesIndex + XPRS announces) → 3 (BitTorrent) → 5 polish**.

Open questions for Max:

1. Sharing default ON (torrent-style) or OFF (privacy-first)?
2. Should `PUT /upload` (accepting others' uploads onto your device) ship in
   v1, or read-only Blossom first?
3. Blossom port 3457 OK? Same port on all platforms?
4. BitTorrent on Android: seed only while charging/Wi-Fi?
5. v1 BitTorrent: v1-only deterministic torrents (max interop) or hybrid
   v1+v2 (BEP-52)?
