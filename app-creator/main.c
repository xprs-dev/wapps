/*
 * tools.xprs.app-creator — in-app wapp authoring.
 *
 * Projects tab lists every installed wapp by asking the host for the
 * shared archive contents (wapps.list_installed). Picking a card
 * populates the metadata form via ui.set_field; New project clears
 * those same fields.
 *
 * Compile and Install run entirely inside the wapp using only generic
 * HAL primitives:
 *   - hal_file_open / hal_file_write — stage source files to a build
 *     directory under /tmp/xprs-wapp-build/<slug>/.
 *   - hal_process_exec — spawn /bin/sh to resolve WASI_SDK_PATH and
 *     run clang against the staged main.c, then later spawn zip to
 *     pack the build dir into a .wapp ZIP.
 *   - {"type":"wapp.install"} message — the same generic install
 *     primitive the Wapp Store uses, pointed at the local ZIP.
 *
 * Run tests forwards a tests.run outbox message and surfaces the
 * tests.case + tests.complete echoes back into the output log.
 *
 * Wire protocol — wapp → host:
 *   {"type":"wapps.list_installed","req_id":1}
 *   {"type":"ui.set_field","name":"<field>","value":"<v>"}
 *   {"type":"ui.data","target":"projects","items":[...]}
 *   {"type":"ui.log.append","name":"output","text":"..."}
 *   {"type":"wapp.install","source":"<dir>","file":"<slug>.wapp",
 *    "name":"<slug>","version":"<v>"}
 *   {"type":"tests.run","req_id":1,"target":"<wapp_id>"}
 *
 * Wire protocol — host → wapp:
 *   {"type":"wapps.list_installed.response","items":[...]}
 *   {"type":"action","action":"<name>"}
 *   {"type":"wapp.installed","name":"...","version":"..."}
 *   {"type":"tests.case","suite":"...","name":"...","passed":true,...}
 *   {"type":"tests.complete","error":null}
 *
 * Build: WASI_SDK_PATH=$HOME/wasi-sdk make
 */

#include "../hal/xprs_wasm_hal.h"

/* ── Minimal string helpers (no libc) ─────────────────────────────── */

