/*
 * install — XPRS Wapp Installer / Shop
 *
 * Reads index.json from a configured source (URL or local path),
 * displays available wapps with versions, and sends install/remove
 * requests to the renderer.
 *
 * Commands:
 *   source [url|path]   Get/set repository source
 *   list / refresh      Fetch index and show available wapps
 *   install <name>      Install or update a wapp
 *   remove <name>       Remove an installed wapp
 *   installed           Show installed wapps
 *   update [name]       Update one or all outdated wapps
 *   help                Show this help
 *
 * The renderer handles the actual download and installation when it
 * receives a {"type":"wapp.install",...} or {"type":"wapp.remove",...}
 * message.
 *
 * Build: cd wapps/archive/install && make
 */

#include "../hal/xprs_wasm_hal.h"

/* ── Helpers ─────────────────────────────────────────────────────────── */

static unsigned str_len(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

static void str_copy(char *d, const char *s, unsigned m) {
    unsigned i = 0;
    while (i < m - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void str_cat(char *d, const char *s, unsigned m) {
    unsigned l = str_len(d);
    unsigned i = 0;
    while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; }
    d[l + i] = '\0';
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static const char *next_word(const char *s, char *w, unsigned m) {
    s = skip_spaces(s);
    unsigned i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < m - 1) w[i++] = *s++;
    w[i] = '\0';
    return s;
}

static unsigned u64_to_str(uint64_t v, char *buf, unsigned buf_len) {
    char tmp[21];
    unsigned i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v > 0 && i < 20) { tmp[i++] = '0' + (char)(v % 10); v /= 10; } }
    unsigned out = 0;
    while (i > 0 && out < buf_len - 1) buf[out++] = tmp[--i];
    buf[out] = '\0';
    return out;
}

/* ── Output ──────────────────────────────────────────────────────────── */

static void send_output(const char *text, const char *level) {
    char buf[1024] = "{\"type\":\"ui.append\",\"target\":\"output-list\",\"item\":{\"text\":\"";
    unsigned len = str_len(buf);
    for (unsigned i = 0; text[i] && len < sizeof(buf) - 40; i++) {
        if (text[i] == '"')       { buf[len++] = '\\'; buf[len++] = '"'; }
        else if (text[i] == '\\') { buf[len++] = '\\'; buf[len++] = '\\'; }
        else if (text[i] == '\n') { buf[len++] = '\\'; buf[len++] = 'n'; }
        else                      { buf[len++] = text[i]; }
    }
    str_copy(buf + len, "\",\"level\":\"", sizeof(buf) - len); len = str_len(buf);
    str_cat(buf + len, level, sizeof(buf) - len); len = str_len(buf);
    str_copy(buf + len, "\"}}", sizeof(buf) - len); len = str_len(buf);
    hal_msg_send(buf, len);
}

/* Forward declaration — defined later next to the sources state. */
static void send_sources_list(void);

/* ── Catalog entry ───────────────────────────────────────────────────── */

#define MAX_ENTRIES 64

typedef struct {
    char name[64];              /* folder name, e.g. "maps" */
    char id[128];               /* manifest id */
    char version[32];
    char title[96];             /* short display name, e.g. "Wapp Store" */
    char description[200];      /* long-form, e.g. "Discover, install ..." */
    char file[128];             /* relative path, e.g. "maps/maps-1.0.0.wapp" */
    uint32_t size;
    char source_raw[256];       /* the raw source URL/path this came from */
    char source_host[96];       /* extracted host part (or "local" for files) */
    char publisher_npub[80];    /* optional — from index.json, empty if unsigned */
} CatalogEntry;

static CatalogEntry catalog[MAX_ENTRIES];
static int catalog_count = 0;

/* ── Source config ───────────────────────────────────────────────────── */

/* Multi-source support: the Settings tab hands us a newline-separated
 * list of repositories (URLs or local paths). The wapp stores the raw
 * buffer in KV under the same "source" key used by the old
 * single-source build — the extra newlines are ignored by any older
 * consumer. A `list`/`refresh` command fetches each source sequentially
 * using a small state machine, accumulating catalog entries across
 * every source. */

#define MAX_SOURCES 16
#define SOURCES_RAW_CAP 4096

static char sources_raw[SOURCES_RAW_CAP] = "";
static char sources[MAX_SOURCES][256];
static int source_count = 0;

/* Current fetch state for the multi-source queue. */
static int fetching_idx = -1;           /* -1 = idle */
static char fetch_current_src[256] = "";
static char fetch_current_host[96] = "";

static int source_str_is_url(const char *s) {
    return str_starts(s, "http://") || str_starts(s, "https://");
}

/* Extract a short human-readable host label from a source string.
 * For URLs, this is the hostname (before any port / path). For file
 * paths, we use the literal string "local" so cards have something
 * to display. */
static void extract_host(const char *src, char *host, unsigned host_len) {
    const char *p = src;
    /* A Reticulum folder source (rns:npub… / npub… / 64-hex) isn't a URL or a
     * local path — label it so the card chip reads "Reticulum", not "local". */
    if (str_starts(p, "rns:") || str_starts(p, "reticulum:") ||
        str_starts(p, "npub1")) {
        str_copy(host, "Reticulum", host_len);
        return;
    }
    if (str_starts(p, "https://")) p += 8;
    else if (str_starts(p, "http://")) p += 7;
    else {
        str_copy(host, "local", host_len);
        return;
    }
    unsigned i = 0;
    while (*p && *p != '/' && *p != ':' && i < host_len - 1) {
        host[i++] = *p++;
    }
    host[i] = '\0';
    if (i == 0) str_copy(host, "local", host_len);
}

