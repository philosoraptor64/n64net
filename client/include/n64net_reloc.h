/* n64net_reloc.h -- a native port's game state in a form another device can load (for n64net_resync_*).
 *
 * A native port's snapshot is raw process memory: its globals and its memory pools. It is full of pointers, and
 * those differ from one device to the next (the game library and the heap load at a different address each
 * launch). Raw bytes from the host would be garbage on the guest. Telling a pointer from data by its value alone
 * isn't safe either: a float of 1.0 next to a count of 1 reads as 0x13f800000, which is inside the game library
 * on one Mac and not on another. So nothing is guessed on the host's side and nothing is lost:
 *
 *  - The state is a list of spans (the port's snapshot ranges), read as 8-byte words.
 *  - A word whose value lies in a known region (the game library's image, or a memory pool the port registers)
 *    goes as "region r + offset". The header carries the host's region starts, so its original value is known
 *    too.
 *  - A word whose value lies elsewhere in mapped memory ("foreign": the heap, system libraries: an asset the
 *    game loaded, say) goes with a mark and its value.
 *  - Anything else goes as it is.
 *
 * Both devices encode their own state the same way, so the parts that are in step come out equal and n64net only
 * sends the pieces that differ. The guest then decides word by word, with its own word G beside the host's:
 *  - the same meaning (same encoding): G stays;
 *  - host "region r + off": G stays if it equals the host's original value (data that only looked like a
 *    pointer there); otherwise the guest's own address for r + off, unless G is plain data (then the host's value
 *    is data too);
 *  - host foreign H: G stays if equal; else the guest's address for the same thing if it is known (from other
 *    places where the host has H and the guest a foreign word of its own: an asset pointer appears in pieces
 *    that are in step too); else G if it is foreign itself (a pointer of this device); else H (data);
 *  - anything else: the host's word.
 * Memory that is per device (camera, rendering, the save cache) can be kept out: n64net_reloc_keep.
 *
 * Check a port: resync two devices in step (n64net_resync_request) and compare their saved states; every
 * difference there is something per device (see the Starship port's notes).
 *
 * Header only (C or C++). Apple only for now (n64net_reloc_init returns 0 elsewhere: no resync). Both devices
 * must run the same build of the game library (the header carries its UUID; another one isn't loaded).
 */
#ifndef N64NET_RELOC_H
#define N64NET_RELOC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __APPLE__
#include <mach-o/loader.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define N64NET_RELOC_MAX_REGIONS 14
#define N64NET_RELOC_MAX_SPANS 48
#define N64NET_RELOC_MAX_KEEP 64
#define N64NET_RELOC_TAG_REGION 0xFA11ull
#define N64NET_RELOC_TAG_FOREIGN 0xFA12ull
#define N64NET_RELOC_KIND_ESCAPE 14
#define N64NET_RELOC_MAGIC 0x324C3634u /* "64L2" */

typedef struct {
    uint64_t lo, hi;
} n64net_reloc_range;

typedef struct {
    void *base;    /* the span in this process (8-byte aligned) */
    size_t used;   /* bytes in use now (the rest of cap is sent as zeros) */
    size_t cap;    /* room for it (the same on every device) */
} n64net_reloc_span;

typedef struct {
    n64net_reloc_range region[N64NET_RELOC_MAX_REGIONS];
    int num_regions;
    n64net_reloc_range keep[N64NET_RELOC_MAX_KEEP];   /* per-device memory: not sent, not overwritten */
    int num_keep;
    uint8_t uuid[16];
    /* mapped memory of this process (sorted), refreshed by n64net_reloc_scan */
    n64net_reloc_range *vm;
    int num_vm, cap_vm;
} n64net_reloc;

typedef struct {
    uint64_t words, region_ptrs, foreign, escaped;   /* save; load: the host's words of each kind */
    uint64_t same, translated, own_pointer, matched, guessed;
    /* load: kept (same meaning or the same value); turned into this device's address; host's foreign word, this
     * device's own pointer kept; host's foreign word turned into this device's (seen elsewhere); host's word
     * taken where this device had something else entirely (diverged memory: a best guess) */
} n64net_reloc_stats;

