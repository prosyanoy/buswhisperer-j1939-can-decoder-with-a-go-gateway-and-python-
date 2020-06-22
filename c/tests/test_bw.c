/* Minimal dependency-free unit tests for libbuswhisperer. */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "bw_j1939.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-6)

typedef struct { bw_signal_t s[16]; char text[64]; size_t n; } sink_t;

static void collect(const bw_signal_t *s, void *u)
{
    sink_t *k = u;
    if (k->n < 16) {
        k->s[k->n] = *s;
        if (s->text) {   /* text points into the session buffer: copy it */
            size_t n = s->text_len < 63 ? s->text_len : 63;
            memcpy(k->text, s->text, n);
            k->text[n] = 0;
            k->s[k->n].text = k->text;
        }
        k->n++;
    }
}

static bw_frame_t mk(uint64_t ts, uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da,
                     const uint8_t *d, uint8_t dlc)
{
    bw_frame_t f;
    memset(&f, 0, sizeof f);
    f.ts_us = ts; f.can_id = bw_id_make(prio, pgn, sa, da); f.dlc = dlc;
    if (dlc) memcpy(f.data, d, dlc);
    return f;
}

static void test_id(void)
{
    bw_id_t id;
    CHECK(bw_id_parse(0x0CF00400u, &id) == 0);
    CHECK(id.priority == 3 && id.pgn == 61444 && id.sa == 0x00 && !id.is_pdu1);
    CHECK(id.da == 0xFF);
    CHECK(bw_id_parse(0x18FEF100u, &id) == 0);
    CHECK(id.priority == 6 && id.pgn == 65265);
    /* PDU1: PS is a destination address and is excluded from the PGN */
    CHECK(bw_id_parse(0x18EC2A11u, &id) == 0);
    CHECK(id.pgn == BW_PGN_TP_CM && id.is_pdu1 && id.da == 0x2A && id.sa == 0x11);
    CHECK(bw_id_parse(0x20000000u, &id) == -1);
    CHECK(bw_id_parse(0x1FFFFFFFu, &id) == 0 && id.priority == 7 && id.pgn == 0x3FFFFu - 0xFFu + 0xFFu);
    CHECK(bw_id_make(3, 61444, 0, 0xFF) == 0x0CF00400u);
    CHECK(bw_id_make(6, BW_PGN_TP_CM, 0x11, 0x2A) == 0x18EC2A11u);
    for (uint32_t v = 0; v < 0x1FFFFFFFu; v += 0x10001u) {   /* round trip */
        bw_id_parse(v, &id);
        CHECK(bw_id_make(id.priority, id.pgn, id.sa, id.da) == v);
    }
}

static void test_decode_basic(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    /* EEC1: torque raw 150 (=25%), speed raw 12000 (=1500 rpm) */
    uint8_t eec1[8] = { 0, 0, 150, 12000 & 0xFF, 12000 >> 8, 0, 0, 0 };
    bw_frame_t f = mk(1, 3, 61444, 0, 0xFF, eec1, 8);
    bw_ctx_init(c);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 2);
    CHECK(strcmp(k.s[0].name, "engine_speed") == 0);
    NEAR(k.s[0].value, 1500.0);
    NEAR(k.s[1].value, 25.0);

    k.n = 0;
    uint8_t ccvs[8] = { 0xFF, 0x00, 0x64, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; /* 0x6400/256 = 100 km/h */
    f = mk(2, 6, 65265, 0, 0xFF, ccvs, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 1);
    NEAR(k.s[0].value, 100.0);

    k.n = 0;
    uint8_t et1[8] = { 130, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  /* 90 degC */
    f = mk(3, 6, 65262, 0, 0xFF, et1, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 1);
    NEAR(k.s[0].value, 90.0);
    CHECK(strcmp(k.s[0].unit, "degC") == 0);
    free(c);
}

static void test_not_available_and_short(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    bw_ctx_init(c);
    uint8_t na[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    bw_frame_t f = mk(1, 3, 61444, 0, 0xFF, na, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0);       /* all N/A */
    uint8_t err[8] = { 0xFB, 0, 0, 0, 0, 0, 0, 0 };    /* 0xFB = reserved/error range */
    f = mk(1, 6, 65262, 0, 0xFF, err, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0);
    uint8_t ok1[1] = { 100 };                          /* short frame still decodes byte 0 */
    f = mk(1, 6, 65262, 0, 0xFF, ok1, 1);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 1);
    f = mk(1, 3, 61444, 0, 0xFF, ok1, 1);             /* EEC1 needs 5 bytes: skipped */
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0);
    f = mk(1, 3, 61444, 0, 0xFF, ok1, 1);
    f.dlc = 9;                                         /* illegal DLC */
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0 && c->stats.frames_malformed == 1);
    f.dlc = 8; f.can_id = 0xFFFFFFFFu;                 /* illegal identifier */
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0 && c->stats.frames_malformed == 2);
    free(c);
}