static unsigned str_len(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq_n(const char *a, const char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int find_substr(const char *hay, unsigned hlen, const char *needle) {
    unsigned nl = str_len(needle);
    if (nl == 0 || hlen < nl) return -1;
    for (unsigned i = 0; i + nl <= hlen; i++) {
        if (str_eq_n(hay + i, needle, nl)) return (int)i;
    }
    return -1;
}

static void append_range(char *dst, unsigned max, unsigned *pos,
                         const char *src, unsigned slen) {
    for (unsigned i = 0; i < slen && *pos + 1 < max; i++) {
        dst[(*pos)++] = src[i];
    }
}

static void append_cstr(char *dst, unsigned max, unsigned *pos, const char *s) {
    append_range(dst, max, pos, s, str_len(s));
}

/*
 * Find `"<key>":"...value..."` in [hay, hay+hlen) and copy the
 * still-JSON-escaped value into out. Re-embedding it in another JSON
 * message round-trips correctly. Returns bytes written, or -1 if the
 * key was not found. Always null-terminates.
 */
static int extract_json_string_field(
    const char *hay, unsigned hlen,
    const char *key,
    char *out, unsigned outmax
) {
    char token[64];
    unsigned tp = 0;
    token[tp++] = '"';
    const unsigned kl = str_len(key);
    for (unsigned i = 0; i < kl && tp + 3 < sizeof(token); i++) {
        token[tp++] = key[i];
    }
    token[tp++] = '"';
    token[tp++] = ':';
    token[tp++] = '"';
    token[tp] = '\0';

    const int found = find_substr(hay, hlen, token);
    if (found < 0) {
        if (outmax > 0) out[0] = '\0';
        return -1;
    }

    unsigned i = (unsigned)found + tp;
    unsigned op = 0;
    while (i < hlen && op + 1 < outmax) {
        const char c = hay[i];
        if (c == '\\' && i + 1 < hlen) {
            if (op + 2 >= outmax) break;
            out[op++] = c;
            out[op++] = hay[i + 1];
            i += 2;
        } else if (c == '"') {
            break;
        } else {
            out[op++] = c;
            i++;
        }
    }
    out[op] = '\0';
    return (int)op;
}

/* Match the matching close brace for an open at start. Tracks string
 * boundaries so braces inside JSON strings don't throw off the count. */
static int find_close_brace(const char *hay, unsigned hlen, unsigned start) {
    int depth = 0;
    int in_string = 0;
    for (unsigned i = start; i < hlen; i++) {
        const char c = hay[i];
        if (in_string) {
            if (c == '\\' && i + 1 < hlen) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') in_string = 1;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return (int)i;
        }
    }
    return -1;
}

/* ── Installed-wapps cache ───────────────────────────────────────────
 * The wapps.list_installed.response is parsed once per refresh and
 * stored here so action handlers can populate form fields without
 * re-asking the host. Field strings are kept in their JSON-escaped
 * form so they can be spliced back into outgoing JSON messages
 * verbatim. */

#define MAX_WAPPS 32
#define SLUG_LEN  64
#define TEXT_LEN  256
#define LONG_LEN  512

static char wapps_slug   [MAX_WAPPS][SLUG_LEN];
static char wapps_id     [MAX_WAPPS][TEXT_LEN];
static char wapps_title  [MAX_WAPPS][TEXT_LEN];
static char wapps_version[MAX_WAPPS][SLUG_LEN];
static char wapps_desc   [MAX_WAPPS][LONG_LEN];
static char wapps_summary[MAX_WAPPS][LONG_LEN];
static char wapps_icon   [MAX_WAPPS][TEXT_LEN];
static unsigned wapp_count = 0;

static void copy_field(const char *src, int slen, char *dst, unsigned dmax) {
    unsigned n = slen > 0 ? (unsigned)slen : 0;
    if (n >= dmax) n = dmax - 1;
    for (unsigned i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

/* Parse a wapps.list_installed response into the cache. The items
 * array is scanned object-by-object using brace matching. Each
 * object's fields are extracted via the same JSON-string helper used
 * elsewhere in the file. */
static void parse_list_response(const char *hay, unsigned hlen) {
    wapp_count = 0;
    const int items_at = find_substr(hay, hlen, "\"items\":[");
    if (items_at < 0) return;
    unsigned i = (unsigned)items_at + 9;
    while (i < hlen && wapp_count < MAX_WAPPS) {
        while (i < hlen && hay[i] != '{' && hay[i] != ']') i++;
        if (i >= hlen || hay[i] == ']') break;
        const unsigned obj_start = i;
        const int close_at = find_close_brace(hay, hlen, obj_start);
        if (close_at < 0) break;
        const unsigned obj_len = (unsigned)close_at - obj_start + 1;
        const char *obj = hay + obj_start;

        char tmp[LONG_LEN];
        int n;
        n = extract_json_string_field(obj, obj_len, "id", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_id[wapp_count], TEXT_LEN);
        n = extract_json_string_field(obj, obj_len, "name", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_slug[wapp_count], SLUG_LEN);
        n = extract_json_string_field(obj, obj_len, "title", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_title[wapp_count], TEXT_LEN);
        n = extract_json_string_field(obj, obj_len, "version", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_version[wapp_count], SLUG_LEN);
        n = extract_json_string_field(obj, obj_len, "description",
                                      tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_desc[wapp_count], LONG_LEN);
        n = extract_json_string_field(obj, obj_len, "summary",
                                      tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_summary[wapp_count], LONG_LEN);
        n = extract_json_string_field(obj, obj_len, "icon", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_icon[wapp_count], TEXT_LEN);

        wapp_count++;
        i = (unsigned)close_at + 1;
    }
}

static int find_wapp_by_slug(const char *slug, unsigned slen) {
    for (unsigned k = 0; k < wapp_count; k++) {
        const unsigned ml = str_len(wapps_slug[k]);
        if (ml != slen) continue;
        if (str_eq_n(wapps_slug[k], slug, slen)) return (int)k;
    }
    return -1;
}

/* ── Outbox helpers ───────────────────────────────────────────────── */

static void send_list_installed(void) {
    const char *m = "{\"type\":\"wapps.list_installed\",\"req_id\":1}";
    hal_msg_send(m, str_len(m));
}

/* Push a value into a named form field. The value is expected to be
 * already JSON-escaped (as it is when extracted from an inbox payload
 * via extract_json_string_field). */
static void send_set_field(const char *name, const char *value) {
    /* Sized for source.c payloads; metadata fields fit easily.
     * Forum's main.c is ~46 KB; install's is ~40 KB; widen to 128 KB
     * so any wapp's source rounds-trips through the editor. */
    static char buf[128 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.set_field\",\"name\":\"");
    append_cstr(buf, sizeof(buf), &op, name);
    append_cstr(buf, sizeof(buf), &op, "\",\"value\":\"");
    append_cstr(buf, sizeof(buf), &op, value);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

/* Render the cached wapps as cards in the projects group. Each card
 * carries a single "Edit" action whose name is "select:<slug>" so the
 * inbox handler knows which wapp to populate the form with. */
static void render_projects(void) {
    static char buf[16 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
        "{\"type\":\"ui.data\",\"target\":\"projects\",\"items\":[");
    for (unsigned k = 0; k < wapp_count; k++) {
        if (k) append_cstr(buf, sizeof(buf), &op, ",");
        append_cstr(buf, sizeof(buf), &op, "{\"id\":\"");
        append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"title\":\"");
        if (wapps_title[k][0]) {
            append_cstr(buf, sizeof(buf), &op, wapps_title[k]);
        } else {
            append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        }
        append_cstr(buf, sizeof(buf), &op, "\",\"subtitle\":\"v");
        append_cstr(buf, sizeof(buf), &op, wapps_version[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"description\":\"");
        if (wapps_summary[k][0]) {
            append_cstr(buf, sizeof(buf), &op, wapps_summary[k]);
        } else {
            append_cstr(buf, sizeof(buf), &op, wapps_desc[k]);
        }
        append_cstr(buf, sizeof(buf), &op, "\",\"icon_path\":\"wapp:");
        append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        append_cstr(buf, sizeof(buf), &op,
            "\",\"actions\":[{\"name\":\"select:");
        append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        append_cstr(buf, sizeof(buf), &op,
            "\",\"label\":\"Edit\",\"icon\":\"edit\"}]}");
    }
    append_cstr(buf, sizeof(buf), &op, "]}");
    hal_msg_send(buf, op);
}

/* ── Action dispatch ──────────────────────────────────────────────── */

/* Forward declarations — defined further down. */
static void select_screen(const char *name);
static void send_read_source(const char *slug, unsigned slen);
static void trans_load(const char *slug, unsigned slen);
static void render_files_tree(void);
static void load_active_file_into_editor(void);
static uint32_t kv_read(const char *key, char *out, uint32_t omax);
static void escape_into(char *dst, unsigned dmax, unsigned *op,
                        const char *src, uint32_t slen);

/* Files tab state. Two source files are listed and switchable: main.c
 * and screens/home.ui.json. Each has a shadow KV slot so edits
 * survive switching between files. en.json lives on the Translations
 * tab and stays bound to its own source_lang field. */
static const char *FILE_MAIN = "main.c";
static const char *FILE_UI   = "screens/home.ui.json";
static const char *KV_BUF_MAIN = "_buf_main";
static const char *KV_BUF_UI   = "_buf_ui";

/* Save whatever is currently in the source field into the shadow
 * KV slot owned by the active file. Called before flipping which
 * file the editor shows so the user's in-progress edits survive. */
static void persist_editor_to_active_buffer(void) {
    static char active[64];
    uint32_t an = hal_kv_get("active_file", 11, active, sizeof(active) - 1);
    active[an] = '\0';
    static char src[96 * 1024];
    uint32_t sn = hal_kv_get("source", 6, src, sizeof(src) - 1);
    src[sn] = '\0';
    const char *slot = (an > 0 && str_eq_n(active, FILE_UI, an))
                       ? KV_BUF_UI : KV_BUF_MAIN;
    hal_kv_set(slot, str_len(slot), src, sn);
}

static void on_select(const char *slug, unsigned slen) {
    const int idx = find_wapp_by_slug(slug, slen);
    if (idx < 0) return;
    send_set_field("wapp_title",       wapps_title[idx]);
    send_set_field("wapp_name",        wapps_slug[idx]);
    send_set_field("wapp_id",          wapps_id[idx]);
    send_set_field("wapp_version",     wapps_version[idx]);
    send_set_field("wapp_description",
                   wapps_summary[idx][0] ? wapps_summary[idx]
                                         : wapps_desc[idx]);
    send_read_source(wapps_slug[idx], str_len(wapps_slug[idx]));
    trans_load(wapps_slug[idx], str_len(wapps_slug[idx]));
    select_screen("Files");
}

static void on_new_project(void) {
    send_set_field("wapp_title",       "");
    send_set_field("wapp_name",        "");
    send_set_field("wapp_id",          "");
    send_set_field("wapp_version",     "0.1.0");
    send_set_field("wapp_description", "");
    send_set_field("source_lang",      "");
    /* Clear shadow buffers + the editor; default active = main.c. */
    hal_kv_set(KV_BUF_MAIN, str_len(KV_BUF_MAIN), "", 0);
    hal_kv_set(KV_BUF_UI,   str_len(KV_BUF_UI),   "", 0);
    hal_kv_set("active_file", 11, FILE_MAIN, str_len(FILE_MAIN));
    send_set_field("source", "");
    send_set_field("active_file_label", FILE_MAIN);
    render_files_tree();
    select_screen("Files");
}

/* Switch which file the editor shows. Persists the current editor
 * content first, then loads the new file's shadow buffer. */
static void on_pick_file(const char *path, unsigned plen) {
    persist_editor_to_active_buffer();
    hal_kv_set("active_file", 11, path, plen);
    static char label[80];
    unsigned ln = plen < sizeof(label) - 1 ? plen : sizeof(label) - 1;
    for (unsigned i = 0; i < ln; i++) label[i] = path[i];
    label[ln] = '\0';
    send_set_field("active_file_label", label);
    load_active_file_into_editor();
    render_files_tree();
}

/* Read the form state from KV (the host's binding layer mirrors form
 * fields into the wapp's KV automatically) and orchestrate the
 * compile / install pipeline locally via hal_file_* and
 * hal_process_exec. */

#define BUILD_ROOT "/tmp/xprs-wapp-build"

/* Active host-process state for compile / install. The wapp polls
 * these in module_tick once a task is in flight. */
enum { TASK_NONE = 0, TASK_COMPILE = 1, TASK_INSTALL_ZIP = 2 };
static int  active_kind   = TASK_NONE;
static int32_t active_handle = -1;
static char active_slug[80] = "";
static char active_version[32] = "";

/* Append a single line of text to the wapp's output log via the
 * generic ui.log.append primitive. text is plain (not JSON-escaped);
 * we escape on the way out. */
static void emit_log(const char *text, unsigned tlen) {
    static char buf[8 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.log.append\",\"name\":\"output\","
                "\"text\":\"");
    escape_into(buf, sizeof(buf), &op, text, tlen);
    append_cstr(buf, sizeof(buf), &op, "\\n\"}");
    hal_msg_send(buf, op);
}

/* Show a transient toast via the generic ui.snackbar primitive. level
 * is one of "info" / "success" / "warn" / "error". */
static void emit_snackbar(const char *text, const char *level) {
    static char buf[2048];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op, "{\"type\":\"ui.snackbar\",\"text\":\"");
    escape_into(buf, sizeof(buf), &op, text, str_len(text));
    append_cstr(buf, sizeof(buf), &op, "\",\"level\":\"");
    append_cstr(buf, sizeof(buf), &op, level);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}
static void emit_log_cstr(const char *text) {
    emit_log(text, str_len(text));
}

/* Append raw bytes from a process pipe straight into the log. The
 * pipe output already contains its own newlines, so we don't add one;
 * we still JSON-escape so embedded quotes / backslashes round-trip. */
static void emit_log_raw(const char *bytes, unsigned blen) {
    if (blen == 0) return;
    static char buf[16 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.log.append\",\"name\":\"output\","
                "\"text\":\"");
    escape_into(buf, sizeof(buf), &op, bytes, blen);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

/* Build directory path: BUILD_ROOT "/" slug "/" tail. tail may be empty
 * for just the dir itself. Returns bytes written (excluding NUL). */
static unsigned build_path(char *out, unsigned omax,
                           const char *slug, unsigned slen,
                           const char *tail) {
    unsigned op = 0;
    append_cstr(out, omax, &op, BUILD_ROOT "/");
    append_range(out, omax, &op, slug, slen);
    if (tail && tail[0]) {
        append_cstr(out, omax, &op, "/");
        append_cstr(out, omax, &op, tail);
    }
    if (op < omax) out[op] = '\0';
    return op;
}

/* Stage one file under BUILD_ROOT/<slug>/<rel> with the given byte
 * content. Returns 1 on success, 0 on failure (logs the error). */
static int stage_file(const char *slug, unsigned slen,
                      const char *rel,
                      const char *content, uint32_t clen) {
    char path[320];
    build_path(path, sizeof(path), slug, slen, rel);
    int32_t h = hal_file_open(path, str_len(path), 1 /* write */);
    if (h < 0) {
        char msg[400] = "compile: cannot open ";
        unsigned mp = str_len(msg);
        append_cstr(msg, sizeof(msg), &mp, path);
        append_cstr(msg, sizeof(msg), &mp, " for write");
        emit_log(msg, mp);
        return 0;
    }
    if (clen > 0) {
        hal_file_write(h, content, clen);
    }
    hal_file_close(h);
    return 1;
}

/* Spawn /bin/sh -c <script>, return the handle or -1. The script
 * argument is JSON-escaped on the way into argv_json. */
static int32_t spawn_shell(const char *script) {
    static char argv_json[16 * 1024];
    unsigned op = 0;
    append_cstr(argv_json, sizeof(argv_json), &op,
                "[\"/bin/sh\",\"-c\",\"");
    escape_into(argv_json, sizeof(argv_json), &op,
                script, str_len(script));
    append_cstr(argv_json, sizeof(argv_json), &op, "\"]");
    return hal_process_exec(argv_json, op, "", 0);
}

/* Persist the editor + read source/slug, then stage main.c and kick
 * off clang via /bin/sh so the shell can resolve $WASI_SDK_PATH and
 * locate wapps/hal/. State machine in module_tick streams output and
 * picks up the exit code. */
static void do_compile(void) {
    if (active_kind != TASK_NONE) {
        emit_log_cstr("compile: another task is still running");
        return;
    }
    static char source_buf[96 * 1024];
    static char slug_buf[80];
    persist_editor_to_active_buffer();
    uint32_t n = hal_kv_get(KV_BUF_MAIN, str_len(KV_BUF_MAIN),
                            source_buf, sizeof(source_buf) - 1);
    if (n == 0) {
        emit_log_cstr("compile: source is empty");
        return;
    }
    source_buf[n] = '\0';
    uint32_t sn = kv_read("wapp_name", slug_buf, sizeof(slug_buf));
    if (sn == 0) {
        emit_log_cstr("compile: set the folder slug (Settings → Name) first");
        return;
    }
    if (!stage_file(slug_buf, sn, "main.c", source_buf, n)) return;

    /* Build the compile script. Resolves the SDK and HAL include
     * directory, then execs clang. The wapp doesn't know $HOME or
     * the user's xprs checkout location, so the shell does the
     * lookup. */
    static char build_dir[256];
    build_path(build_dir, sizeof(build_dir), slug_buf, sn, "");

    static char script[4096];
    unsigned op = 0;
    append_cstr(script, sizeof(script), &op,
        "set -e; "
        "SDK=\"${WASI_SDK_PATH:-$HOME/wasi-sdk}\"; "
        "[ -x \"$SDK/bin/clang\" ] || { "
        "  echo \"clang not found at $SDK/bin/clang\" >&2; exit 1; }; "
        "HAL=\"\"; "
        "for d in \"$HOME/code/xprs/wapps/hal\" "
        "         \"/usr/local/share/xprs/wapps/hal\" "
        "         \"/opt/xprs/wapps/hal\"; do "
        "  [ -f \"$d/xprs_wasm_hal.h\" ] && HAL=\"$d\" && break; "
        "done; "
        "[ -n \"$HAL\" ] || { "
        "  echo \"xprs_wasm_hal.h not found on any known path\" >&2; "
        "  exit 1; }; "
        "BUILD=\"");
    append_cstr(script, sizeof(script), &op, build_dir);
    append_cstr(script, sizeof(script), &op,
        "\"; "
        "echo \"clang $BUILD/main.c (HAL=$HAL)\"; "
        "exec \"$SDK/bin/clang\" --target=wasm32-wasi -O2 -flto "
        "-I \"$HAL\" -Wall -Wextra -Werror -fno-exceptions -DNDEBUG "
        "-Wl,--no-entry "
        "-Wl,--export=module_init -Wl,--export=module_tick "
        "-Wl,--export=module_handle_event -Wl,--export=module_destroy "
        "-Wl,--export=module_tick_interval_ms "
        "-Wl,--strip-all -nostartfiles "
        "-o \"$BUILD/app.wasm\" \"$BUILD/main.c\"");

    int32_t h = spawn_shell(script);
    if (h < 0) {
        emit_log_cstr("compile: hal_process_exec returned -1");
        return;
    }
    active_kind   = TASK_COMPILE;
    active_handle = h;
    /* Remember slug so module_tick's exit handler can log paths. */
    unsigned cn = sn < sizeof(active_slug) - 1 ? sn : sizeof(active_slug) - 1;
    for (unsigned i = 0; i < cn; i++) active_slug[i] = slug_buf[i];
    active_slug[cn] = '\0';
    emit_log_cstr("compile: starting...");
}

/* Read field at key into out, return bytes written (0 if missing).
 * Always null-terminates. */
static uint32_t kv_read(const char *key, char *out, uint32_t omax) {
    uint32_t n = hal_kv_get(key, str_len(key), out, omax - 1);
    out[n] = '\0';
    return n;
}

/* JSON-escape src into dst at *op. Used by do_install for fields that
 * came from KV (where they sit in raw form). */
static void escape_into(char *dst, unsigned dmax, unsigned *op,
                        const char *src, uint32_t slen) {
    for (uint32_t i = 0; i < slen && *op + 8 < dmax; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[(*op)++] = '\\'; dst[(*op)++] = (char)c;
        } else if (c == '\n') { dst[(*op)++] = '\\'; dst[(*op)++] = 'n'; }
        else if (c == '\r') { dst[(*op)++] = '\\'; dst[(*op)++] = 'r'; }
        else if (c == '\t') { dst[(*op)++] = '\\'; dst[(*op)++] = 't'; }
        else if (c < 0x20) { /* drop */ }
        else { dst[(*op)++] = (char)c; }
    }
}

/* Stage all install artefacts under BUILD_ROOT/<slug>/, generate the
 * manifest.json from the form fields, then spawn zip to pack the
 * directory into BUILD_ROOT/<slug>.wapp. The state machine in
 * module_tick takes over once zip is running and ultimately sends
 * the generic wapp.install message at the end of this pipeline. */
static void do_install(void) {
    if (active_kind != TASK_NONE) {
        emit_log_cstr("install: another task is still running");
        return;
    }

    /* Sync the editor content into its file's shadow before
     * gathering install fields, so unsaved edits go in too. */
    persist_editor_to_active_buffer();

    static char id[256], title[256], slug[80], desc[1024], version[64];
    uint32_t id_n   = kv_read("wapp_id",          id,      sizeof(id));
    uint32_t tit_n  = kv_read("wapp_title",       title,   sizeof(title));
    uint32_t slug_n = kv_read("wapp_name",        slug,    sizeof(slug));
    uint32_t desc_n = kv_read("wapp_description", desc,    sizeof(desc));
    uint32_t ver_n  = kv_read("wapp_version",     version, sizeof(version));
    if (id_n == 0)   { emit_log_cstr("install: missing id");      return; }
    if (slug_n == 0) { emit_log_cstr("install: missing name");    return; }
    if (ver_n == 0)  {
        const char *def = "0.1.0";
        for (unsigned i = 0; i < 5; i++) version[i] = def[i];
        version[5] = '\0';
        ver_n = 5;
    }
    if (tit_n == 0)  {
        for (unsigned i = 0; i < slug_n; i++) title[i] = slug[i];
        title[slug_n] = '\0';
        tit_n = slug_n;
    }

    /* Verify app.wasm exists (compile must have run). */
    static char wasm_path[320];
    build_path(wasm_path, sizeof(wasm_path), slug, slug_n, "app.wasm");
    int32_t wf = hal_file_open(wasm_path, str_len(wasm_path), 0);
    if (wf < 0) {
        emit_log_cstr("install: app.wasm missing — hit Compile first");
        return;
    }
    hal_file_close(wf);

    /* manifest.json — derived from the form fields. */
    static char manifest[4096];
    unsigned mp = 0;
    append_cstr(manifest, sizeof(manifest), &mp, "{\n  \"id\": \"");
    escape_into(manifest, sizeof(manifest), &mp, id, id_n);
    append_cstr(manifest, sizeof(manifest), &mp, "\",\n  \"version\": \"");
    escape_into(manifest, sizeof(manifest), &mp, version, ver_n);
    append_cstr(manifest, sizeof(manifest), &mp, "\",\n  \"kind\": \"app\",\n  \"title\": \"");
    escape_into(manifest, sizeof(manifest), &mp, title, tit_n);
    append_cstr(manifest, sizeof(manifest), &mp, "\",\n  \"description\": \"");
    escape_into(manifest, sizeof(manifest), &mp, desc, desc_n);
    append_cstr(manifest, sizeof(manifest), &mp, "\",\n  \"summary\": \"");
    escape_into(manifest, sizeof(manifest), &mp, desc, desc_n);
    append_cstr(manifest, sizeof(manifest), &mp,
        "\",\n  \"entry_ui\": \"screens/home.ui.json\",\n"
        "  \"tick_interval_ms\": 0,\n  \"permissions\": [],\n"
        "  \"provides\": {\"functions\":[],\"events\":[],"
        "\"variables\":[],\"widgets\":[]},\n"
        "  \"requires\": {\"hal\":[\"log\",\"kv\",\"msg\"],"
        "\"events\":[],\"libraries\":[],\"variables\":[]}\n}\n");
    if (!stage_file(slug, slug_n, "manifest.json", manifest, mp)) return;

    /* screens/home.ui.json — read from KV shadow buffer. */
    static char ui[96 * 1024];
    uint32_t ui_n = hal_kv_get(KV_BUF_UI, str_len(KV_BUF_UI),
                               ui, sizeof(ui) - 1);
    if (ui_n == 0) {
        const char *def = "[]\n";
        for (unsigned i = 0; i < 3; i++) ui[i] = def[i];
        ui_n = 3;
    }
    if (!stage_file(slug, slug_n, "screens/home.ui.json", ui, ui_n)) return;

    /* lang/en.json — read from the source_lang field. */
    static char lang[96 * 1024];
    uint32_t lang_n = kv_read("source_lang", lang, sizeof(lang));
    if (lang_n == 0) {
        const char *def = "{\n}\n";
        for (unsigned i = 0; i < 4; i++) lang[i] = def[i];
        lang_n = 4;
    }
    if (!stage_file(slug, slug_n, "lang/en.json", lang, lang_n)) return;

    /* Pack the build dir into <slug>.wapp via /usr/bin/zip wrapped in
     * /bin/sh so we don't have to know zip's absolute path. */
    static char build_dir[256];
    build_path(build_dir, sizeof(build_dir), slug, slug_n, "");

    static char script[1024];
    unsigned op = 0;
    append_cstr(script, sizeof(script), &op,
                "set -e; cd \"");
    append_cstr(script, sizeof(script), &op, build_dir);
    append_cstr(script, sizeof(script), &op,
                "\"; rm -f \"../");
    append_range(script, sizeof(script), &op, slug, slug_n);
    append_cstr(script, sizeof(script), &op,
                ".wapp\"; zip -q -r \"../");
    append_range(script, sizeof(script), &op, slug, slug_n);
    append_cstr(script, sizeof(script), &op,
                ".wapp\" .");

    int32_t h = spawn_shell(script);
    if (h < 0) {
        emit_log_cstr("install: hal_process_exec returned -1");
        return;
    }
    active_kind   = TASK_INSTALL_ZIP;
    active_handle = h;
    unsigned cn = slug_n < sizeof(active_slug) - 1
                  ? slug_n : sizeof(active_slug) - 1;
    for (unsigned i = 0; i < cn; i++) active_slug[i] = slug[i];
    active_slug[cn] = '\0';
    unsigned vn = ver_n < sizeof(active_version) - 1
                  ? ver_n : sizeof(active_version) - 1;
    for (unsigned i = 0; i < vn; i++) active_version[i] = version[i];
    active_version[vn] = '\0';
    emit_log_cstr("install: packaging .wapp...");
}

/* Send the generic wapp.install message pointing at the local ZIP
 * the install pipeline just produced. The host's existing handler
 * (used by the Wapp Store) reads the ZIP via installFromBytes. */
static void send_local_wapp_install(void) {
    static char msg[1024];
    unsigned op = 0;
    append_cstr(msg, sizeof(msg), &op,
                "{\"type\":\"wapp.install\",\"source\":\""
                BUILD_ROOT "/\",\"file\":\"");
    append_cstr(msg, sizeof(msg), &op, active_slug);
    append_cstr(msg, sizeof(msg), &op, ".wapp\",\"name\":\"");
    append_cstr(msg, sizeof(msg), &op, active_slug);
    append_cstr(msg, sizeof(msg), &op, "\",\"version\":\"");
    append_cstr(msg, sizeof(msg), &op, active_version);
    append_cstr(msg, sizeof(msg), &op, "\"}");
    hal_msg_send(msg, op);
}

/* Drain a process pipe into the wapp's output log until empty. */
static void drain_pipe_to_log(uint32_t (*fn)(int32_t, char *, uint32_t)) {
    static char buf[4096];
    for (;;) {
        uint32_t n = fn(active_handle, buf, sizeof(buf));
        if (n == 0) return;
        emit_log_raw(buf, n);
    }
}

/* Called from module_tick when a host process is in flight. Drains
 * stdout / stderr each tick and advances the state machine when the
 * process exits. */
static void poll_active_task(void) {
    if (active_kind == TASK_NONE || active_handle < 0) return;
    drain_pipe_to_log(hal_process_read_stdout);
    drain_pipe_to_log(hal_process_read_stderr);

    int32_t state = hal_process_poll(active_handle);
    if (state == 0) return; /* still running */

    int32_t code = hal_process_exit_code(active_handle);
    hal_process_free(active_handle);
    int kind = active_kind;
    active_kind   = TASK_NONE;
    active_handle = -1;

    if (kind == TASK_COMPILE) {
        if (state == 1 && code == 0) {
            emit_log_cstr("compile: OK");
            emit_snackbar("Compile succeeded", "success");
        } else {
            emit_log_cstr("compile: FAILED");
            emit_snackbar("Compile failed — see log", "error");
        }
    } else if (kind == TASK_INSTALL_ZIP) {
        if (state == 1 && code == 0) {
            emit_log_cstr("install: archive built, handing to host...");
            send_local_wapp_install();
        } else {
            emit_log_cstr("install: zip FAILED");
            emit_snackbar("Install failed — see log", "error");
        }
    }
}

static void do_run_tests(void) {
    static char id_buf[256];
    uint32_t n = kv_read("wapp_id", id_buf, sizeof(id_buf));
    if (n == 0) {
        const char *m = "{\"type\":\"ui.log.append\",\"name\":\"output\","
                        "\"text\":\"[tests] no wapp_id set — pick a "
                        "project first.\\n\"}";
        hal_msg_send(m, str_len(m));
        return;
    }
    static char buf[512];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"tests.run\",\"req_id\":1,\"target\":\"");
    append_range(buf, sizeof(buf), &op, id_buf, n);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);

    op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.log.append\",\"name\":\"output\","
                "\"text\":\"[tests] running tests in ");
    append_range(buf, sizeof(buf), &op, id_buf, n);
    append_cstr(buf, sizeof(buf), &op, "...\\n\"}");
    hal_msg_send(buf, op);
}

/* ── Module lifecycle ─────────────────────────────────────────────── */

static void select_screen(const char *name) {
    static char buf[128];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.select_screen\",\"name\":\"");
    append_cstr(buf, sizeof(buf), &op, name);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

/* Files cards: one card per source file. The active file gets a
 * highlighted icon. Tapping a card emits action "file:<path>" which
 * the wapp dispatches to switch the editor's content. */
static void render_files_tree(void) {
    static char active[64];
    uint32_t an = hal_kv_get("active_file", 11, active, sizeof(active) - 1);
    active[an] = '\0';
    if (an == 0) {
        const char *def = FILE_MAIN;
        for (unsigned i = 0; def[i]; i++) active[i] = def[i];
        active[str_len(def)] = '\0';
        an = str_len(def);
    }

    static char buf[2048];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
        "{\"type\":\"ui.data\",\"target\":\"files\",\"items\":[");
    const char *paths[] = { FILE_MAIN, FILE_UI };
    const char *labels[] = { "main.c", "screens/home.ui.json" };
    for (int i = 0; i < 2; i++) {
        if (i) append_cstr(buf, sizeof(buf), &op, ",");
        const int is_active = (str_len(paths[i]) == an &&
                               str_eq_n(paths[i], active, an));
        append_cstr(buf, sizeof(buf), &op, "{\"id\":\"");
        append_cstr(buf, sizeof(buf), &op, paths[i]);
        append_cstr(buf, sizeof(buf), &op, "\",\"title\":\"");
        append_cstr(buf, sizeof(buf), &op, labels[i]);
        append_cstr(buf, sizeof(buf), &op,
                    is_active ? "\",\"subtitle\":\"editing\","
                              : "\",\"subtitle\":\"\",");
        append_cstr(buf, sizeof(buf), &op,
                    "\"actions\":[{\"name\":\"file:");
        append_cstr(buf, sizeof(buf), &op, paths[i]);
        append_cstr(buf, sizeof(buf), &op,
                    "\",\"label\":\"Open\",\"icon\":\"edit\"}]}");
    }
    append_cstr(buf, sizeof(buf), &op, "]}");
    hal_msg_send(buf, op);
}

