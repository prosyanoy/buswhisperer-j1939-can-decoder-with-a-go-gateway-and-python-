#include "bw_j1939.h"

int bw_id_parse(uint32_t can_id, bw_id_t *out)
{
    if (out == NULL || (can_id & ~0x1FFFFFFFu) != 0)
        return -1;
    out->priority = (uint8_t)((can_id >> 26) & 0x7u);
    out->edp      = (uint8_t)((can_id >> 25) & 0x1u);
    out->dp       = (uint8_t)((can_id >> 24) & 0x1u);
    out->pf       = (uint8_t)((can_id >> 16) & 0xFFu);
    out->ps       = (uint8_t)((can_id >> 8) & 0xFFu);
    out->sa       = (uint8_t)(can_id & 0xFFu);
    out->is_pdu1  = out->pf < 240u;
    out->da       = out->is_pdu1 ? out->ps : 0xFFu;
    out->pgn      = ((uint32_t)out->edp << 17) | ((uint32_t)out->dp << 16) |
                    ((uint32_t)out->pf << 8) | (out->is_pdu1 ? 0u : out->ps);
    return 0;
}

uint32_t bw_id_make(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t da)
{
    uint32_t pf = (pgn >> 8) & 0xFFu;
    uint32_t ps = (pf < 240u) ? da : (pgn & 0xFFu);
    return ((uint32_t)(priority & 7u) << 26) | (((pgn >> 16) & 3u) << 24) |
           (pf << 16) | (ps << 8) | sa;
}
