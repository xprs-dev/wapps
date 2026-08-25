/*
 * Native integration test for the mesh wapp. Drives module_init/tick/
 * handle_event against the canned mock HAL and asserts the messages the wapp
 * emits (graph/status pushes, hub list, host-action forwarding).
 *
 * Run:  sh tests/native/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* From the wapp */
void module_init(void);
void module_tick(void);
void module_handle_event(void);

/* From hal_mock.c */
void cap_clear(void);
int  cap_contains(const char *s);
int  cap_count(void);
const char *cap_at(int i);
void inbox_set(const char *s);
const char *last_filter(void);
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap);

static int g_fail = 0;
static void check(int cond, const char *name) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("mesh native test\n");

    module_init();

    /* ── tick streams graph + hubs ── */
    cap_clear();
    module_tick();
    check(cap_contains("\"type\":\"ui.graph.set\""), "tick pushes graph to host");
    check(cap_contains("\"nodes\""), "graph carries nodes");
    check(cap_contains("\"edges\""), "graph carries edges");
    check(cap_contains("\"type\":\"ui.graph.hubs\""), "hubs forwarded to host");
    check(cap_contains("rns.beleth.net:4242"), "hub endpoint forwarded");
    check(cap_contains("\"connected\":true"), "hub connected flag forwarded");

    /* ── graph_filter from the page persists + re-fetches with the filter ── */
    cap_clear();
    inbox_set("{\"command\":\"graph_filter\",\"xprsOnly\":true,"
              "\"service\":\"files\",\"search\":\"ab\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""), "filter triggers graph re-push");
    check(strstr(last_filter(), "\"xprsOnly\":true") != NULL,
          "filter forwards xprsOnly");
    check(strstr(last_filter(), "\"service\":\"files\"") != NULL,
          "filter forwards service");
    check(strstr(last_filter(), "\"search\":\"ab\"") != NULL,
          "filter forwards search");
    char kv[16];
    /* "xonly", not "geo": save_state renamed the key (main.c) and this check
     * was left behind, so it read a key nothing writes and passed on nothing. */
    check(hal_kv_get("xonly", 5, kv, sizeof(kv) - 1) > 0 && kv[0] == '1',
          "xprsOnly persisted to kv");
    /* The role bucket rides the same filter, and is OMITTED when unset so the
     * common frame stays byte-identical to what earlier builds sent. */
    check(strstr(last_filter(), "\"role\"") == NULL,
          "no role key in the filter when no role is picked");

    cap_clear();
    inbox_set("{\"command\":\"graph_filter\",\"xprsOnly\":true,"
              "\"service\":\"\",\"search\":\"\",\"role\":\"super\"}");
    module_handle_event();
    check(strstr(last_filter(), "\"role\":\"super\"") != NULL,
          "filter forwards role");

    /* An undefined bucket is forwarded like any other string -- the host
     * whitelists it. What must NOT happen is the wapp choking on it. */
    cap_clear();
    inbox_set("{\"command\":\"graph_filter\",\"role\":\"nonsense\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""),
          "unknown role still re-pushes the graph");

    /* Buffer regression. json_str UNESCAPES, so filling g_search to its full
     * 63 characters with quotes takes 126 escaped bytes on the way in -- and
     * every one of them escapes again on the way out. service does the same.
     * That is 17 + 12+62 + 12+126 + 16 + 2 = 247 bytes, and at the old
     * filter[160] it truncated MID-STRING. str_cat and json_cat_escaped both
     * stop silently, so the wapp shipped invalid JSON, the host's decode threw
     * (wapp_engine.dart wraps it in catch), every field fell back to its
     * default and the graph came back UNFILTERED with no error anywhere. */
    {
        char cmd[600];
        unsigned c = 0;
        const char *head =
            "{\"command\":\"graph_filter\",\"xprsOnly\":true,\"service\":\"";
        for (const char *h = head; *h; h++) cmd[c++] = *h;
        for (int i = 0; i < 31; i++) { cmd[c++] = '\\'; cmd[c++] = '"'; }
        const char *mid = "\",\"search\":\"";
        for (const char *h = mid; *h; h++) cmd[c++] = *h;
        for (int i = 0; i < 63; i++) { cmd[c++] = '\\'; cmd[c++] = '"'; }
        const char *tail = "\",\"role\":\"normal\"}";
        for (const char *h = tail; *h; h++) cmd[c++] = *h;
        cmd[c] = '\0';

        cap_clear();
        inbox_set(cmd);
        module_handle_event();
        const char *f = last_filter();
        size_t fl = strlen(f);
        check(fl > 200, "worst-case filter is actually long (guard is real)");
        check(fl > 0 && f[fl - 1] == '}', "worst-case filter is still closed JSON");
        check(strstr(f, "\"role\":\"normal\"") != NULL,
              "role survives a worst-case escaped search");
    }

    /* ── hub_add forwards a host action carrying the endpoint field ── */
    cap_clear();
    inbox_set("{\"command\":\"hub_add\",\"fields\":{\"hub_endpoint\":\"newhub:4242\"}}");
    module_handle_event();
    check(cap_contains("\"type\":\"rns.hub.add\"") && cap_contains("newhub:4242"),
          "hub_add forwards rns.hub.add");

    /* ── hub_remove ── */
    cap_clear();
    inbox_set("{\"command\":\"hub_remove\",\"fields\":{\"hub_endpoint\":\"newhub:4242\"}}");
    module_handle_event();
    check(cap_contains("\"type\":\"rns.hub.remove\""), "hub_remove forwards rns.hub.remove");

    /* ── apply_settings forwards the passive toggle ── */
    cap_clear();
    inbox_set("{\"command\":\"apply_settings\",\"fields\":{\"passive\":true}}");
    module_handle_event();
    check(cap_contains("\"type\":\"rns.passive.set\"") && cap_contains("true"),
          "apply_settings forwards rns.passive.set true");

    /* ── ready re-streams a frame ── */
    cap_clear();
    inbox_set("{\"command\":\"ready\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""), "ready streams a graph frame");

    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
    return g_fail;
}