/* Sends a BAM for `total` bytes of payload (byte i = 'A'+i%26) for `pgn`.
 * Returns number of frames after the announce that were fed (DT packets). */
static void bam_announce(bw_ctx_t *c, sink_t *k, uint64_t ts, uint8_t sa, uint32_t pgn, unsigned total)
{
    uint8_t cm[8] = { 32, (uint8_t)(total & 0xFF), (uint8_t)(total >> 8), (uint8_t)((total + 6) / 7), 0xFF,
                      (uint8_t)(pgn & 0xFF), (uint8_t)((pgn >> 8) & 0xFF), (uint8_t)((pgn >> 16) & 0xFF) };
    bw_frame_t f = mk(ts, 7, BW_PGN_TP_CM, sa, 0xFF, cm, 8);
    bw_ctx_feed(c, &f, collect, k);
}

static size_t bam_dt(bw_ctx_t *c, sink_t *k, uint64_t ts, uint8_t sa, unsigned seq, unsigned total)
{
    uint8_t d[8];
    d[0] = (uint8_t)seq;
    for (int i = 0; i < 7; i++) {
        unsigned idx = (seq - 1) * 7 + (unsigned)i;
        d[1 + i] = idx < total ? (uint8_t)('A' + idx % 26) : 0xFF;
    }
    bw_frame_t f = mk(ts, 7, BW_PGN_TP_DT, sa, 0xFF, d, 8);
    return bw_ctx_feed(c, &f, collect, k);
}

static void test_bam_vin(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    bw_ctx_init(c);
    /* 17-char VIN + '*' = 18 bytes -> 3 packets */
    const char vin[] = "1FUJGLDR12LM12345*";
    unsigned total = (unsigned)strlen(vin);
    uint8_t cm[8] = { 32, (uint8_t)total, 0, 3, 0xFF, 0xEC, 0xFE, 0x00 };
    bw_frame_t f = mk(1000, 7, BW_PGN_TP_CM, 0x00, 0xFF, cm, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0);
    for (unsigned seq = 1; seq <= 3; seq++) {
        uint8_t d[8] = { (uint8_t)seq, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        for (unsigned i = 0; i < 7 && (seq - 1) * 7 + i < total; i++)
            d[1 + i] = (uint8_t)vin[(seq - 1) * 7 + i];
        f = mk(1000 + seq * 50000, 7, BW_PGN_TP_DT, 0x00, 0xFF, d, 8);
        size_t n = bw_ctx_feed(c, &f, collect, &k);
        CHECK(n == (seq == 3 ? 1u : 0u));
    }
    CHECK(k.n == 1 && strcmp(k.s[0].name, "vin") == 0);
    CHECK(k.s[0].text_len == 17 && memcmp(k.s[0].text, "1FUJGLDR12LM12345", 17) == 0);
    CHECK(c->stats.tp_started == 1 && c->stats.tp_completed == 1);
    free(c);
}

static void test_tp_errors(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    bw_ctx_init(c);

    /* oversize and undersize announcements */
    bam_announce(c, &k, 0, 1, 65260, 1786);
    bam_announce(c, &k, 0, 1, 65260, 8);
    CHECK(c->stats.tp_errors == 2 && c->stats.tp_started == 0);
    /* packet count disagrees with size */
    uint8_t bad[8] = { 32, 20, 0, 9, 0xFF, 0xEC, 0xFE, 0 };
    bw_frame_t f = mk(0, 7, BW_PGN_TP_CM, 1, 0xFF, bad, 8);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_errors == 3);
    /* BAM sent to a specific destination */
    uint8_t d2[8] = { 32, 20, 0, 3, 0xFF, 0xEC, 0xFE, 0 };
    f = mk(0, 7, BW_PGN_TP_CM, 1, 0x20, d2, 8);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_errors == 4);
    /* unknown control byte */
    uint8_t d3[8] = { 99 };
    f = mk(0, 7, BW_PGN_TP_CM, 1, 0xFF, d3, 8);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_errors == 5);

    /* orphan DT */
    CHECK(bam_dt(c, &k, 10, 1, 1, 20) == 0 && c->stats.tp_orphans == 1);

    /* out-of-order packet kills the session */
    bam_announce(c, &k, 100, 2, 65260, 20);
    CHECK(bam_dt(c, &k, 200, 2, 1, 20) == 0);
    CHECK(bam_dt(c, &k, 300, 2, 3, 20) == 0);
    CHECK(c->stats.tp_errors == 6);
    CHECK(bam_dt(c, &k, 400, 2, 2, 20) == 0 && c->stats.tp_orphans == 2);

    /* duplicate packet is ignored, message still completes */
    bam_announce(c, &k, 1000, 3, 65260, 20);
    bam_dt(c, &k, 1100, 3, 1, 20);
    bam_dt(c, &k, 1200, 3, 1, 20);
    bam_dt(c, &k, 1300, 3, 2, 20);
    CHECK(c->stats.tp_duplicates == 1);
    CHECK(bam_dt(c, &k, 1400, 3, 3, 20) == 1 && k.n == 1);
    CHECK(k.s[0].text_len == 20 && k.text[0] == 'A' && k.text[19] == 'T');

    /* a short TP.DT frame is an error */
    uint8_t sh[3] = { 1, 2, 3 };
    f = mk(0, 7, BW_PGN_TP_DT, 3, 0xFF, sh, 3);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_errors == 7);
    free(c);
}

