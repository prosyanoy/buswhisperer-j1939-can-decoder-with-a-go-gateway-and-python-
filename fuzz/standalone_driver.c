/*
 * Coverage-guided mutational fuzz driver for toolchains without libFuzzer
 * (gcc). Library and target are compiled with -fsanitize-coverage=trace-pc,
 * which calls __sanitizer_cov_trace_pc on every basic block; this driver hashes
 * those into an edge bitmap and keeps inputs that reach new edges.
 *
 *   fuzz_driver [-n iterations] [-s seed] [-o crash_dir] [file ...]
 * Files given on the command line are replayed (regression mode) and then
 * used as extra seeds.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bw_j1939.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAP_BITS   16
#define MAX_LEN    2048
#define MAX_CORPUS 4096

static uint8_t g_map[1u << MAP_BITS];
static uintptr_t g_prev;
static unsigned g_new_edges;
static unsigned g_total_edges;

void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    uintptr_t h = (pc ^ (g_prev << 1)) * 0x9E3779B1u;
    h = (h >> 7) & ((1u << MAP_BITS) - 1);
    if (!g_map[h]) {
        g_map[h] = 1;
        g_new_edges++;
        g_total_edges++;
    }
    g_prev = pc >> 2;
}

/* ---- crash capture ----------------------------------------------------- */
static uint8_t g_cur[MAX_LEN];
static size_t g_cur_len;
static const char *g_crash_dir = ".";

static void on_crash(int sig)
{
    char path[512];
    snprintf(path, sizeof path, "%s/crash-%d-%zu", g_crash_dir, sig, g_cur_len);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(g_cur, 1, g_cur_len, f); fclose(f); }
    fprintf(stderr, "\nCRASH (signal %d): input saved to %s\n", sig, path);
    _exit(1);
}