/* Parse the newline-separated sources_raw into the sources[] array. */
static void parse_sources_raw(void) {
    source_count = 0;
    const char *p = sources_raw;
    while (*p && source_count < MAX_SOURCES) {
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        unsigned i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < 255) {
            sources[source_count][i++] = *p++;
        }
        /* trim trailing whitespace */
        while (i > 0 && (sources[source_count][i - 1] == ' ' ||
                         sources[source_count][i - 1] == '\t')) {
            i--;
        }
        sources[source_count][i] = '\0';
        if (i > 0) source_count++;
    }
}

/* Default catalog source when no user configuration exists yet. The
 * Settings tab can add or replace it; this is just the seed so a
 * fresh install isn't staring at an empty catalog. Self-hosted on
 * xprs.dev — the store appends "/index.json" and downloads
 * "<base>/<file>" for each wapp.
 *
 * The shipped binary reaches NO proprietary host: the URL is used exactly as
 * configured, and the github.com -> raw.githubusercontent.com rewriter that
 * used to live here is gone. A catalog served from a code-hosting site has to
 * publish plain-file URLs like any other host. */
#define DEFAULT_SOURCE "rns:npub1dwfaavw4k2af0snm3q2n4c7vd046xl7ze8scp553x5upw84wm5ns5deehf"

/* Bump when DEFAULT_SOURCE changes so devices upgrading from an older store
 * (which may hold a stale local/dev/HTTP source in KV) are migrated ONCE to the
 * current default. After migration we honour user edits again (the marker stays
 * set, so a later saved source is loaded normally). */
#define SOURCE_SCHEMA "rns1"

static void load_sources(void) {
    char ver[16] = "";
    uint32_t vn = hal_kv_get("src_schema", 10, ver, sizeof(ver) - 1);
    if (vn > 0) ver[vn] = '\0';
    if (!str_eq(ver, SOURCE_SCHEMA)) {
        /* One-time migration to the Reticulum default catalog. */
        str_copy(sources_raw, DEFAULT_SOURCE, sizeof(sources_raw));
        hal_kv_set("source", 6, sources_raw, str_len(sources_raw));
        hal_kv_set("src_schema", 10, SOURCE_SCHEMA, str_len(SOURCE_SCHEMA));
        parse_sources_raw();
        return;
    }
    uint32_t n = hal_kv_get("source", 6, sources_raw, sizeof(sources_raw) - 1);
    if (n > 0) {
        sources_raw[n] = '\0';
    } else {
        str_copy(sources_raw, DEFAULT_SOURCE, sizeof(sources_raw));
    }
    parse_sources_raw();
}

static void save_sources(void) {
    hal_kv_set("source", 6, sources_raw, str_len(sources_raw));
    parse_sources_raw();
}

/* Emit the current sources[] list as a structured message so the
 * host can render a proper list UI (instead of guessing the state by
 * parsing text logs). Format:
 *   {"type":"store.sources","sources":["url1","url2",...]}
 * Sent on module_init and after every save_sources() call so the
 * settings tab stays in sync across add / remove cycles. */
static void send_sources_list(void) {
    char buf[SOURCES_RAW_CAP + 128];
    str_copy(buf, "{\"type\":\"store.sources\",\"sources\":[", sizeof(buf));
    unsigned len = str_len(buf);
    for (int i = 0; i < source_count; i++) {
        if (i > 0 && len < sizeof(buf) - 2) { buf[len++] = ','; }
        if (len < sizeof(buf) - 2) { buf[len++] = '"'; }
        for (unsigned j = 0; sources[i][j] && len < sizeof(buf) - 8; j++) {
            char c = sources[i][j];
            if (c == '"')      { buf[len++] = '\\'; buf[len++] = '"'; }
            else if (c == '\\'){ buf[len++] = '\\'; buf[len++] = '\\'; }
            else               { buf[len++] = c; }
        }
        if (len < sizeof(buf) - 2) { buf[len++] = '"'; }
    }
    if (len < sizeof(buf) - 3) { buf[len++] = ']'; buf[len++] = '}'; }
    buf[len] = '\0';
    hal_msg_send(buf, len);
}

/* ── Installed versions (stored in KV as "inst:<name>" = "<version>") ─ */

static void get_installed_version(const char *name, char *ver, unsigned ver_len) {
    char key[80] = "inst:";
    str_cat(key, name, sizeof(key));
    uint32_t n = hal_kv_get(key, str_len(key), ver, ver_len - 1);
    if (n > 0) ver[n] = '\0';
    else ver[0] = '\0';
}

static void set_installed_version(const char *name, const char *ver) {
    char key[80] = "inst:";
    str_cat(key, name, sizeof(key));
    hal_kv_set(key, str_len(key), ver, str_len(ver));
}

static void remove_installed_version(const char *name) {
    char key[80] = "inst:";
    str_cat(key, name, sizeof(key));
    hal_kv_delete(key, str_len(key));
}

/* ── Minimal JSON parsing for index.json ─────────────────────────────
 *
 * Expected format:
 * [
 *   {"file":"maps/maps-1.0.0.wapp","id":"...","version":"1.0.0",
 *    "size":7767,"description":"..."},
 *   ...
 * ]
 */

/* Find value for a string key in a JSON object substring.
 * Writes value into val (unquoted for strings, raw for numbers).
 * Returns pointer past the value, or NULL if not found. */
static const char *json_find_str(const char *obj, const char *obj_end,
                                  const char *key, char *val, unsigned val_len) {
    unsigned klen = str_len(key);
    val[0] = '\0';
    const char *p = obj;
    while (p < obj_end) {
        /* Look for "key" */
        if (*p == '"') {
            int match = 1;
            for (unsigned i = 0; i < klen; i++) {
                if (p[1 + i] != key[i]) { match = 0; break; }
            }
            if (match && p[1 + klen] == '"') {
                /* Found key, skip to colon and value */
                p += 2 + klen;
                while (p < obj_end && *p != ':') p++;
                if (p >= obj_end) return 0;
                p++; /* skip colon */
                while (p < obj_end && (*p == ' ' || *p == '\t')) p++;
                if (*p == '"') {
                    /* String value */
                    p++;
                    unsigned vi = 0;
                    while (p < obj_end && *p != '"' && vi < val_len - 1) {
                        val[vi++] = *p++;
                    }
                    val[vi] = '\0';
                    return p;
                } else {
                    /* Number or other */
                    unsigned vi = 0;
                    while (p < obj_end && *p != ',' && *p != '}' && *p != ' '
                           && vi < val_len - 1) {
                        val[vi++] = *p++;
                    }
                    val[vi] = '\0';
                    return p;
                }
            }
        }
        p++;
    }
    return 0;
}

