/* n64net_reloc.h -- a native port's game state in a form another device can load (for n64net_resync_*).
 *
 * A native port's snapshot is raw process memory: its globals and its memory pools. It is full of pointers, and
 * those differ from one device to the next (the game library and the heap load at a different address each
 * launch). Raw bytes from the host would be garbage on the guest. So the state goes over translated:
 *
 *  - The state is a list of spans (the port's snapshot ranges). It is read as 8-byte words.
 *  - A word that points into a known region (this game library's own image, or a memory pool the port
 *    registers) becomes "region r + offset". The guest turns it back into an address in its own copy.
 *  - Anything else goes as it is. That includes words that point elsewhere in mapped memory ("foreign": the heap
 *    outside the pools, system libraries). Those can't be translated; but most words that look like that are
 *    really data (a float of 3000.0 next to a count of 1 reads as 0x1453b8000, inside the heap), so they must
 *    not be guessed at. A port whose state does hold heap pointers registers that memory as a pool; to find
 *    them, resync two devices in step (n64net_resync_request) and compare (n64net_reloc_survey lists the
 *    foreign-looking words). mark_foreign = 1 instead sends them as a mark and the guest keeps its own word.
 *  - Raw data that happens to look like a mark (top 16 bits 0xFA11) is rare; the guest keeps its own word there
 *    and it is counted.
 *
 * Both devices translate their own state the same way, so the parts that are in step come out byte for byte
 * equal and n64net only sends the pieces that differ.
 *
 * Header only (C or C++). Apple only for now (n64net_reloc_init returns 0 elsewhere: no resync). Both devices
 * must run the same build of the game library (the header carries its UUID; a different one isn't loaded).
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
#define N64NET_RELOC_TAG 0xFA11ull
#define N64NET_RELOC_KIND_ESCAPE 14
#define N64NET_RELOC_KIND_FOREIGN 15
#define N64NET_RELOC_MAGIC 0x524C3634u /* "64LR" */

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
    int mark_foreign;   /* 0 (default): foreign-looking words go as they are (see above) */
    uint8_t uuid[16];
    /* mapped memory of this process (sorted), refreshed by n64net_reloc_scan */
    n64net_reloc_range *vm;
    int num_vm, cap_vm;
} n64net_reloc;

typedef struct {
    uint64_t words, region_ptrs, foreign, escaped;  /* save and load (foreign: words that look like pointers
                                                       elsewhere; marked only with mark_foreign) */
    uint64_t own_kept, bad;                          /* load: foreign/escaped words kept from this device; marks
                                                        that couldn't be turned back (out of range) */
} n64net_reloc_stats;

/* the fixed header in front of the spans */
typedef struct {
    uint32_t magic, num_spans;
    uint8_t uuid[16];
    uint64_t used[N64NET_RELOC_MAX_SPANS];
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

/* a memory pool outside the image (malloc'd arenas): pointers into it go as offsets. Same order on every device.
 * Returns its region number, or -1. */
static inline int n64net_reloc_add(n64net_reloc *r, const void *base, size_t size) {
    return n64net_reloc_add_region_(r, (uint64_t)(uintptr_t)base, (uint64_t)(uintptr_t)base + size);
}

/* this process's mapped memory, to tell pointers from data (call before save/load/survey) */
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
    if (w < 0x100000000ull || w >= (1ull << 47)) return 0;   /* below 4 GB: __PAGEZERO; above: not user space */
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (w < r->vm[m].lo) hi = m - 1;
        else if (w >= r->vm[m].hi) lo = m + 1;
        else return 1;
    }
    return 0;
}

/* one word in its portable form */
static inline uint64_t n64net_reloc_enc_(const n64net_reloc *r, uint64_t w, n64net_reloc_stats *st) {
    int i;
    if ((w >> 48) == N64NET_RELOC_TAG) {
        st->escaped++;
        return (N64NET_RELOC_TAG << 48) | ((uint64_t)N64NET_RELOC_KIND_ESCAPE << 44);
    }
    if (w < 0x100000000ull) return w;
    for (i = r->num_regions - 1; i >= 0; i--) {   /* pools first (one may lie inside the image) */
        if (w >= r->region[i].lo && w < r->region[i].hi) {
            st->region_ptrs++;
            return (N64NET_RELOC_TAG << 48) | ((uint64_t)i << 44) | (w - r->region[i].lo);
        }
    }
    if (n64net_reloc_mapped_(r, w)) {
        st->foreign++;
        if (!r->mark_foreign) return w;
        return (N64NET_RELOC_TAG << 48) | ((uint64_t)N64NET_RELOC_KIND_FOREIGN << 44);
    }
    return w;
}

/* Write the spans' portable form into out (n64net_reloc_state_size bytes). */
static inline void n64net_reloc_save(const n64net_reloc *r, const n64net_reloc_span *s, int n, uint8_t *out,
                                     n64net_reloc_stats *st) {
    n64net_reloc_header h;
    n64net_reloc_stats dummy;
    uint8_t *p = out + sizeof(h);
    int i;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    memset(&h, 0, sizeof(h));
    h.magic = N64NET_RELOC_MAGIC;
    h.num_spans = (uint32_t)n;
    memcpy(h.uuid, r->uuid, 16);
    for (i = 0; i < n && i < N64NET_RELOC_MAX_SPANS; i++) {
        const size_t cap = (s[i].cap + 7) & ~(size_t)7;
        const size_t used = s[i].used < s[i].cap ? s[i].used : s[i].cap;
        const size_t words = used / 8;
        const uint64_t *src = (const uint64_t *)s[i].base;
        size_t k;
        h.used[i] = used;
        for (k = 0; k < words; k++) {
            uint64_t w;
            memcpy(&w, src + k, 8);
            w = n64net_reloc_enc_(r, w, st);
            memcpy(p + k * 8, &w, 8);
        }
        memcpy(p + words * 8, (const uint8_t *)s[i].base + words * 8, used - words * 8);   /* a short tail */
        memset(p + used, 0, cap - used);
        st->words += words;
        p += cap;
    }
    memcpy(out, &h, sizeof(h));
}

