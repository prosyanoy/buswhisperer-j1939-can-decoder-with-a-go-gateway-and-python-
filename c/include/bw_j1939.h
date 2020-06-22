/*
 * BusWhisperer J1939 decoder: zero-allocation, table-driven.
 *
 * No function in this library calls malloc/free; all state lives in the
 * caller-provided bw_ctx_t. Every input is treated as hostile and bounds-checked.
 */
#ifndef BW_J1939_H
#define BW_J1939_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits (SAE J1939-21) ------------------------------------------- */
#define BW_MAX_TP_BYTES      1785u        /* 255 packets * 7 bytes          */
#define BW_TP_MAX_SESSIONS   4            /* concurrent reassembly sessions */
#define BW_TP_TIMEOUT_US     750000ull    /* T1/T2: max gap between packets */

#define BW_PGN_TP_CM  0x00EC00u
#define BW_PGN_TP_DT  0x00EB00u

/* ---- frames and identifiers ------------------------------------------ */
typedef struct {
    uint64_t ts_us;     /* capture timestamp, microseconds                  */
    uint32_t can_id;    /* 29-bit extended identifier                       */
    uint8_t  dlc;       /* 0..8                                             */
    uint8_t  data[8];
} bw_frame_t;

typedef struct {
    uint8_t  priority;  /* 0..7                                             */
    uint8_t  edp;
    uint8_t  dp;
    uint8_t  pf;        /* PDU format                                       */
    uint8_t  ps;        /* PDU specific (DA for PDU1, group ext. for PDU2)  */
    uint8_t  sa;        /* source address                                   */
    uint8_t  da;        /* destination address (0xFF for PDU2 / broadcast)  */
    bool     is_pdu1;   /* pf < 240: destination specific                   */
    uint32_t pgn;       /* 18-bit parameter group number                    */
} bw_id_t;

/* Returns 0 on success, -1 if can_id has bits above bit 28 set. */
int      bw_id_parse(uint32_t can_id, bw_id_t *out);
/* Builds a 29-bit identifier (inverse of bw_id_parse for valid input). */
uint32_t bw_id_make(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t da);

/* ---- signal definitions ---------------------------------------------- */
typedef enum { BW_SIG_NUMERIC = 0, BW_SIG_ASCII = 1 } bw_sig_kind_t;

typedef struct {
    uint32_t      pgn;
    const char   *name;
    const char   *unit;
    bw_sig_kind_t kind;
    uint8_t       start_bit;  /* little-endian bit index from byte 0 bit 0  */
    uint8_t       bit_len;    /* 1..32 (numeric only)                       */
    double        scale;
    double        offset;
    uint32_t      raw_max;    /* raw values above this are error/N/A codes  */
} bw_signal_def_t;

extern const bw_signal_def_t bw_signal_table[];
extern const size_t          bw_signal_table_len;

typedef struct {
    uint64_t    ts_us;
    uint8_t     sa;
    uint32_t    pgn;
    const char *name;
    const char *unit;
    double      value;     /* numeric signals                               */
    const char *text;      /* ASCII signals (not NUL terminated), else NULL */
    size_t      text_len;
} bw_signal_t;

typedef void (*bw_signal_cb)(const bw_signal_t *sig, void *user);

/* Decodes every table signal for `pgn` found in data[0..len). Returns the
 * number of signals emitted. Signals that do not fit in `len`, or whose raw
 * value is an error/not-available code, are skipped. */
size_t bw_decode_payload(uint64_t ts_us, uint8_t sa, uint32_t pgn,
                         const uint8_t *data, size_t len,
                         bw_signal_cb cb, void *user);

/* ---- stream context with TP (BAM / RTS-CTS) reassembly ---------------- */
typedef struct {
    uint64_t frames;          /* frames fed                                 */
    uint64_t frames_malformed;/* dlc > 8 or invalid identifier              */
    uint64_t signals;         /* signals emitted                            */
    uint64_t tp_started;
    uint64_t tp_completed;
    uint64_t tp_timeouts;
    uint64_t tp_aborted;      /* peer abort, or session replaced/evicted    */
    uint64_t tp_errors;       /* bad size, sequence error, bad control      */
    uint64_t tp_orphans;      /* TP.DT without a session                    */
    uint64_t tp_duplicates;   /* repeated TP.DT ignored                     */
} bw_stats_t;

typedef struct {
    bool     active;
    bool     is_bam;
    uint8_t  sa, da;
    uint8_t  npackets;
    uint16_t next_seq;        /* 1..npackets+1                              */
    uint16_t total;           /* message size in bytes                      */
    uint32_t pgn;
    uint64_t last_ts_us;
    uint64_t started_order;
    uint8_t  buf[BW_MAX_TP_BYTES];
} bw_tp_session_t;

typedef struct {
    bw_tp_session_t sess[BW_TP_MAX_SESSIONS];
    uint64_t        order;
    bw_stats_t      stats;
} bw_ctx_t;

void   bw_ctx_init(bw_ctx_t *ctx);
/* Feeds one frame; returns the number of signals emitted. */
size_t bw_ctx_feed(bw_ctx_t *ctx, const bw_frame_t *f, bw_signal_cb cb, void *user);
/* Expires sessions idle for longer than BW_TP_TIMEOUT_US at time now_us. */
void   bw_ctx_expire(bw_ctx_t *ctx, uint64_t now_us);

/* ---- candump text parsing -------------------------------------------- */
enum {
    BW_PARSE_OK      = 0,
    BW_PARSE_SKIP    = 1,   /* blank line or comment                        */
    BW_PARSE_SYNTAX  = -1,
    BW_PARSE_STD_ID  = -2,  /* 11-bit identifier: not J1939                 */
    BW_PARSE_RTR     = -3,
    BW_PARSE_FD      = -4,
    BW_PARSE_RANGE   = -5
};

/* Parses one candump line (-L log format "(ts) can0 ID#DATA", or classic
 * "can0  ID  [n]  b0 b1 ..."). `line` need not be NUL terminated. */
int bw_candump_parse(const char *line, size_t len, bw_frame_t *out);

#ifdef __cplusplus
}
#endif
#endif