/* Extract folder name from file path: "maps/maps-1.0.0.wapp" -> "maps" */
static void extract_name(const char *file, char *name, unsigned name_len) {
    unsigned i = 0;
    while (file[i] && file[i] != '/' && i < name_len - 1) {
        name[i] = file[i];
        i++;
    }
    name[i] = '\0';
}

static int str_to_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* Parse index.json buffer and APPEND entries to catalog[]. Each
 * appended entry is tagged with the current fetch's source_raw and
 * source_host so the host can render per-origin chips on the store
 * cards. The multi-source state machine calls this once per fetched
 * index and then advances to the next source without resetting
 * catalog_count — the caller resets the count only when a new
 * `list`/`refresh` command fires. */
static void parse_index(const char *json, unsigned json_len) {
    const char *end = json + json_len;
    const char *p = json;

    while (p < end && catalog_count < MAX_ENTRIES) {
        /* Find next object start */
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        const char *obj_start = p;

        /* Find matching close brace */
        int depth = 0;
        while (p < end) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        const char *obj_end = p;

        CatalogEntry *e = &catalog[catalog_count];
        char size_str[16] = "";

        /* Zero the whole entry so leftover bytes from a previous
         * list don't leak into the new one. */
        for (unsigned i = 0; i < sizeof(*e); i++) ((char *)e)[i] = 0;

        json_find_str(obj_start, obj_end, "file", e->file, sizeof(e->file));
        json_find_str(obj_start, obj_end, "id", e->id, sizeof(e->id));
        json_find_str(obj_start, obj_end, "version", e->version, sizeof(e->version));
        json_find_str(obj_start, obj_end, "title", e->title, sizeof(e->title));
        json_find_str(obj_start, obj_end, "description", e->description, sizeof(e->description));
        json_find_str(obj_start, obj_end, "size", size_str, sizeof(size_str));
        json_find_str(obj_start, obj_end, "publisher_npub", e->publisher_npub, sizeof(e->publisher_npub));

        /* Legacy schema migration: pre-title catalogs put the short
         * display name in the `description` field and (often) had no
         * long-form text at all. If we got a description but no title,
         * promote the description to the title and clear it. */
        if (e->title[0] == '\0' && e->description[0] != '\0') {
            str_copy(e->title, e->description, sizeof(e->title));
            e->description[0] = '\0';
        }

        e->size = (uint32_t)str_to_int(size_str);
        extract_name(e->file, e->name, sizeof(e->name));
        str_copy(e->source_raw, fetch_current_src, sizeof(e->source_raw));
        str_copy(e->source_host, fetch_current_host, sizeof(e->source_host));

        if (e->name[0] && e->version[0]) {
            catalog_count++;
        }
    }
}

/* Forward declarations */
static void show_catalog(void);
static void advance_fetch_queue(void);
static void push_status_card(const char *id, const char *title,
                             const char *subtitle);

/* ── Fetch index ─────────────────────────────────────────────────────── */

/* Pending HTTP request for async fetch. */
static int32_t pending_req = -1;
static char index_buf[32768];


/* Kick off the fetch for sources[fetching_idx]. For URL sources the
 * HTTP request is started via hal_http and polled from module_tick.
 * For local paths we delegate to the host via a
 * {"type":"wapp.fetch_index",...} message and the response lands in
 * module_handle_event as `wapp.index`. Either way, on completion
 * advance_fetch_queue() is invoked to move to the next source. */
static void start_current_fetch(void) {
    if (fetching_idx < 0 || fetching_idx >= source_count) return;
    const char *src = sources[fetching_idx];
    str_copy(fetch_current_src, src, sizeof(fetch_current_src));
    extract_host(src, fetch_current_host, sizeof(fetch_current_host));

    char msg[256] = "Fetching ";
    str_cat(msg, fetch_current_host, sizeof(msg));
    str_cat(msg, "...", sizeof(msg));
    send_output(msg, "info");

    if (source_str_is_url(src)) {
        char url[600] = "";
        str_cat(url, src, sizeof(url));
        unsigned slen = str_len(src);
        if (slen < 5 || !str_eq(src + slen - 5, ".json")) {
            if (src[slen - 1] != '/') str_cat(url, "/", sizeof(url));
            str_cat(url, "index.json", sizeof(url));
        }
        hal_log(1, "[install] http GET", 18);
        hal_log(1, url, str_len(url));
        pending_req = hal_http_request(0, url, str_len(url), "", 0);
        if (pending_req < 0) {
            send_output("  failed to start HTTP request", "err");
            hal_log(2, "[install] http_request returned <0", 34);
            push_status_card("__http_err",
                             "Could not start HTTP request",
                             fetch_current_host);
            advance_fetch_queue();
        }
    } else {
        char m[700] = "{\"type\":\"wapp.fetch_index\",\"source\":\"";
        str_cat(m, src, sizeof(m));
        str_cat(m, "\"}", sizeof(m));
        hal_msg_send(m, str_len(m));
    }
}

/* Called after a source's response is fully parsed (success or skip).
 * Moves to the next source in the queue, or finalises the catalog
 * display when every source has been consulted. */
static void advance_fetch_queue(void) {
    fetching_idx++;
    if (fetching_idx >= source_count) {
        fetching_idx = -1;
        show_catalog();
        return;
    }
    start_current_fetch();
}