/* the fixed header in front of the spans */
typedef struct {
    uint32_t magic, num_spans, num_regions, pad;
    uint8_t uuid[16];
    uint64_t used[N64NET_RELOC_MAX_SPANS];
    uint64_t region_lo[N64NET_RELOC_MAX_REGIONS];
} n64net_reloc_header;

/* bytes n64net_resync_enable needs for these spans */
static inline size_t n64net_reloc_state_size(const n64net_reloc_span *s, int n) {
    size_t t = sizeof(n64net_reloc_header);
    int i;
    for (i = 0; i < n; i++) t += (s[i].cap + 7) & ~(size_t)7;
    return t;
}

#ifdef __APPLE__

static inline int n64net_reloc_add_region_(n64net_reloc *r, uint64_t lo, uint64_t hi) {
    if (r->num_regions >= N64NET_RELOC_MAX_REGIONS || hi <= lo) return -1;
    r->region[r->num_regions].lo = lo;
    r->region[r->num_regions].hi = hi;
    return r->num_regions++;
}

/* image: the game library's mach header (pass &__dso_handle from inside it). Region 0 = its whole mapping. */
static inline int n64net_reloc_init(n64net_reloc *r, const void *image) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)image;
    const uint8_t *lc;
    uint64_t lo = ~0ull, hi = 0, text_vm = 0;
    int64_t slide;
    uint32_t i;
    int have_text = 0;
    memset(r, 0, sizeof(*r));
    if (!mh || mh->magic != MH_MAGIC_64) return 0;
    lc = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const struct segment_command_64 *)c;
            if (s->fileoff == 0 && s->filesize) { text_vm = s->vmaddr; have_text = 1; }
        } else if (c->cmd == LC_UUID) {
            memcpy(r->uuid, ((const struct uuid_command *)c)->uuid, 16);
        }
        lc += c->cmdsize;
    }
    if (!have_text) return 0;
    slide = (int64_t)((uint64_t)(uintptr_t)mh - text_vm);
    lc = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const struct segment_command_64 *)c;
            if (strcmp(s->segname, "__PAGEZERO") != 0 && s->vmsize) {
                if (s->vmaddr + slide < lo) lo = s->vmaddr + slide;
                if (s->vmaddr + slide + s->vmsize > hi) hi = s->vmaddr + slide + s->vmsize;
            }
        }
        lc += c->cmdsize;
    }
    return n64net_reloc_add_region_(r, lo, hi) == 0;
}

/* a memory pool outside the image (malloc'd arenas): pointers into it go as offsets. Same order and size on every
 * device. Returns its region number, or -1. */
static inline int n64net_reloc_add(n64net_reloc *r, const void *base, size_t size) {
    return n64net_reloc_add_region_(r, (uint64_t)(uintptr_t)base, (uint64_t)(uintptr_t)base + size);
}

/* per-device memory inside the spans (each device's own camera, rendering, caches): sent as zeros, and a load
 * leaves this device's own bytes there. Same list on every device. 0 = full. */
static inline int n64net_reloc_keep(n64net_reloc *r, const void *base, size_t size) {
    if (r->num_keep >= N64NET_RELOC_MAX_KEEP || !size) return 0;
    r->keep[r->num_keep].lo = (uint64_t)(uintptr_t)base;
    r->keep[r->num_keep].hi = (uint64_t)(uintptr_t)base + size;
    r->num_keep++;
    return 1;
}

/* this process's mapped memory, to tell pointers from data (call before save/load) */
static inline void n64net_reloc_scan(n64net_reloc *r) {
    vm_address_t a = 0;
    r->num_vm = 0;
    for (;;) {
        vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) !=
            KERN_SUCCESS)
            break;
        if (info.protection != VM_PROT_NONE) {
            if (r->num_vm && r->vm[r->num_vm - 1].hi == a) {
                r->vm[r->num_vm - 1].hi = a + sz;
            } else {
                if (r->num_vm == r->cap_vm) {
                    int nc = r->cap_vm ? r->cap_vm * 2 : 1024;
                    n64net_reloc_range *nv = (n64net_reloc_range *)realloc(r->vm, (size_t)nc * sizeof(*nv));
                    if (!nv) break;
                    r->vm = nv;
                    r->cap_vm = nc;
                }
                r->vm[r->num_vm].lo = a;
                r->vm[r->num_vm].hi = a + sz;
                r->num_vm++;
            }
        }
        a += sz;
    }
}

