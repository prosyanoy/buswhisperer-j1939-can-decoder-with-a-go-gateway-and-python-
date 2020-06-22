#include "bw_j1939.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

typedef struct { const char *p; const char *end; } cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end && is_ws(*c->p))
        c->p++;
}

/* "(1609459200.123456)" -> microseconds; digits past microseconds are dropped */
static int parse_ts(cur_t *c, uint64_t *out)
{
    uint64_t sec = 0, frac = 0;
    unsigned nsec = 0, nfrac = 0;
    c->p++;                                /* '(' */
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        if (++nsec > 12) return BW_PARSE_RANGE;
        sec = sec * 10 + (uint64_t)(*c->p++ - '0');
    }
    if (nsec == 0) return BW_PARSE_SYNTAX;
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            if (nfrac < 6) {
                frac = frac * 10 + (uint64_t)(*c->p - '0');
                nfrac++;
            }
            c->p++;
        }
        for (; nfrac < 6; nfrac++)
            frac *= 10;
    }
    if (c->p >= c->end || *c->p != ')') return BW_PARSE_SYNTAX;
    c->p++;
    *out = sec * 1000000ull + frac;
    return BW_PARSE_OK;
}

static int parse_id(cur_t *c, uint32_t *id)
{
    unsigned n = 0;
    uint32_t v = 0;
    while (c->p < c->end && hexval(*c->p) >= 0) {
        if (++n > 8) return BW_PARSE_RANGE;
        v = (v << 4) | (uint32_t)hexval(*c->p++);
    }
    if (n == 3) return BW_PARSE_STD_ID;
    if (n != 8) return BW_PARSE_SYNTAX;
    if (v > 0x1FFFFFFFu) return BW_PARSE_RANGE;
    *id = v;
    return BW_PARSE_OK;
}

int bw_candump_parse(const char *line, size_t len, bw_frame_t *out)
{
    cur_t c;
    int rc;

    if (line == NULL || out == NULL) return BW_PARSE_SYNTAX;
    c.p = line; c.end = line + len;
    out->ts_us = 0; out->can_id = 0; out->dlc = 0;
    for (int i = 0; i < 8; i++) out->data[i] = 0;

    skip_ws(&c);
    if (c.p >= c.end || *c.p == '#') return BW_PARSE_SKIP;

    if (*c.p == '(') {
        if ((rc = parse_ts(&c, &out->ts_us)) != BW_PARSE_OK) return rc;
        skip_ws(&c);
    }
    const char *name = c.p;                    /* interface name */
    while (c.p < c.end && !is_ws(*c.p)) c.p++;
    if (c.p == name) return BW_PARSE_SYNTAX;
    skip_ws(&c);

    if ((rc = parse_id(&c, &out->can_id)) != BW_PARSE_OK) return rc;

    if (c.p < c.end && *c.p == '#') {          /* ID#DATA (-L format) */
        c.p++;
        if (c.p < c.end && *c.p == '#') return BW_PARSE_FD;
        if (c.p < c.end && *c.p == 'R') return BW_PARSE_RTR;
        unsigned n = 0;
        while (c.p < c.end && hexval(*c.p) >= 0) {
            int hi = hexval(*c.p++);
            if (c.p >= c.end || hexval(*c.p) < 0) return BW_PARSE_SYNTAX; /* odd digit count */
            int lo = hexval(*c.p++);
            if (n >= 8) return BW_PARSE_RANGE;
            out->data[n++] = (uint8_t)((hi << 4) | lo);
        }
        out->dlc = (uint8_t)n;
    } else {                                   /* ID [n] b0 b1 ... */
        skip_ws(&c);
        if (c.p >= c.end || *c.p != '[') return BW_PARSE_SYNTAX;
        c.p++;
        unsigned n = 0, digits = 0;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
            if (++digits > 3) return BW_PARSE_RANGE;
            n = n * 10 + (unsigned)(*c.p++ - '0');
        }
        if (digits == 0 || c.p >= c.end || *c.p != ']') return BW_PARSE_SYNTAX;
        c.p++;
        if (n > 8) return BW_PARSE_RANGE;
        for (unsigned i = 0; i < n; i++) {
            skip_ws(&c);
            if (c.p + 1 >= c.end || hexval(c.p[0]) < 0 || hexval(c.p[1]) < 0)
                return BW_PARSE_SYNTAX;
            out->data[i] = (uint8_t)((hexval(c.p[0]) << 4) | hexval(c.p[1]));
            c.p += 2;
        }
        out->dlc = (uint8_t)n;
    }
    skip_ws(&c);
    return c.p < c.end ? BW_PARSE_SYNTAX : BW_PARSE_OK;   /* trailing junk */
}