/* Entry point for the `list`/`refresh` command. Wipes the catalog
 * and starts walking the sources[] array. */
static void begin_fetch_all(void) {
    if (source_count == 0) {
        send_output("No repositories configured. Add at least one URL or path in Settings.", "err");
        push_status_card("__no_sources",
                         "No repositories configured",
                         "Add a source in Settings to populate the catalog.");
        return;
    }
    catalog_count = 0;
    fetching_idx = 0;
    /* Show an immediate placeholder so the user knows we're working —
     * the empty-state placeholder is misleading while a fetch is in
     * flight. show_catalog() replaces this card when the queue
     * finishes. */
    push_status_card("__loading",
                     "Loading catalog...",
                     "Fetching available wapps");
    hal_log(1, "[install] begin fetch", 21);
    start_current_fetch();
}

/* ── Display ─────────────────────────────────────────────────────────── */

/* Emit a single-card ui.data payload as a visible status placeholder.
 * The user sees this in the catalog area while fetching, instead of
 * the bare "No wapps found yet" empty-state which is misleading
 * during normal startup. */
static void push_status_card(const char *id,
                             const char *title,
                             const char *subtitle) {
    char buf[640];
    str_copy(buf,
             "{\"type\":\"ui.data\",\"target\":\"catalog\",\"items\":[{",
             sizeof(buf));
    unsigned len = str_len(buf);
    str_copy(buf + len, "\"id\":\"", sizeof(buf) - len);
    len = str_len(buf);
    for (unsigned i = 0; id[i] && len < sizeof(buf) - 8; i++) buf[len++] = id[i];
    str_copy(buf + len, "\",\"title\":\"", sizeof(buf) - len);
    len = str_len(buf);
    for (unsigned i = 0; title[i] && len < sizeof(buf) - 8; i++) {
        char c = title[i];
        if (c == '"' || c == '\\') buf[len++] = '\\';
        buf[len++] = c;
    }
    if (subtitle && subtitle[0]) {
        str_copy(buf + len, "\",\"subtitle\":\"", sizeof(buf) - len);
        len = str_len(buf);
        for (unsigned i = 0; subtitle[i] && len < sizeof(buf) - 8; i++) {
            char c = subtitle[i];
            if (c == '"' || c == '\\') buf[len++] = '\\';
            buf[len++] = c;
        }
    }
    str_copy(buf + len, "\"}]}", sizeof(buf) - len);
    len = str_len(buf);
    hal_msg_send(buf, len);
}

/* Append a JSON-escaped string literal to buf. The caller already wrote
 * the opening quote; this writes the body and the closing quote. */
static unsigned append_json_string(char *buf, unsigned len, unsigned cap,
                                   const char *s) {
    for (unsigned i = 0; s[i] && len < cap - 8; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  { buf[len++] = '\\'; buf[len++] = '"'; }
        else if (c == '\\') { buf[len++] = '\\'; buf[len++] = '\\'; }
        else if (c == '\n') { buf[len++] = '\\'; buf[len++] = 'n'; }
        else if (c == '\r') { buf[len++] = '\\'; buf[len++] = 'r'; }
        else if (c == '\t') { buf[len++] = '\\'; buf[len++] = 't'; }
        else if (c < 0x20) {
            /* Any other control char (a stray 0x14 in a manifest description
             * was producing invalid JSON the host silently dropped) -> \u00XX. */
            static const char hex[] = "0123456789abcdef";
            buf[len++] = '\\'; buf[len++] = 'u'; buf[len++] = '0'; buf[len++] = '0';
            buf[len++] = hex[(c >> 4) & 0xF];
            buf[len++] = hex[c & 0xF];
        }
        else                { buf[len++] = (char)c; }
    }
    if (len < cap - 1) buf[len++] = '"';
    buf[len] = '\0';
    return len;
}

/* Buffer for the catalog ui.data emission. ~16KB handles ~50 entries
 * with all fields populated. */
static char catalog_buf[16384];

/* Emit the catalog as a structured `ui.data` message that the host's
 * generic `$type="cards"` group renders. The host knows nothing about
 * catalogs — it just renders the list of items the wapp pushes. */
