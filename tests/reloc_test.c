/* n64net_reloc: a "host" and a "guest" in one process, each with its own pool (so pool pointers must move) and
 * its own heap blocks (so heap pointers must not be copied). */
#include "n64net_reloc.h"
#include <mach-o/ldsyms.h>

static char g_pool_a[4096], g_pool_b[4096];
static void *g_ptrs[64];
static uint64_t g_data[64];

#define CHECK(c, what)                                                                                           \
    do {                                                                                                         \
        if (!(c)) {                                                                                              \
            printf("FAIL: %s\n", what);                                                                          \
            bad++;                                                                                               \
        }                                                                                                        \
    } while (0)

int main(void) {
    n64net_reloc host, guest;
    n64net_reloc_span spans[2] = {{g_ptrs, sizeof g_ptrs, sizeof g_ptrs}, {g_data, sizeof g_data, 1024}};
    n64net_reloc_stats st;
    void *heap_h[64], *heap_g[64], *before[64];
    uint8_t *state;
    size_t used[2];
    int i, bad = 0;
    if (!n64net_reloc_init(&host, &_mh_execute_header) || !n64net_reloc_init(&guest, &_mh_execute_header)) {
        puts("FAIL: init");
        return 1;
    }
    n64net_reloc_add(&host, g_pool_a, sizeof g_pool_a);
    n64net_reloc_add(&guest, g_pool_b, sizeof g_pool_b);   /* the guest has its pool elsewhere */
    n64net_reloc_keep(&host, &g_data[10], 16);
    n64net_reloc_keep(&guest, &g_data[10], 16);
    for (i = 0; i < 64; i++) {
        heap_h[i] = malloc(64);
        heap_g[i] = malloc(64);
    }
    /* the host's state */
    for (i = 0; i < 64; i++) {
        g_ptrs[i] = i % 4 == 0 ? (void *)&g_pool_a[i] : i % 4 == 1 ? heap_h[i] : i % 4 == 2 ? (void *)&main
                                                                                       : (void *)(long)(i * 77);
        g_data[i] = (uint64_t)i * 12345;
    }
    g_ptrs[61] = heap_h[1];   /* the same asset as slot 1 */
    g_data[5] = 0xFA11ull << 48 | 5;   /* data that looks like a mark */
    g_data[7] = (uint64_t)(uintptr_t)&g_pool_a[100];   /* data that happens to look like a pool pointer */
    g_data[10] = 111;   /* per device */
    n64net_reloc_scan(&host);
    state = malloc(n64net_reloc_state_size(spans, 2));
    n64net_reloc_save(&host, spans, 2, state, &st);
    printf("save: %llu words, %llu in regions, %llu foreign, %llu escaped\n", (unsigned long long)st.words,
           (unsigned long long)st.region_ptrs, (unsigned long long)st.foreign, (unsigned long long)st.escaped);
    memcpy(before, g_ptrs, sizeof before);

    /* the guest's own state: in step, with its own pool and heap blocks; except slot 61 (diverged: NULL) and
     * g_data[20] (diverged data) */
    for (i = 0; i < 64; i++) g_ptrs[i] = i % 4 == 0 ? (void *)&g_pool_b[i] : i % 4 == 1 ? heap_g[i] : before[i];
    g_ptrs[61] = NULL;
    for (i = 0; i < 64; i++) g_data[i] = (uint64_t)i * 12345;
    g_data[5] = 99;
    g_data[7] = (uint64_t)(uintptr_t)&g_pool_a[100];   /* the same data */
    g_data[10] = 222;
    g_data[20] = 1;
    n64net_reloc_scan(&guest);
    if (!n64net_reloc_load(&guest, spans, 2, state, used, &st)) {
        puts("FAIL: load refused");
        return 1;
    }
    printf("load: same %llu, translated %llu, own pointer %llu, matched %llu, guessed %llu, escaped %llu\n",
           (unsigned long long)st.same, (unsigned long long)st.translated, (unsigned long long)st.own_pointer,
           (unsigned long long)st.matched, (unsigned long long)st.guessed, (unsigned long long)st.escaped);
    for (i = 0; i < 61; i++) {
        void *want = i % 4 == 0 ? (void *)&g_pool_b[i] : i % 4 == 1 ? heap_g[i] : before[i];
        if (g_ptrs[i] != want) {
            printf("FAIL: pointer %d\n", i);
            bad++;
        }
    }
    CHECK(g_ptrs[61] == heap_g[1], "a diverged asset pointer becomes this device's (seen in slot 1)");
    CHECK(g_data[5] == 99, "an escaped word stays");
    CHECK(g_data[7] == (uint64_t)(uintptr_t)&g_pool_a[100], "data that looked like a pointer on the host stays");
    CHECK(g_data[10] == 222, "per-device memory stays");
    CHECK(g_data[20] == 20 * 12345, "diverged data takes the host's");
    CHECK(used[1] == sizeof g_data, "used size");
    {
        n64net_reloc other = guest;
        other.uuid[0] ^= 1;   /* another build: refused */
        CHECK(!n64net_reloc_load(&other, spans, 2, state, used, &st), "another build is refused");
    }
    printf("%s\n", bad ? "FAILED" : "PASS");
    return bad != 0;
}
