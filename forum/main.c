/*
 * XPRS Forum wapp.
 *
 * Browses, reads, and posts to a `forum` collection stored in the
 * active profile. Format follows
 * docs/collections/types/forum-format-specification.md verbatim so
 * files written by this wapp remain readable by the native forum
 * browser and vice versa.
 *
 * The wapp talks to the host through outbox messages only — no new
 * HAL functions. Each request carries a numeric req_id; the host
 * replies with a `<type>.response` message bearing the same req_id.
 *
 * Permissions required (declared in manifest.permissions):
 *   collection.forum.read
 *   collection.forum.write
 *   identity.read
 *   sign
 */

#include "../hal/xprs_wasm_hal.h"

/* ─── Helpers ───────────────────────────────────────────────────── */

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

static void str_copy(char *d, const char *s, unsigned cap) {
    unsigned i = 0;
    while (i < cap - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void str_cat(char *d, const char *s, unsigned cap) {
    unsigned l = str_len(d);
    unsigned i = 0;
    while (l + i < cap - 1 && s[i]) { d[l + i] = s[i]; i++; }
    d[l + i] = '\0';
}

static unsigned u_to_str(unsigned v, char *buf, unsigned cap) {
    char tmp[12];
    unsigned i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v > 0 && i < 11) { tmp[i++] = '0' + (char)(v % 10); v /= 10; } }
    unsigned out = 0;
    while (i > 0 && out < cap - 1) buf[out++] = tmp[--i];
    buf[out] = '\0';
    return out;
}

static void u_to_str_padded(unsigned v, unsigned width, char *buf,
                            unsigned cap) {
    /* Right-justified decimal padded with leading zeros. */
    char tmp[12];
    unsigned i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v > 0 && i < 11) { tmp[i++] = '0' + (char)(v % 10); v /= 10; } }
    unsigned out = 0;
    while (i < width && out < cap - 1) { buf[out++] = '0'; width--; }
    while (i > 0 && out < cap - 1) buf[out++] = tmp[--i];
    buf[out] = '\0';
}

/* Append a JSON-escaped string literal to buf. The caller already
 * wrote the opening quote; this writes the body and the closing
 * quote. Returns the new length. */
static unsigned json_str(char *buf, unsigned len, unsigned cap,
                         const char *s) {
    for (unsigned i = 0; s[i] && len < cap - 8; i++) {
        char c = s[i];
        if      (c == '"')  { buf[len++] = '\\'; buf[len++] = '"'; }
        else if (c == '\\') { buf[len++] = '\\'; buf[len++] = '\\'; }
        else if (c == '\n') { buf[len++] = '\\'; buf[len++] = 'n'; }
        else if (c == '\r') { buf[len++] = '\\'; buf[len++] = 'r'; }
        else if (c == '\t') { buf[len++] = '\\'; buf[len++] = 't'; }
        else if ((unsigned char)c < 0x20) { /* skip other control */ }
        else                { buf[len++] = c; }
    }
    if (len < cap - 1) buf[len++] = '"';
    buf[len] = '\0';
    return len;
}

/* JSON-decode a string literal — handles \", \\, \n, \r, \t, \/ and
 * a passthrough for everything else. Reads up to len bytes from in;
 * writes up to out_cap-1 bytes plus a NUL into out. Returns the
 * number of bytes written. */
