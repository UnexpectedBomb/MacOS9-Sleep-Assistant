/* test-map-scan.c -- runs map_scan.h's MsScan (the code the driver runs) on the host against made-up memory maps, and
 * against uwake7's scan (copied below as OldScan) for the call counts. Build and run:
 *     cc -std=c99 -Wall -Wextra -O1 -o "$TMP/test-map-scan" scripts/test-map-scan.c && "$TMP/test-map-scan"
 * Exit 0 = every case as expected. The probe answers like GetPhysical, in two ways (both must give the same runs):
 *   strict  - any unmapped byte in [l, l+size) fails the call (as a LockMemory of an unmapped range would)
 *   partial - fails only if the first byte is unmapped; otherwise answers the mapped prefix
 * and at most 8 runs per call (the rest is left for the next call, as the driver's table has 8 entries). */
#include <stdio.h>
#include <string.h>
#include "../map_scan.h"

typedef struct { unsigned long l, len, p; } Seg;
typedef struct { const Seg *s; int n; int partial; } Map;

static int Find(const Map *m, unsigned long x)           /* the segment holding logical x, or -1 */
{
    int i;
    for (i = 0; i < m->n; i++) if (x >= m->s[i].l && x - m->s[i].l < m->s[i].len) return i;
    return -1;
}
static int Probe(unsigned long l, unsigned long size, unsigned long ph[8], unsigned long ln[8], unsigned long *count, void *ctx)
{
    const Map *m = (const Map *)ctx; unsigned long x = l, end = l + size; int c = 0;
    if (!m->partial) {                                    /* strict: every byte mapped, or the call fails */
        unsigned long y;
        for (y = l; y < end; y += 0x1000UL) if (Find(m, y) < 0) return -1;
    }
    while (x < end && c < 8) {
        int i = Find(m, x); unsigned long take;
        if (i < 0) break;
        take = m->s[i].l + m->s[i].len - x; if (take > end - x) take = end - x;
        ph[c] = m->s[i].p + (x - m->s[i].l); ln[c] = take; c++; x += take;
    }
    if (c == 0) return -1;
    *count = (unsigned long)c;
    return 0;
}
/* uwake7's loop, for the comparison (the same as MsScan without the megabyte skip) */
static unsigned long OldScan(unsigned long start, unsigned long end, void *ctx, unsigned long *calls)
{
    unsigned long l = start, n = 0, got, size, ph[8], ln[8], count; int tries, i;
    *calls = 0;
    while (l < end) {
        size = end - l; got = 0; if (size > kMsMB) size = kMsMB;
        for (tries = 0; tries < 2 && got == 0; tries++) {
            if (tries == 1) size = kMsBlk;
            count = 8; (*calls)++;
            if (Probe(l, size, ph, ln, &count, ctx) != 0) continue;
            for (i = 0; i < (int)count && i < 8; i++) { if (ln[i] == 0 || got + ln[i] > size) break; got += ln[i]; n++; }
        }
        l += got ? got : kMsBlk;
    }
    return n;
}

static int Check(const char *name, const Seg *s, int ns, unsigned long end, const Seg *want, int nw)
{
    static unsigned long tl[1024], tp[1024], tn[1024];
    int pass, bad = 0;
    for (pass = 0; pass < 2; pass++) {
        Map m = { s, ns, pass }; unsigned long n, calls, holes, ocalls; int full, i;
        n = MsScan(kMsBlk, end, Probe, &m, tl, tp, tn, 1024, &calls, &holes, &full);
        (void)OldScan(kMsBlk, end, &m, &ocalls);
        if ((int)n != nw || full) bad = 1;
        for (i = 0; i < nw && i < (int)n; i++)
            if (tl[i] != want[i].l || tn[i] != want[i].len || tp[i] != want[i].p) bad = 1;
        printf("%-34s %-7s runs %lu (want %d), holes %lu, calls %lu (uwake7's walk: %lu)%s\n", name,
               pass ? "partial" : "strict", n, nw, holes, calls, ocalls, bad ? "  <-- WRONG" : "");
        if (bad) for (i = 0; i < (int)n; i++) printf("      got logical %08lX-%08lX -> physical %08lX\n", tl[i], tl[i] + tn[i] - 1, tp[i]);
    }
    return bad;
}

int main(void)
{
    int bad = 0;
    /* 1. the FW800 as uwake7B measured it (2026-11-13): 3 runs, then nothing from 5FF70000 to 'lram' (7FFF0000) */
    static const Seg fw[] = { { 0x00010000UL, 0x00BEC000UL, 0x00014000UL }, { 0x00BFC000UL, 0x000A7000UL, 0x00E58000UL },
                              { 0x00CA3000UL, 0x5F2CD000UL, 0x01000000UL } };
    /* 2. QEMU (snh runs): 3 runs to the 128 MB cap */
    static const Seg qe[] = { { 0x00010000UL, 0x00BEC000UL, 0x00014000UL }, { 0x00BFC000UL, 0x000A8000UL, 0x00E57000UL },
                              { 0x00CA4000UL, 0x0735C000UL, 0x01000000UL } };
    /* 3. a run that ends mid-megabyte, a hole, a run that starts mid-megabyte, a hole, a run on a megabyte boundary */
    static const Seg mid[] = { { 0x00010000UL, 0x01070000UL, 0x00100000UL }, { 0x02080000UL, 0x00F80000UL, 0x10000000UL },
                               { 0x05000000UL, 0x01000000UL, 0x20000000UL } };
    /* 4. physical fragments: 40 runs of 64 KB alternating between two physical areas (more than 8 per megabyte) */
    static Seg fr[40]; static Seg frw[40];
    /* 5. an island: 64 KB mapped in the middle of an otherwise unmapped megabyte (the documented blind spot) */
    static const Seg isl[] = { { 0x00010000UL, 0x00FF0000UL, 0x00010000UL }, { 0x03040000UL, 0x00010000UL, 0x30000000UL } };
    static const Seg islw[] = { { 0x00010000UL, 0x00FF0000UL, 0x00010000UL } };
    int i;
    for (i = 0; i < 40; i++) {
        fr[i].l = 0x00010000UL + (unsigned long)i * 0x10000UL; fr[i].len = 0x10000UL;
        fr[i].p = (i & 1) ? 0x40000000UL + (unsigned long)i * 0x10000UL : 0x50000000UL + (unsigned long)i * 0x10000UL;
        frw[i] = fr[i];
    }
    bad |= Check("FW800 (uwake7B's map)", fw, 3, 0x7FFF0000UL, fw, 3);
    bad |= Check("QEMU (128 MB cap)", qe, 3, 0x08000000UL, qe, 3);
    bad |= Check("runs ending/starting mid-MB", mid, 3, 0x08000000UL, mid, 3);
    bad |= Check("40 physical fragments", fr, 40, 0x00800000UL, frw, 40);
    bad |= Check("an island inside a hole MB", isl, 2, 0x08000000UL, islw, 1);
    printf(bad ? "FAILED\n" : "all cases as expected\n");
    return bad;
}