/* Read active_file from KV → load the matching shadow buffer into
 * the source field via ui.set_field. */
static void load_active_file_into_editor(void) {
    static char active[64];
    uint32_t an = hal_kv_get("active_file", 11, active, sizeof(active) - 1);
    active[an] = '\0';
    const char *slot = (an > 0 && str_eq_n(active, FILE_UI, an))
                       ? KV_BUF_UI : KV_BUF_MAIN;
    static char content[96 * 1024];
    uint32_t cn = hal_kv_get(slot, str_len(slot), content,
                             sizeof(content) - 1);
    content[cn] = '\0';
    send_set_field("source", content);
}

static void send_read_source(const char *slug, unsigned slen) {
    static char buf[256];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"wapps.read_source\",\"req_id\":2,\"slug\":\"");
    append_range(buf, sizeof(buf), &op, slug, slen);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

/* ── Translation editor ───────────────────────────────────────────────
 * Manages lang/<code>.json files for the currently edited wapp.
 * The UI shows a three-pane layout: language selector | reference (en)
 * list | editing form + active-lang list. */

#define MAX_TRANS   64
#define TKEY_LEN    80
#define TVAL_LEN   256
#define MAX_LANGS    8
#define LANG_LEN    16

static char trans_edit_slug[SLUG_LEN];
static char trans_active_lang[LANG_LEN];   /* e.g. "fr"              */
static char trans_lang_codes[MAX_LANGS][LANG_LEN];
static int  trans_lang_count;