static inline void n64net_reloc_free(n64net_reloc *r) {
    free(r->vm);
    r->vm = NULL;
    r->num_vm = r->cap_vm = 0;
}

static inline int n64net_reloc_mapped_(const n64net_reloc *r, uint64_t w) {
    int lo = 0, hi = r->num_vm - 1;
    if (w < 0x100000000ull || w >= (1ull << 48)) return 0;   /* below 4 GB: __PAGEZERO */
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (w < r->vm[m].lo) hi = m - 1;
        else if (w >= r->vm[m].hi) lo = m + 1;
        else return 1;
    }
    return 0;
}

static inline int n64net_reloc_region_of_(const n64net_reloc *r, uint64_t w) {
    int i;
    for (i = r->num_regions - 1; i >= 0; i--)   /* pools first (one may lie inside the image) */
        if (w >= r->region[i].lo && w < r->region[i].hi) return i;
    return -1;
}

/* one word in its portable form */
static inline uint64_t n64net_reloc_enc_(const n64net_reloc *r, uint64_t w, n64net_reloc_stats *st) {
    int i;
    const uint64_t top = w >> 48;
    if (top == N64NET_RELOC_TAG_REGION || top == N64NET_RELOC_TAG_FOREIGN) {
        st->escaped++;
        return (N64NET_RELOC_TAG_REGION << 48) | ((uint64_t)N64NET_RELOC_KIND_ESCAPE << 44);
    }
    if (w < 0x100000000ull) return w;
    if ((i = n64net_reloc_region_of_(r, w)) >= 0) {
        st->region_ptrs++;
        return (N64NET_RELOC_TAG_REGION << 48) | ((uint64_t)i << 44) | (w - r->region[i].lo);
    }
    if (n64net_reloc_mapped_(r, w)) {
        st->foreign++;
        return (N64NET_RELOC_TAG_FOREIGN << 48) | w;
    }
    return w;
}

/* the part of [lo, hi) a keep range covers, as offsets from lo; 0 if none */
static inline int n64net_reloc_keep_in_(const n64net_reloc *r, int k, uint64_t lo, uint64_t hi, size_t *a,
                                        size_t *b) {
    const uint64_t x = r->keep[k].lo > lo ? r->keep[k].lo : lo, y = r->keep[k].hi < hi ? r->keep[k].hi : hi;
    if (x >= y) return 0;
    *a = (size_t)(x - lo);
    *b = (size_t)(y - lo);
    return 1;
}

/* Write the spans' portable form into out (n64net_reloc_state_size bytes). */
static inline void n64net_reloc_save(const n64net_reloc *r, const n64net_reloc_span *s, int n, uint8_t *out,
                                     n64net_reloc_stats *st) {
    n64net_reloc_header h;
    n64net_reloc_stats dummy;
    uint8_t *p = out + sizeof(h);
    int i, k;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    memset(&h, 0, sizeof(h));
    h.magic = N64NET_RELOC_MAGIC;
    h.num_spans = (uint32_t)n;
    h.num_regions = (uint32_t)r->num_regions;
    memcpy(h.uuid, r->uuid, 16);
    for (i = 0; i < r->num_regions; i++) h.region_lo[i] = r->region[i].lo;
    for (i = 0; i < n && i < N64NET_RELOC_MAX_SPANS; i++) {
        const size_t cap = (s[i].cap + 7) & ~(size_t)7;
        const size_t used = s[i].used < s[i].cap ? s[i].used : s[i].cap;
        const size_t words = used / 8;
        const uint64_t *src = (const uint64_t *)s[i].base;
        const uint64_t lo = (uint64_t)(uintptr_t)s[i].base;
        size_t j, a, b;
        h.used[i] = used;
        for (j = 0; j < words; j++) {
            uint64_t w;
            memcpy(&w, src + j, 8);
            w = n64net_reloc_enc_(r, w, st);
            memcpy(p + j * 8, &w, 8);
        }
        memcpy(p + words * 8, (const uint8_t *)s[i].base + words * 8, used - words * 8);   /* a short tail */
        memset(p + used, 0, cap - used);
        for (k = 0; k < r->num_keep; k++)
            if (n64net_reloc_keep_in_(r, k, lo, lo + used, &a, &b)) memset(p + a, 0, b - a);
        st->words += words;
        p += cap;
    }
    memcpy(out, &h, sizeof(h));
}

