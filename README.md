# XPRS Wapps — default / recommended catalog

This repository is the source of truth for **built-in XPRS
wapps** and a curated catalog of recommended ones. Each top-level
directory is one wapp; building the repo produces a set of
`.wapp` ZIPs under `binaries/` together with an `index.json` the
XPRS launcher knows how to consume.

The repo is designed to be hostable as-is (e.g. on GitHub,
GitHub Pages, an S3 bucket, or any plain HTTP server): point a
XPRS **wapp store** at the URL of `binaries/index.json` (or
the directory containing it) and the launcher will list every
shipped wapp for installation.

## Layout

```
.
├── README.md                ← this file
├── wapp-interfaces.md       ← engine contract every wapp targets
├── wapps.md                 ← package format & GeoUI reference
│
├── <wapp-id>/               ← one folder per wapp; see "Anatomy" below
│   ├── manifest.json
│   ├── main.c
│   ├── Makefile             ← include ../sdk/Makefile.common
│   ├── screens/*.ui.json
│   └── media/, lang/, store/, permissions.json, signature.json
│
├── sdk/                     ← shared HAL header + Makefile fragments
│   ├── Makefile.common
│   ├── Makefile.library
│   └── examples/            ← (optional) tiny HAL demos
├── hal/                     ← xprs_wasm_hal.h, included by every wapp
├── modules/                 ← non-wapp library / example modules
│
├── binaries/                ← built artefacts
│   ├── index.json           ← store catalogue (path, id, version, size)
│   └── <wapp-id>/<wapp-id>-<version>.wapp
│
├── Makefile                 ← top-level orchestration
├── build-archive.sh         ← builds + packages all wapps, regenerates index.json
├── build-all.sh             ← builds the example modules under modules/
└── install-wasi-sdk.sh      ← one-shot wasi-sdk installer
```

## Anatomy of a wapp folder

A `.wapp` is a ZIP of the wapp folder's contents. The minimum
shape is documented in detail in `wapp-interfaces.md` §17. In
short:

| File | Required | Purpose |
|---|---|---|
| `manifest.json` | yes | id, version, declared HAL/events, file handlers |
| `app.wasm`      | yes (built) | compiled WebAssembly entry point |
| `main.c`        | source | typical wapp entry; `module_init/tick/handle_event/destroy` |
| `Makefile`      | source | usually one line: `include ../sdk/Makefile.common` |
| `screens/*.ui.json` | optional | GeoUI screen definitions |
| `media/` | optional | icons (SVG), screenshots |
| `lang/<locale>.json` | optional | translations resolved by `hal_i18n_get` |
| `store/description.json` | optional | richer text shown in store cards |
| `permissions.json` | optional | NDF access-control (Stage 3) |
| `signature.json` | optional | publisher's NIP-78 NostrEvent |
| `social.sqlite3` | optional | shipped reactions/comments seed |

## Building

```sh
# One-time: install wasi-sdk into ~/wasi-sdk
make install-sdk

# Build & package every wapp into binaries/
make

# Or just one
make maps
```

Each invocation rewrites `binaries/index.json` to match the
`.wapp` files actually present, so the store catalog never drifts
from what's been built.

## Hosting as a wapp store

After `make`, the layout below is everything an XPRS client
needs to install wapps from this repo:

```
<host root>/
├── index.json
├── app-creator/app-creator-0.1.0.wapp
├── install/install-1.0.0.wapp
├── maps/maps-1.0.0.wapp
└── …
```

Two convenient ways to expose it:

- **GitHub raw**: enable a `gh-pages` branch (or use a GitHub
  Actions workflow that publishes `binaries/` to one) and point
  the launcher at
  `https://<user>.github.io/<repo>/binaries/index.json`.
- **GitHub Releases**: bundle `binaries/` as a release asset and
  publish the asset URL.

In the XPRS launcher, open the **Wapp Store** wapp's
**Settings** tab and add the `index.json` URL (or the directory
containing it) as a source. The store then lists every entry
from the index.

## Adding a new wapp

1. Create a folder at the repo root: `mkdir my-wapp && cd my-wapp`.
2. Write a minimal `manifest.json` — see `wapp-interfaces.md` §16.
3. Drop a `Makefile` containing `include ../sdk/Makefile.common`
   plus `MODULE_NAME := app` and `MODULE_SRCS := main.c`.
4. Implement `module_init/tick/handle_event/destroy` in
   `main.c`. The HAL surface is in `hal/xprs_wasm_hal.h`.
5. Add screens under `screens/` (optional).
6. From the repo root: `make my-wapp`.
7. The packaged `.wapp` lands at
   `binaries/my-wapp/my-wapp-<version>.wapp` and
   `binaries/index.json` is regenerated.

## Built-in wapps shipped today

- **install** — the on-device wapp store (lists, installs, updates).
- **maps** — satellite tiles with offline cache.
- **terminal** — built-in command terminal.
- **app-creator** — author/edit a wapp from inside XPRS (Stage 3).
- **functionalities** — registry inspector for inter-wapp APIs.
- **tasks** — task monitor / scheduler reference UI.
- **tester** — HAL smoke tests.
- **widget_demo** — GeoUI widget showcase.

## Engine contract

`wapp-interfaces.md` is the canonical engine contract — anything
a wapp talks to (storage, messaging, location, file
associations, notifications…) is specified there. Read it when
porting a wapp from another runtime, or when implementing a new
XPRS engine for a new platform.
