/* map_scan.h -- usb-wake's memory-map scan (uwake7; trimmed in uwake8), in plain C so that a host test runs the same
 * code (scripts/test-map-scan.c). The driver calls it with a probe that asks the system (LockMemory, GetPhysical with
 * room for 8 runs, UnlockMemory: MapProbe in usb_wake_resident.c); the test, with a probe that answers from a made-up
 * map. Only `unsigned long` here: it is the Mac's UInt32 (MacTypes.h), and 64 bits on the host, where nothing wraps.
 *
 * Every logical megabyte from `start` to `end` is asked about; a megabyte that fails is asked about as its first 64 KB.
 * A 64 KB that fails too is a hole. uwake7 then stepped 64 KB at a time through the hole: on the FW800 (Gestalt 'lram'
 * 2047 MB, of which OS 9 maps 1.5 GB) that walk was about 16400 of uwake7B's 17947 calls and most of its 232 ticks (3.9 s
 * just after startup). uwake8: when the hole starts a whole megabyte, its LAST 64 KB is asked about as well, and if that
 * fails too the megabyte is taken as a hole in one step. The cost: a mapped island lying wholly inside a megabyte whose
 * first and last 64 KB are both unmapped is not seen. OS 9's maps are a few large runs (the FW800's: 3; QEMU's: 3), and
 * the map check after each wake re-translates every block the recorder reads, so a missed island costs readings, never
 * a wrong one. Adjacent runs merge; the caller sorts. Returns the number of runs; *full is set when `max` ran out. */
#ifndef MAP_SCAN_H
#define MAP_SCAN_H

typedef int (*MsProbe)(unsigned long l, unsigned long size, unsigned long ph[8], unsigned long ln[8],
                       unsigned long *count, void *ctx);   /* 0 = answered: *count runs (physical start, length) */

#define kMsMB  0x100000UL
#define kMsBlk 0x10000UL

static unsigned long MsScan(unsigned long start, unsigned long end, MsProbe probe, void *ctx,
                            unsigned long *tl, unsigned long *tp, unsigned long *tn, unsigned long max,
                            unsigned long *calls, unsigned long *holes, int *full)
{
    unsigned long l = start, n = 0;
    *calls = 0; *holes = 0; *full = 0;
    while (l < end) {
        unsigned long size = end - l, got = 0, ph[8], ln[8], count;
        int tries, i;
        if (size > kMsMB) size = kMsMB;
        for (tries = 0; tries < 2 && got == 0; tries++) {
            if (tries == 1) size = kMsBlk;                       /* the megabyte failed: its first 64 KB */
            count = 8;
            (*calls)++;
            if (probe(l, size, ph, ln, &count, ctx) != 0) continue;
            for (i = 0; i < (int)count && i < 8; i++) {
                if (ln[i] == 0 || got + ln[i] > size) break;
                if (n > 0 && tl[n - 1] + tn[n - 1] == l + got && tp[n - 1] + tn[n - 1] == ph[i]) tn[n - 1] += ln[i];
                else if (n < max) { tl[n] = l + got; tp[n] = ph[i]; tn[n] = ln[i]; n++; }
                else { *full = 1; break; }
                got += ln[i];
            }
        }
        if (*full) break;
        if (got != 0) { l += got; continue; }
        /* a hole. uwake8: a hole that starts a whole megabyte whose last 64 KB is a hole too is a megabyte of hole */
        if ((l & (kMsMB - 1UL)) == 0 && end - l >= kMsMB) {
            count = 8;
            (*calls)++;
            if (probe(l + kMsMB - kMsBlk, kMsBlk, ph, ln, &count, ctx) != 0) {
                *holes += kMsMB / kMsBlk; l += kMsMB; continue;
            }
        }
        (*holes)++; l += kMsBlk;
    }
    return n;
}

#endif