static void show_catalog(void) {
    unsigned len = 0;
    str_copy(catalog_buf,
             "{\"type\":\"ui.data\",\"target\":\"catalog\",\"items\":[",
             sizeof(catalog_buf));
    len = str_len(catalog_buf);

    for (int i = 0; i < catalog_count; i++) {
        CatalogEntry *e = &catalog[i];
        char inst_ver[32];
        get_installed_version(e->name, inst_ver, sizeof(inst_ver));

        if (i > 0 && len < sizeof(catalog_buf) - 2) {
            catalog_buf[len++] = ',';
        }

        /* {"id":"<slug>","title":"<title>", */
        if (len < sizeof(catalog_buf) - 16) catalog_buf[len++] = '{';
        str_copy(catalog_buf + len, "\"id\":\"", sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf), e->name);

        str_copy(catalog_buf + len, ",\"title\":\"", sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf),
                                 e->title[0] ? e->title : e->name);

        /* subtitle: "v<version> (NN KB)" + status. ASCII only — the host reads
         * wapp strings as Latin-1, so a UTF-8 middot would render as mojibake. */
        char subtitle[128] = "v";
        str_cat(subtitle, e->version, sizeof(subtitle));
        if (e->size >= 1024) {
            char sz[16];
            u64_to_str((uint64_t)(e->size / 1024), sz, sizeof(sz));
            str_cat(subtitle, " (", sizeof(subtitle));
            str_cat(subtitle, sz, sizeof(subtitle));
            str_cat(subtitle, " KB)", sizeof(subtitle));
        }
        if (inst_ver[0] && !str_eq(inst_ver, e->version)) {
            str_cat(subtitle, " - update v", sizeof(subtitle));
            str_cat(subtitle, inst_ver, sizeof(subtitle));
        }
        str_copy(catalog_buf + len, ",\"subtitle\":\"",
                 sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf), subtitle);

        /* description */
        if (e->description[0]) {
            str_copy(catalog_buf + len, ",\"description\":\"",
                     sizeof(catalog_buf) - len);
            len = str_len(catalog_buf);
            len = append_json_string(catalog_buf, len, sizeof(catalog_buf),
                                     e->description);
        }

        /* icon_path: "wapp:<slug>" — host resolves to the installed
         * wapp's manifest.icon path. Falls back to a generic icon
         * when the wapp isn't installed yet. */
        char icon_ref[80] = "wapp:";
        str_cat(icon_ref, e->name, sizeof(icon_ref));
        str_copy(catalog_buf + len, ",\"icon_path\":\"",
                 sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf), icon_ref);

        /* chips: source host + publisher */
        int chip_count = 0;
        if (e->source_host[0] || e->publisher_npub[0]) {
            str_copy(catalog_buf + len, ",\"chips\":[",
                     sizeof(catalog_buf) - len);
            len = str_len(catalog_buf);
            if (e->source_host[0]) {
                str_copy(catalog_buf + len, "{\"label\":\"",
                         sizeof(catalog_buf) - len);
                len = str_len(catalog_buf);
                len = append_json_string(catalog_buf, len, sizeof(catalog_buf),
                                         e->source_host);
                const char *icn = str_eq(e->source_host, "local") ?
                    ",\"icon\":\"folder\"}" : ",\"icon\":\"cloud\"}";
                str_copy(catalog_buf + len, icn, sizeof(catalog_buf) - len);
                len = str_len(catalog_buf);
                chip_count++;
            }
            if (e->publisher_npub[0]) {
                if (chip_count > 0 && len < sizeof(catalog_buf) - 2) {
                    catalog_buf[len++] = ',';
                }
                /* A full npub is 63 chars and overflows the chip; show a short
                 * "npub1abcd...wxyz" form (first 9 + last 4). */
                char pub_short[40];
                unsigned pl = str_len(e->publisher_npub);
                if (pl > 20) {
                    unsigned k = 0;
                    for (; k < 9; k++) pub_short[k] = e->publisher_npub[k];
                    pub_short[k++] = '.'; pub_short[k++] = '.'; pub_short[k++] = '.';
                    for (unsigned j = pl - 4; j < pl; j++) pub_short[k++] = e->publisher_npub[j];
                    pub_short[k] = '\0';
                } else {
                    str_copy(pub_short, e->publisher_npub, sizeof(pub_short));
                }
                str_copy(catalog_buf + len, "{\"icon\":\"person_add\",\"label\":\"",
                         sizeof(catalog_buf) - len);
                len = str_len(catalog_buf);
                len = append_json_string(catalog_buf, len, sizeof(catalog_buf),
                                         pub_short);
                str_copy(catalog_buf + len, "}", sizeof(catalog_buf) - len);
                len = str_len(catalog_buf);
            }
            if (len < sizeof(catalog_buf) - 2) catalog_buf[len++] = ']';
        }

        /* actions: one button — Install / Installed / Update.
         * Action name "install:<slug>" so the host's generic action
         * dispatch reaches the install command. */
        char act[96] = "install:";
        str_cat(act, e->name, sizeof(act));
        str_copy(catalog_buf + len, ",\"actions\":[{\"name\":\"",
                 sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf), act);

        const char *label, *icon;
        int disabled = 0;
        if (inst_ver[0] && str_eq(inst_ver, e->version)) {
            label = "Installed"; icon = "check"; disabled = 1;
        } else if (inst_ver[0]) {
            label = "Update"; icon = "upgrade";
        } else {
            label = "Install"; icon = "download";
        }
        str_copy(catalog_buf + len, ",\"label\":\"",
                 sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf), label);

        str_copy(catalog_buf + len, ",\"icon\":\"",
                 sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
        len = append_json_string(catalog_buf, len, sizeof(catalog_buf), icon);

        if (disabled) {
            str_copy(catalog_buf + len, ",\"disabled\":true",
                     sizeof(catalog_buf) - len);
            len = str_len(catalog_buf);
        }
        str_copy(catalog_buf + len, "}]}", sizeof(catalog_buf) - len);
        len = str_len(catalog_buf);
    }

    if (len < sizeof(catalog_buf) - 2) catalog_buf[len++] = ']';
    if (len < sizeof(catalog_buf) - 2) catalog_buf[len++] = '}';
    catalog_buf[len] = '\0';
    char dbg[80] = "[install] show_catalog ";
    char nb[16];
    u64_to_str((uint64_t)catalog_count, nb, sizeof(nb));
    str_cat(dbg, nb, sizeof(dbg));
    str_cat(dbg, " entries, ", sizeof(dbg));
    char lb[16];
    u64_to_str((uint64_t)len, lb, sizeof(lb));
    str_cat(dbg, lb, sizeof(dbg));
    str_cat(dbg, " bytes", sizeof(dbg));
    hal_log(1, dbg, str_len(dbg));
    hal_msg_send(catalog_buf, len);
}

/* Emit a layout-attribute change for the catalog. Called by the
 * view-list / view-grid action handlers. The host's generic ui.attr
 * mechanism flips the cards group's `layout` attribute live. */
static void send_layout(const char *mode) {
    char buf[160] =
        "{\"type\":\"ui.attr\",\"target\":\"catalog\","
        "\"attr\":\"layout\",\"value\":\"";
    str_cat(buf, mode, sizeof(buf));
    str_cat(buf, "\"}", sizeof(buf));
    hal_msg_send(buf, str_len(buf));
}

static void load_and_send_layout(void) {
    char saved[16];
    uint32_t n = hal_kv_get("view_mode", 9, saved, sizeof(saved) - 1);
    if (n > 0 && n < sizeof(saved)) saved[n] = '\0';
    else str_copy(saved, "list", sizeof(saved));
    if (!str_eq(saved, "list") && !str_eq(saved, "grid")) {
        str_copy(saved, "list", sizeof(saved));
    }
    send_layout(saved);
}