static unsigned json_unescape(const char *in, unsigned len,
                              char *out, unsigned out_cap) {
    unsigned o = 0;
    for (unsigned i = 0; i < len && o < out_cap - 1; i++) {
        char c = in[i];
        if (c == '\\' && i + 1 < len) {
            char n = in[i + 1];
            char put = 0;
            if      (n == 'n')  put = '\n';
            else if (n == 'r')  put = '\r';
            else if (n == 't')  put = '\t';
            else if (n == '"')  put = '"';
            else if (n == '\\') put = '\\';
            else if (n == '/')  put = '/';
            else                put = n;
            out[o++] = put;
            i++;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

/* ─── Tiny JSON field extractor ────────────────────────────────────
 * Find a string value for "key" in a JSON object substring (anywhere
 * inside the buffer). Writes the unescaped value into out, returns
 * 1 on hit, 0 otherwise. */
static int json_find_string(const char *buf, unsigned len,
                            const char *key, char *out, unsigned out_cap) {
    unsigned klen = str_len(key);
    out[0] = '\0';
    for (unsigned i = 0; i + klen + 3 < len; i++) {
        if (buf[i] != '"') continue;
        int match = 1;
        for (unsigned j = 0; j < klen; j++) {
            if (buf[i + 1 + j] != key[j]) { match = 0; break; }
        }
        if (!match) continue;
        if (buf[i + 1 + klen] != '"') continue;
        unsigned p = i + 2 + klen;
        while (p < len && (buf[p] == ' ' || buf[p] == ':' || buf[p] == '\t')) p++;
        if (p >= len || buf[p] != '"') continue;
        p++;
        unsigned start = p;
        while (p < len) {
            if (buf[p] == '\\' && p + 1 < len) { p += 2; continue; }
            if (buf[p] == '"') break;
            p++;
        }
        if (p >= len) return 0;
        json_unescape(buf + start, p - start, out, out_cap);
        return 1;
    }
    return 0;
}

/* Find a numeric or unquoted value (e.g. true/false/0). Writes raw
 * substring into out. */
static int json_find_raw(const char *buf, unsigned len,
                         const char *key, char *out, unsigned out_cap) {
    unsigned klen = str_len(key);
    out[0] = '\0';
    for (unsigned i = 0; i + klen + 3 < len; i++) {
        if (buf[i] != '"') continue;
        int match = 1;
        for (unsigned j = 0; j < klen; j++) {
            if (buf[i + 1 + j] != key[j]) { match = 0; break; }
        }
        if (!match) continue;
        if (buf[i + 1 + klen] != '"') continue;
        unsigned p = i + 2 + klen;
        while (p < len && (buf[p] == ' ' || buf[p] == ':' || buf[p] == '\t')) p++;
        unsigned o = 0;
        while (p < len && o < out_cap - 1 &&
               buf[p] != ',' && buf[p] != '}' && buf[p] != ']' &&
               buf[p] != '\n' && buf[p] != ' ') {
            out[o++] = buf[p++];
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* ─── State ─────────────────────────────────────────────────────── */

#define BUF_THREAD     32768   /* one thread file body */
#define BUF_CARDS      32768   /* ui.data emit buffer */
#define BUF_REQ        32768   /* outbox request buffer */
#define BUF_NAME       128
#define BUF_PATH       384
#define MAX_PENDING    16

enum view_mode {
    VIEW_SECTIONS = 0,
    VIEW_THREADS  = 1,
    VIEW_POSTS    = 2,
};

enum compose_mode {
    COMPOSE_NONE     = 0,
    COMPOSE_NEW      = 1,
    COMPOSE_REPLY    = 2,
};

enum pending_kind {
    REQ_NONE                = 0,
    REQ_LIST_SECTIONS       = 1,
    REQ_LIST_THREADS        = 2,
    REQ_READ_THREAD         = 3,
    REQ_IDENTITY            = 4,
    REQ_SIGN_NEW            = 5,
    REQ_SIGN_REPLY          = 6,
    REQ_WRITE_NEW           = 7,
    REQ_WRITE_REPLY         = 8,
    REQ_MKDIR_SECTION       = 9,
};

typedef struct {
    int kind;       /* enum pending_kind */
    int req_id;
} pending_t;

static pending_t pending[MAX_PENDING];
static int next_req_id = 1;

static int view = VIEW_SECTIONS;
static char current_section[BUF_NAME] = "";
static char current_thread[BUF_NAME]  = "";

static char thread_buf[BUF_THREAD];
static unsigned thread_buf_len = 0;

static char identity_callsign[64] = "";
static char identity_npub[80]     = "";

static int compose = COMPOSE_NONE;
static char compose_section[BUF_NAME] = "";
static char compose_thread[BUF_NAME]  = "";  /* thread filename for reply */
/* Pending sign/write state for the in-flight compose. */
static char compose_post_buf[BUF_THREAD];   /* the post text being signed */
static unsigned compose_post_len = 0;
static char compose_thread_path[BUF_PATH];  /* relative path being written */
static int compose_target_is_new = 0;

static char cards_buf[BUF_CARDS];
static char req_buf[BUF_REQ];

/* ─── Pending request table ────────────────────────────────────── */

static int register_pending(int kind) {
    int id = next_req_id++;
    if (next_req_id <= 0) next_req_id = 1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].kind == REQ_NONE) {
            pending[i].kind = kind;
            pending[i].req_id = id;
            return id;
        }
    }
    /* Table full — overwrite slot 0 (best-effort). */
    pending[0].kind = kind;
    pending[0].req_id = id;
    return id;
}

static int take_pending(int req_id) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].req_id == req_id && pending[i].kind != REQ_NONE) {
            int kind = pending[i].kind;
            pending[i].kind = REQ_NONE;
            pending[i].req_id = 0;
            return kind;
        }
    }
    return REQ_NONE;
}

/* ─── Outbox emit helpers ──────────────────────────────────────── */

static void emit_msg(const char *json, unsigned len) {
    hal_msg_send(json, len);
}

static void emit_ui_data(const char *target_group,
                         const char *items_json) {
    unsigned len = 0;
    str_copy(req_buf,
             "{\"type\":\"ui.data\",\"target\":\"",
             sizeof(req_buf));
    len = str_len(req_buf);
    len = json_str(req_buf, len - 1, sizeof(req_buf), target_group);
    /* json_str overwrites the closing quote we already wrote.
     * Re-add the field separator. */
    str_copy(req_buf + len, ",\"items\":", sizeof(req_buf) - len);
    len = str_len(req_buf);
    str_cat(req_buf, items_json, sizeof(req_buf));
    str_cat(req_buf, "}", sizeof(req_buf));
    emit_msg(req_buf, str_len(req_buf));
}

/* Fire a single-card status placeholder so the user sees feedback
 * while async work is in flight. */
static void show_status(const char *id, const char *title,
                        const char *subtitle) {
    char items[768];
    str_copy(items, "[{\"id\":\"", sizeof(items));
    unsigned len = str_len(items);
    len = json_str(items, len - 1, sizeof(items), id);
    str_copy(items + len, ",\"title\":\"", sizeof(items) - len);
    len = str_len(items);
    len = json_str(items, len - 1, sizeof(items), title);
    if (subtitle && subtitle[0]) {
        str_copy(items + len, ",\"subtitle\":\"", sizeof(items) - len);
        len = str_len(items);
        len = json_str(items, len - 1, sizeof(items), subtitle);
    }
    str_copy(items + len, "}]", sizeof(items) - len);
    emit_ui_data("view", items);
}

static void log_info(const char *msg) {
    hal_log(1, msg, str_len(msg));
}

/* ─── Profile-storage outbox requests ──────────────────────────── */

