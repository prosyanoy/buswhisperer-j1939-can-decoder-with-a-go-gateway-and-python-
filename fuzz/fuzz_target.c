/*
 * Fuzz target for the whole library: the candump text parser, the identifier
 * parser, signal decoding and TP reassembly.
 *
 * Build with libFuzzer (clang):  see `make fuzz-libfuzzer`
 * Build with the gcc driver:     see scripts/fuzz.sh / fuzz/standalone_driver.c
 *
 * Input is consumed twice:
 *   1. as raw text (lines split on '\n') for bw_candump_parse
 *   2. as a binary frame stream, 14 bytes per frame:
 *        [0..3] CAN id (LE)  [4] low nibble = dlc (0..15), bit 7 = keep id bits >28
 *        [5..12] data        [13] timestamp delta in 4 ms units
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bw_j1939.h"

static volatile uint8_t g_sink;

static void touch(const bw_signal_t *s, void *user)
{
    (void)user;
    g_sink ^= (uint8_t)s->name[0] ^ (uint8_t)s->unit[0];
    for (size_t i = 0; i < s->text_len; i++)      /* ASan checks the pointer */
        g_sink ^= (uint8_t)s->text[i];
    if (s->value != s->value)                     /* NaN would break JSON output */
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bw_ctx_t ctx;
    uint8_t *copy = malloc(size ? size : 1);     /* exact-size: overreads trap */
    if (copy == NULL)
        return 0;
    memcpy(copy, data, size);

    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || copy[i] == '\n') {
            bw_frame_t f;
            if (bw_candump_parse((const char *)copy + start, i - start, &f) == BW_PARSE_OK)
                bw_ctx_feed(&ctx, &f, touch, NULL);
            start = i + 1;
        }
    }

    bw_ctx_init(&ctx);
    uint64_t ts = 0;
    for (size_t off = 0; off + 14 <= size; off += 14) {
        const uint8_t *p = copy + off;
        bw_frame_t f;
        memset(&f, 0, sizeof f);
        f.can_id = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24);
        if (!(p[4] & 0x80))
            f.can_id &= 0x1FFFFFFFu;
        f.dlc = p[4] & 0x0F;
        memcpy(f.data, p + 5, 8);
        ts += (uint64_t)p[13] * 4000u;
        f.ts_us = ts;
        bw_ctx_feed(&ctx, &f, touch, NULL);
    }
    bw_ctx_expire(&ctx, ts + 10 * BW_TP_TIMEOUT_US);
    free(copy);
    return 0;
}