static void set_layout(const char *mode) {
    if (!str_eq(mode, "list") && !str_eq(mode, "grid")) return;
    hal_kv_set("view_mode", 9, mode, str_len(mode));
    send_layout(mode);
}

/* ── Find catalog entry by name ──────────────────────────────────────── */

static CatalogEntry *find_entry(const char *name) {
    for (int i = 0; i < catalog_count; i++) {
        if (str_eq(catalog[i].name, name)) return &catalog[i];
    }
    return 0;
}

/* ── Install / remove / update ───────────────────────────────────────── */

static void do_install(const char *name) {
    CatalogEntry *e = find_entry(name);
    if (!e) {
        char msg[128] = "Not in catalog: ";
        str_cat(msg, name, sizeof(msg));
        str_cat(msg, ". Run 'list' first.", sizeof(msg));
        send_output(msg, "err");
        return;
    }

    /* The source is used verbatim: no host-specific URL rewriting. */
    char src[256];
    str_copy(src, e->source_raw, sizeof(src));

    /* Build install message for the renderer. The source is whatever
     * repository THIS entry came from, not a global — that way a
     * multi-repo catalog can still install each wapp from its own
     * origin without the user needing to toggle sources.
     *   {"type":"wapp.install","source":"<source>","file":"<file>",
     *    "name":"<name>","version":"<version>"} */
    char msg[1024] = "{\"type\":\"wapp.install\",\"source\":\"";
    str_cat(msg, src, sizeof(msg));
    str_cat(msg, "\",\"file\":\"", sizeof(msg));
    str_cat(msg, e->file, sizeof(msg));
    str_cat(msg, "\",\"name\":\"", sizeof(msg));
    str_cat(msg, e->name, sizeof(msg));
    str_cat(msg, "\",\"version\":\"", sizeof(msg));
    str_cat(msg, e->version, sizeof(msg));
    str_cat(msg, "\"}", sizeof(msg));
    hal_msg_send(msg, str_len(msg));

    char out[128] = "Installing ";
    str_cat(out, e->name, sizeof(out));
    str_cat(out, " v", sizeof(out));
    str_cat(out, e->version, sizeof(out));
    str_cat(out, "...", sizeof(out));
    send_output(out, "info");
}

static void do_remove(const char *name) {
    char ver[32];
    get_installed_version(name, ver, sizeof(ver));
    if (!ver[0]) {
        char msg[128] = "Not installed: ";
        str_cat(msg, name, sizeof(msg));
        send_output(msg, "err");
        return;
    }

    /* Send remove message to renderer */
    char msg[256] = "{\"type\":\"wapp.remove\",\"name\":\"";
    str_cat(msg, name, sizeof(msg));
    str_cat(msg, "\"}", sizeof(msg));
    hal_msg_send(msg, str_len(msg));

    remove_installed_version(name);

    char out[128] = "Removed ";
    str_cat(out, name, sizeof(out));
    send_output(out, "info");
}

static void do_update(const char *name) {
    if (catalog_count == 0) {
        send_output("No catalog loaded. Run 'list' first.", "err");
        return;
    }

    if (name[0]) {
        /* Update specific wapp */
        CatalogEntry *e = find_entry(name);
        if (!e) {
            char msg[128] = "Not in catalog: ";
            str_cat(msg, name, sizeof(msg));
            send_output(msg, "err");
            return;
        }
        char inst_ver[32];
        get_installed_version(name, inst_ver, sizeof(inst_ver));
        if (!inst_ver[0]) {
            char msg[128] = "Not installed: ";
            str_cat(msg, name, sizeof(msg));
            str_cat(msg, ". Use 'install' instead.", sizeof(msg));
            send_output(msg, "err");
            return;
        }
        if (str_eq(inst_ver, e->version)) {
            char msg[128] = "";
            str_cat(msg, name, sizeof(msg));
            str_cat(msg, " is already up to date (v", sizeof(msg));
            str_cat(msg, inst_ver, sizeof(msg));
            str_cat(msg, ").", sizeof(msg));
            send_output(msg, "info");
            return;
        }
        do_install(name);
        return;
    }

    /* Update all outdated */
    int updated = 0;
    for (int i = 0; i < catalog_count; i++) {
        CatalogEntry *e = &catalog[i];
        char inst_ver[32];
        get_installed_version(e->name, inst_ver, sizeof(inst_ver));
        if (inst_ver[0] && !str_eq(inst_ver, e->version)) {
            do_install(e->name);
            updated++;
        }
    }
    if (updated == 0) {
        send_output("All installed wapps are up to date.", "info");
    }
}

static void show_installed(void) {
    char buf[2048];
    uint32_t count = hal_kv_list("inst:", 5, buf, sizeof(buf) - 1);
    if (count == 0) {
        send_output("No wapps installed.", "info");
        return;
    }

    char hdr[32];
    u64_to_str((uint64_t)count, hdr, sizeof(hdr));
    char msg[64] = "";
    str_cat(msg, hdr, sizeof(msg));
    str_cat(msg, " wapp(s) installed:", sizeof(msg));
    send_output(msg, "info");

    char *p = buf;
    for (uint32_t i = 0; i < count; i++) {
        /* Key is "inst:<name>", strip prefix */
        const char *name = p + 5; /* skip "inst:" */
        char ver[32];
        get_installed_version(name, ver, sizeof(ver));

        char line[128] = "  ";
        str_cat(line, name, sizeof(line));
        unsigned pad = str_len(line);
        while (pad < 18) { line[pad++] = ' '; line[pad] = '\0'; }
        str_cat(line, "v", sizeof(line));
        str_cat(line, ver, sizeof(line));
        send_output(line, "out");

        while (*p) p++;
        p++;
    }
}

