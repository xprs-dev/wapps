/*
 * xprs — what this device can hear on the air, and what it is carrying.
 *
 *   hal_xprs_stations  → stations heard, pre-shaped as people-widget sections
 *   hal_xprs_traffic   → the recent packet ring, oldest first
 *   hal_mesh_held      → mail this device holds for OTHER people
 *   hal_mesh_scf_status→ how much of it, and whether carrying is on
 *   hal_mesh_set_pref  → "scfEnabled=0|1", the owner's answer on carrying
 *
 * It transmits nothing of its own. It is a window onto the radio, and shows
 * traffic addressed to OTHER people too — on a mesh that is most of what goes
 * past, and seeing it is the difference between believing the network works
 * and knowing it does.
 *
 * The packet list is a people-widget rather than a log, because a log can only
 * be read top to bottom: sections give free filtering (the widget switches
 * them locally, no round trip), a row gives a tap target for the full packet,
 * and a row action gives the star.
 *
 * Favourites are kept as the packet's OWN JSON, not as an id. The host ring
 * holds 200 sightings and then forgets; a favourite that was only a pointer
 * would rot into a dead reference within minutes of a busy channel.
 *
 * Nothing here arrived over the internet. The host records a packet's bearer
 * where it lands and only collects radio and local ones, so this view cannot
 * show an internet peer even by mistake.
 *
 * Build: cd wapps/xprs && make
 */

#include "../hal/xprs_wasm_hal.h"