static void test_tp_timeout(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    bw_ctx_init(c);
    bam_announce(c, &k, 0, 4, 65260, 20);
    bam_dt(c, &k, 100000, 4, 1, 20);
    /* gap of 800 ms > 750 ms: next packet finds no session */
    CHECK(bam_dt(c, &k, 100000 + 800000, 4, 2, 20) == 0);
    CHECK(c->stats.tp_timeouts == 1 && c->stats.tp_orphans == 1);

    /* exactly at the limit is still alive */
    bam_announce(c, &k, 5000000, 4, 65260, 20);
    bam_dt(c, &k, 5000000 + BW_TP_TIMEOUT_US, 4, 1, 20);
    CHECK(c->stats.tp_timeouts == 1);
    /* explicit expiry */
    bw_ctx_expire(c, 5000000 + 2 * BW_TP_TIMEOUT_US + 1);
    CHECK(c->stats.tp_timeouts == 2);
    /* clock going backwards never expires */
    bam_announce(c, &k, 9000000, 4, 65260, 20);
    bw_ctx_expire(c, 1);
    CHECK(c->stats.tp_timeouts == 2);
    free(c);
}

static void test_tp_concurrency_and_eviction(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    bw_ctx_init(c);
    for (uint8_t sa = 1; sa <= BW_TP_MAX_SESSIONS; sa++)
        bam_announce(c, &k, 10, sa, 65260, 20);
    CHECK(c->stats.tp_started == BW_TP_MAX_SESSIONS && c->stats.tp_aborted == 0);
    bam_announce(c, &k, 11, 9, 65260, 20);               /* evicts sa=1 */
    CHECK(c->stats.tp_aborted == 1);
    CHECK(bam_dt(c, &k, 12, 1, 1, 20) == 0 && c->stats.tp_orphans == 1);
    /* interleaved sessions from different senders both complete */
    bam_dt(c, &k, 13, 2, 1, 20); bam_dt(c, &k, 14, 3, 1, 20);
    bam_dt(c, &k, 15, 2, 2, 20); bam_dt(c, &k, 16, 3, 2, 20);
    bam_dt(c, &k, 17, 2, 3, 20); bam_dt(c, &k, 18, 3, 3, 20);
    CHECK(k.n == 2 && c->stats.tp_completed == 2);
    /* restart of the same pair counts as abort and resets sequence */
    bam_announce(c, &k, 20, 4, 65260, 20);
    uint64_t ab = c->stats.tp_aborted;
    bam_announce(c, &k, 21, 4, 65260, 20);
    CHECK(c->stats.tp_aborted == ab + 1);
    free(c);
}