/* ---- rng / corpus ------------------------------------------------------ */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}
static size_t rndn(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

typedef struct { uint8_t *d; size_t n; } entry_t;
static entry_t g_corpus[MAX_CORPUS];
static size_t g_ncorpus;

static void add_corpus(const uint8_t *d, size_t n)
{
    if (g_ncorpus >= MAX_CORPUS) return;
    g_corpus[g_ncorpus].d = malloc(n ? n : 1);
    memcpy(g_corpus[g_ncorpus].d, d, n);
    g_corpus[g_ncorpus].n = n;
    g_ncorpus++;
}

static void run_one(const uint8_t *d, size_t n)
{
    if (n > MAX_LEN) n = MAX_LEN;
    memcpy(g_cur, d, n);
    g_cur_len = n;
    g_prev = 0;
    LLVMFuzzerTestOneInput(g_cur, n);
}

/* ---- structure-aware seeds -------------------------------------------- */
static size_t put_frame(uint8_t *o, uint32_t id, const uint8_t *d, unsigned dlc, unsigned dt)
{
    o[0] = (uint8_t)id; o[1] = (uint8_t)(id >> 8); o[2] = (uint8_t)(id >> 16);
    o[3] = (uint8_t)(id >> 24); o[4] = (uint8_t)dlc;
    memset(o + 5, 0xFF, 8);
    memcpy(o + 5, d, dlc > 8 ? 8 : dlc);
    o[13] = (uint8_t)dt;
    return 14;
}

static void add_seeds(void)
{
    uint8_t b[14 * 16];
    size_t n = 0;
    uint8_t eec1[8] = { 0, 0, 150, 0xE0, 0x2E, 0, 0, 0 };
    uint8_t ccvs[8] = { 0xFF, 0x00, 0x64, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t et1[8]  = { 130, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    n += put_frame(b + n, bw_id_make(3, 61444, 0, 0xFF), eec1, 8, 5);
    n += put_frame(b + n, bw_id_make(6, 65265, 0, 0xFF), ccvs, 8, 5);
    n += put_frame(b + n, bw_id_make(6, 65262, 0, 0xFF), et1, 8, 5);
    add_corpus(b, n);

    n = 0;                                   /* BAM carrying a VIN */
    uint8_t cm[8] = { 32, 18, 0, 3, 0xFF, 0xEC, 0xFE, 0 };
    n += put_frame(b + n, bw_id_make(7, BW_PGN_TP_CM, 0, 0xFF), cm, 8, 1);
    const char vin[] = "1FUJGLDR12LM12345*";
    for (unsigned s = 1; s <= 3; s++) {
        uint8_t dt[8] = { (uint8_t)s, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        for (unsigned i = 0; i < 7 && (s - 1) * 7 + i < 18; i++)
            dt[1 + i] = (uint8_t)vin[(s - 1) * 7 + i];
        n += put_frame(b + n, bw_id_make(7, BW_PGN_TP_DT, 0, 0xFF), dt, 8, 10);
    }
    add_corpus(b, n);

    n = 0;                                   /* RTS/CTS/DT/abort */
    uint8_t rts[8] = { 16, 14, 0, 2, 2, 0xEC, 0xFE, 0 };
    uint8_t cts[8] = { 17, 2, 1, 0xFF, 0xFF, 0xEC, 0xFE, 0 };
    uint8_t abt[8] = { 255, 1, 0xFF, 0xFF, 0xFF, 0xEC, 0xFE, 0 };
    uint8_t d1[8]  = { 1, 'A', 'B', 'C', 'D', 'E', 'F', 'G' };
    n += put_frame(b + n, bw_id_make(7, BW_PGN_TP_CM, 0x11, 0x2A), rts, 8, 1);
    n += put_frame(b + n, bw_id_make(7, BW_PGN_TP_CM, 0x2A, 0x11), cts, 8, 1);
    n += put_frame(b + n, bw_id_make(7, BW_PGN_TP_DT, 0x11, 0x2A), d1, 8, 1);
    n += put_frame(b + n, bw_id_make(7, BW_PGN_TP_CM, 0x2A, 0x11), abt, 8, 1);
    add_corpus(b, n);

    static const char *lines[] = {
        "(1609459200.123456) can0 0CF00400#0011223344556677\n(1609459200.223456) can0 18FEF100#FF0064FFFFFFFFFF\n",
        "can0  18FEEE00   [8]  82 FF FF FF FF FF FF FF\n# comment\n\n",
        "(1.0) vcan0 18ECFF00#2012000300FFECFE\n(1.1) vcan0 18EBFF00#0141424344454647\n",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        add_corpus((const uint8_t *)lines[i], strlen(lines[i]));
}

/* ---- mutation ---------------------------------------------------------- */
static const uint8_t interesting[] = { 0, 1, 7, 8, 9, 0x0F, 0x10, 0x20, 0x7F, 0x80, 0xEB, 0xEC,
                                       0xFA, 0xFB, 0xFE, 0xFF, '#', '\n', '(', ')', '.', '[', ']' };

static size_t mutate(uint8_t *b, size_t n)
{
    unsigned rounds = 1 + (unsigned)rndn(4);
    while (rounds--) {
        switch (rndn(9)) {
        case 0: if (n) b[rndn(n)] ^= (uint8_t)(1u << rndn(8)); break;
        case 1: if (n) b[rndn(n)] = (uint8_t)rnd(); break;
        case 2: if (n) b[rndn(n)] = interesting[rndn(sizeof interesting)]; break;
        case 3: if (n) b[rndn(n)] = (uint8_t)(b[rndn(n)] + (int)rndn(9) - 4); break;
        case 4:                                      /* insert byte */
            if (n < MAX_LEN) {
                size_t at = rndn(n + 1);
                memmove(b + at + 1, b + at, n - at);
                b[at] = interesting[rndn(sizeof interesting)];
                n++;
            }
            break;
        case 5:                                      /* delete range */
            if (n > 1) {
                size_t at = rndn(n), len = 1 + rndn(n - at < 16 ? n - at : 16);
                memmove(b + at, b + at + len, n - at - len);
                n -= len;
            }
            break;
        case 6:                                      /* duplicate range */
            if (n && n < MAX_LEN - 32) {
                size_t at = rndn(n), len = 1 + rndn(n - at < 28 ? n - at : 28);
                memmove(b + at + len, b + at, n - at);
                n += len;
            }
            break;
        case 7:                                      /* splice another entry */
            if (g_ncorpus) {
                const entry_t *o = &g_corpus[rndn(g_ncorpus)];
                if (o->n) {
                    size_t from = rndn(o->n), len = 1 + rndn(o->n - from < 28 ? o->n - from : 28);
                    size_t at = rndn(n + 1);
                    if (n + len <= MAX_LEN) {
                        memmove(b + at + len, b + at, n - at);
                        memcpy(b + at, o->d + from, len);
                        n += len;
                    }
                }
            }
            break;
        default:                                     /* frame-aligned field poke */
            if (n >= 14) {
                size_t fr = rndn(n / 14) * 14;
                b[fr + rndn(14)] = interesting[rndn(sizeof interesting)];
            }
        }
    }
    return n;
}

static void load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    uint8_t buf[MAX_LEN];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    run_one(buf, n);
    add_corpus(buf, n);
}

int main(int argc, char **argv)
{
    unsigned long iters = 100000;
    int argi = 1;

    for (; argi < argc && argv[argi][0] == '-' && argv[argi][1]; argi += 2) {
        if (argi + 1 >= argc) { fputs("missing option value\n", stderr); return 2; }
        if (argv[argi][1] == 'n') iters = strtoul(argv[argi + 1], NULL, 10);
        else if (argv[argi][1] == 's') g_rng ^= strtoull(argv[argi + 1], NULL, 10) * 0x9E3779B97F4A7C15ull;
        else if (argv[argi][1] == 'o') g_crash_dir = argv[argi + 1];
        else { fprintf(stderr, "unknown option %s\n", argv[argi]); return 2; }
    }
    int sigs[] = { SIGABRT, SIGSEGV, SIGILL, SIGTRAP, SIGBUS, SIGFPE };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], on_crash);

    add_seeds();
    size_t nseeds = g_ncorpus;
    for (; argi < argc; argi++) load_file(argv[argi]);
    for (size_t i = 0; i < nseeds; i++) run_one(g_corpus[i].d, g_corpus[i].n);

    uint8_t buf[MAX_LEN + 64];
    for (unsigned long it = 0; it < iters; it++) {
        const entry_t *e = &g_corpus[rndn(g_ncorpus)];
        memcpy(buf, e->d, e->n);
        size_t n = mutate(buf, e->n);
        if (n > MAX_LEN) n = MAX_LEN;
        g_new_edges = 0;
        run_one(buf, n);
        if (g_new_edges) add_corpus(g_cur, g_cur_len);
    }
    printf("fuzz: %lu execs, corpus %zu entries, %u edges reached, no crashes\n",
           iters, g_ncorpus, g_total_edges);
    return 0;
}
