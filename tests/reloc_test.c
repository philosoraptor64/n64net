/* n64net_reloc: pointers into the image and into a registered pool survive a save/load (here in one process:
 * the pool is moved to show the offsets are what travels); foreign words keep this device's own value. */
#include "n64net_reloc.h"
#include <mach-o/ldsyms.h>

static char g_pool_a[4096], g_pool_b[4096];
static void *g_ptrs[64];
static long g_data[64];

int main(void) {
    n64net_reloc host, guest;
    n64net_reloc_span spans[2] = {{g_ptrs, sizeof g_ptrs, sizeof g_ptrs}, {g_data, sizeof g_data, 1024}};
    n64net_reloc_stats st;
    void *foreign[64], *before[64];
    uint8_t *state;
    size_t used[2];
    int i, bad = 0;
    if (!n64net_reloc_init(&host, &_mh_execute_header) || !n64net_reloc_init(&guest, &_mh_execute_header)) {
        puts("FAIL: init");
        return 1;
    }
    for (i = 0; i < 64; i++) {
        foreign[i] = malloc(16);
        g_ptrs[i] = i % 4 == 0 ? (void *)&g_pool_a[i] : i % 4 == 1 ? foreign[i] : i % 4 == 2 ? (void *)&main
                                                                                          : (void *)(long)(i * 77);
        g_data[i] = i * 12345;
    }
    g_data[5] = (long)(0xFA11ull << 48 | 5);   /* data that looks like a mark */
    n64net_reloc_add(&host, g_pool_a, sizeof g_pool_a);
    n64net_reloc_add(&guest, g_pool_b, sizeof g_pool_b);   /* the "guest" has its pool elsewhere */
    n64net_reloc_scan(&host);
    n64net_reloc_scan(&guest);
    state = malloc(n64net_reloc_state_size(spans, 2));
    n64net_reloc_save(&host, spans, 2, state, &st);
    printf("save: %llu words, %llu translated, %llu foreign, %llu escaped\n", (unsigned long long)st.words,
           (unsigned long long)st.region_ptrs, (unsigned long long)st.foreign, (unsigned long long)st.escaped);
    memcpy(before, g_ptrs, sizeof before);
    for (i = 0; i < 64; i++) g_ptrs[i] = i % 4 == 1 ? foreign[i] : NULL;   /* the guest's own foreign words */
    memset(g_data, 0, sizeof g_data);
    g_data[5] = 99;
    if (!n64net_reloc_load(&guest, spans, 2, state, used, &st)) {
        puts("FAIL: load refused");
        return 1;
    }
    for (i = 0; i < 64; i++) {
        void *want = i % 4 == 0 ? (void *)&g_pool_b[i] : before[i];
        if (g_ptrs[i] != want) bad++;
        if (i != 5 && g_data[i] != i * 12345) bad++;
    }
    if (g_data[5] != 99) bad++;   /* escaped: kept */
    if (used[1] != sizeof g_data) bad++;
    {
        uint8_t other[16] = {1};
        memcpy(guest.uuid, other, 16);   /* another build: refused */
        if (n64net_reloc_load(&guest, spans, 2, state, used, &st)) bad++;
    }
    printf("%s (%d wrong)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