static void req_profile_list(const char *path, int kind) {
    int id = register_pending(kind);
    str_copy(req_buf,
             "{\"type\":\"profile.list\",\"req_id\":",
             sizeof(req_buf));
    char num[16];
    u_to_str((unsigned)id, num, sizeof(num));
    str_cat(req_buf, num, sizeof(req_buf));
    str_cat(req_buf, ",\"scope\":\"collection.forum\",\"path\":\"",
            sizeof(req_buf));
    unsigned len = str_len(req_buf);
    len = json_str(req_buf, len - 1, sizeof(req_buf), path);
    str_copy(req_buf + len, "}", sizeof(req_buf) - len);
    emit_msg(req_buf, str_len(req_buf));
}

static void req_profile_read(const char *path, int kind) {
    int id = register_pending(kind);
    str_copy(req_buf,
             "{\"type\":\"profile.read\",\"req_id\":",
             sizeof(req_buf));
    char num[16];
    u_to_str((unsigned)id, num, sizeof(num));
    str_cat(req_buf, num, sizeof(req_buf));
    str_cat(req_buf, ",\"scope\":\"collection.forum\",\"path\":\"",
            sizeof(req_buf));
    unsigned len = str_len(req_buf);
    len = json_str(req_buf, len - 1, sizeof(req_buf), path);
    str_copy(req_buf + len, "}", sizeof(req_buf) - len);
    emit_msg(req_buf, str_len(req_buf));
}

static void req_profile_write(const char *path, const char *body,
                              const char *mode, int kind) {
    int id = register_pending(kind);
    str_copy(req_buf,
             "{\"type\":\"profile.write\",\"req_id\":",
             sizeof(req_buf));
    char num[16];
    u_to_str((unsigned)id, num, sizeof(num));
    str_cat(req_buf, num, sizeof(req_buf));
    str_cat(req_buf,
            ",\"scope\":\"collection.forum\",\"mode\":\"",
            sizeof(req_buf));
    str_cat(req_buf, mode, sizeof(req_buf));
    str_cat(req_buf, "\",\"path\":\"", sizeof(req_buf));
    unsigned len = str_len(req_buf);
    len = json_str(req_buf, len - 1, sizeof(req_buf), path);
    str_copy(req_buf + len, ",\"data\":\"", sizeof(req_buf) - len);
    len = str_len(req_buf);
    len = json_str(req_buf, len - 1, sizeof(req_buf), body);
    str_copy(req_buf + len, "}", sizeof(req_buf) - len);
    emit_msg(req_buf, str_len(req_buf));
}

static void req_identity(void) {
    int id = register_pending(REQ_IDENTITY);
    str_copy(req_buf,
             "{\"type\":\"identity.get\",\"req_id\":",
             sizeof(req_buf));
    char num[16];
    u_to_str((unsigned)id, num, sizeof(num));
    str_cat(req_buf, num, sizeof(req_buf));
    str_cat(req_buf, "}", sizeof(req_buf));
    emit_msg(req_buf, str_len(req_buf));
}

/* ─── Forum format builders ───────────────────────────────────── */

/* Format Unix epoch seconds as "YYYY-MM-DD HH:MM_ss" (UTC). The host
 * passes hal_time_epoch in UTC; matching the spec's column scheme. */