/* host foreign value -> this device's (load) */
typedef struct {
    uint64_t h, g;   /* g == 0: seen with different values (not used) */
} n64net_reloc_pair_;

static inline n64net_reloc_pair_ *n64net_reloc_find_(n64net_reloc_pair_ *t, size_t mask, uint64_t h) {
    size_t i = (size_t)((h >> 3) * 0x9E3779B97F4A7C15ull) & mask;
    while (t[i].h && t[i].h != h) i = (i + 1) & mask;
    return &t[i];
}

/* Load a portable state into this device's spans. Returns 0 (nothing changed) if it doesn't fit: another build,
 * other spans or regions, a span bigger than its room here. *used_out (may be NULL) gets the host's used size per
 * span (the port sets its pool's end from it if that isn't itself in the state). */
static inline int n64net_reloc_load(const n64net_reloc *r, const n64net_reloc_span *s, int n, const uint8_t *in,
                                    size_t *used_out, n64net_reloc_stats *st) {
    n64net_reloc_header h;
    n64net_reloc_stats dummy;
    n64net_reloc_pair_ *pairs = NULL;
    size_t mask = 0, nforeign = 0;
    uint8_t *kept[N64NET_RELOC_MAX_KEEP];
    const uint8_t *p;
    int i, k, pass;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    memcpy(&h, in, sizeof(h));
    if (h.magic != N64NET_RELOC_MAGIC || h.num_spans != (uint32_t)n || n > N64NET_RELOC_MAX_SPANS ||
        h.num_regions != (uint32_t)r->num_regions || memcmp(h.uuid, r->uuid, 16) != 0)
        return 0;
    for (i = 0; i < n; i++)
        if (h.used[i] > s[i].cap) return 0;
    /* pass 0: count the host's foreign words; pass 1: pair them with this device's own foreign words where the
     * two differ (one asset at different addresses); pass 2: write */
    for (pass = 0; pass < 3; pass++) {
        if (pass == 1) {
            size_t want = 16;
            while (want < nforeign * 2) want *= 2;
            pairs = (n64net_reloc_pair_ *)calloc(want, sizeof(*pairs));
            if (!pairs) want = 0;
            mask = want ? want - 1 : 0;
        }
        if (pass == 2)
            for (k = 0; k < r->num_keep; k++) {   /* per-device bytes: put back after */
                kept[k] = (uint8_t *)malloc((size_t)(r->keep[k].hi - r->keep[k].lo));
                if (kept[k]) memcpy(kept[k], (const void *)(uintptr_t)r->keep[k].lo, (size_t)(r->keep[k].hi - r->keep[k].lo));
            }
        p = in + sizeof(h);
        for (i = 0; i < n; i++) {
            const size_t cap = (s[i].cap + 7) & ~(size_t)7;
            const size_t used = (size_t)h.used[i];
            const size_t words = used / 8;
            uint64_t *dst = (uint64_t *)s[i].base;
            /* this device's own words beyond its used size are old: compare only within it */
            size_t j;
            for (j = 0; j < words; j++) {
                uint64_t e, g, ge, w;
                n64net_reloc_stats tmp;
                unsigned top;
                memcpy(&e, p + j * 8, 8);
                top = (unsigned)(e >> 48);
                if (pass == 0) {
                    nforeign += top == N64NET_RELOC_TAG_FOREIGN;
                    continue;
                }
                memcpy(&g, dst + j, 8);
                memset(&tmp, 0, sizeof(tmp));
                ge = n64net_reloc_enc_(r, g, &tmp);
                if (pass == 1) {
                    if (top == N64NET_RELOC_TAG_FOREIGN && ge != e && tmp.foreign && pairs) {
                        n64net_reloc_pair_ *q = n64net_reloc_find_(pairs, mask, e & ((1ull << 48) - 1));
                        if (!q->h) { q->h = e & ((1ull << 48) - 1); q->g = g; }
                        else if (q->g != g) q->g = 0;
                    }
                    continue;
                }
                if (ge == e) {   /* the same meaning */
                    st->same++;
                    continue;
                }
                if (top == N64NET_RELOC_TAG_REGION) {
                    const unsigned kind = (unsigned)(e >> 44) & 15;
                    const uint64_t off = e & ((1ull << 44) - 1);
                    if (kind == N64NET_RELOC_KIND_ESCAPE) {   /* (data that looked like a mark: keep own) */
                        st->escaped++;
                        continue;
                    }
                    st->region_ptrs++;
                    if ((int)kind >= r->num_regions || r->region[kind].lo + off >= r->region[kind].hi) {
                        st->guessed++;   /* (can't be: same build) */
                        continue;
                    }
                    if (g == h.region_lo[kind] + off) {   /* data that looked like a pointer on the host */
                        st->same++;
                        continue;
                    }
                    if (g == 0 || tmp.region_ptrs || tmp.foreign) {   /* a pointer field here too */
                        w = r->region[kind].lo + off;
                        st->translated++;
                    } else {
                        w = h.region_lo[kind] + off;   /* plain data here: the host's value is data too */
                        st->guessed++;
                    }
                } else if (top == N64NET_RELOC_TAG_FOREIGN) {
                    const uint64_t hv = e & ((1ull << 48) - 1);
                    n64net_reloc_pair_ *q = pairs ? n64net_reloc_find_(pairs, mask, hv) : NULL;
                    st->foreign++;
                    if (g == hv) {
                        st->same++;
                        continue;
                    }
                    if (q && q->h == hv && q->g) {
                        w = q->g;
                        st->matched++;
                    } else if (tmp.foreign) {
                        st->own_pointer++;
                        continue;
                    } else {
                        w = hv;
                        st->guessed++;
                    }
                } else {
                    w = e;
                }
                memcpy(dst + j, &w, 8);
            }
            if (pass == 2) {
                memcpy((uint8_t *)s[i].base + words * 8, p + words * 8, used - words * 8);
                st->words += words;
                if (used_out) used_out[i] = used;
            }
            p += cap;
        }
    }
    for (k = 0; k < r->num_keep; k++) {
        if (kept[k]) {
            memcpy((void *)(uintptr_t)r->keep[k].lo, kept[k], (size_t)(r->keep[k].hi - r->keep[k].lo));
            free(kept[k]);
        }
    }
    free(pairs);
    return 1;
}

#else /* not Apple: no resync for native ports */

static inline int n64net_reloc_init(n64net_reloc *r, const void *image) { (void)image; memset(r, 0, sizeof(*r)); return 0; }
static inline int n64net_reloc_add(n64net_reloc *r, const void *b, size_t n) { (void)r; (void)b; (void)n; return -1; }
static inline int n64net_reloc_keep(n64net_reloc *r, const void *b, size_t n) { (void)r; (void)b; (void)n; return 0; }
static inline void n64net_reloc_scan(n64net_reloc *r) { (void)r; }
static inline void n64net_reloc_free(n64net_reloc *r) { (void)r; }
static inline void n64net_reloc_save(const n64net_reloc *r, const n64net_reloc_span *s, int n, uint8_t *out,
                                     n64net_reloc_stats *st) { (void)r; (void)s; (void)n; (void)out; (void)st; }
static inline int n64net_reloc_load(const n64net_reloc *r, const n64net_reloc_span *s, int n, const uint8_t *in,
                                    size_t *u, n64net_reloc_stats *st) { (void)r; (void)s; (void)n; (void)in; (void)u; (void)st; return 0; }

#endif

#ifdef __cplusplus
}
#endif
#endif