/* ── Command dispatch ────────────────────────────────────────────────── */

static void cmd_help(void) {
    send_output("Wapp Store commands:", "info");
    send_output("  sources            List configured repositories", "out");
    send_output("  list               Fetch all repos and show catalog", "out");
    send_output("  install <name>     Install a wapp", "out");
    send_output("  update [name]      Update one or all wapps", "out");
    send_output("  remove <name>      Remove a wapp", "out");
    send_output("  installed          Show installed wapps", "out");
    send_output("  help               Show this help", "out");
}

static void dispatch(const char *input) {
    char cmd[32];
    const char *args = next_word(input, cmd, sizeof(cmd));
    (void)args;

    if (cmd[0] == '\0') return;

    if (str_eq(cmd, "help")) {
        cmd_help();
    }
    else if (str_eq(cmd, "sources") || str_eq(cmd, "source")) {
        if (source_count == 0) {
            send_output("No repositories configured. Add them in Settings.", "err");
            return;
        }
        char hdr[32];
        u64_to_str((uint64_t)source_count, hdr, sizeof(hdr));
        char msg[64] = "";
        str_cat(msg, hdr, sizeof(msg));
        str_cat(msg, " repositories:", sizeof(msg));
        send_output(msg, "info");
        for (int i = 0; i < source_count; i++) {
            char line[320] = "  ";
            str_cat(line, sources[i], sizeof(line));
            send_output(line, "out");
        }
    }
    else if (str_eq(cmd, "list") || str_eq(cmd, "refresh")) {
        begin_fetch_all();
    }
    else if (str_eq(cmd, "install")) {
        char name[64];
        next_word(args, name, sizeof(name));
        if (!name[0]) { send_output("Usage: install <name>", "err"); return; }
        do_install(name);
    }
    else if (str_eq(cmd, "update")) {
        char name[64];
        next_word(args, name, sizeof(name));
        do_update(name);
    }
    else if (str_eq(cmd, "remove")) {
        char name[64];
        next_word(args, name, sizeof(name));
        if (!name[0]) { send_output("Usage: remove <name>", "err"); return; }
        do_remove(name);
    }
    else if (str_eq(cmd, "installed")) {
        show_installed();
    }
    else {
        char msg[128] = "Unknown command: ";
        str_cat(msg, cmd, sizeof(msg));
        str_cat(msg, ". Type 'help'.", sizeof(msg));
        send_output(msg, "err");
    }
}

/* ── Module entry points ─────────────────────────────────────────────── */

void module_init(void) {
    hal_log(1, "[install] init", 14);
    load_sources();
    send_sources_list();
    /* Emit the saved view mode so the host renders the catalog in
     * the user's last-chosen layout. */
    load_and_send_layout();

    if (source_count > 0) {
        begin_fetch_all();
    }
}

void module_tick(void) {
    /* Check for pending HTTP response */
    if (pending_req >= 0) {
        int32_t status = hal_http_poll(pending_req);
        if (status == 0) return; /* still pending */

        if (status < 0) {
            char msg[96] = "HTTP request failed for ";
            str_cat(msg, fetch_current_host, sizeof(msg));
            send_output(msg, "err");
            hal_log(2, "[install] http poll <0", 22);
            push_status_card("__http_err",
                             "HTTP request failed",
                             fetch_current_host);
            hal_http_free(pending_req);
            pending_req = -1;
            advance_fetch_queue();
            return;
        }

        int32_t code = hal_http_status(pending_req);
        if (code < 200 || code >= 300) {
            char msg[96] = "HTTP error ";
            char code_buf[16];
            u64_to_str((uint64_t)(code > 0 ? code : 0), code_buf, sizeof(code_buf));
            str_cat(msg, code_buf, sizeof(msg));
            str_cat(msg, " from ", sizeof(msg));
            str_cat(msg, fetch_current_host, sizeof(msg));
            send_output(msg, "err");
            hal_log(2, msg, str_len(msg));
            push_status_card("__http_err", msg, fetch_current_host);
            hal_http_free(pending_req);
            pending_req = -1;
            advance_fetch_queue();
            return;
        }

        int32_t n = hal_http_read_response(pending_req, index_buf,
                                            sizeof(index_buf) - 1);
        hal_http_free(pending_req);
        pending_req = -1;

        if (n <= 0) {
            send_output("  empty response", "err");
            hal_log(2, "[install] http empty body", 25);
            push_status_card("__http_err",
                             "Empty response from source",
                             fetch_current_host);
            advance_fetch_queue();
            return;
        }
        index_buf[n] = '\0';
        int prev = catalog_count;
        parse_index(index_buf, (unsigned)n);
        char done[96] = "[install] parsed ";
        char nbuf[16];
        u64_to_str((uint64_t)(catalog_count - prev), nbuf, sizeof(nbuf));
        str_cat(done, nbuf, sizeof(done));
        str_cat(done, " entries from ", sizeof(done));
        str_cat(done, fetch_current_host, sizeof(done));
        hal_log(1, done, str_len(done));
        advance_fetch_queue();
    }
}

