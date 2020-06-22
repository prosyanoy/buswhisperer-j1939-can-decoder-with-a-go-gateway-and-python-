#include <string.h>
#include "bw_j1939.h"

#define TP_CM_RTS   16u
#define TP_CM_CTS   17u
#define TP_CM_EOM   19u
#define TP_CM_BAM   32u
#define TP_CM_ABORT 255u

void bw_ctx_init(bw_ctx_t *ctx)
{
    memset(ctx, 0, sizeof *ctx);
}

void bw_ctx_expire(bw_ctx_t *ctx, uint64_t now_us)
{
    for (int i = 0; i < BW_TP_MAX_SESSIONS; i++) {
        bw_tp_session_t *s = &ctx->sess[i];
        /* A clock that steps backwards never expires a session. */
        if (s->active && now_us > s->last_ts_us &&
            now_us - s->last_ts_us > BW_TP_TIMEOUT_US) {
            s->active = false;
            ctx->stats.tp_timeouts++;
        }
    }
}

static bw_tp_session_t *find_session(bw_ctx_t *ctx, uint8_t sa, uint8_t da)
{
    for (int i = 0; i < BW_TP_MAX_SESSIONS; i++) {
        bw_tp_session_t *s = &ctx->sess[i];
        if (s->active && s->sa == sa && s->da == da)
            return s;
    }
    return NULL;
}

static bw_tp_session_t *alloc_session(bw_ctx_t *ctx, uint8_t sa, uint8_t da)
{
    bw_tp_session_t *s = find_session(ctx, sa, da);
    if (s != NULL) {                      /* restarted by the same peer pair */
        ctx->stats.tp_aborted++;
        return s;
    }
    bw_tp_session_t *oldest = &ctx->sess[0];
    for (int i = 0; i < BW_TP_MAX_SESSIONS; i++) {
        if (!ctx->sess[i].active)
            return &ctx->sess[i];
        if (ctx->sess[i].started_order < oldest->started_order)
            oldest = &ctx->sess[i];
    }
    ctx->stats.tp_aborted++;              /* table full: evict the oldest */
    return oldest;
}

static void tp_start(bw_ctx_t *ctx, const bw_frame_t *f, const bw_id_t *id, bool bam)
{
    const uint8_t *d = f->data;
    uint32_t total = (uint32_t)d[1] | ((uint32_t)d[2] << 8);
    uint32_t pkts  = d[3];
    uint32_t pgn   = (uint32_t)d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16);

    if (f->dlc != 8 || total < 9 || total > BW_MAX_TP_BYTES ||
        pkts != (total + 6u) / 7u || pgn > 0x3FFFFu ||
        (bam && id->da != 0xFFu) || (!bam && id->da == 0xFFu)) {
        ctx->stats.tp_errors++;
        return;
    }
    bw_tp_session_t *s = alloc_session(ctx, id->sa, id->da);
    s->active = true;
    s->is_bam = bam;
    s->sa = id->sa;
    s->da = id->da;
    s->npackets = (uint8_t)pkts;
    s->next_seq = 1;
    s->total = (uint16_t)total;
    s->pgn = pgn;
    s->last_ts_us = f->ts_us;
    s->started_order = ++ctx->order;
    ctx->stats.tp_started++;
}

static void tp_cm(bw_ctx_t *ctx, const bw_frame_t *f, const bw_id_t *id)
{
    if (f->dlc < 1) {
        ctx->stats.tp_errors++;
        return;
    }
    switch (f->data[0]) {
    case TP_CM_BAM:   tp_start(ctx, f, id, true);  break;
    case TP_CM_RTS:   tp_start(ctx, f, id, false); break;
    case TP_CM_CTS:
    case TP_CM_EOM:   break;              /* flow control: nothing to track */
    case TP_CM_ABORT: {
        /* Sent by the receiver (reverse key) or, rarely, the sender. */
        bw_tp_session_t *s = find_session(ctx, id->da, id->sa);
        if (s == NULL)
            s = find_session(ctx, id->sa, id->da);
        if (s != NULL) {
            s->active = false;
            ctx->stats.tp_aborted++;
        }
        break;
    }
    default:
        ctx->stats.tp_errors++;
    }
}

static size_t tp_dt(bw_ctx_t *ctx, const bw_frame_t *f, const bw_id_t *id,
                    bw_signal_cb cb, void *user)
{
    if (f->dlc != 8) {
        ctx->stats.tp_errors++;
        return 0;
    }
    bw_tp_session_t *s = find_session(ctx, id->sa, id->da);
    if (s == NULL) {
        ctx->stats.tp_orphans++;
        return 0;
    }
    uint8_t seq = f->data[0];
    if (seq >= 1u && (unsigned)seq + 1u == s->next_seq) {        /* retransmitted packet */
        ctx->stats.tp_duplicates++;
        return 0;
    }
    if (seq != s->next_seq) {             /* gap or reorder: abandon */
        s->active = false;
        ctx->stats.tp_errors++;
        return 0;
    }
    size_t off = (size_t)(seq - 1u) * 7u;
    size_t n = s->total - off;
    if (n > 7u)
        n = 7u;
    memcpy(s->buf + off, f->data + 1, n);
    s->next_seq++;
    s->last_ts_us = f->ts_us;
    if (seq != s->npackets)
        return 0;

    s->active = false;
    ctx->stats.tp_completed++;
    return bw_decode_payload(f->ts_us, s->sa, s->pgn, s->buf, s->total, cb, user);
}

size_t bw_ctx_feed(bw_ctx_t *ctx, const bw_frame_t *f, bw_signal_cb cb, void *user)
{
    bw_id_t id;
    size_t n = 0;

    ctx->stats.frames++;
    bw_ctx_expire(ctx, f->ts_us);
    if (f->dlc > 8 || bw_id_parse(f->can_id, &id) != 0) {
        ctx->stats.frames_malformed++;
        return 0;
    }
    if (id.pgn == BW_PGN_TP_CM)
        tp_cm(ctx, f, &id);
    else if (id.pgn == BW_PGN_TP_DT)
        n = tp_dt(ctx, f, &id, cb, user);
    else
        n = bw_decode_payload(f->ts_us, id.sa, id.pgn, f->data, f->dlc, cb, user);
    ctx->stats.signals += n;
    return n;
}