static char trans_ref_key[MAX_TRANS][TKEY_LEN];
static char trans_ref_val[MAX_TRANS][TVAL_LEN];
static int  trans_ref_count;

static char trans_tgt_key[MAX_TRANS][TKEY_LEN];
static char trans_tgt_val[MAX_TRANS][TVAL_LEN];
static int  trans_tgt_count;

/* Convert an int to a decimal string; returns chars written. */
static unsigned trans_itoa(int n, char *out, unsigned max) {
    if (max < 2) return 0;
    if (n <= 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    char tmp[12]; unsigned tp = 0;
    while (n > 0 && tp < sizeof(tmp)) { tmp[tp++] = '0' + (n % 10); n /= 10; }
    unsigned op = 0;
    for (int j = (int)tp - 1; j >= 0 && op + 1 < max; j--) out[op++] = tmp[j];
    out[op] = '\0';
    return op;
}

/* Decode escape sequences produced by extract_json_string_field
 * (which copies `\` verbatim) into actual chars for a proper parser. */
static unsigned trans_decode(const char *src, unsigned slen,
                              char *dst, unsigned dmax) {
    unsigned op = 0;
    for (unsigned i = 0; i < slen && op + 1 < dmax; i++) {
        if (src[i] == '\\' && i + 1 < slen) {
            char e = src[i + 1]; i++;
            if      (e == 'n')  dst[op++] = '\n';
            else if (e == 't')  dst[op++] = '\t';
            else if (e == 'r')  dst[op++] = '\r';
            else if (e == '"')  dst[op++] = '"';
            else if (e == '\\') dst[op++] = '\\';
            else                dst[op++] = e;
        } else {
            dst[op++] = src[i];
        }
    }
    dst[op] = '\0';
    return op;
}

/* Parse a flat JSON object into parallel key/value arrays.
 * Values are stored as plain (unescaped) strings. Returns count. */
static int trans_parse_flat(const char *json, unsigned n,
                             char keys[][TKEY_LEN], char vals[][TVAL_LEN],
                             int max_kv) {
    int count = 0;
    unsigned i = 0;
    while (i < n && json[i] != '{') i++;
    if (i >= n) return 0;
    i++;
    while (i < n && count < max_kv) {
        while (i < n && (json[i]==' '||json[i]=='\t'||json[i]=='\n'||
                         json[i]=='\r'||json[i]==',')) i++;
        if (i >= n || json[i] == '}') break;
        if (json[i] != '"') break;
        i++;
        unsigned kp = 0;
        while (i < n && json[i] != '"') {
            if (json[i]=='\\' && i+1<n) { i++; }
            if (kp + 1 < TKEY_LEN) keys[count][kp++] = json[i];
            i++;
        }
        keys[count][kp] = '\0';
        if (i < n) i++;
        while (i < n && (json[i]==' '||json[i]=='\t'||json[i]==':')) i++;
        if (i >= n || json[i] != '"') break;
        i++;
        unsigned vp = 0;
        while (i < n && json[i] != '"') {
            if (json[i]=='\\' && i+1<n) {
                char e = json[i+1]; i += 2;
                if      (e == 'n')  { if (vp+1<TVAL_LEN) vals[count][vp++]='\n'; }
                else if (e == 't')  { if (vp+1<TVAL_LEN) vals[count][vp++]='\t'; }
                else if (e == 'r')  { if (vp+1<TVAL_LEN) vals[count][vp++]='\r'; }
                else if (e == '"')  { if (vp+1<TVAL_LEN) vals[count][vp++]='"'; }
                else if (e == '\\') { if (vp+1<TVAL_LEN) vals[count][vp++]='\\'; }
                else                { if (vp+1<TVAL_LEN) vals[count][vp++]=e; }
            } else {
                if (vp + 1 < TVAL_LEN) vals[count][vp++] = json[i];
                i++;
            }
        }
        vals[count][vp] = '\0';
        if (i < n) i++;
        count++;
    }
    return count;
}

static void trans_render_lang_files(void) {
    static char buf[2 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.data\",\"target\":\"lang_files\",\"items\":[");
    for (int k = 0; k < trans_lang_count; k++) {
        if (k) append_cstr(buf, sizeof(buf), &op, ",");
        const int active = (str_len(trans_lang_codes[k]) ==
                            str_len(trans_active_lang) &&
                            str_eq_n(trans_lang_codes[k], trans_active_lang,
                                     str_len(trans_active_lang)));
        append_cstr(buf, sizeof(buf), &op, "{\"id\":\"lang-select:");
        append_cstr(buf, sizeof(buf), &op, trans_lang_codes[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"title\":\"");
        append_cstr(buf, sizeof(buf), &op, trans_lang_codes[k]);
        append_cstr(buf, sizeof(buf), &op, active ? "\",\"subtitle\":\"editing\","
                                                  : "\",\"subtitle\":\"\",");
        append_cstr(buf, sizeof(buf), &op, "\"actions\":[{\"name\":\"lang-select:");
        append_cstr(buf, sizeof(buf), &op, trans_lang_codes[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"label\":\"Select\",\"icon\":\"check\"}]}");
    }
    append_cstr(buf, sizeof(buf), &op, "]}");
    hal_msg_send(buf, op);
}

static void trans_render_ref(void) {
    static char buf[32 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
        "{\"type\":\"ui.data\",\"target\":\"trans_ref\","
        "\"label\":\"English (reference)\",\"items\":[");
    for (int k = 0; k < trans_ref_count; k++) {
        if (k) append_cstr(buf, sizeof(buf), &op, ",");
        char eid[8]; trans_itoa(k, eid, sizeof(eid));
        char vesc[TVAL_LEN * 2]; unsigned vep = 0;
        escape_into(vesc, sizeof(vesc), &vep,
                    trans_ref_val[k], str_len(trans_ref_val[k]));
        vesc[vep] = '\0';
        append_cstr(buf, sizeof(buf), &op, "{\"id\":\"ref-select:");
        append_cstr(buf, sizeof(buf), &op, eid);
        append_cstr(buf, sizeof(buf), &op, "\",\"title\":\"");
        append_cstr(buf, sizeof(buf), &op, trans_ref_key[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"subtitle\":\"");
        append_cstr(buf, sizeof(buf), &op, vesc);
        append_cstr(buf, sizeof(buf), &op, "\",\"actions\":[{\"name\":\"ref-select:");
        append_cstr(buf, sizeof(buf), &op, eid);
        append_cstr(buf, sizeof(buf), &op, "\",\"label\":\"Edit\",\"icon\":\"edit\"}]}");
    }
    append_cstr(buf, sizeof(buf), &op, "]}");
    hal_msg_send(buf, op);
}

static void trans_render_tgt(void) {
    static char buf[32 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
        "{\"type\":\"ui.data\",\"target\":\"translations\",\"items\":[");
    for (int k = 0; k < trans_tgt_count; k++) {
        if (k) append_cstr(buf, sizeof(buf), &op, ",");
        char eid[8]; trans_itoa(k, eid, sizeof(eid));
        char vesc[TVAL_LEN * 2]; unsigned vep = 0;
        escape_into(vesc, sizeof(vesc), &vep,
                    trans_tgt_val[k], str_len(trans_tgt_val[k]));
        vesc[vep] = '\0';
        append_cstr(buf, sizeof(buf), &op, "{\"id\":\"trans-select:");
        append_cstr(buf, sizeof(buf), &op, eid);
        append_cstr(buf, sizeof(buf), &op, "\",\"title\":\"");
        append_cstr(buf, sizeof(buf), &op, trans_tgt_key[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"subtitle\":\"");
        append_cstr(buf, sizeof(buf), &op, vesc);
        append_cstr(buf, sizeof(buf), &op, "\",\"actions\":[{\"name\":\"trans-select:");
        append_cstr(buf, sizeof(buf), &op, eid);
        append_cstr(buf, sizeof(buf), &op, "\",\"label\":\"Edit\",\"icon\":\"edit\"}]}");
    }
    append_cstr(buf, sizeof(buf), &op, "]}");
    hal_msg_send(buf, op);
}

static void trans_write_active(void) {
    /* Build JSON content */
    static char json_content[16 * 1024];
    unsigned jop = 0;
    append_cstr(json_content, sizeof(json_content), &jop, "{");
    for (int k = 0; k < trans_tgt_count; k++) {
        if (k) append_cstr(json_content, sizeof(json_content), &jop, ",");
        append_cstr(json_content, sizeof(json_content), &jop, "\n  \"");
        append_cstr(json_content, sizeof(json_content), &jop, trans_tgt_key[k]);
        append_cstr(json_content, sizeof(json_content), &jop, "\": \"");
        escape_into(json_content, sizeof(json_content), &jop,
                    trans_tgt_val[k], str_len(trans_tgt_val[k]));
        append_cstr(json_content, sizeof(json_content), &jop, "\"");
    }
    append_cstr(json_content, sizeof(json_content), &jop, "\n}");

    /* Embed content as JSON-escaped string inside the write_lang message */
    static char msg[48 * 1024];
    unsigned mp = 0;
    append_cstr(msg, sizeof(msg), &mp,
                "{\"type\":\"wapps.write_lang\",\"req_id\":13,\"slug\":\"");
    append_cstr(msg, sizeof(msg), &mp, trans_edit_slug);
    append_cstr(msg, sizeof(msg), &mp, "\",\"lang\":\"");
    append_cstr(msg, sizeof(msg), &mp, trans_active_lang);
    append_cstr(msg, sizeof(msg), &mp, "\",\"content\":\"");
    escape_into(msg, sizeof(msg), &mp, json_content, jop);
    append_cstr(msg, sizeof(msg), &mp, "\"}");
    hal_msg_send(msg, mp);
}

static void trans_request_list(void) {
    static char buf[256];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"wapps.list_lang\",\"req_id\":10,\"slug\":\"");
    append_cstr(buf, sizeof(buf), &op, trans_edit_slug);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

static void trans_request_read(const char *lang, int req_id) {
    static char buf[256];
    unsigned op = 0;
    const char *rid = (req_id == 11) ? "11" : "12";
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"wapps.read_lang\",\"req_id\":");
    append_cstr(buf, sizeof(buf), &op, rid);
    append_cstr(buf, sizeof(buf), &op, ",\"slug\":\"");
    append_cstr(buf, sizeof(buf), &op, trans_edit_slug);
    append_cstr(buf, sizeof(buf), &op, "\",\"lang\":\"");
    append_cstr(buf, sizeof(buf), &op, lang);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

/* Load translation data for the selected wapp (called from on_select). */
static void trans_load(const char *slug, unsigned slen) {
    unsigned n = slen < SLUG_LEN - 1 ? slen : SLUG_LEN - 1;
    for (unsigned i = 0; i < n; i++) trans_edit_slug[i] = slug[i];
    trans_edit_slug[n] = '\0';
    /* Reset to "en" as default active lang */
    trans_active_lang[0] = 'e'; trans_active_lang[1] = 'n';
    trans_active_lang[2] = '\0';
    trans_lang_count = 0;
    trans_ref_count = 0;
    trans_tgt_count = 0;
    trans_request_list();
}

/* Handle wapps.list_lang.response */
static void trans_on_list(const char *inbox, unsigned n) {
    trans_lang_count = 0;
    /* Parse ["en","fr",...] from langs field */
    const int arr_at = find_substr(inbox, n, "\"langs\":[");
    if (arr_at < 0) return;
    unsigned i = (unsigned)arr_at + 9;
    while (i < n && trans_lang_count < MAX_LANGS) {
        while (i < n && inbox[i] != '"' && inbox[i] != ']') i++;
        if (i >= n || inbox[i] == ']') break;
        i++;
        unsigned lp = 0;
        while (i < n && inbox[i] != '"' && lp + 1 < LANG_LEN)
            trans_lang_codes[trans_lang_count][lp++] = inbox[i++];
        trans_lang_codes[trans_lang_count][lp] = '\0';
        while (i < n && inbox[i] != '"') i++;
        if (i < n) i++;
        trans_lang_count++;
    }
    /* If "en" not present, seed it */
    int has_en = 0;
    for (int k = 0; k < trans_lang_count; k++)
        if (str_eq_n(trans_lang_codes[k], "en", 2) &&
            str_len(trans_lang_codes[k]) == 2) { has_en = 1; break; }
    if (!has_en && trans_lang_count < MAX_LANGS) {
        trans_lang_codes[0][0]='e'; trans_lang_codes[0][1]='n';
        trans_lang_codes[0][2]='\0';
        trans_lang_count++;
    }
    trans_render_lang_files();
    /* Always fetch English as reference (req_id 11) */
    trans_request_read("en", 11);
    /* Fetch active lang as target (req_id 12) — skip if same as "en" */
    if (!(str_eq_n(trans_active_lang, "en", 2) &&
          str_len(trans_active_lang) == 2))
        trans_request_read(trans_active_lang, 12);
}

/* Handle wapps.read_lang.response — req_id 11 = ref, 12 = target */
static void trans_on_read(const char *inbox, unsigned n, int req_id) {
    /* Extract and decode the content string */
    static char raw[8 * 1024];
    static char content[8 * 1024];
    int rn = extract_json_string_field(inbox, n, "content",
                                       raw, sizeof(raw));
    if (rn < 0) rn = 0;
    unsigned cn = trans_decode(raw, (unsigned)rn, content, sizeof(content));

    if (req_id == 11) {
        trans_ref_count = trans_parse_flat(content, cn,
            trans_ref_key, trans_ref_val, MAX_TRANS);
        trans_render_ref();
        /* If editing "en", mirror ref → tgt */
        if (str_eq_n(trans_active_lang, "en", 2) &&
            str_len(trans_active_lang) == 2) {
            trans_tgt_count = trans_ref_count;
            for (int k = 0; k < trans_ref_count; k++) {
                copy_field(trans_ref_key[k], (int)str_len(trans_ref_key[k]),
                           trans_tgt_key[k], TKEY_LEN);
                copy_field(trans_ref_val[k], (int)str_len(trans_ref_val[k]),
                           trans_tgt_val[k], TVAL_LEN);
            }
            trans_render_tgt();
        }
    } else {
        trans_tgt_count = trans_parse_flat(content, cn,
            trans_tgt_key, trans_tgt_val, MAX_TRANS);
        trans_render_tgt();
    }
}

/* Upsert a key/value into tgt arrays and persist */
static void trans_upsert(const char *key, const char *val) {
    unsigned kl = str_len(key);
    for (int k = 0; k < trans_tgt_count; k++) {
        if (str_len(trans_tgt_key[k]) == kl &&
            str_eq_n(trans_tgt_key[k], key, kl)) {
            copy_field(val, (int)str_len(val), trans_tgt_val[k], TVAL_LEN);
            /* Mirror into ref if editing "en" */
            if (str_eq_n(trans_active_lang,"en",2) &&
                str_len(trans_active_lang)==2) {
                copy_field(val,(int)str_len(val),trans_ref_val[k],TVAL_LEN);
                trans_render_ref();
            }
            trans_write_active();
            trans_render_tgt();
            return;
        }
    }
    if (trans_tgt_count >= MAX_TRANS) return;
    copy_field(key, (int)kl, trans_tgt_key[trans_tgt_count], TKEY_LEN);
    copy_field(val, (int)str_len(val), trans_tgt_val[trans_tgt_count], TVAL_LEN);
    trans_tgt_count++;
    if (str_eq_n(trans_active_lang,"en",2) && str_len(trans_active_lang)==2) {
        copy_field(key,(int)kl,trans_ref_key[trans_ref_count],TKEY_LEN);
        copy_field(val,(int)str_len(val),trans_ref_val[trans_ref_count],TVAL_LEN);
        trans_ref_count++;
        trans_render_ref();
    }
    trans_write_active();
    trans_render_tgt();
}

/* Delete a key from tgt arrays */
static void trans_delete(const char *key) {
    unsigned kl = str_len(key);
    for (int k = 0; k < trans_tgt_count; k++) {
        if (str_len(trans_tgt_key[k]) == kl &&
            str_eq_n(trans_tgt_key[k], key, kl)) {
            for (int j = k; j < trans_tgt_count - 1; j++) {
                copy_field(trans_tgt_key[j+1],(int)str_len(trans_tgt_key[j+1]),
                           trans_tgt_key[j], TKEY_LEN);
                copy_field(trans_tgt_val[j+1],(int)str_len(trans_tgt_val[j+1]),
                           trans_tgt_val[j], TVAL_LEN);
            }
            trans_tgt_count--;
            if (str_eq_n(trans_active_lang,"en",2) &&
                str_len(trans_active_lang)==2) {
                for (int j = k; j < trans_ref_count - 1; j++) {
                    copy_field(trans_ref_key[j+1],(int)str_len(trans_ref_key[j+1]),
                               trans_ref_key[j],TKEY_LEN);
                    copy_field(trans_ref_val[j+1],(int)str_len(trans_ref_val[j+1]),
                               trans_ref_val[j],TVAL_LEN);
                }
                trans_ref_count--;
                trans_render_ref();
            }
            trans_write_active();
            trans_render_tgt();
            return;
        }
    }
}

/* Switch to a different active lang */
static void trans_select_lang(const char *lang) {
    copy_field(lang, (int)str_len(lang), trans_active_lang, LANG_LEN);
    trans_render_lang_files();
    if (str_eq_n(lang,"en",2) && str_len(lang)==2) {
        /* Re-use already-loaded ref as tgt */
        trans_tgt_count = trans_ref_count;
        for (int k = 0; k < trans_ref_count; k++) {
            copy_field(trans_ref_key[k],(int)str_len(trans_ref_key[k]),
                       trans_tgt_key[k],TKEY_LEN);
            copy_field(trans_ref_val[k],(int)str_len(trans_ref_val[k]),
                       trans_tgt_val[k],TVAL_LEN);
        }
        trans_render_tgt();
    } else {
        trans_request_read(lang, 12);
    }
}

/* Populate the edit form from a reference row click */
static void trans_select_ref(int idx) {
    if (idx < 0 || idx >= trans_ref_count) return;
    send_set_field("trans_key", trans_ref_key[idx]);
    /* Look up existing target value for this key */
    unsigned kl = str_len(trans_ref_key[idx]);
    for (int k = 0; k < trans_tgt_count; k++) {
        if (str_len(trans_tgt_key[k]) == kl &&
            str_eq_n(trans_tgt_key[k], trans_ref_key[idx], kl)) {
            send_set_field("trans_value", trans_tgt_val[k]);
            return;
        }
    }
    send_set_field("trans_value", "");
}

/* Populate the edit form from a target row click */
static void trans_select_tgt(int idx) {
    if (idx < 0 || idx >= trans_tgt_count) return;
    send_set_field("trans_key",   trans_tgt_key[idx]);
    send_set_field("trans_value", trans_tgt_val[idx]);
}

/* Create a new language file */
static void trans_create_lang(const char *code) {
    if (str_len(code) == 0) return;
    /* Register code */
    for (int k = 0; k < trans_lang_count; k++)
        if (str_eq_n(trans_lang_codes[k], code, str_len(code)) &&
            str_len(trans_lang_codes[k]) == str_len(code)) return; /* dup */
    if (trans_lang_count < MAX_LANGS) {
        copy_field(code,(int)str_len(code),
                   trans_lang_codes[trans_lang_count++],LANG_LEN);
    }
    /* Write an empty JSON object as the new file */
    static char msg[512];
    unsigned mp = 0;
    append_cstr(msg, sizeof(msg), &mp,
                "{\"type\":\"wapps.write_lang\",\"req_id\":13,\"slug\":\"");
    append_cstr(msg, sizeof(msg), &mp, trans_edit_slug);
    append_cstr(msg, sizeof(msg), &mp, "\",\"lang\":\"");
    append_cstr(msg, sizeof(msg), &mp, code);
    append_cstr(msg, sizeof(msg), &mp, "\",\"content\":\"{}\"}");
    hal_msg_send(msg, mp);
    trans_select_lang(code);
}

void module_init(void) {
    hal_log(1, "[app-creator] init", 18);
    select_screen("Projects");
    send_list_installed();

    /* Restore font-size prefs from KV. Seed defaults if not yet saved. */
    char pref_buf[16];
    uint32_t pn;
    pn = hal_kv_get("pref_editor_font_size", 20,
                    pref_buf, sizeof(pref_buf) - 1);
    pref_buf[pn < sizeof(pref_buf) ? pn : sizeof(pref_buf) - 1] = '\0';
    send_set_field("pref_editor_font_size", pn > 0 ? pref_buf : "18");

    pn = hal_kv_get("pref_log_font_size", 18,
                    pref_buf, sizeof(pref_buf) - 1);
    pref_buf[pn < sizeof(pref_buf) ? pn : sizeof(pref_buf) - 1] = '\0';
    send_set_field("pref_log_font_size", pn > 0 ? pref_buf : "15");
}

void module_tick(void) {
    /* Drive the compile / install state machine. While a task is in
     * flight we drain its stdout/stderr each tick and advance to the
     * next stage on exit. */
    poll_active_task();
}

void module_destroy(void) {
    hal_log(1, "[app-creator] destroy", 21);
}

uint32_t module_tick_interval_ms(void) {
    /* 200 ms gives the compile / install state machine in
     * poll_active_task crisp output without burning CPU when idle
     * (poll_active_task short-circuits when TASK_NONE). */
    return 200;
}

void module_handle_event(void) {
    /* Inbox sized for wapps.read_source.response payloads — they
     * carry source.c + screens/home.ui.json + lang/en.json from
     * the selected project. Larger wapps (Forum, Wapp Store) push
     * us past 32 KB. */
    static char inbox[96 * 1024];
    static char field_buf[512];
    static char out_buf[32 * 1024];

    while (hal_msg_available() != 0) {
        uint32_t n = hal_msg_recv(inbox, sizeof(inbox) - 1);
        if (n == 0) break;
        inbox[n] = '\0';

        /* Installed-wapps response → cache + render. */
        if (find_substr(inbox, n,
                "\"type\":\"wapps.list_installed.response\"") >= 0) {
            parse_list_response(inbox, n);
            render_projects();
            continue;
        }

        /* Source-files response → save raw file content into shadow
         * KV slots, push the active file into the editor, and push
         * en.json into the Translations tab field. The extracted
         * bytes are still JSON-escaped — when the host's jsonDecode
         * sees them inside another JSON message it un-escapes back
         * to the original UTF-8. */
        if (find_substr(inbox, n,
                "\"type\":\"wapps.read_source.response\"") >= 0) {
            static char big_buf[96 * 1024];
            int sl;
            sl = extract_json_string_field(inbox, n, "source",
                                           big_buf, sizeof(big_buf));
            if (sl >= 0) {
                hal_kv_set(KV_BUF_MAIN, str_len(KV_BUF_MAIN),
                           big_buf, (uint32_t)sl);
            }
            sl = extract_json_string_field(inbox, n, "source_ui",
                                           big_buf, sizeof(big_buf));
            if (sl >= 0) {
                hal_kv_set(KV_BUF_UI, str_len(KV_BUF_UI),
                           big_buf, (uint32_t)sl);
            }
            sl = extract_json_string_field(inbox, n, "source_lang",
                                           big_buf, sizeof(big_buf));
            if (sl >= 0) send_set_field("source_lang", big_buf);
            /* Default the editor to main.c. */
            hal_kv_set("active_file", 11, FILE_MAIN, str_len(FILE_MAIN));
            send_set_field("active_file_label", FILE_MAIN);
            load_active_file_into_editor();
            render_files_tree();
            continue;
        }

        /* Test runner case results. */
        if (find_substr(inbox, n, "\"type\":\"tests.case\"") >= 0) {
            const int suite_len = extract_json_string_field(
                inbox, n, "suite", field_buf, sizeof(field_buf));
            char suite_copy[80];
            int sc = suite_len;
            if (sc > 79) sc = 79;
            for (int i = 0; i < sc; i++) suite_copy[i] = field_buf[i];
            suite_copy[sc < 0 ? 0 : sc] = '\0';

            const int name_len = extract_json_string_field(
                inbox, n, "name", field_buf, sizeof(field_buf));
            const int has_pass =
                find_substr(inbox, n, "\"passed\":true") >= 0;

            unsigned op = 0;
            append_cstr(out_buf, sizeof(out_buf), &op,
                        "{\"type\":\"ui.log.append\",\"name\":\"output\","
                        "\"text\":\"[tests] ");
            append_cstr(out_buf, sizeof(out_buf), &op,
                        has_pass ? "PASS  " : "FAIL  ");
            if (sc > 0) {
                append_range(out_buf, sizeof(out_buf), &op, suite_copy,
                             (unsigned)sc);
                append_cstr(out_buf, sizeof(out_buf), &op, ".");
            }
            if (name_len > 0) {
                append_range(out_buf, sizeof(out_buf), &op, field_buf,
                             (unsigned)name_len);
            }
            if (!has_pass) {
                const int err_len = extract_json_string_field(
                    inbox, n, "error", field_buf, sizeof(field_buf));
                if (err_len > 0) {
                    append_cstr(out_buf, sizeof(out_buf), &op,
                                "\\n         ");
                    append_range(out_buf, sizeof(out_buf), &op, field_buf,
                                 (unsigned)err_len);
                }
            }
            append_cstr(out_buf, sizeof(out_buf), &op, "\\n\"}");
            hal_msg_send(out_buf, op);
            continue;
        }

        /* Host echo confirming a wapp.install completed. Surface to
         * the log and trigger a Projects refresh so the new wapp
         * appears the next time the user goes back. */
        if (find_substr(inbox, n,
                "\"type\":\"wapps.list_lang.response\"") >= 0) {
            trans_on_list(inbox, n);
            continue;
        }

        if (find_substr(inbox, n,
                "\"type\":\"wapps.read_lang.response\"") >= 0) {
            const int rid_at = find_substr(inbox, n, "\"req_id\":");
            int req_id = 12;
            if (rid_at >= 0) {
                unsigned ri = (unsigned)rid_at + 9;
                while (ri < n && (inbox[ri]==' '||inbox[ri]=='\t')) ri++;
                req_id = 0;
                while (ri < n && inbox[ri] >= '0' && inbox[ri] <= '9')
                    req_id = req_id * 10 + (inbox[ri++] - '0');
            }
            trans_on_read(inbox, n, req_id);
            continue;
        }

        if (find_substr(inbox, n, "\"type\":\"wapp.installed\"") >= 0) {
            char ver[64];
            int vn = extract_json_string_field(
                inbox, n, "version", ver, sizeof(ver));
            char nm[80];
            int nmn = extract_json_string_field(
                inbox, n, "name", nm, sizeof(nm));
            static char buf[256] = "install: OK — ";
            unsigned bp = str_len(buf);
            if (nmn > 0) append_range(buf, sizeof(buf), &bp, nm,
                                      (unsigned)nmn);
            if (vn > 0) {
                append_cstr(buf, sizeof(buf), &bp, " v");
                append_range(buf, sizeof(buf), &bp, ver, (unsigned)vn);
            }
            emit_log(buf, bp);
            emit_snackbar("Install succeeded", "success");
            send_list_installed();
            continue;
        }

        if (find_substr(inbox, n, "\"type\":\"tests.complete\"") >= 0) {
            const int err_len = extract_json_string_field(
                inbox, n, "error", field_buf, sizeof(field_buf));
            unsigned op = 0;
            append_cstr(out_buf, sizeof(out_buf), &op,
                        "{\"type\":\"ui.log.append\",\"name\":\"output\","
                        "\"text\":\"[tests] complete");
            if (err_len > 0) {
                append_cstr(out_buf, sizeof(out_buf), &op, " — ");
                append_range(out_buf, sizeof(out_buf), &op, field_buf,
                             (unsigned)err_len);
            }
            append_cstr(out_buf, sizeof(out_buf), &op, "\\n\"}");
            hal_msg_send(out_buf, op);
            continue;
        }

        /* Action messages — extract action name, then dispatch. */
        const int act_idx = find_substr(inbox, n, "\"action\":\"");
        if (act_idx < 0) continue;
        const unsigned vstart = (unsigned)act_idx + 10;
        unsigned vend = vstart;
        while (vend < n && inbox[vend] != '"') vend++;
        if (vend >= n) continue;
        const char *a = inbox + vstart;
        const unsigned al = vend - vstart;

        /* Card-emitted "select:<slug>" actions — populate the form
         * from the cached wapp metadata. */
        if (al > 7 && str_eq_n(a, "select:", 7)) {
            on_select(a + 7, al - 7);
            continue;
        }

        /* "file:<path>" — switch the editor to a different source
         * file. Path is one of FILE_MAIN, FILE_UI. */
        if (al > 5 && str_eq_n(a, "file:", 5)) {
            on_pick_file(a + 5, al - 5);
            continue;
        }

        if (al == 11 && str_eq_n(a, "new-project", 11)) {
            on_new_project();
            continue;
        }
        if (al == 16 && str_eq_n(a, "refresh-projects", 16)) {
            send_list_installed();
            continue;
        }
        if (al == 7 && str_eq_n(a, "compile", 7)) {
            do_compile();
            continue;
        }
        if (al == 7 && str_eq_n(a, "install", 7)) {
            do_install();
            continue;
        }
        if (al == 9 && str_eq_n(a, "run-tests", 9)) {
            do_run_tests();
            continue;
        }
        if (al == 15 && str_eq_n(a, "editor-settings", 15)) {
            select_screen("Settings");
            continue;
        }

        /* Translation UI actions ─────────────────────────────── */

        /* lang-select:<code> — switch active language */
        if (al > 12 && str_eq_n(a, "lang-select:", 12)) {
            static char code[LANG_LEN];
            unsigned cl = al - 12;
            if (cl >= LANG_LEN) cl = LANG_LEN - 1;
            for (unsigned ci = 0; ci < cl; ci++) code[ci] = a[12 + ci];
            code[cl] = '\0';
            trans_select_lang(code);
            continue;
        }

        /* ref-select:<idx> — click a reference row to fill the form */
        if (al > 11 && str_eq_n(a, "ref-select:", 11)) {
            int idx = 0;
            for (unsigned ci = 11; ci < al; ci++)
                idx = idx * 10 + (a[ci] - '0');
            trans_select_ref(idx);
            continue;
        }

        /* trans-select:<idx> — click a target row to fill the form */
        if (al > 13 && str_eq_n(a, "trans-select:", 13)) {
            int idx = 0;
            for (unsigned ci = 13; ci < al; ci++)
                idx = idx * 10 + (a[ci] - '0');
            trans_select_tgt(idx);
            continue;
        }

        /* upsert-trans — save the current key/value pair */
        if (al == 12 && str_eq_n(a, "upsert-trans", 12)) {
            static char tk[TKEY_LEN], tv[TVAL_LEN];
            kv_read("trans_key", tk, sizeof(tk));
            kv_read("trans_value", tv, sizeof(tv));
            if (str_len(tk) > 0)
                trans_upsert(tk, tv);
            continue;
        }

        /* delete-trans — delete the current key */
        if (al == 12 && str_eq_n(a, "delete-trans", 12)) {
            static char tk[TKEY_LEN];
            kv_read("trans_key", tk, sizeof(tk));
            if (str_len(tk) > 0) {
                trans_delete(tk);
                send_set_field("trans_key", "");
                send_set_field("trans_value", "");
            }
            continue;
        }

        /* add-lang — create a new language file */
        if (al == 8 && str_eq_n(a, "add-lang", 8)) {
            if (str_len(trans_edit_slug) == 0) {
                emit_snackbar("Select a project first", "info");
                continue;
            }
            static char code[LANG_LEN];
            kv_read("new_lang_code", code, sizeof(code));
            if (str_len(code) > 0) {
                trans_create_lang(code);
                send_set_field("new_lang_code", "");
                emit_snackbar("Language created", "success");
            } else {
                emit_snackbar("Enter a language code above (e.g. fr)", "info");
            }
            continue;
        }
    }
}