/* Load a portable state into this device's spans. Returns 0 (nothing changed) if it doesn't fit: another build,
 * other spans, a span bigger than its room here. *used_out (may be NULL) gets the host's used size per span. */
static inline int n64net_reloc_load(const n64net_reloc *r, const n64net_reloc_span *s, int n, const uint8_t *in,
                                    size_t *used_out, n64net_reloc_stats *st) {
    n64net_reloc_header h;
    n64net_reloc_stats dummy;
    const uint8_t *p = in + sizeof(h);
    int i;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    memcpy(&h, in, sizeof(h));
    if (h.magic != N64NET_RELOC_MAGIC || h.num_spans != (uint32_t)n || n > N64NET_RELOC_MAX_SPANS ||
        memcmp(h.uuid, r->uuid, 16) != 0)
        return 0;
    for (i = 0; i < n; i++)
        if (h.used[i] > s[i].cap) return 0;
    for (i = 0; i < n; i++) {
        const size_t cap = (s[i].cap + 7) & ~(size_t)7;
        const size_t used = (size_t)h.used[i];
        const size_t words = used / 8;
        uint64_t *dst = (uint64_t *)s[i].base;
        size_t k;
        for (k = 0; k < words; k++) {
            uint64_t e, w;
            memcpy(&e, p + k * 8, 8);
            if ((e >> 48) == N64NET_RELOC_TAG) {
                const unsigned kind = (unsigned)(e >> 44) & 15;
                const uint64_t off = e & ((1ull << 44) - 1);
                if (kind == N64NET_RELOC_KIND_FOREIGN || kind == N64NET_RELOC_KIND_ESCAPE) {
                    kind == N64NET_RELOC_KIND_FOREIGN ? st->foreign++ : st->escaped++;
                    st->own_kept++;
                    continue;   /* this device's own word stays */
                }
                if ((int)kind >= r->num_regions || r->region[kind].lo + off >= r->region[kind].hi) {
                    st->bad++;
                    continue;
                }
                st->region_ptrs++;
                w = r->region[kind].lo + off;
            } else {
                w = e;
            }
            memcpy(dst + k, &w, 8);
        }
        memcpy((uint8_t *)s[i].base + words * 8, p + words * 8, used - words * 8);
        st->words += words;
        if (used_out) used_out[i] = used;
        p += cap;
    }
    return 1;
}

/* A port check: log the foreign words of the spans (where they are, and what they point at), up to max lines.
 * log(user, line). Returns the number of foreign words. */
static inline uint64_t n64net_reloc_survey(const n64net_reloc *r, const n64net_reloc_span *s, int n, int max,
                                           void (*log)(void *user, const char *line), void *user) {
    uint64_t count = 0;
    int i, lines = 0;
    char buf[256];
    for (i = 0; i < n; i++) {
        const size_t words = (s[i].used < s[i].cap ? s[i].used : s[i].cap) / 8;
        const uint64_t *src = (const uint64_t *)s[i].base;
        size_t k;
        for (k = 0; k < words; k++) {
            n64net_reloc_stats st;
            uint64_t w;
            memset(&st, 0, sizeof(st));
            memcpy(&w, src + k, 8);
            n64net_reloc_enc_(r, w, &st);
            if (!st.foreign) continue;
            count++;
            if (lines < max && log) {
                vm_address_t a = (vm_address_t)w;
                vm_size_t sz = 0;
                vm_region_extended_info_data_t info;
                mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
                mach_port_t obj = MACH_PORT_NULL;
                unsigned tag = 0;
                memset(&info, 0, sizeof(info));
                if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &cnt,
                                 &obj) == KERN_SUCCESS)
                    tag = info.user_tag;
                snprintf(buf, sizeof(buf), "reloc survey: span %d +0x%zx -> 0x%llx (vm tag %u, region +0x%llx)", i,
                         k * 8, (unsigned long long)w, tag, (unsigned long long)(w - a));
                log(user, buf);
                lines++;
            }
        }
    }
    return count;
}

#else /* not Apple: no resync for native ports */

static inline int n64net_reloc_init(n64net_reloc *r, const void *image) { (void)image; memset(r, 0, sizeof(*r)); return 0; }
static inline int n64net_reloc_add(n64net_reloc *r, const void *b, size_t n) { (void)r; (void)b; (void)n; return -1; }
static inline void n64net_reloc_scan(n64net_reloc *r) { (void)r; }
static inline void n64net_reloc_free(n64net_reloc *r) { (void)r; }
static inline void n64net_reloc_save(const n64net_reloc *r, const n64net_reloc_span *s, int n, uint8_t *out,
                                     n64net_reloc_stats *st) { (void)r; (void)s; (void)n; (void)out; (void)st; }
static inline int n64net_reloc_load(const n64net_reloc *r, const n64net_reloc_span *s, int n, const uint8_t *in,
                                    size_t *u, n64net_reloc_stats *st) { (void)r; (void)s; (void)n; (void)in; (void)u; (void)st; return 0; }
static inline uint64_t n64net_reloc_survey(const n64net_reloc *r, const n64net_reloc_span *s, int n, int max,
                                           void (*log)(void *, const char *), void *user) {
    (void)r; (void)s; (void)n; (void)max; (void)log; (void)user; return 0; }

#endif

#ifdef __cplusplus
}
#endif
#endif
