/*
 * bw_decode: reads candump logs (files or stdin), writes one JSON object per
 * decoded signal to stdout. With --stats, a JSON summary goes to stderr.
 * Malformed lines are counted and skipped, never fatal.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bw_j1939.h"

#define LINE_MAX_BYTES 512

static void print_json_string(FILE *out, const char *s, size_t n)
{
    fputc('"', out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            fprintf(out, "\\%c", c);
        else if (c < 0x20 || c > 0x7E)
            fputc('?', out);
        else
            fputc(c, out);
    }
    fputc('"', out);
}

static void emit(const bw_signal_t *s, void *user)
{
    FILE *out = user;
    fprintf(out, "{\"ts_us\":%" PRIu64 ",\"sa\":%u,\"pgn\":%" PRIu32 ",\"name\":",
            s->ts_us, (unsigned)s->sa, s->pgn);
    print_json_string(out, s->name, strlen(s->name));
    fputs(",\"unit\":", out);
    print_json_string(out, s->unit, strlen(s->unit));
    if (s->text != NULL) {
        fputs(",\"text\":", out);
        print_json_string(out, s->text, s->text_len);
    } else {
        fprintf(out, ",\"value\":%.10g", s->value);
    }
    fputs("}\n", out);
}

typedef struct {
    uint64_t lines, parsed, skipped, bad_syntax, std_ids, rtr, fd, range, overlong;
} parse_stats_t;

static void count_error(parse_stats_t *ps, int rc)
{
    switch (rc) {
    case BW_PARSE_STD_ID: ps->std_ids++; break;
    case BW_PARSE_RTR:    ps->rtr++; break;
    case BW_PARSE_FD:     ps->fd++; break;
    case BW_PARSE_RANGE:  ps->range++; break;
    default:              ps->bad_syntax++; break;
    }
}

static void handle_line(const char *line, size_t n, bw_ctx_t *ctx, parse_stats_t *ps)
{
    bw_frame_t f;
    int rc = bw_candump_parse(line, n, &f);
    if (rc == BW_PARSE_OK) {
        ps->parsed++;
        bw_ctx_feed(ctx, &f, emit, stdout);
    } else if (rc == BW_PARSE_SKIP) {
        ps->skipped++;
    } else {
        count_error(ps, rc);
    }
}

/* Reads byte-wise so embedded NULs and overlong lines are handled exactly:
 * an overlong line is counted once and dropped up to its newline. */
static void process(FILE *in, bw_ctx_t *ctx, parse_stats_t *ps)
{
    char line[LINE_MAX_BYTES];
    size_t n = 0;
    bool overlong = false, any = false;
    int c;

    while ((c = getc(in)) != EOF) {
        any = true;
        if (c == '\n') {
            ps->lines++;
            if (overlong) ps->overlong++;
            else handle_line(line, n, ctx, ps);
            n = 0; overlong = false; any = false;
        } else if (n < sizeof line) {
            line[n++] = (char)c;
        } else {
            overlong = true;
        }
    }
    if (any) {                             /* final line without newline */
        ps->lines++;
        if (overlong) ps->overlong++;
        else handle_line(line, n, ctx, ps);
    }
}

int main(int argc, char **argv)
{
    static bw_ctx_t ctx;                   /* ~7 KB; static, no heap */
    parse_stats_t ps;
    bool stats = false;
    int nfiles = 0;

    memset(&ps, 0, sizeof ps);
    bw_ctx_init(&ctx);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stats") == 0) {
            stats = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            puts("usage: bw_decode [--stats] [logfile ...]   (stdin if no file or '-')");
            return 0;
        } else {
            FILE *in = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
            if (in == NULL) {
                perror(argv[i]);
                return 2;
            }
            process(in, &ctx, &ps);
            if (in != stdin) fclose(in);
            nfiles++;
        }
    }
    if (nfiles == 0)
        process(stdin, &ctx, &ps);
    fflush(stdout);

    if (stats) {
        const bw_stats_t *s = &ctx.stats;
        fprintf(stderr,
            "{\"lines\":%" PRIu64 ",\"parsed\":%" PRIu64 ",\"skipped\":%" PRIu64
            ",\"bad_syntax\":%" PRIu64 ",\"std_ids\":%" PRIu64 ",\"rtr\":%" PRIu64
            ",\"fd\":%" PRIu64 ",\"range\":%" PRIu64 ",\"overlong\":%" PRIu64
            ",\"frames\":%" PRIu64 ",\"frames_malformed\":%" PRIu64
            ",\"signals\":%" PRIu64 ",\"tp_started\":%" PRIu64 ",\"tp_completed\":%" PRIu64
            ",\"tp_timeouts\":%" PRIu64 ",\"tp_aborted\":%" PRIu64 ",\"tp_errors\":%" PRIu64
            ",\"tp_orphans\":%" PRIu64 ",\"tp_duplicates\":%" PRIu64 "}\n",
            ps.lines, ps.parsed, ps.skipped, ps.bad_syntax, ps.std_ids, ps.rtr, ps.fd,
            ps.range, ps.overlong, s->frames, s->frames_malformed, s->signals,
            s->tp_started, s->tp_completed, s->tp_timeouts, s->tp_aborted,
            s->tp_errors, s->tp_orphans, s->tp_duplicates);
    }
    return 0;
}
