#include "bw_j1939.h"

/* Signal definitions (SAE J1939-71). Add a row to decode a new signal. */
const bw_signal_def_t bw_signal_table[] = {
    /* pgn      name                       unit    kind            start len scale     offset raw_max */
    { 61444u, "engine_speed",             "rpm",  BW_SIG_NUMERIC, 24, 16, 0.125,      0.0,   0xFAFFu }, /* EEC1 SPN 190 */
    { 61444u, "engine_torque",            "%",    BW_SIG_NUMERIC, 16,  8, 1.0,     -125.0,   0xFAu   }, /* EEC1 SPN 513 */
    { 61443u, "accelerator_pedal",        "%",    BW_SIG_NUMERIC,  8,  8, 0.4,        0.0,   0xFAu   }, /* EEC2 SPN 91  */
    { 65265u, "vehicle_speed",            "km/h", BW_SIG_NUMERIC,  8, 16, 1.0/256.0,  0.0,   0xFAFFu }, /* CCVS SPN 84  */
    { 65262u, "coolant_temp",             "degC", BW_SIG_NUMERIC,  0,  8, 1.0,      -40.0,   0xFAu   }, /* ET1 SPN 110  */
    { 65266u, "fuel_rate",                "L/h",  BW_SIG_NUMERIC,  0, 16, 0.05,       0.0,   0xFAFFu }, /* LFE SPN 183  */
    { 65260u, "vin",                      "",     BW_SIG_ASCII,    0,  0, 1.0,        0.0,   0       }, /* VI, multi-packet */
};
const size_t bw_signal_table_len = sizeof bw_signal_table / sizeof bw_signal_table[0];

static uint32_t extract_bits(const uint8_t *data, unsigned start, unsigned nbits)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = start + i;
        v |= (((uint32_t)data[bit >> 3] >> (bit & 7u)) & 1u) << i;
    }
    return v;
}

size_t bw_decode_payload(uint64_t ts_us, uint8_t sa, uint32_t pgn,
                         const uint8_t *data, size_t len,
                         bw_signal_cb cb, void *user)
{
    size_t emitted = 0;
    if (data == NULL || cb == NULL)
        return 0;
    for (size_t i = 0; i < bw_signal_table_len; i++) {
        const bw_signal_def_t *d = &bw_signal_table[i];
        bw_signal_t s;
        if (d->pgn != pgn)
            continue;
        s.ts_us = ts_us; s.sa = sa; s.pgn = pgn;
        s.name = d->name; s.unit = d->unit;
        s.value = 0.0; s.text = NULL; s.text_len = 0;

        if (d->kind == BW_SIG_ASCII) {
            size_t n = 0;
            while (n < len && data[n] != '*' && data[n] != 0x00 && data[n] != 0xFF)
                n++;                       /* J1939 pads/terminates with '*' */
            if (n == 0)
                continue;
            s.text = (const char *)data;
            s.text_len = n;
        } else {
            if (d->bit_len == 0 || d->bit_len > 32u ||
                (size_t)d->start_bit + d->bit_len > len * 8u)
                continue;
            uint32_t raw = extract_bits(data, d->start_bit, d->bit_len);
            if (raw > d->raw_max)
                continue;                  /* error / not available */
            s.value = (double)raw * d->scale + d->offset;
        }
        cb(&s, user);
        emitted++;
    }
    return emitted;
}
