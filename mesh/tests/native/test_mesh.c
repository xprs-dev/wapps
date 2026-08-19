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
    check(hal_kv_get("geo", 3, kv, sizeof(kv) - 1) > 0 && kv[0] == '1',
          "xprsOnly persisted to kv");

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