static void test_rts_cts_and_abort(void)
{
    sink_t k = {0};
    bw_ctx_t *c = calloc(1, sizeof *c);
    bw_ctx_init(c);
    /* sa 0x11 -> da 0x2A, 14 bytes = 2 packets */
    uint8_t rts[8] = { 16, 14, 0, 2, 2, 0xEC, 0xFE, 0 };
    bw_frame_t f = mk(0, 7, BW_PGN_TP_CM, 0x11, 0x2A, rts, 8);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_started == 1);
    uint8_t cts[8] = { 17, 2, 1, 0xFF, 0xFF, 0xEC, 0xFE, 0 };
    f = mk(1, 7, BW_PGN_TP_CM, 0x2A, 0x11, cts, 8);
    bw_ctx_feed(c, &f, collect, &k);
    uint8_t d1[8] = { 1, 'A', 'B', 'C', 'D', 'E', 'F', 'G' };
    uint8_t d2[8] = { 2, 'H', 'I', 'J', 'K', 'L', 'M', 'N' };
    f = mk(2, 7, BW_PGN_TP_DT, 0x11, 0x2A, d1, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 0);
    f = mk(3, 7, BW_PGN_TP_DT, 0x11, 0x2A, d2, 8);
    CHECK(bw_ctx_feed(c, &f, collect, &k) == 1);
    CHECK(k.n == 1 && k.s[0].sa == 0x11 && k.s[0].text_len == 14);
    CHECK(memcmp(k.text, "ABCDEFGHIJKLMN", 14) == 0);
    CHECK(c->stats.tp_completed == 1);

    /* receiver aborts a running session */
    f = mk(10, 7, BW_PGN_TP_CM, 0x11, 0x2A, rts, 8);
    bw_ctx_feed(c, &f, collect, &k);
    uint8_t ab[8] = { 255, 1, 0xFF, 0xFF, 0xFF, 0xEC, 0xFE, 0 };
    f = mk(11, 7, BW_PGN_TP_CM, 0x2A, 0x11, ab, 8);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_aborted == 1);
    f = mk(12, 7, BW_PGN_TP_DT, 0x11, 0x2A, d1, 8);
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_orphans == 1);

    /* RTS to the broadcast address is invalid */
    f = mk(13, 7, BW_PGN_TP_CM, 0x11, 0xFF, rts, 8);
    uint64_t e = c->stats.tp_errors;
    bw_ctx_feed(c, &f, collect, &k);
    CHECK(c->stats.tp_errors == e + 1);
    free(c);
}

static int parse(const char *s, bw_frame_t *f) { return bw_candump_parse(s, strlen(s), f); }

static void test_candump(void)
{
    bw_frame_t f;
    CHECK(parse("(1609459200.123456) can0 0CF00400#0011223344556677\n", &f) == BW_PARSE_OK);
    CHECK(f.ts_us == 1609459200123456ull && f.can_id == 0x0CF00400u && f.dlc == 8);
    CHECK(f.data[0] == 0x00 && f.data[7] == 0x77 && f.data[3] == 0x33);
    CHECK(parse("(1.5) vcan0 18FEF100#FF", &f) == BW_PARSE_OK && f.ts_us == 1500000 && f.dlc == 1);
    CHECK(parse("(2.1234567) can0 18FEF100#", &f) == BW_PARSE_OK && f.ts_us == 2123456 && f.dlc == 0);
    CHECK(parse("  can0  18FEF100   [3]  01 aB 03\r\n", &f) == BW_PARSE_OK);
    CHECK(f.dlc == 3 && f.data[1] == 0xAB && f.ts_us == 0);
    CHECK(parse("", &f) == BW_PARSE_SKIP && parse("   \n", &f) == BW_PARSE_SKIP);
    CHECK(parse("# comment", &f) == BW_PARSE_SKIP);
    CHECK(parse("(1.0) can0 123#1122", &f) == BW_PARSE_STD_ID);
    CHECK(parse("(1.0) can0 18FEF100#R", &f) == BW_PARSE_RTR);
    CHECK(parse("(1.0) can0 18FEF100##0112233", &f) == BW_PARSE_FD);
    CHECK(parse("(1.0) can0 18FEF100#112233445566778899", &f) == BW_PARSE_RANGE);
    CHECK(parse("(1.0) can0 FFFFFFFF#00", &f) == BW_PARSE_RANGE);
    CHECK(parse("(1.0) can0 18FEF100#123", &f) == BW_PARSE_SYNTAX);      /* odd digits */
    CHECK(parse("(1.0) can0 18FEF100#ZZ", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("(1.0) can0 18FEF100#00 junk", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("(abc) can0 18FEF100#00", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("(1.0 can0 18FEF100#00", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("(9999999999999.0) can0 18FEF100#00", &f) == BW_PARSE_RANGE);
    CHECK(parse("(1.0)", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("(1.0) can0", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("can0 18FEF100 [9] 00 00 00 00 00 00 00 00 00", &f) == BW_PARSE_RANGE);
    CHECK(parse("can0 18FEF100 [4] 00 11", &f) == BW_PARSE_SYNTAX);
    CHECK(parse("can0 18FEF100 4 00 11", &f) == BW_PARSE_SYNTAX);
    CHECK(bw_candump_parse(NULL, 0, &f) == BW_PARSE_SYNTAX);
    /* not NUL terminated: only `len` bytes may be read */
    const char buf[] = "(1.0) can0 18FEF100#0102030405060708FFFF";
    CHECK(bw_candump_parse(buf, strlen("(1.0) can0 18FEF100#01020304"), &f) == BW_PARSE_OK && f.dlc == 4);
}

int main(void)
{
    test_id();
    test_decode_basic();
    test_not_available_and_short();
    test_bam_vin();
    test_tp_errors();
    test_tp_timeout();
    test_tp_concurrency_and_eviction();
    test_rts_cts_and_abort();
    test_candump();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