static void format_timestamp(uint64_t epoch_sec, char *out,
                             unsigned cap) {
    /* Lightweight gmtime — civil date from days-since-epoch. */
    uint64_t total = epoch_sec;
    unsigned ss = (unsigned)(total % 60); total /= 60;
    unsigned mm = (unsigned)(total % 60); total /= 60;
    unsigned hh = (unsigned)(total % 24); total /= 24;
    /* total = days since 1970-01-01 (Thursday). */
    int d = (int)total;
    /* Civil-from-days: en.wikipedia.org/wiki/Julian_day#Calculation */
    int y, m, dy;
    {
        int z = d + 719468;
        int era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = (unsigned)(z - era * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        y = (int)yoe + era * 400;
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        unsigned mp = (5 * doy + 2) / 153;
        dy = (int)(doy - (153 * mp + 2) / 5 + 1);
        m = (int)(mp < 10 ? mp + 3 : mp - 9);
        if (m <= 2) y++;
    }
    char yb[8], mb[4], db[4], hb[4], nb[4], sb[4];
    u_to_str_padded((unsigned)y, 4, yb, sizeof(yb));
    u_to_str_padded((unsigned)m, 2, mb, sizeof(mb));
    u_to_str_padded((unsigned)dy, 2, db, sizeof(db));
    u_to_str_padded(hh, 2, hb, sizeof(hb));
    u_to_str_padded(mm, 2, nb, sizeof(nb));
    u_to_str_padded(ss, 2, sb, sizeof(sb));
    str_copy(out, yb, cap);
    str_cat(out, "-", cap);
    str_cat(out, mb, cap);
    str_cat(out, "-", cap);
    str_cat(out, db, cap);
    str_cat(out, " ", cap);
    str_cat(out, hb, cap);
    str_cat(out, ":", cap);
    str_cat(out, nb, cap);
    str_cat(out, "_", cap);
    str_cat(out, sb, cap);
}

/* Sanitise a thread title into a filename suffix per spec:
 * lowercase, alphanumeric + hyphens, max 60 chars. */
static void sanitise_title(const char *title, char *out, unsigned cap) {
    unsigned o = 0;
    int prev_dash = 1;
    for (unsigned i = 0; title[i] && o < cap - 1 && o < 60; i++) {
        char c = title[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c;
            prev_dash = 0;
        } else if (!prev_dash && o > 0) {
            out[o++] = '-';
            prev_dash = 1;
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    if (o == 0) { out[o++] = 't'; }
    out[o] = '\0';
}

/* ─── Card emitters per view ──────────────────────────────────── */

static void show_loading(const char *what) {
    show_status("__loading", "Loading…", what);
}

static void render_sections_from_list(const char *json, unsigned len) {
    /* Walk entries:[{name,is_dir,...}, ...] and emit one card per
     * directory entry. The forum collection's section folders are
     * the sub-directories under collections/forum/. */
    unsigned out = 0;
    str_copy(cards_buf, "[", sizeof(cards_buf));
    out = 1;
    int first = 1;
    for (unsigned i = 0; i < len; i++) {
        if (json[i] != '{') continue;
        /* Find matching close brace. */
        int depth = 0;
        unsigned start = i;
        unsigned end = i;
        for (unsigned p = i; p < len; p++) {
            if (json[p] == '{') depth++;
            else if (json[p] == '}') { depth--; if (depth == 0) { end = p; break; } }
        }
        if (end <= start) break;
        char name[BUF_NAME];
        char is_dir[8];
        json_find_string(json + start, end - start + 1, "name", name, sizeof(name));
        json_find_raw(json + start, end - start + 1, "is_dir", is_dir, sizeof(is_dir));
        i = end;
        if (!str_eq(is_dir, "true")) continue;
        if (name[0] == '.' || name[0] == '_' ||
            str_eq(name, "extra") || str_eq(name, "files")) continue;
        if (!first && out < sizeof(cards_buf) - 2) cards_buf[out++] = ',';
        first = 0;
        str_copy(cards_buf + out, "{\"id\":\"", sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), name);
        str_copy(cards_buf + out, ",\"title\":\"", sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), name);
        str_copy(cards_buf + out, ",\"subtitle\":\"section\"",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        str_copy(cards_buf + out,
                 ",\"actions\":[{\"name\":\"open-section:",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        for (unsigned j = 0; name[j] && out < sizeof(cards_buf) - 8; j++) {
            cards_buf[out++] = name[j];
        }
        str_copy(cards_buf + out,
                 "\",\"label\":\"Open\",\"icon\":\"open\"}]}",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
    }
    str_cat(cards_buf, "]", sizeof(cards_buf));
    emit_ui_data("view", cards_buf);
}

static void render_threads_from_list(const char *json, unsigned len) {
    unsigned out = 0;
    str_copy(cards_buf, "[", sizeof(cards_buf));
    out = 1;
    int first = 1;
    for (unsigned i = 0; i < len; i++) {
        if (json[i] != '{') continue;
        int depth = 0;
        unsigned start = i;
        unsigned end = i;
        for (unsigned p = i; p < len; p++) {
            if (json[p] == '{') depth++;
            else if (json[p] == '}') { depth--; if (depth == 0) { end = p; break; } }
        }
        if (end <= start) break;
        char name[BUF_NAME];
        char is_dir[8];
        json_find_string(json + start, end - start + 1, "name", name, sizeof(name));
        json_find_raw(json + start, end - start + 1, "is_dir", is_dir, sizeof(is_dir));
        i = end;
        if (str_eq(is_dir, "true")) continue;
        unsigned nlen = str_len(name);
        if (nlen <= 4 || !str_eq(name + nlen - 4, ".txt")) continue;
        /* Display name = filename without "thread-" prefix and ".txt"
         * suffix, replacing dashes with spaces so it reads natural. */
        char display[BUF_NAME];
        const char *body = name;
        if (str_starts(body, "thread-")) body += 7;
        unsigned dlen = 0;
        while (body[dlen] && body[dlen] != '.' && dlen < sizeof(display) - 1) {
            display[dlen] = (body[dlen] == '-') ? ' ' : body[dlen];
            dlen++;
        }
        display[dlen] = '\0';
        if (display[0] == '\0') str_copy(display, name, sizeof(display));
        if (!first && out < sizeof(cards_buf) - 2) cards_buf[out++] = ',';
        first = 0;
        str_copy(cards_buf + out, "{\"id\":\"", sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), name);
        str_copy(cards_buf + out, ",\"title\":\"", sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), display);
        str_copy(cards_buf + out, ",\"subtitle\":\"",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), name);
        str_copy(cards_buf + out,
                 ",\"actions\":[{\"name\":\"open-thread:",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        for (unsigned j = 0; name[j] && out < sizeof(cards_buf) - 8; j++) {
            cards_buf[out++] = name[j];
        }
        str_copy(cards_buf + out,
                 "\",\"label\":\"Read\",\"icon\":\"open\"}]}",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
    }
    str_cat(cards_buf, "]", sizeof(cards_buf));
    emit_ui_data("view", cards_buf);
}

/* ─── Thread file parsing ─────────────────────────────────────── */

/* Find the next post boundary in thread_buf starting at off.
 * Boundaries are lines that begin with "> 2" followed by a digit
 * (matches a post timestamp like "> 2025-...") OR end-of-buffer.
 * Returns the boundary offset or thread_buf_len when none found. */
static unsigned next_boundary(unsigned off) {
    while (off < thread_buf_len) {
        if (off == 0 || thread_buf[off - 1] == '\n') {
            if (off + 4 < thread_buf_len &&
                thread_buf[off]     == '>' &&
                thread_buf[off + 1] == ' ' &&
                thread_buf[off + 2] == '2' &&
                thread_buf[off + 3] >= '0' && thread_buf[off + 3] <= '9') {
                return off;
            }
        }
        off++;
    }
    return thread_buf_len;
}

/* Strip metadata lines ("--> key: value") from end of a post block.
 * Returns the new effective end (exclusive). Also extracts the
 * author callsign from the first "> ts -- AUTHOR" line into out_author
 * if non-NULL (for replies). */
static unsigned strip_metadata(unsigned start, unsigned end,
                               char *out_meta, unsigned out_meta_cap) {
    if (out_meta) out_meta[0] = '\0';
    unsigned scan = end;
    while (scan > start) {
        /* Find start of last line. */
        unsigned ls = scan;
        while (ls > start && thread_buf[ls - 1] != '\n') ls--;
        /* Strip trailing whitespace from the line. */
        unsigned le = scan;
        while (le > ls && (thread_buf[le - 1] == '\n' || thread_buf[le - 1] == '\r' ||
                            thread_buf[le - 1] == ' ' || thread_buf[le - 1] == '\t')) le--;
        if (le == ls) {  /* empty line */
            scan = ls > 0 ? ls - 1 : start;
            continue;
        }
        if (le - ls >= 4 && thread_buf[ls] == '-' && thread_buf[ls + 1] == '-' &&
            thread_buf[ls + 2] == '>' && thread_buf[ls + 3] == ' ') {
            if (out_meta) {
                /* prepend so older lines come first when assembled later */
                /* simpler: collect newest-first, caller can reorder if needed */
                if (out_meta[0]) {
                    char tmp[256];
                    str_copy(tmp, out_meta, sizeof(tmp));
                    out_meta[0] = '\0';
                    unsigned cw = 0;
                    for (unsigned i = ls; i < le && cw < out_meta_cap - 1; i++)
                        out_meta[cw++] = thread_buf[i];
                    if (cw < out_meta_cap - 2) out_meta[cw++] = '\n';
                    out_meta[cw] = '\0';
                    str_cat(out_meta, tmp, out_meta_cap);
                } else {
                    unsigned cw = 0;
                    for (unsigned i = ls; i < le && cw < out_meta_cap - 1; i++)
                        out_meta[cw++] = thread_buf[i];
                    out_meta[cw] = '\0';
                }
            }
            scan = ls > 0 ? ls - 1 : start;
            continue;
        }
        break;
    }
    /* Trim trailing blank lines. */
    while (scan > start && (thread_buf[scan - 1] == '\n' ||
                             thread_buf[scan - 1] == '\r' ||
                             thread_buf[scan - 1] == ' ' ||
                             thread_buf[scan - 1] == '\t')) scan--;
    return scan;
}

/* Render a thread: original post + replies.
 * Header (first 4 lines) is parsed for AUTHOR and CREATED. The text
 * body (from line 5 to first metadata or boundary) is the original
 * post. Each subsequent boundary marks a reply. */
static void render_thread_view(void) {
    unsigned out = 0;
    str_copy(cards_buf, "[", sizeof(cards_buf));
    out = 1;

    /* Parse header — find "AUTHOR: " and "CREATED: " on lines 2/3. */
    char author[64] = "";
    char created[32] = "";
    char title[128] = "";
    {
        unsigned p = 0;
        /* line 1: "# THREAD: ..." */
        unsigned ls = 0;
        while (p < thread_buf_len && thread_buf[p] != '\n') p++;
        if (str_starts(thread_buf + ls, "# THREAD: ")) {
            unsigned t = ls + 10;
            unsigned to = 0;
            while (t < p && to < sizeof(title) - 1)
                title[to++] = thread_buf[t++];
            while (to > 0 && (title[to - 1] == '\r' || title[to - 1] == ' '))
                to--;
            title[to] = '\0';
        }
        if (p < thread_buf_len) p++;
        /* skip blank line */
        while (p < thread_buf_len && (thread_buf[p] == '\n' || thread_buf[p] == '\r')) p++;
        /* Parse next 3 metadata lines. */
        for (int i = 0; i < 3 && p < thread_buf_len; i++) {
            unsigned line_start = p;
            while (p < thread_buf_len && thread_buf[p] != '\n') p++;
            unsigned line_end = p;
            while (line_end > line_start && (thread_buf[line_end - 1] == '\r' ||
                                              thread_buf[line_end - 1] == ' '))
                line_end--;
            if (str_starts(thread_buf + line_start, "AUTHOR: ")) {
                unsigned t = line_start + 8;
                unsigned o = 0;
                while (t < line_end && o < sizeof(author) - 1)
                    author[o++] = thread_buf[t++];
                author[o] = '\0';
            } else if (str_starts(thread_buf + line_start, "CREATED: ")) {
                unsigned t = line_start + 9;
                unsigned o = 0;
                while (t < line_end && o < sizeof(created) - 1)
                    created[o++] = thread_buf[t++];
                created[o] = '\0';
            }
            if (p < thread_buf_len) p++;
        }
        /* Skip blank line after header. */
        while (p < thread_buf_len && (thread_buf[p] == '\n' ||
                                       thread_buf[p] == '\r')) p++;

        /* Original post body extends to the first reply marker. */
        unsigned op_end = next_boundary(p);
        unsigned op_text_end = strip_metadata(p, op_end, 0, 0);

        /* Emit original-post card. */
        str_copy(cards_buf + out, "{\"id\":\"op\",\"title\":\"",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        out = json_str(cards_buf, out - 1, sizeof(cards_buf),
                        title[0] ? title : "(thread)");
        str_copy(cards_buf + out, ",\"subtitle\":\"",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        char sub[160];
        str_copy(sub, author[0] ? author : "?", sizeof(sub));
        if (created[0]) {
            str_cat(sub, " · ", sizeof(sub));
            str_cat(sub, created, sizeof(sub));
        }
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), sub);
        str_copy(cards_buf + out, ",\"description\":\"",
                 sizeof(cards_buf) - out);
        out = str_len(cards_buf);
        /* Body */
        char body[2048];
        unsigned bl = 0;
        for (unsigned k = p; k < op_text_end && bl < sizeof(body) - 1; k++)
            body[bl++] = thread_buf[k];
        body[bl] = '\0';
        out = json_str(cards_buf, out - 1, sizeof(cards_buf), body);
        str_copy(cards_buf + out, "}", sizeof(cards_buf) - out);
        out = str_len(cards_buf);

        /* Walk replies. */
        unsigned r = op_end;
        int n_reply = 0;
        while (r < thread_buf_len && n_reply < 100) {
            unsigned r_end = next_boundary(r + 1);
            /* Reply marker: "> YYYY-MM-DD HH:MM_ss -- CALLSIGN" */
            unsigned line_end = r;
            while (line_end < r_end && thread_buf[line_end] != '\n') line_end++;
            char r_ts[32] = "";
            char r_who[64] = "";
            if (line_end - r > 22) {
                /* Extract timestamp (chars 2..21 = 19 chars). */
                unsigned o = 0;
                for (unsigned k = r + 2; k < line_end &&
                     thread_buf[k] != ' ' && o < sizeof(r_ts) - 1; k++)
                    r_ts[o++] = thread_buf[k];
                r_ts[o] = '\0';
                /* Find " -- " then read author. */
                for (unsigned k = r; k + 3 < line_end; k++) {
                    if (thread_buf[k] == ' ' && thread_buf[k + 1] == '-' &&
                        thread_buf[k + 2] == '-' && thread_buf[k + 3] == ' ') {
                        unsigned a = k + 4;
                        unsigned ao = 0;
                        while (a < line_end && ao < sizeof(r_who) - 1 &&
                               thread_buf[a] != '\r')
                            r_who[ao++] = thread_buf[a++];
                        r_who[ao] = '\0';
                        break;
                    }
                }
            }
            unsigned body_start = (line_end < thread_buf_len) ? line_end + 1 : line_end;
            unsigned body_end = strip_metadata(body_start, r_end, 0, 0);
            char rbody[2048];
            unsigned rb = 0;
            for (unsigned k = body_start; k < body_end && rb < sizeof(rbody) - 1; k++)
                rbody[rb++] = thread_buf[k];
            rbody[rb] = '\0';

            if (out < sizeof(cards_buf) - 2) cards_buf[out++] = ',';
            str_copy(cards_buf + out, "{\"id\":\"r", sizeof(cards_buf) - out);
            out = str_len(cards_buf);
            char nb[16];
            u_to_str((unsigned)n_reply, nb, sizeof(nb));
            str_cat(cards_buf, nb, sizeof(cards_buf));
            out = str_len(cards_buf);
            str_copy(cards_buf + out, "\",\"title\":\"",
                     sizeof(cards_buf) - out);
            out = str_len(cards_buf);
            out = json_str(cards_buf, out - 1, sizeof(cards_buf),
                           r_who[0] ? r_who : "(anon)");
            str_copy(cards_buf + out, ",\"subtitle\":\"",
                     sizeof(cards_buf) - out);
            out = str_len(cards_buf);
            out = json_str(cards_buf, out - 1, sizeof(cards_buf), r_ts);
            str_copy(cards_buf + out, ",\"description\":\"",
                     sizeof(cards_buf) - out);
            out = str_len(cards_buf);
            out = json_str(cards_buf, out - 1, sizeof(cards_buf), rbody);
            str_copy(cards_buf + out, "}", sizeof(cards_buf) - out);
            out = str_len(cards_buf);

            r = r_end;
            n_reply++;
        }
    }
    str_cat(cards_buf, "]", sizeof(cards_buf));
    emit_ui_data("view", cards_buf);
}

/* ─── View transitions ────────────────────────────────────────── */

static void go_sections(void) {
    view = VIEW_SECTIONS;
    current_section[0] = '\0';
    current_thread[0]  = '\0';
    show_loading("Loading sections…");
    req_profile_list("", REQ_LIST_SECTIONS);
}

static void go_threads(const char *section) {
    view = VIEW_THREADS;
    str_copy(current_section, section, sizeof(current_section));
    current_thread[0] = '\0';
    show_loading("Loading threads…");
    req_profile_list(section, REQ_LIST_THREADS);
}

static void go_thread(const char *thread_file) {
    view = VIEW_POSTS;
    str_copy(current_thread, thread_file, sizeof(current_thread));
    show_loading("Reading thread…");
    char path[BUF_PATH];
    str_copy(path, current_section, sizeof(path));
    str_cat(path, "/", sizeof(path));
    str_cat(path, thread_file, sizeof(path));
    req_profile_read(path, REQ_READ_THREAD);
}

/* ─── Compose flow ────────────────────────────────────────────── */

/* Read a wapp KV-mirrored field value (set by the host whenever the
 * user types into a $type=string field). Returns bytes copied. */
static unsigned read_field(const char *name, char *out, unsigned cap) {
    return hal_kv_get(name, str_len(name), out, cap);
}

/* Build the full text of a new thread file from the title/body
 * fields and the cached identity. Stored in compose_post_buf. */
static void build_new_thread_post(const char *title, const char *body,
                                  uint64_t epoch_sec) {
    char ts[32];
    format_timestamp(epoch_sec, ts, sizeof(ts));
    str_copy(compose_post_buf, "# THREAD: ", sizeof(compose_post_buf));
    str_cat(compose_post_buf, title, sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\n\nAUTHOR: ", sizeof(compose_post_buf));
    str_cat(compose_post_buf,
            identity_callsign[0] ? identity_callsign : "ANON",
            sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\nCREATED: ", sizeof(compose_post_buf));
    str_cat(compose_post_buf, ts, sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\nSECTION: ", sizeof(compose_post_buf));
    str_cat(compose_post_buf, compose_section, sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\n\n", sizeof(compose_post_buf));
    str_cat(compose_post_buf, body, sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\n", sizeof(compose_post_buf));
    compose_post_len = str_len(compose_post_buf);
}

/* Build the full text of a reply block to be appended to an existing
 * thread file. Includes a leading blank line so it sits cleanly
 * after the previous post. */
static void build_reply_post(const char *body, uint64_t epoch_sec) {
    char ts[32];
    format_timestamp(epoch_sec, ts, sizeof(ts));
    str_copy(compose_post_buf, "\n> ", sizeof(compose_post_buf));
    str_cat(compose_post_buf, ts, sizeof(compose_post_buf));
    str_cat(compose_post_buf, " -- ", sizeof(compose_post_buf));
    str_cat(compose_post_buf,
            identity_callsign[0] ? identity_callsign : "ANON",
            sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\n", sizeof(compose_post_buf));
    str_cat(compose_post_buf, body, sizeof(compose_post_buf));
    str_cat(compose_post_buf, "\n", sizeof(compose_post_buf));
    compose_post_len = str_len(compose_post_buf);
}

/* v0.1.0 ships posts unsigned. Spec marks signing as optional, and
 * BIP-340 needs a SHA-256 of the post text — we don't bring SHA-256
 * into the wapp yet, and the host sign.schnorr expects a 32-byte
 * digest. v0.2.0 will add either a sha256 outbox helper or a
 * wapp-side SHA-256 plus the signing call. */

static void compose_submit_after_identity(void) {
    char title[256] = "";
    char body[BUF_THREAD - 512] = "";
    read_field("title", title, sizeof(title));
    read_field("body",  body,  sizeof(body));
    /* Trim leading/trailing whitespace on title — empty body is OK
     * for replies if the spec allows attachments-only, but we
     * require at least a body for new threads. */
    while (title[0] == ' ' || title[0] == '\t') {
        for (unsigned i = 0; title[i]; i++) title[i] = title[i + 1];
    }
    unsigned bl = str_len(body);
    while (bl > 0 && (body[bl - 1] == ' ' || body[bl - 1] == '\n' ||
                       body[bl - 1] == '\r' || body[bl - 1] == '\t')) {
        body[--bl] = '\0';
    }
    if (compose == COMPOSE_NEW) {
        if (title[0] == '\0' || body[0] == '\0') {
            show_status("__err",
                        "Please fill in both title and body",
                        "Then tap Post again.");
            return;
        }
        char fname[BUF_NAME];
        str_copy(fname, "thread-", sizeof(fname));
        char slug[80];
        sanitise_title(title, slug, sizeof(slug));
        str_cat(fname, slug, sizeof(fname));
        str_cat(fname, ".txt", sizeof(fname));
        str_copy(compose_thread, fname, sizeof(compose_thread));
        str_copy(compose_thread_path, compose_section,
                 sizeof(compose_thread_path));
        str_cat(compose_thread_path, "/", sizeof(compose_thread_path));
        str_cat(compose_thread_path, fname, sizeof(compose_thread_path));
        compose_target_is_new = 1;
        build_new_thread_post(title, body, hal_time_epoch());
    } else if (compose == COMPOSE_REPLY) {
        if (body[0] == '\0') {
            show_status("__err",
                        "Reply body cannot be empty",
                        "Then tap Post again.");
            return;
        }
        str_copy(compose_thread_path, compose_section,
                 sizeof(compose_thread_path));
        str_cat(compose_thread_path, "/", sizeof(compose_thread_path));
        str_cat(compose_thread_path, compose_thread,
                sizeof(compose_thread_path));
        compose_target_is_new = 0;
        build_reply_post(body, hal_time_epoch());
    } else {
        return;
    }
#if !FORUM_SIGN_DISABLED
    /* (signing path — needs sha256 helper) */
#endif
    /* Unsigned post: write directly. */
    req_profile_write(compose_thread_path, compose_post_buf,
                      compose_target_is_new ? "write" : "append",
                      compose_target_is_new ? REQ_WRITE_NEW
                                            : REQ_WRITE_REPLY);
}

/* ─── Action dispatch ─────────────────────────────────────────── */

static void handle_action(const char *action) {
    if (str_eq(action, "back")) {
        if (view == VIEW_POSTS)         go_threads(current_section);
        else if (view == VIEW_THREADS)  go_sections();
        return;
    }
    if (str_eq(action, "refresh")) {
        if      (view == VIEW_SECTIONS) go_sections();
        else if (view == VIEW_THREADS)  go_threads(current_section);
        else if (view == VIEW_POSTS)    go_thread(current_thread);
        return;
    }
    if (str_eq(action, "new-thread")) {
        if (view == VIEW_THREADS && current_section[0]) {
            compose = COMPOSE_NEW;
            str_copy(compose_section, current_section,
                     sizeof(compose_section));
            compose_thread[0] = '\0';
            /* Clear any prior field values so the user starts fresh. */
            hal_kv_delete("title", 5);
            hal_kv_delete("body", 4);
        } else {
            show_status("__hint",
                        "Open a section first",
                        "Pick a section, then tap New thread.");
        }
        return;
    }
    if (str_starts(action, "open-section:")) {
        go_threads(action + 13);
        return;
    }
    if (str_starts(action, "open-thread:")) {
        go_thread(action + 12);
        return;
    }
    if (str_eq(action, "post")) {
        if (compose == COMPOSE_NONE) {
            show_status("__hint",
                        "Nothing to post",
                        "Tap New thread or Reply first.");
            return;
        }
        compose_submit_after_identity();
        return;
    }
    if (str_eq(action, "cancel")) {
        compose = COMPOSE_NONE;
        compose_section[0] = '\0';
        compose_thread[0]  = '\0';
        return;
    }
}

/* ─── Response dispatch ───────────────────────────────────────── */

static void handle_response(const char *type, const char *buf,
                            unsigned len) {
    char rb[16];
    json_find_raw(buf, len, "req_id", rb, sizeof(rb));
    int req_id = 0;
    for (unsigned i = 0; rb[i]; i++) {
        if (rb[i] >= '0' && rb[i] <= '9') {
            req_id = req_id * 10 + (rb[i] - '0');
        }
    }
    int kind = take_pending(req_id);
    char status_raw[16];
    json_find_raw(buf, len, "status", status_raw, sizeof(status_raw));
    int status = 0;
    {
        const char *p = status_raw;
        int sign = 1;
        if (*p == '-') { sign = -1; p++; }
        for (; *p >= '0' && *p <= '9'; p++) status = status * 10 + (*p - '0');
        status *= sign;
    }
    if (status != 0) {
        char err[256];
        json_find_string(buf, len, "error", err, sizeof(err));
        char title[80];
        str_copy(title, "Error: ", sizeof(title));
        str_cat(title, type, sizeof(title));
        show_status("__err", title, err[0] ? err : "operation failed");
        /* Reset compose flow on write/sign errors so the user can retry. */
        if (kind == REQ_WRITE_NEW || kind == REQ_WRITE_REPLY ||
            kind == REQ_SIGN_NEW || kind == REQ_SIGN_REPLY) {
            compose = COMPOSE_NONE;
        }
        return;
    }
    if (str_eq(type, "profile.list.response")) {
        /* entries lives in buf — pass to the right renderer. */
        const char *eptr = 0;
        unsigned elen = 0;
        const char *ek = "\"entries\":";
        unsigned eklen = str_len(ek);
        for (unsigned i = 0; i + eklen < len; i++) {
            int m = 1;
            for (unsigned j = 0; j < eklen; j++) {
                if (buf[i + j] != ek[j]) { m = 0; break; }
            }
            if (!m) continue;
            unsigned p = i + eklen;
            while (p < len && buf[p] != '[') p++;
            if (p >= len) break;
            unsigned start = p;
            int depth = 0;
            while (p < len) {
                if (buf[p] == '[') depth++;
                else if (buf[p] == ']') { depth--; if (depth == 0) { p++; break; } }
                p++;
            }
            eptr = buf + start;
            elen = p - start;
            break;
        }
        if (eptr) {
            if (kind == REQ_LIST_SECTIONS) render_sections_from_list(eptr, elen);
            else if (kind == REQ_LIST_THREADS) render_threads_from_list(eptr, elen);
        }
        return;
    }
    if (str_eq(type, "profile.read.response")) {
        if (kind != REQ_READ_THREAD) return;
        char data[BUF_THREAD];
        json_find_string(buf, len, "data", data, sizeof(data));
        unsigned dlen = str_len(data);
        if (dlen >= sizeof(thread_buf)) dlen = sizeof(thread_buf) - 1;
        for (unsigned i = 0; i < dlen; i++) thread_buf[i] = data[i];
        thread_buf[dlen] = '\0';
        thread_buf_len = dlen;
        render_thread_view();
        return;
    }
    if (str_eq(type, "profile.write.response")) {
        if (kind == REQ_WRITE_NEW || kind == REQ_WRITE_REPLY) {
            int was_new = (kind == REQ_WRITE_NEW);
            compose = COMPOSE_NONE;
            compose_section[0] = '\0';
            compose_thread[0]  = '\0';
            log_info("[forum] post written");
            /* Refresh the view so the user sees their work. */
            if (was_new) {
                go_threads(current_section);
            } else if (current_section[0] && current_thread[0]) {
                go_thread(current_thread);
            }
        }
        return;
    }
    if (str_eq(type, "identity.get.response")) {
        json_find_string(buf, len, "callsign",
                         identity_callsign, sizeof(identity_callsign));
        json_find_string(buf, len, "npub",
                         identity_npub, sizeof(identity_npub));
        return;
    }
}

/* ─── Module entry points ─────────────────────────────────────── */

void module_init(void) {
    log_info("[forum] init");
    for (int i = 0; i < MAX_PENDING; i++) {
        pending[i].kind = REQ_NONE;
        pending[i].req_id = 0;
    }
    req_identity();
    go_sections();
}

void module_handle_event(void) {
    char buf[BUF_THREAD + 1024];
    unsigned avail = hal_msg_available();
    if (avail == 0) return;
    unsigned n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = '\0';

    /* Find type field. */
    char type[64];
    if (!json_find_string(buf, n, "type", type, sizeof(type))) return;

    /* Action from a button press: {type:"action", action:"..."} */
    if (str_eq(type, "action")) {
        char action[128];
        json_find_string(buf, n, "action", action, sizeof(action));
        if (action[0]) handle_action(action);
        return;
    }
    /* Response cycles for outbox requests. */
    if (str_starts(type, "profile.") ||
        str_starts(type, "identity.") ||
        str_starts(type, "sign.")) {
        handle_response(type, buf, n);
        return;
    }
    /* Unknown — log and ignore. */
    {
        char dbg[128] = "[forum] unknown msg type: ";
        str_cat(dbg, type, sizeof(dbg));
        log_info(dbg);
    }
}

void module_destroy(void) {
    log_info("[forum] destroy");
}

void module_tick(void) {}
uint32_t module_tick_interval_ms(void) { return 0; }