/* ── String helpers (no libc under wasm32-wasi -nostartfiles) ─────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Find "key":<value> in flat JSON starting at `json`, copy the raw value. */
static int json_raw(const char *json, const char *key, char *out, unsigned m) {
    char pat[48];
    str_copy(pat, "\"", sizeof(pat));
    str_cat(pat, key, sizeof(pat));
    str_cat(pat, "\":", sizeof(pat));
    unsigned pl = str_len(pat);
    for (const char *p = json; *p; p++) {
        unsigned i = 0;
        while (i < pl && p[i] == pat[i]) i++;
        if (i != pl) continue;
        p += pl;
        unsigned o = 0;
        int instr = 0;
        if (*p == '"') { instr = 1; p++; }
        while (*p && o < m - 1) {
            if (instr) { if (*p == '"') break; }
            else if (*p == ',' || *p == '}') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

static void json_esc(char *d, unsigned m, const char *s) {
    unsigned l = str_len(d);
    for (unsigned i = 0; s[i] && l < m - 2; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { d[l++] = '\\'; d[l++] = c; }
        else if (c == '\n' || c == '\r' || c == '\t') { d[l++] = ' '; }
        else { d[l++] = c; }
    }
    d[l] = '\0';
}

/* Copy one {...} object out of an array, brace-balanced. */
static int obj_copy(const char *p, char *out, unsigned m) {
    if (*p != '{') return 0;
    int depth = 0, instr = 0;
    unsigned o = 0;
    for (; *p && o < m - 1; p++) {
        char c = *p;
        if (instr) { if (c == '"' && p[-1] != '\\') instr = 0; }
        else if (c == '"') instr = 1;
        else if (c == '{') depth++;
        else if (c == '}') { out[o++] = c; if (--depth == 0) { out[o] = '\0'; return 1; } continue; }
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_stations[32768];
static char g_traffic[65536];
static char g_held[32768];
static char g_scf[1024];
static char g_msg[60000];
static char g_favs[16384];     /* favourite packets, own JSON, 0x1E-separated */
static char g_obj[1200];
static char g_open[1200];      /* the packet the detail panel is showing      */

#define FAV_SEP '\036'         /* 0x1E: a record separator, never in JSON text */
#define FAV_MAX 40

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* ── Favourites ──────────────────────────────────────────────────────── */
static void favs_load(void) {
    uint32_t n = hal_kv_get("xprs.favs", 9, g_favs, sizeof(g_favs) - 1);
    if (n >= sizeof(g_favs)) n = 0;
    g_favs[n] = '\0';
}
static void favs_save(void) { hal_kv_set("xprs.favs", 9, g_favs, str_len(g_favs)); }

/* Walk the favourites; each record is one packet object. */
static const char *fav_next(const char *p, char *out, unsigned m) {
    /* ALWAYS terminate the caller's buffer. Every walk below tests `rec[0]`
     * to decide whether it got a record, so returning without touching `out`
     * left them reading uninitialised stack — which is why an empty store
     * counted one favourite. */
    out[0] = '\0';
    if (!p || !*p) return 0;
    unsigned o = 0;
    while (*p && *p != FAV_SEP && o < m - 1) out[o++] = *p++;
    out[o] = '\0';
    while (*p == FAV_SEP) p++;
    return o ? p : 0;
}

static int fav_has(const char *id) {
    char rec[1200], want[16];
    for (const char *p = g_favs; (p = fav_next(p, rec, sizeof(rec))) || rec[0]; ) {
        if (json_raw(rec, "id", want, sizeof(want)) && str_eq(want, id)) return 1;
        if (!p) break;
    }
    return 0;
}

static void fav_remove(const char *id) {
    static char keep[16384];
    char rec[1200], want[16];
    keep[0] = '\0';
    for (const char *p = g_favs; (p = fav_next(p, rec, sizeof(rec))) || rec[0]; ) {
        int drop = json_raw(rec, "id", want, sizeof(want)) && str_eq(want, id);
        if (!drop && rec[0]) {
            if (keep[0]) { unsigned l = str_len(keep); if (l < sizeof(keep) - 2) { keep[l] = FAV_SEP; keep[l + 1] = '\0'; } }
            str_cat(keep, rec, sizeof(keep));
        }
        if (!p) break;
    }
    str_copy(g_favs, keep, sizeof(g_favs));
    favs_save();
}

/* Count records, so the oldest can be shed at the cap. */
static int fav_count(void) {
    int n = 0;
    char rec[1200];
    for (const char *p = g_favs; (p = fav_next(p, rec, sizeof(rec))) || rec[0]; ) {
        if (rec[0]) n++;
        if (!p) break;
    }
    return n;
}

static void fav_add(const char *obj) {
    char id[16] = "";
    if (!json_raw(obj, "id", id, sizeof(id))) return;
    if (fav_has(id)) return;
    while (fav_count() >= FAV_MAX) {
        char rec[1200], oldest[16] = "";
        const char *p = fav_next(g_favs, rec, sizeof(rec));
        (void)p;
        if (!json_raw(rec, "id", oldest, sizeof(oldest))) break;
        fav_remove(oldest);
    }
    if (g_favs[0]) {
        unsigned l = str_len(g_favs);
        if (l < sizeof(g_favs) - 2) { g_favs[l] = FAV_SEP; g_favs[l + 1] = '\0'; }
    }
    str_cat(g_favs, obj, sizeof(g_favs));
    favs_save();
}

/* ── Time ────────────────────────────────────────────────────────────── */
/* Epoch seconds → local "HH:MM". The offset is a host call, so it is read
 * once: a redraw formats every row (docs/performance.md section 4.2). */
static int g_tz_min = 0, g_tz_known = 0;
static void fmt_clock(char *d, unsigned m, unsigned epoch) {
    if (!g_tz_known) { g_tz_min = hal_time_utc_offset(); g_tz_known = 1; }
    long long secs = (long long)epoch + (long long)g_tz_min * 60;
    long long day = secs % 86400;
    if (day < 0) day += 86400;
    unsigned hh = (unsigned)(day / 3600), mm = (unsigned)((day / 60) % 60);
    char b[8];
    b[0] = (char)('0' + hh / 10); b[1] = (char)('0' + hh % 10); b[2] = ':';
    b[3] = (char)('0' + mm / 10); b[4] = (char)('0' + mm % 10); b[5] = '\0';
    str_copy(d, b, m);
}

/* ── What a key means (docs/XPRS.md section 4.2) ─────────────────────── */
/* The packet is readable without a decoder, but "what does lx: mean" is a
 * fair question the first time. Wapp-side on purpose: the host stays generic
 * and knows nothing about XPRS vocabulary. */
static const char *key_meaning(const char *k) {
    if (str_eq(k, "t"))    return "Packet type";
    if (str_eq(k, "f"))    return "From — the station that composed it";
    if (str_eq(k, "d"))    return "To — the station it is addressed to";
    if (str_eq(k, "ts"))   return "When it was composed (UTC)";
    if (str_eq(k, "m"))    return "The message itself";
    if (str_eq(k, "q"))    return "What the sender wants back";
    if (str_eq(k, "s"))    return "What this packet answers or reports";
    if (str_eq(k, "r"))    return "The packet this one refers to";
    if (str_eq(k, "n"))    return "Part i of n";
    if (str_eq(k, "via"))  return "Stations that relayed it, oldest first";
    if (str_eq(k, "sig"))  return "Signature over the packet";
    if (str_eq(k, "lx"))   return "Where to write to this station (LXMF)";
    if (str_eq(k, "link")) return "The bearer this reading is about";
    if (str_eq(k, "peers"))return "How many stations it can hear";
    if (str_eq(k, "hears"))return "Which stations it can hear";
    if (str_eq(k, "mail")) return "Messages it is holding for others";
    if (str_eq(k, "pos"))  return "Position";
    if (str_eq(k, "batt")) return "Battery level";
    if (str_eq(k, "urg"))  return "Urgency";
    if (str_eq(k, "until"))return "Stops mattering after";
    if (str_eq(k, "dest")) return "Region it is addressed to";
    if (str_eq(k, "near")) return "How close to that region counts";
    if (str_eq(k, "root")) return "The packet its thread hangs from";
    if (str_eq(k, "cw"))   return "What it contains, warned before rendering";
    /* Telemetry a station reports about itself (section 10). */
    if (str_eq(k, "uptime"))  return "How long it has run without interruption";
    if (str_eq(k, "lifetime"))return "How long it has run in total, across restarts";
    if (str_eq(k, "snr"))     return "Signal-to-noise ratio";
    if (str_eq(k, "rssi"))    return "How loud it was heard";
    if (str_eq(k, "busy"))    return "How much of the last hour the channel was busy";
    if (str_eq(k, "txtime"))  return "How much of the last hour it was transmitting";
    if (str_eq(k, "temp"))    return "Temperature";
    if (str_eq(k, "hum"))     return "Humidity";
    if (str_eq(k, "type"))    return "What the station is, or is riding on";
    if (str_eq(k, "alt"))     return "Altitude";
    if (str_eq(k, "spd"))     return "Speed";
    if (str_eq(k, "crs"))     return "Course";
    return "";
}

/* ── One row in the packet list ──────────────────────────────────────── */
static void item_json(char *out, unsigned m, const char *obj, int starred) {
    char bearer[16] = "", rssi[12] = "", from[24] = "", to[24] = "";
    char type[24] = "", mine[8] = "", id[16] = "", ts[16] = "";
    json_raw(obj, "bearer", bearer, sizeof(bearer));
    json_raw(obj, "rssi", rssi, sizeof(rssi));
    json_raw(obj, "from", from, sizeof(from));
    json_raw(obj, "to", to, sizeof(to));
    json_raw(obj, "type", type, sizeof(type));
    json_raw(obj, "mine", mine, sizeof(mine));
    json_raw(obj, "id", id, sizeof(id));
    json_raw(obj, "ts", ts, sizeof(ts));

    str_cat(out, "{\"id\":\"", m); json_esc(out, m, id);
    /* Title: who is talking to whom, which is what a person scans for. */
    str_cat(out, "\",\"title\":\"", m);
    json_esc(out, m, from[0] ? from : "(unknown)");
    if (to[0]) { str_cat(out, " \\u2192 ", m); json_esc(out, m, to); }
    else       { str_cat(out, " \\u2192 everyone", m); }

    /* Subtitle: the human summary — when, what, how loud. */
    str_cat(out, "\",\"subtitle\":\"", m);
    { unsigned e = 0; for (unsigned i = 0; ts[i] >= '0' && ts[i] <= '9'; i++) e = e * 10 + (unsigned)(ts[i] - '0');
      char clock[8]; fmt_clock(clock, sizeof(clock), e); json_esc(out, m, clock); }
    str_cat(out, "  \\u00b7  ", m);
    json_esc(out, m, type[0] ? type : "packet");

    str_cat(out, "\",\"tags\":[\"", m);
    { char up[16]; str_copy(up, bearer, sizeof(up));
      for (unsigned i = 0; up[i]; i++) if (up[i] >= 'a' && up[i] <= 'z') up[i] = (char)(up[i] - 32);
      json_esc(out, m, up[0] ? up : "AIR"); }
    str_cat(out, "\",\"", m);
    if (str_eq(mine, "true")) str_cat(out, "for us", m);
    else if (to[0])           str_cat(out, "passing", m);
    else                      str_cat(out, "broadcast", m);
    if (rssi[0] && !str_eq(rssi, "0")) {
        str_cat(out, "\",\"", m); json_esc(out, m, rssi); str_cat(out, " dBm", m);
    }
    str_cat(out, "\"],", m);

    /* The star. A row action, so it never costs a screen change. */
    str_cat(out, "\"action\":\"", m);
    str_cat(out, starred ? "unfav" : "fav", m);
    str_cat(out, "\",\"actionLabel\":\"", m);
    str_cat(out, starred ? "\\u2605" : "\\u2606", m);
    str_cat(out, "\"}", m);
}

/* ── Traffic ─────────────────────────────────────────────────────────── */

/* Does this packet belong in section [want]?
 * 0 all · 1 for us · 2 messages · 3 beacons · 4 everything else */
static int in_section(const char *obj, int want) {
    char type[24] = "", mine[8] = "";
    json_raw(obj, "type", type, sizeof(type));
    json_raw(obj, "mine", mine, sizeof(mine));
    if (want == 0) return 1;
    if (want == 1) return str_eq(mine, "true");
    if (want == 2) return str_eq(type, "message");
    if (want == 3) return str_eq(type, "observation");
    return !str_eq(type, "message") && !str_eq(type, "observation");
}

/* A section names an ICON, so the filter strip is icons rather than six
 * wrapped words. The title rides along as the tooltip. */
static void section_open_icon(const char *title, const char *icon) {
    if (g_msg[str_len(g_msg) - 1] != '[') str_cat(g_msg, ",", sizeof(g_msg));
    str_cat(g_msg, "{\"title\":\"", sizeof(g_msg));
    json_esc(g_msg, sizeof(g_msg), title);
    if (icon && icon[0]) {
        str_cat(g_msg, "\",\"icon\":\"", sizeof(g_msg));
        json_esc(g_msg, sizeof(g_msg), icon);
    }
    str_cat(g_msg, "\",\"items\":[", sizeof(g_msg));
}
static void section_close(void) { str_cat(g_msg, "]}", sizeof(g_msg)); }

/* Newest first: a person looking at a radio wants the last thing said. */
static void push_packets(void) {
    int n = hal_xprs_traffic(g_traffic, sizeof(g_traffic) - 1);
    if (n < 0) return;
    g_traffic[n > 0 ? n : 0] = '\0';

    static const char *names[5] = {"All", "For us", "Messages", "Beacons", "Other"};
    static const char *icons[5] = {"list", "mail", "chat", "radar", "more_horiz"};
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"packets\",\"sections\":[",
             sizeof(g_msg));

    for (int sec = 0; sec < 5; sec++) {
        /* The widget appends "(n)" itself — see
         * people_view_field.dart: a title carrying its own count renders as
         * "All (24) (24)". */
        section_open_icon(names[sec], icons[sec]);

        /* Walk backwards over the array so the newest row is first. The ring
         * is small (200) and this runs on a 3s tick, so a rescan is cheaper
         * than keeping a parallel index in sync. */
        int emitted = 0;
        for (const char *p = g_traffic + str_len(g_traffic); p >= g_traffic && emitted < 60; p--) {
            if (*p != '{') continue;
            if (!obj_copy(p, g_obj, sizeof(g_obj))) continue;
            if (!in_section(g_obj, sec)) continue;
            char id[16] = "";
            json_raw(g_obj, "id", id, sizeof(id));
            if (emitted) str_cat(g_msg, ",", sizeof(g_msg));
            item_json(g_msg, sizeof(g_msg), g_obj, fav_has(id));
            emitted++;
        }
        section_close();
    }

    /* Favourites last, and from their own copies — the ring has long since
     * forgotten most of them. */
    {
        if (g_msg[str_len(g_msg) - 1] != '[') str_cat(g_msg, ",", sizeof(g_msg));
        /* The star is written as an escape so the source stays ASCII. */
        str_cat(g_msg, "{\"title\":\"Favourites\",\"icon\":\"star\",\"items\":[", sizeof(g_msg));
        int emitted = 0;
        char rec[1200];
        for (const char *p = g_favs; (p = fav_next(p, rec, sizeof(rec))) || rec[0]; ) {
            if (rec[0]) {
                if (emitted) str_cat(g_msg, ",", sizeof(g_msg));
                item_json(g_msg, sizeof(g_msg), rec, 1);
                emitted++;
            }
            if (!p) break;
        }
        section_close();
    }

    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Stations ────────────────────────────────────────────────────────── */
static void push_stations(void) {
    int n = hal_xprs_stations(g_stations, sizeof(g_stations) - 1);
    if (n <= 0) return;
    g_stations[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":",
             sizeof(g_msg));
    str_cat(g_msg, g_stations, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Carried: what we hold for other people ──────────────────────────── */
static void push_carry_switch(void) {
    int n = hal_mesh_scf_status(g_scf, sizeof(g_scf) - 1);
    if (n <= 0) return;
    g_scf[n] = '\0';
    char en[8] = "1";
    json_raw(g_scf, "enabled", en, sizeof(en));
    str_copy(g_msg, "{\"type\":\"ui.field.set\",\"field\":\"carry_on\",\"value\":",
             sizeof(g_msg));
    str_cat(g_msg, str_eq(en, "0") ? "false" : "true", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void push_held(void) {
    int n = hal_mesh_held(g_held, sizeof(g_held) - 1);
    if (n < 0) return;
    g_held[n > 0 ? n : 0] = '\0';

    int counts[2] = {0, 0};
    for (const char *p = g_held; *p; p++) {
        if (*p != '{') continue;
        if (obj_copy(p, g_obj, sizeof(g_obj))) {
            char st[8] = "0"; json_raw(g_obj, "state", st, sizeof(st));
            counts[str_eq(st, "1") ? 1 : 0]++;
        }
        while (*p && *p != '}') p++;
        if (!*p) break;
    }

    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"held\",\"sections\":[",
             sizeof(g_msg));
    static const char *names[2] = {"Waiting to hand on", "Delivered, kept a while"};
    static const char *icons[2] = {"upload", "check"};
    for (int want = 0; want < 2; want++) {
        section_open_icon(names[want], icons[want]);
        int emitted = 0;
        for (const char *p = g_held; *p && emitted < 60; p++) {
            if (*p != '{') continue;
            if (obj_copy(p, g_obj, sizeof(g_obj))) {
                char st[8] = "0"; json_raw(g_obj, "state", st, sizeof(st));
                if ((str_eq(st, "1") ? 1 : 0) == want) {
                    char am[24] = "", target[24] = "", sender[24] = "", size[16] = "", ts[16] = "";
                    json_raw(g_obj, "am", am, sizeof(am));
                    json_raw(g_obj, "target", target, sizeof(target));
                    json_raw(g_obj, "sender", sender, sizeof(sender));
                    json_raw(g_obj, "size", size, sizeof(size));
                    json_raw(g_obj, "ts", ts, sizeof(ts));
                    if (emitted) str_cat(g_msg, ",", sizeof(g_msg));
                    str_cat(g_msg, "{\"id\":\"", sizeof(g_msg));
                    json_esc(g_msg, sizeof(g_msg), am);
                    str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
                    json_esc(g_msg, sizeof(g_msg), sender[0] ? sender : "(unknown)");
                    str_cat(g_msg, " \\u2192 ", sizeof(g_msg));
                    json_esc(g_msg, sizeof(g_msg), target[0] ? target : "(anyone)");
                    str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg));
                    { unsigned e = 0; for (unsigned i = 0; ts[i] >= '0' && ts[i] <= '9'; i++) e = e * 10 + (unsigned)(ts[i] - '0');
                      char clock[8]; fmt_clock(clock, sizeof(clock), e); json_esc(g_msg, sizeof(g_msg), clock); }
                    str_cat(g_msg, "  \\u00b7  ", sizeof(g_msg));
                    json_esc(g_msg, sizeof(g_msg), size);
                    str_cat(g_msg, " bytes\"}", sizeof(g_msg));
                    emitted++;
                }
            }
            while (*p && *p != '}') p++;
            if (!*p) break;
        }
        section_close();
    }
    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Carry: browse a nearby station's bag (hal_mesh_carry) ───────────── *
 *
 * Two modes on one people-widget. With no station chosen it lists the
 * stations whose beacon says they hold mail; tapping one starts the browse
 * (a real dial — seconds), and the tick polls until the listing arrives.
 * Rows then toggle a pick mark exactly like the traffic star, and the
 * "Carry selected" action pulls custody of the picks over the session. */
static char g_carry_station[12] = "";   /* "" = showing the station list */
static char g_carry[8192];              /* last hal_mesh_carry reply */
static char g_picked[26][8];
static int  g_picked_n;
static int  g_carry_pulled;             /* >0: show the "took N" note once */

static void carry_cmd(const char *cmd) {
    int n = hal_mesh_carry(cmd, str_len(cmd), g_carry, sizeof(g_carry) - 1);
    g_carry[n > 0 ? n : 0] = '\0';
}

static int pick_has(const char *id) {
    for (int i = 0; i < g_picked_n; i++) if (str_eq(g_picked[i], id)) return 1;
    return 0;
}
static void pick_toggle(const char *id) {
    for (int i = 0; i < g_picked_n; i++) {
        if (str_eq(g_picked[i], id)) {
            g_picked[i][0] = '\0';
            str_copy(g_picked[i], g_picked[g_picked_n - 1], sizeof(g_picked[i]));
            g_picked_n--;
            return;
        }
    }
    if (g_picked_n < (int)(sizeof(g_picked) / sizeof(g_picked[0])))
        str_copy(g_picked[g_picked_n++], id, sizeof(g_picked[0]));
}

static void fmt_age(char *d, unsigned m, const char *secs) {
    unsigned s = 0;
    for (unsigned i = 0; secs[i] >= '0' && secs[i] <= '9'; i++) s = s * 10 + (unsigned)(secs[i] - '0');
    char b[24];
    if (s < 60)          { str_copy(b, "just now", sizeof(b)); }
    else if (s < 3600)   { unsigned v = s / 60;    b[0] = 0; if (v >= 10) { b[0] = (char)('0' + v / 10); b[1] = (char)('0' + v % 10); b[2] = 0; } else { b[0] = (char)('0' + v); b[1] = 0; } str_cat(b, "m ago", sizeof(b)); }
    else if (s < 86400)  { unsigned v = s / 3600;  b[0] = (char)('0' + v / 10); b[1] = (char)('0' + v % 10); b[2] = 0; if (b[0] == '0') { b[0] = b[1]; b[1] = 0; } str_cat(b, "h ago", sizeof(b)); }
    else                 { unsigned v = s / 86400; b[0] = (char)('0' + v % 10); b[1] = 0; str_cat(b, "d ago", sizeof(b)); }
    str_copy(d, b, m);
}

/* One row of the station's listing. */
static void carry_item(const char *obj, int first) {
    char id[12] = "", target[24] = "", len[12] = "", age[16] = "", urg[8] = "";
    json_raw(obj, "id", id, sizeof(id));
    json_raw(obj, "target", target, sizeof(target));
    json_raw(obj, "len", len, sizeof(len));
    json_raw(obj, "age", age, sizeof(age));
    json_raw(obj, "urg", urg, sizeof(urg));
    static const char *urgn[4] = {"low", "normal", "high", "urgent"};
    int u = (urg[0] >= '0' && urg[0] <= '3') ? urg[0] - '0' : 1;

    if (!first) str_cat(g_msg, ",", sizeof(g_msg));
    str_cat(g_msg, "{\"id\":\"", sizeof(g_msg)); json_esc(g_msg, sizeof(g_msg), id);
    str_cat(g_msg, "\",\"title\":\"For ", sizeof(g_msg));
    json_esc(g_msg, sizeof(g_msg), target[0] ? target : "(anyone)");
    str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg));
    json_esc(g_msg, sizeof(g_msg), len);
    str_cat(g_msg, " bytes  \\u00b7  parked ", sizeof(g_msg));
    { char a[16]; fmt_age(a, sizeof(a), age); json_esc(g_msg, sizeof(g_msg), a); }
    str_cat(g_msg, "\",\"tags\":[\"", sizeof(g_msg));
    str_cat(g_msg, urgn[u], sizeof(g_msg));
    str_cat(g_msg, "\"],\"action\":\"", sizeof(g_msg));
    str_cat(g_msg, pick_has(id) ? "unpick" : "pick", sizeof(g_msg));
    str_cat(g_msg, "\",\"actionLabel\":\"", sizeof(g_msg));
    str_cat(g_msg, pick_has(id) ? "\\u2611" : "\\u2610", sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
}

static void push_remote(void) {
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"remote\",\"sections\":[",
             sizeof(g_msg));

    if (!g_carry_station[0]) {
        /* The stations whose beacon carries a mail count. */
        int n = hal_xprs_stations(g_stations, sizeof(g_stations) - 1);
        if (n > 0) g_stations[n] = '\0';
        section_open_icon("Stations carrying mail", "luggage");
        int emitted = 0;
        for (const char *p = g_stations; *p; p++) {
            if (*p != '{') continue;
            if (!obj_copy(p, g_obj, sizeof(g_obj))) continue;
            char id[16] = "";
            if (!json_raw(g_obj, "id", id, sizeof(id))) continue;
            /* The mail count rides the tags as "mail N" (section 10.6.5). */
            const char *mail = 0;
            for (const char *q = g_obj; *q; q++) {
                if (q[0] == 'm' && q[1] == 'a' && q[2] == 'i' && q[3] == 'l' &&
                    q[4] == ' ' && q[5] >= '0' && q[5] <= '9') { mail = q + 5; break; }
            }
            if (!mail) { while (*p && *p != '}') p++; if (!*p) break; continue; }
            char cnt[8]; unsigned ci = 0;
            while (mail[ci] >= '0' && mail[ci] <= '9' && ci < sizeof(cnt) - 1) { cnt[ci] = mail[ci]; ci++; }
            cnt[ci] = '\0';
            if (emitted) str_cat(g_msg, ",", sizeof(g_msg));
            str_cat(g_msg, "{\"id\":\"", sizeof(g_msg)); json_esc(g_msg, sizeof(g_msg), id);
            str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg)); json_esc(g_msg, sizeof(g_msg), id);
            str_cat(g_msg, "\",\"subtitle\":\"carrying ", sizeof(g_msg));
            str_cat(g_msg, cnt, sizeof(g_msg));
            str_cat(g_msg, " \\u00b7 tap to open its bag\",\"tags\":[\"", sizeof(g_msg));
            str_cat(g_msg, cnt, sizeof(g_msg));
            str_cat(g_msg, " held\"]}", sizeof(g_msg));
            emitted++;
            while (*p && *p != '}') p++;
            if (!*p) break;
        }
        section_close();
        str_cat(g_msg, "]}", sizeof(g_msg));
        send_msg(g_msg);
        return;
    }

    /* A station is open: poll the broker and draw what it has. */
    carry_cmd("{\"op\":\"status\"}");
    char state[12] = "";
    json_raw(g_carry, "state", state, sizeof(state));

    {
        char title[64];
        str_copy(title, "Mail carried by ", sizeof(title));
        str_cat(title, g_carry_station, sizeof(title));
        section_open_icon(title, "luggage");
    }

    if (str_eq(state, "busy")) {
        str_cat(g_msg, "{\"id\":\"-\",\"title\":\"Asking ", sizeof(g_msg));
        json_esc(g_msg, sizeof(g_msg), g_carry_station);
        str_cat(g_msg, "\\u2026\",\"subtitle\":\"dialling the station - a few seconds\"}",
                sizeof(g_msg));
    } else if (str_eq(state, "fail")) {
        str_cat(g_msg,
                "{\"id\":\"-\",\"title\":\"Could not reach it\","
                "\"subtitle\":\"radio busy or out of range - "
                "tap a station to try again\"}", sizeof(g_msg));
    } else if (str_eq(state, "pulling")) {
        str_cat(g_msg, "{\"id\":\"-\",\"title\":\"Taking custody\\u2026\","
                       "\"subtitle\":\"the messages transfer over this session\"}",
                sizeof(g_msg));
    } else {
        int first = 1;
        if (str_eq(state, "pulled") && g_carry_pulled > 0) {
            char note[8];
            unsigned ni = 0, v = (unsigned)g_carry_pulled;
            if (v >= 10) note[ni++] = (char)('0' + v / 10);
            note[ni++] = (char)('0' + v % 10);
            note[ni] = '\0';
            str_cat(g_msg, "{\"id\":\"-\",\"title\":\"Took ", sizeof(g_msg));
            str_cat(g_msg, note, sizeof(g_msg));
            str_cat(g_msg, " with you\",\"subtitle\":\"now on the Carried screen - "
                           "yours to deliver\"}", sizeof(g_msg));
            first = 0;
        }
        int emitted = 0;
        for (const char *p = g_carry; *p; p++) {
            if (*p != '{') continue;
            /* Skip the reply envelope itself (it carries "state"). */
            if (p == g_carry) continue;
            if (!obj_copy(p, g_obj, sizeof(g_obj))) continue;
            char t[24] = "";
            if (!json_raw(g_obj, "target", t, sizeof(t))) { while (*p && *p != '}') p++; if (!*p) break; continue; }
            carry_item(g_obj, first && !emitted);
            emitted++; first = 0;
            while (*p && *p != '}') p++;
            if (!*p) break;
        }
        if (!emitted && !str_eq(state, "pulled")) {
            if (!first) str_cat(g_msg, ",", sizeof(g_msg));
            str_cat(g_msg,
                    "{\"id\":\"-\",\"title\":\"Its bag is empty\","
                    "\"subtitle\":\"nothing pickable right now - mail parked "
                    "before the XPRS update delivers on its own\"}",
                    sizeof(g_msg));
        }
    }
    section_close();
    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);
}

static void carry_open(const char *station) {
    str_copy(g_carry_station, station, sizeof(g_carry_station));
    g_picked_n = 0;
    g_carry_pulled = 0;
    char cmd[64];
    str_copy(cmd, "{\"op\":\"browse\",\"station\":\"", sizeof(cmd));
    str_cat(cmd, station, sizeof(cmd));
    str_cat(cmd, "\"}", sizeof(cmd));
    carry_cmd(cmd);
    push_remote();
}

static void carry_pull(void) {
    if (!g_picked_n || !g_carry_station[0]) return;
    static char cmd[512];
    str_copy(cmd, "{\"op\":\"pull\",\"station\":\"", sizeof(cmd));
    str_cat(cmd, g_carry_station, sizeof(cmd));
    str_cat(cmd, "\",\"ids\":[", sizeof(cmd));
    for (int i = 0; i < g_picked_n; i++) {
        if (i) str_cat(cmd, ",", sizeof(cmd));
        str_cat(cmd, "\"", sizeof(cmd));
        str_cat(cmd, g_picked[i], sizeof(cmd));
        str_cat(cmd, "\"", sizeof(cmd));
    }
    str_cat(cmd, "]}", sizeof(cmd));
    g_carry_pulled = g_picked_n;
    g_picked_n = 0;
    carry_cmd(cmd);
    push_remote();
}

static void carry_close(void) {
    g_carry_station[0] = '\0';
    g_picked_n = 0;
    g_carry_pulled = 0;
    carry_cmd("{\"op\":\"reset\"}");
    push_remote();
}

/* ── The packet, in full ─────────────────────────────────────────────── */

/* Find a packet by id, in the ring first and then the favourites. */
static int find_packet(const char *id, char *out, unsigned m) {
    char got[16];
    for (const char *p = g_traffic; *p; p++) {
        if (*p != '{') continue;
        if (obj_copy(p, out, m) && json_raw(out, "id", got, sizeof(got)) && str_eq(got, id)) return 1;
        while (*p && *p != '}') p++;
        if (!*p) break;
    }
    char rec[1200];
    for (const char *p = g_favs; (p = fav_next(p, rec, sizeof(rec))) || rec[0]; ) {
        if (rec[0] && json_raw(rec, "id", got, sizeof(got)) && str_eq(got, id)) {
            str_copy(out, rec, m);
            return 1;
        }
        if (!p) break;
    }
    return 0;
}

/* One row per field of the wire, with what that field means.
 *
 * A `details` field, not a people list: a people row draws an identicon and a
 * name, so pointing it at the fields of a packet gave every one of them a
 * meaningless generated face and truncated the explanation into "the station
 * that composed…". This reads as an account of a message instead. */
static void detail_item(const char *label, const char *value, const char *key,
                        int mono, int first) {
    if (!first) str_cat(g_msg, ",", sizeof(g_msg));
    str_cat(g_msg, "{\"label\":\"", sizeof(g_msg));
    json_esc(g_msg, sizeof(g_msg), label);
    str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
    json_esc(g_msg, sizeof(g_msg), value);
    if (key && key[0]) {
        str_cat(g_msg, "\",\"key\":\"", sizeof(g_msg));
        json_esc(g_msg, sizeof(g_msg), key);
    }
    str_cat(g_msg, mono ? "\",\"mono\":true}" : "\"}", sizeof(g_msg));
}

/* Is this value an identifier rather than words? Those read better in
 * monospace, where the shape of each character is comparable. */
static int looks_like_id(const char *v) {
    unsigned n = 0, hex = 0;
    for (; v[n]; n++) {
        char c = v[n];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) hex++;
    }
    return n >= 12 && hex == n;
}

static void push_detail(const char *obj) {
    char wire[400] = "", type[24] = "", bearer[16] = "", rssi[12] = "", id[16] = "";
    char from[24] = "", to[24] = "", ts[16] = "", mine[8] = "";
    json_raw(obj, "wire", wire, sizeof(wire));
    json_raw(obj, "type", type, sizeof(type));
    json_raw(obj, "bearer", bearer, sizeof(bearer));
    json_raw(obj, "rssi", rssi, sizeof(rssi));
    json_raw(obj, "id", id, sizeof(id));
    json_raw(obj, "from", from, sizeof(from));
    json_raw(obj, "to", to, sizeof(to));
    json_raw(obj, "ts", ts, sizeof(ts));
    json_raw(obj, "mine", mine, sizeof(mine));

    str_copy(g_msg, "{\"type\":\"ui.field.set\",\"field\":\"detail\",\"value\":[",
             sizeof(g_msg));

    /* A plain-language summary first, so the screen answers "what is this?"
     * before it answers "what are its fields?". */
    str_cat(g_msg, "{\"title\":\"In short\",\"items\":[", sizeof(g_msg));
    {
        char line[200];
        str_copy(line, from[0] ? from : "An unknown station", sizeof(line));
        if (str_eq(mine, "true"))      str_cat(line, " sent this to us", sizeof(line));
        else if (to[0])              { str_cat(line, " sent this to ", sizeof(line)); str_cat(line, to, sizeof(line)); }
        else                           str_cat(line, " broadcast this to everyone", sizeof(line));
        detail_item("What happened", line, "", 0, 1);

        char when[8]; unsigned e = 0;
        for (unsigned i = 0; ts[i] >= '0' && ts[i] <= '9'; i++) e = e * 10 + (unsigned)(ts[i] - '0');
        fmt_clock(when, sizeof(when), e);
        detail_item("Heard at", when, "", 0, 0);
        detail_item("Kind of packet", type[0] ? type : "packet", "t", 0, 0);
    }
    str_cat(g_msg, "]},", sizeof(g_msg));

    /* Every field of the wire, with what it means. */
    str_cat(g_msg, "{\"title\":\"What it says\",\"items\":[", sizeof(g_msg));
    int first = 1;
    unsigned i = 0;
    while (wire[i]) {
        while (wire[i] == ' ') i++;
        if (!wire[i]) break;
        char key[16] = ""; unsigned k = 0;
        while (wire[i] && wire[i] != ':' && wire[i] != ' ' && k < sizeof(key) - 1) key[k++] = wire[i++];
        key[k] = '\0';
        if (wire[i] != ':') { while (wire[i] && wire[i] != ' ') i++; continue; }
        i++;
        char val[300] = ""; unsigned v = 0;
        int greedy = str_eq(key, "m") || str_eq(key, "cw");
        while (wire[i] && (greedy || wire[i] != ' ') && v < sizeof(val) - 1) val[v++] = wire[i++];
        val[v] = '\0';
        const char *mean = key_meaning(key);
        detail_item(mean[0] ? mean : "Field", val, key, looks_like_id(val), first);
        first = 0;
    }
    if (first) detail_item("Nothing readable in this packet", "", "", 0, 1);
    str_cat(g_msg, "]},", sizeof(g_msg));

    /* How it reached us, and the bytes exactly as they were on the air. */
    str_cat(g_msg, "{\"title\":\"How it arrived\",\"items\":[", sizeof(g_msg));
    {
        char b[40];
        str_copy(b, "", sizeof(b));
        for (unsigned j = 0; bearer[j] && j < 8; j++) {
            char c = bearer[j];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            unsigned l = str_len(b); b[l] = c; b[l + 1] = '\0';
        }
        if (rssi[0] && !str_eq(rssi, "0")) { str_cat(b, " at ", sizeof(b)); str_cat(b, rssi, sizeof(b)); str_cat(b, " dBm", sizeof(b)); }
        detail_item("Bearer that carried it - never the internet", b[0] ? b : "air", "", 0, 1);
        detail_item("The packet exactly as it was on the air", wire, "", 1, 0);
        detail_item("Identifier - derived from the packet, never transmitted", id, "", 1, 0);
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);

    char m[200];
    str_copy(m, "{\"type\":\"ui.screen.open\",\"name\":\"Packet\",\"title\":\"", sizeof(m));
    json_esc(m, sizeof(m), type[0] ? type : "packet");
    str_cat(m, "\"}", sizeof(m));
    send_msg(m);
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[xprs] listening to the air", 27);
    favs_load();
    push_stations();
    push_packets();
    push_carry_switch();
    push_held();
    return 0;
}

int32_t module_tick(void) {
    push_stations();
    push_packets();
    push_remote();          /* cheap: a status poll, or the station list */
    return 0;
}

int32_t module_handle_event(void) {
    static char buf[2048];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';
    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        favs_load();
        push_stations();
        push_packets();
        push_carry_switch();
        push_held();
        push_remote();
        return 0;
    }

    /* Carry screen: open a station's bag / pick / take / back. */
    if (str_eq(cmd, "remote_tap")) {
        char id[16] = "";
        json_raw(buf, "remote_id", id, sizeof(id));
        if (!id[0] || str_eq(id, "-")) return 0;
        if (!g_carry_station[0]) carry_open(id);
        else { pick_toggle(id); push_remote(); }
        return 0;
    }
    if (str_eq(cmd, "remote_pick") || str_eq(cmd, "remote_unpick")) {
        char id[16] = "";
        json_raw(buf, "remote_id", id, sizeof(id));
        if (id[0] && !str_eq(id, "-")) { pick_toggle(id); push_remote(); }
        return 0;
    }
    if (str_eq(cmd, "remote_carry")) { carry_pull(); return 0; }
    if (str_eq(cmd, "remote_back"))  { carry_close(); return 0; }

    /* Star / unstar, from the row itself. */
    if (str_eq(cmd, "packets_fav") || str_eq(cmd, "packets_unfav")) {
        char id[16] = "";
        json_raw(buf, "packets_id", id, sizeof(id));
        if (!id[0]) return 0;
        if (str_eq(cmd, "packets_unfav")) fav_remove(id);
        else if (find_packet(id, g_obj, sizeof(g_obj))) fav_add(g_obj);
        push_packets();
        return 0;
    }

    /* A row tapped: the whole packet, field by field. */
    if (str_eq(cmd, "packets_tap")) {
        char id[16] = "";
        json_raw(buf, "packets_id", id, sizeof(id));
        if (id[0] && find_packet(id, g_open, sizeof(g_open))) push_detail(g_open);
        return 0;
    }

    /* The star on the detail panel acts on whatever it is showing. */
    if (str_eq(cmd, "detail_fav")) {
        if (!g_open[0]) return 0;
        char id[16] = "";
        json_raw(g_open, "id", id, sizeof(id));
        if (!id[0]) return 0;
        if (fav_has(id)) fav_remove(id); else fav_add(g_open);
        push_packets();
        return 0;
    }

    if (str_eq(cmd, "detail_back")) {
        send_msg("{\"type\":\"ui.screen.close\"}");
        return 0;
    }

    /* The owner's answer on carrying other people's mail. */
    if (str_eq(cmd, "carry_apply")) {
        char on[8] = "";
        json_raw(buf, "carry_on", on, sizeof(on));
        const char *kv = str_eq(on, "true") ? "scfEnabled=1" : "scfEnabled=0";
        hal_mesh_set_pref(kv, str_len(kv));
        push_carry_switch();
        push_held();
        return 0;
    }

    if (str_eq(cmd, "held_refresh")) {
        push_carry_switch();
        push_held();
        return 0;
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 3000; }

void module_destroy(void) {}