void module_handle_event(void) {
    /* Static (not stack) and large: the host's `wapp.index` catalog response
     * carries the whole index.json plus per-entry enrichment — several KB for a
     * dozen wapps. A small buffer silently dropped/truncated it, leaving the
     * store stuck on "Loading catalog...". 32 KB handles ~80 wapps. */
    static char buf[32768];
    if (hal_msg_available() == 0) return;
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = '\0';

    /* JSON messages: {"command":"..."} or {"type":"action","action":"save","fields":{...}} */
    if (buf[0] == '{') {
        /* Check for action (settings save) */
        const char *action_key = "\"action\":\"";
        const char *p = buf;
        while (*p) {
            int match = 1;
            unsigned akl = str_len(action_key);
            for (unsigned i = 0; i < akl; i++) {
                if (p[i] != action_key[i]) { match = 0; break; }
            }
            if (match) {
                p += akl;
                char action[32];
                unsigned ai = 0;
                while (*p && *p != '"' && ai < sizeof(action) - 1)
                    action[ai++] = *p++;
                action[ai] = '\0';

                if (str_eq(action, "view-list")) {
                    set_layout("list");
                    return;
                }
                if (str_eq(action, "view-grid")) {
                    set_layout("grid");
                    return;
                }
                if (str_eq(action, "refresh")) {
                    begin_fetch_all();
                    return;
                }
                if (str_eq(action, "open-sources")) {
                    /* Settings live behind the top-right menu now (no tab):
                     * open the hidden sources screen as a panel. */
                    const char *m =
                        "{\"type\":\"ui.screen.open\",\"name\":\"@screen.sources\","
                        "\"title\":\"Repositories\"}";
                    hal_msg_send(m, str_len(m));
                    return;
                }
                /* Generic action prefix: "install:<slug>" → run
                 * the catalog's install for that wapp. The card's
                 * action button emits this when tapped. */
                if (str_starts(action, "install:")) {
                    do_install(action + 8);
                    return;
                }
                if (str_eq(action, "set_sources")) {
                    /* The host already validated each URL and hands
                     * us a pre-joined newline-separated list in the
                     * "source" field. The value is a JSON string
                     * that may contain escaped newlines (\\n); un-
                     * escape those back to real newlines before
                     * persisting. */
                    const char *src_key = "\"source\":\"";
                    const char *q = buf;
                    while (*q) {
                        int m = 1;
                        unsigned skl = str_len(src_key);
                        for (unsigned i = 0; i < skl; i++) {
                            if (q[i] != src_key[i]) { m = 0; break; }
                        }
                        if (m) {
                            q += skl;
                            unsigned si = 0;
                            while (*q && *q != '"' &&
                                   si < sizeof(sources_raw) - 1) {
                                if (*q == '\\' && *(q + 1)) {
                                    q++;
                                    if (*q == 'n') sources_raw[si++] = '\n';
                                    else if (*q == 't') sources_raw[si++] = '\t';
                                    else if (*q == 'r') sources_raw[si++] = '\r';
                                    else sources_raw[si++] = *q;
                                    q++;
                                } else {
                                    sources_raw[si++] = *q++;
                                }
                            }
                            sources_raw[si] = '\0';
                            save_sources();
                            send_sources_list();
                            if (source_count > 0) {
                                begin_fetch_all();
                            } else {
                                catalog_count = 0;
                                show_catalog();
                            }
                            return;
                        }
                        q++;
                    }
                    /* save with no source field → just ack */
                    send_sources_list();
                    return;
                }
                return;
            }
            p++;
        }

        /* Check for wapp.installed confirmation from renderer */
        {
            const char *inst_key = "\"wapp.installed\"";
            const char *ip = buf;
            while (*ip) {
                int im = 1;
                unsigned ikl = str_len(inst_key);
                for (unsigned ii = 0; ii < ikl; ii++) {
                    if (ip[ii] != inst_key[ii]) { im = 0; break; }
                }
                if (im) {
                    /* Extract name and version */
                    char iname[64] = "", iver[32] = "";
                    json_find_str(buf, buf + n, "name", iname, sizeof(iname));
                    json_find_str(buf, buf + n, "version", iver, sizeof(iver));
                    if (iname[0] && iver[0]) {
                        set_installed_version(iname, iver);
                    }
                    return;
                }
                ip++;
            }
        }

        /* Check for wapp.index response from renderer */
        const char *idx_key = "\"wapp.index\"";
        const char *tp = buf;
        while (*tp) {
            int tm = 1;
            unsigned tkl = str_len(idx_key);
            for (unsigned ti = 0; ti < tkl; ti++) {
                if (tp[ti] != idx_key[ti]) { tm = 0; break; }
            }
            if (tm) {
                /* Find "data":" and extract the JSON array */
                const char *dk = "\"data\":";
                const char *dq = buf;
                while (*dq) {
                    int dm = 1;
                    unsigned dkl = str_len(dk);
                    for (unsigned di = 0; di < dkl; di++) {
                        if (dq[di] != dk[di]) { dm = 0; break; }
                    }
                    if (dm) {
                        dq += dkl;
                        while (*dq == ' ') dq++;
                        /* The rest until end of outer object is the index JSON */
                        unsigned dlen = str_len(dq);
                        /* Strip trailing } from outer wrapper */
                        if (dlen > 0 && dq[dlen - 1] == '}') dlen--;
                        if (dlen > 0 && dlen < sizeof(index_buf)) {
                            for (unsigned i = 0; i < dlen; i++)
                                index_buf[i] = dq[i];
                            index_buf[dlen] = '\0';
                            parse_index(index_buf, dlen);
                        }
                        advance_fetch_queue();
                        return;
                    }
                    dq++;
                }
                advance_fetch_queue();
                return;
            }
            tp++;
        }

        /* Check for command field */
        const char *key = "\"command\":\"";
        p = buf;
        while (*p) {
            int match = 1;
            unsigned kl = str_len(key);
            for (unsigned i = 0; i < kl; i++) {
                if (p[i] != key[i]) { match = 0; break; }
            }
            if (match) {
                p += kl;
                char cmd[512];
                unsigned ci = 0;
                while (*p && *p != '"' && ci < sizeof(cmd) - 1) {
                    if (*p == '\\' && *(p + 1)) { p++; cmd[ci++] = *p++; }
                    else { cmd[ci++] = *p++; }
                }
                cmd[ci] = '\0';
                dispatch(cmd);
                return;
            }
            p++;
        }
    }

    /* Plain text fallback */
    dispatch(buf);
}

void module_destroy(void) {
    if (pending_req >= 0) {
        hal_http_free(pending_req);
        pending_req = -1;
    }
    hal_log(1, "[install] destroy", 17);
}

uint32_t module_tick_interval_ms(void) { return 500; }
