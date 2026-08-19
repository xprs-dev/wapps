/*
 * tools.xprs.movies — local video player wapp.
 *
 * The wapp itself is intentionally thin: video decode + rendering
 * happen in the host (media_kit on Flutter desktop/mobile). This
 * module only:
 *   - Receives the file path from the host's file-association launch
 *     path (a `file.open` message after init — see Section 18 of
 *     wapp-interfaces.md).
 *   - Persists the last-played path in KV so re-opening the wapp
 *     resumes the same file.
 *   - Forwards a single `video.load` message to the host's
 *     <group $type="video"> renderer, which runs a real player.
 *   - Surfaces simple play/pause/seek commands from the GeoUI
 *     screen as `video.play` / `video.pause` / `video.seek`
 *     messages so the host can drive the Player.
 *
 * Anything that needs hardware acceleration — codec selection,
 * frame timing, audio output, subtitle rendering — is the host's
 * job. Iteration 2 will add SRT auto-attach + a `video.subtitle`
 * message for sidecar tracks.
 *
 * Build: cd wapps && make movies
 */

#include "../hal/xprs_wasm_hal.h"

/* ── Tiny string helpers (no libc) ───────────────────────────── */

static unsigned str_len(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_eq_n(const char *a, const char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Find the substring needle in haystack; return pointer or NULL. */
static const char *str_find(const char *hay, const char *needle) {
    unsigned nl = str_len(needle);
    if (nl == 0) return hay;
    for (const char *p = hay; *p; p++) {
        if (str_eq_n(p, needle, nl)) return p;
    }
    return 0;
}

/* Append `src` (literal, NUL-terminated) into `dst` of capacity `cap`,
 * leaving room for the trailing NUL. */
static void str_cat(char *dst, const char *src, unsigned cap) {
    unsigned len = str_len(dst);
    unsigned i = 0;
    while (len + i + 1 < cap && src[i]) { dst[len + i] = src[i]; i++; }
    dst[len + i] = '\0';
}

/* Append `src` of length `n` into `dst` of capacity `cap`, but
 * JSON-escape any " and \ characters along the way so the
 * concatenation is safe to drop into a "..." JSON string. */
static void str_cat_json(char *dst, const char *src, unsigned n, unsigned cap) {
    unsigned len = str_len(dst);
    for (unsigned i = 0; i < n; i++) {
        char c = src[i];
        if (len + 3 >= cap) break;
        if (c == '"' || c == '\\') { dst[len++] = '\\'; }
        dst[len++] = c;
    }
    dst[len] = '\0';
}

/* ── Tiny JSON field reader ──────────────────────────────────── */

/*
 * Read a JSON string value: given input like
 *   {"path":"/foo/bar.mp4","mode":"view"}
 * read_string_field(buf, "path", out, cap) writes "/foo/bar.mp4"
 * into out (NUL-terminated). Returns the number of bytes written,
 * 0 when the key was missing or the value wasn't a string. Does
 * not handle nested escapes beyond \" and \\ — wapps are sandboxed
 * and the host is the one producing these messages, so the input
 * is well-formed.
 */
static unsigned read_string_field(
    const char *buf, const char *key, char *out, unsigned cap) {
    /* Build the lookup token: "<key>": */
    char token[64] = "\"";
    str_cat(token, key, sizeof(token));
    str_cat(token, "\":\"", sizeof(token));

    const char *p = str_find(buf, token);
    if (!p) { out[0] = '\0'; return 0; }
    p += str_len(token);

    unsigned i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i;
}

/* Get the value of "type" out of a JSON message. Returns its
 * length. */
static unsigned read_type(const char *buf, char *out, unsigned cap) {
    return read_string_field(buf, "type", out, cap);
}

/* ── State (mirrored to KV so reopening resumes) ──────────────── */

static char  current_path[512];
static int   has_path = 0;

static void load_state(void) {
    int n = hal_kv_get("video.path", 10, current_path, sizeof(current_path) - 1);
    if (n > 0) {
        current_path[n] = '\0';
        has_path = 1;
    }
}

static void save_path(const char *path, unsigned len) {
    if (len >= sizeof(current_path)) len = sizeof(current_path) - 1;
    for (unsigned i = 0; i < len; i++) current_path[i] = path[i];
    current_path[len] = '\0';
    has_path = 1;
    hal_kv_set("video.path", 10, current_path, len);
}

/* ── Outgoing messages to the host video group ────────────────── */

static void send_video_load(const char *path, unsigned plen, int autoplay) {
    char msg[640] = "{\"type\":\"video.load\",\"path\":\"";
    str_cat_json(msg, path, plen, sizeof(msg));
    str_cat(msg, autoplay ? "\",\"autoplay\":true}" : "\",\"autoplay\":false}",
            sizeof(msg));
    hal_msg_send(msg, str_len(msg));
}

static void send_video_subtitle(const char *path, unsigned plen) {
    char msg[640] = "{\"type\":\"video.subtitle\",\"path\":\"";
    str_cat_json(msg, path, plen, sizeof(msg));
    str_cat(msg, "\"}", sizeof(msg));
    hal_msg_send(msg, str_len(msg));
}

/* Quick check for a path's extension, case-insensitive. Returns 1
 * when [path] ends with ".<ext>" (where ext is short, lowercase). */
static int path_has_ext(const char *path, unsigned plen, const char *ext) {
    unsigned el = str_len(ext);
    if (plen < el + 1) return 0;
    if (path[plen - el - 1] != '.') return 0;
    for (unsigned i = 0; i < el; i++) {
        char a = path[plen - el + i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (a != ext[i]) return 0;
    }
    return 1;
}

/* True iff the path looks like a subtitle file. */
static int is_subtitle_path(const char *path, unsigned plen) {
    return path_has_ext(path, plen, "srt") ||
           path_has_ext(path, plen, "vtt") ||
           path_has_ext(path, plen, "ass") ||
           path_has_ext(path, plen, "ssa") ||
           path_has_ext(path, plen, "sub");
}

static void send_simple(const char *type) {
    char msg[64] = "{\"type\":\"";
    str_cat(msg, type, sizeof(msg));
    str_cat(msg, "\"}", sizeof(msg));
    hal_msg_send(msg, str_len(msg));
}

static void send_seek(long long ms) {
    char num[24]; unsigned i = 0;
    long long v = ms < 0 ? -ms : ms;
    char tmp[24]; unsigned t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 23) { tmp[t++] = '0' + (int)(v % 10); v /= 10; }
    if (ms < 0) num[i++] = '-';
    while (t > 0 && i < 23) num[i++] = tmp[--t];
    num[i] = '\0';

    char msg[80] = "{\"type\":\"video.seek\",\"position_ms\":";
    str_cat(msg, num, sizeof(msg));
    str_cat(msg, "}", sizeof(msg));
    hal_msg_send(msg, str_len(msg));
}

/* Ask the host to show a native file picker filtered to the given
 * extensions. The host replies with a single `file.open` message
 * carrying the chosen path; module_handle_event already routes that
 * through save_path + send_video_load (or send_video_subtitle for
 * subtitle types). The "kind" hint lets the host pick a sensible
 * dialog title without having to parse the extension list. */
static void send_file_pick_video(void) {
    const char *msg =
        "{\"type\":\"file.pick\","
        "\"kind\":\"video\","
        "\"title\":\"Pick a video\","
        "\"extensions\":["
            "\"mp4\",\"m4v\",\"mkv\",\"webm\",\"mov\",\"avi\","
            "\"flv\",\"wmv\",\"mpeg\",\"mpg\",\"ogv\","
            "\"ts\",\"mts\",\"m2ts\",\"3gp\""
        "]}";
    hal_msg_send(msg, str_len(msg));
}

static void send_file_pick_subtitle(void) {
    const char *msg =
        "{\"type\":\"file.pick\","
        "\"kind\":\"subtitle\","
        "\"title\":\"Pick a subtitle\","
        "\"extensions\":["
            "\"srt\",\"vtt\",\"ass\",\"ssa\",\"sub\""
        "]}";
    hal_msg_send(msg, str_len(msg));
}

/* ── Command dispatch ─────────────────────────────────────────── */

static void on_command(const char *cmd) {
    if (str_eq(cmd, "play"))         send_simple("video.play");
    else if (str_eq(cmd, "pause"))   send_simple("video.pause");
    else if (str_eq(cmd, "stop"))    send_simple("video.stop");
    else if (str_eq(cmd, "back10"))  send_seek(-10000);  /* relative — host applies */
    else if (str_eq(cmd, "fwd10"))   send_seek(10000);
    else if (str_eq(cmd, "pick_video"))    send_file_pick_video();
    else if (str_eq(cmd, "pick_subtitle")) send_file_pick_subtitle();
    else if (str_eq(cmd, "reload")) {
        if (has_path) send_video_load(current_path, str_len(current_path), 1);
    }
}

/* ── Lifecycle ────────────────────────────────────────────────── */

void module_init(void) {
    hal_log(1, "[movies] init", 13);
    load_state();
    /* If we already had a video from a previous session, push it
     * to the host so the player reopens at the same file. */
    if (has_path) {
        send_video_load(current_path, str_len(current_path), 0);
    }
}

void module_tick(void) {
    /* Nothing periodic — the host's player drives its own clock. */
}

void module_handle_event(void) {
    char buf[1024];
    while (hal_msg_available() > 0) {
        uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
        if (n == 0) break;
        buf[n] = '\0';

        char type[32];
        unsigned tlen = read_type(buf, type, sizeof(type));

        if (tlen > 0 && str_eq(type, "file.open")) {
            char path[512];
            unsigned plen = read_string_field(buf, "path", path, sizeof(path));
            if (plen == 0) { hal_log(2, "[movies] file.open missing path", 31); continue; }

            // Subtitle file → attach to the currently-playing video
            // (the host swaps the active SubtitleTrack). When no
            // video is loaded yet we just persist the SRT path so a
            // later video.load will pair with it.
            if (is_subtitle_path(path, plen)) {
                hal_kv_set("video.subtitle_path", 19, path, plen);
                if (has_path) {
                    send_video_subtitle(path, plen);
                } else {
                    hal_log(1, "[movies] subtitle stashed (no video yet)", 41);
                }
                continue;
            }

            save_path(path, plen);
            send_video_load(path, plen, 1);

            // If the user previously stashed a subtitle path while
            // no video was loaded, attach it now.
            char sub[512];
            int slen = hal_kv_get("video.subtitle_path", 19, sub, sizeof(sub) - 1);
            if (slen > 0) {
                send_video_subtitle(sub, (unsigned)slen);
                hal_kv_delete("video.subtitle_path", 19);
            }
            continue;
        }

        /* Action buttons in the GeoUI screen arrive as
         * {"type":"action","action":"<name>"} — same convention as
         * the install/maps wapps. */
        if (tlen > 0 && str_eq(type, "action")) {
            char act[32];
            read_string_field(buf, "action", act, sizeof(act));
            if (act[0]) on_command(act);
            continue;
        }

        /* Bare {"command":"<name>"} from older renderers. */
        char cmd[32];
        unsigned clen = read_string_field(buf, "command", cmd, sizeof(cmd));
        if (clen > 0) on_command(cmd);
    }
}

void module_destroy(void) {
    hal_log(1, "[movies] destroy", 16);
}

uint32_t module_tick_interval_ms(void) { return 0; }
