/*
 * geni_master.c - portable GENIbus master.
 */
#include "geni_master.h"

#include "geni_units.h"

#include <string.h>

/* Max IDs in one INFO APDU: reply is up to 4 bytes per ID within 63. */
#define INFO_IDS_MAX (GENI_APDU_DATA_MAX / 4u)

void geni_master_init(geni_master_t *m, uint8_t own_addr,
                      geni_transact_fn transact, void *ctx)
{
    memset(m, 0, sizeof *m);
    m->own_addr = own_addr;
    m->transact = transact;
    m->ctx = ctx;
    m->pdu_max = GENI_PDU_MAX_DEFAULT;
}

/* Data bytes allowed in one APDU (request or reply), given pdu_max. */
static uint8_t apdu_data_max(const geni_master_t *m)
{
    uint8_t lim = GENI_APDU_DATA_MAX;
    if (m->pdu_max >= 3u && (uint8_t)(m->pdu_max - 2u) < lim)
        lim = (uint8_t)(m->pdu_max - 2u);
    return lim;
}

static geni_err_t count(geni_master_t *m, geni_err_t e)
{
    if ((unsigned)e < GENI_E_COUNT)
        m->stats.err[e]++;
    return e;
}

/* A reply already counted as OK turned out to be inconsistent. */
static geni_err_t recount(geni_master_t *m, geni_err_t e)
{
    if (m->stats.err[GENI_OK])
        m->stats.err[GENI_OK]--;
    if ((unsigned)e < GENI_E_COUNT)
        m->stats.err[e]++;
    return e;
}

/* ------------------------------------------------------------ transfer */

geni_err_t geni_transfer(geni_master_t *m, uint8_t da, geni_op_t *ops,
                         uint8_t n_ops)
{
    geni_builder_t b;
    uint16_t tx_len = 0, rx_len = 0;
    geni_err_t e;

    if (m == NULL || m->transact == NULL || (n_ops && ops == NULL) ||
        da == GENI_ADDR_BROADCAST)
        return GENI_E_BAD_ARG;

    geni_tx_begin(&b, m->tx, sizeof m->tx, GENI_SD_REQUEST, da, m->own_addr,
                  m->pdu_max);
    for (uint8_t i = 0; i < n_ops; i++)
        geni_tx_apdu(&b, ops[i].cls, ops[i].os, ops[i].req, ops[i].req_len);
    e = geni_tx_end(&b, &tx_len);
    if (e != GENI_OK)
        return e; /* caller error: not a bus event, not counted */

    m->stats.requests++;
    e = m->transact(m->ctx, m->tx, tx_len, m->rx, sizeof m->rx, &rx_len, 1);
    if (e != GENI_OK)
        return count(m, e);

    geni_frame_t f;
    e = geni_parse_frame(m->rx, rx_len, &f);
    if (e != GENI_OK)
        return count(m, e);
    if (f.sd != GENI_SD_REPLY)
        return count(m, GENI_E_BAD_SD);
    if (f.da != m->own_addr || (da != GENI_ADDR_CONNREQ && f.sa != da))
        return count(m, GENI_E_BAD_ADDR);

    /* Match reply APDUs to the request, in order. Validate fully first so
     * a malformed reply leaves no partial results. */
    uint16_t pos = 0;
    geni_apdu_t a;
    for (uint8_t i = 0; i < n_ops; i++) {
        if (geni_next_apdu(&f, &pos, &a) != 1 || a.cls != ops[i].cls)
            return count(m, GENI_E_BAD_APDU);
        if (a.ack == GENI_ACK_OK && a.len > ops[i].resp_cap)
            return count(m, GENI_E_OVERFLOW);
    }
    if (geni_next_apdu(&f, &pos, &a) != 0)
        return count(m, GENI_E_BAD_APDU);

    int nack = 0;
    pos = 0;
    for (uint8_t i = 0; i < n_ops; i++) {
        geni_next_apdu(&f, &pos, &a);
        ops[i].ack = a.ack;
        ops[i].resp_len = 0;
        if (a.ack != GENI_ACK_OK) {
            nack = 1;
            /* ACK 2: data holds the first unknown ID; keep it if room. */
            if (a.len && ops[i].resp_cap) {
                ops[i].resp[0] = a.data[0];
                ops[i].resp_len = 1;
            }
            continue;
        }
        if (a.len)
            memcpy(ops[i].resp, a.data, a.len);
        ops[i].resp_len = a.len;
    }
    m->last_sa = f.sa;
    return count(m, nack ? GENI_E_NACK : GENI_OK);
}

static geni_err_t one_op(geni_master_t *m, uint8_t addr, geni_op_t *op,
                         uint8_t *ack)
{
    geni_err_t e = geni_transfer(m, addr, op, 1);
    if (ack)
        *ack = (e == GENI_OK || e == GENI_E_NACK) ? op->ack : 0u;
    return e;
}

geni_err_t geni_get(geni_master_t *m, uint8_t addr, uint8_t cls,
                    const uint8_t *ids, uint8_t n, uint8_t *vals, uint8_t *ack)
{
    uint8_t tmp[GENI_APDU_DATA_MAX];
    if (n == 0 || n > apdu_data_max(m) || ids == NULL || vals == NULL)
        return GENI_E_BAD_ARG;
    geni_op_t op = { cls, GENI_OS_GET, ids, n, tmp, sizeof tmp, 0, 0 };
    geni_err_t e = one_op(m, addr, &op, ack);
    if (e == GENI_OK) {
        if (op.resp_len != n)
            return recount(m, GENI_E_BAD_APDU);
        memcpy(vals, tmp, n);
    }
    return e;
}

geni_err_t geni_info(geni_master_t *m, uint8_t addr, uint8_t cls,
                     const uint8_t *ids, uint8_t n, geni_info_t *infos,
                     uint8_t *ack)
{
    uint8_t tmp[GENI_APDU_DATA_MAX];
    if (n == 0 || n > INFO_IDS_MAX || ids == NULL || infos == NULL)
        return GENI_E_BAD_ARG;
    geni_op_t op = { cls, GENI_OS_INFO, ids, n, tmp, sizeof tmp, 0, 0 };
    geni_err_t e = one_op(m, addr, &op, ack);
    if (e != GENI_OK)
        return e;
    uint8_t pos = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t used = geni_info_decode(&tmp[pos], (uint8_t)(op.resp_len - pos),
                                        &infos[i]);
        if (used == 0)
            return recount(m, GENI_E_BAD_APDU);
        pos = (uint8_t)(pos + used);
    }
    if (pos != op.resp_len)
        return recount(m, GENI_E_BAD_APDU);
    return GENI_OK;
}

geni_err_t geni_cmd(geni_master_t *m, uint8_t addr, const uint8_t *ids,
                    uint8_t n, uint8_t *ack)
{
    if (n == 0 || n > apdu_data_max(m) || ids == NULL)
        return GENI_E_BAD_ARG;
    geni_op_t op = { GENI_CLASS_COMMAND, GENI_OS_SET, ids, n, NULL, 0, 0, 0 };
    return one_op(m, addr, &op, ack);
}

geni_err_t geni_set(geni_master_t *m, uint8_t addr, uint8_t cls,
                    const uint8_t *ids, const uint8_t *vals, uint8_t n,
                    uint8_t *ack)
{
    uint8_t req[GENI_APDU_DATA_MAX];
    if (n == 0 || (uint16_t)n * 2u > apdu_data_max(m) || ids == NULL ||
        vals == NULL)
        return GENI_E_BAD_ARG;
    for (uint8_t i = 0; i < n; i++) {
        req[2u * i] = ids[i];
        req[2u * i + 1u] = vals[i];
    }
    geni_op_t op = { cls, GENI_OS_SET, req, (uint8_t)(2u * n), NULL, 0, 0, 0 };
    return one_op(m, addr, &op, ack);
}

geni_err_t geni_get_string(geni_master_t *m, uint8_t addr, uint8_t id,
                           char *out, uint8_t cap, uint8_t *ack)
{
    uint8_t tmp[GENI_APDU_DATA_MAX];
    if (out == NULL || cap == 0)
        return GENI_E_BAD_ARG;
    out[0] = '\0';
    geni_op_t op = { GENI_CLASS_STRING, GENI_OS_GET, &id, 1, tmp, sizeof tmp, 0, 0 };
    geni_err_t e = one_op(m, addr, &op, ack);
    if (e != GENI_OK)
        return e;
    uint8_t n = 0;
    while (n < op.resp_len && tmp[n] != 0 && n + 1u < cap) {
        uint8_t c = tmp[n];
        out[n] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '?';
        n++;
    }
    out[n] = '\0';
    return GENI_OK;
}

/* ----------------------------------------------------------- discovery */

static void store_unit(geni_master_t *m, const geni_unit_t *u)
{
    for (uint8_t i = 0; i < m->n_units; i++) {
        if (m->units[i].addr == u->addr) {
            m->units[i] = *u;
            return;
        }
    }
    if (m->n_units < GENI_MAX_UNITS)
        m->units[m->n_units++] = *u;
}

geni_err_t geni_discover(geni_master_t *m, uint8_t silent_rounds,
                         uint16_t max_requests, geni_unit_cb cb, void *user,
                         uint8_t *found)
{
    static const uint8_t ids0[] = { 2, 3 };     /* df_buf_len, unit_bus_mode */
    static const uint8_t ids4[] = { 46, 47 };   /* unit_addr, group_addr     */
    static const uint8_t ids2[] = { 148, 149 }; /* unit_family, unit_type    */
    uint8_t r0[4], r4[4], r2[4];
    uint8_t silent = 0, nfound = 0;

    if (silent_rounds == 0)
        silent_rounds = 1;

    for (uint16_t req = 0; req < max_requests && silent < silent_rounds; req++) {
        geni_op_t ops[3] = {
            { GENI_CLASS_PROTOCOL, GENI_OS_GET, ids0, 2, r0, sizeof r0, 0, 0 },
            { GENI_CLASS_CONFIG,   GENI_OS_GET, ids4, 2, r4, sizeof r4, 0, 0 },
            { GENI_CLASS_MEASURED, GENI_OS_GET, ids2, 2, r2, sizeof r2, 0, 0 },
        };
        geni_err_t e = geni_transfer(m, GENI_ADDR_CONNREQ, ops, 3);
        if (e == GENI_E_TIMEOUT) {
            silent++;
            continue;
        }
        silent = 0;
        if (e != GENI_OK && e != GENI_E_NACK)
            continue; /* CRC/length/... : collision, try again */

        geni_unit_t u;
        memset(&u, 0, sizeof u);
        u.addr = m->last_sa;
        if (ops[0].ack == 0 && ops[0].resp_len >= 2) {
            u.df_buf_len = r0[0];
            u.bus_mode = r0[1];
        }
        if (ops[1].ack == 0 && ops[1].resp_len >= 2)
            u.group = r4[1];
        if (ops[2].ack == 0 && ops[2].resp_len >= 2) {
            u.family = r2[0];
            u.type = r2[1];
        }
        if (u.addr == m->own_addr || u.addr == GENI_ADDR_CONNREQ ||
            u.addr == GENI_ADDR_BROADCAST)
            continue;
        store_unit(m, &u);
        nfound++;
        if (cb)
            cb(user, &u);

        /* Address it directly once so it keeps quiet for the next 20 s. */
        uint8_t v;
        (void)geni_get(m, u.addr, GENI_CLASS_MEASURED, &ids2[0], 1, &v, NULL);
    }
    if (found)
        *found = nfound;
    return GENI_OK;
}

/* ---------------------------------------------------------- item table */

static int class_has_info(uint8_t cls)
{
    return cls == GENI_CLASS_MEASURED || cls == GENI_CLASS_COMMAND ||
           cls == GENI_CLASS_CONFIG || cls == GENI_CLASS_REFERENCE;
}

static int class_gettable(uint8_t cls)
{
    return cls == GENI_CLASS_PROTOCOL || cls == GENI_CLASS_MEASURED ||
           cls == GENI_CLASS_CONFIG || cls == GENI_CLASS_REFERENCE;
}

static void info_mark_nack(geni_item_state_t *s, uint8_t ack)
{
    s->info_state = GENI_INFO_NACK;
    s->info_ack = ack;
    if (ack == GENI_ACK_ID_UNKNOWN)
        s->unknown = 1;
}

geni_err_t geni_read_info(geni_master_t *m, uint8_t addr, geni_table_t *t)
{
    uint8_t ids[INFO_IDS_MAX], idx[INFO_IDS_MAX];
    geni_info_t infos[INFO_IDS_MAX];

    for (uint8_t i = 0; i < t->n; i++) {
        t->st[i].info_state = GENI_INFO_NONE;
        t->st[i].info_ack = 0;
        t->st[i].unknown = 0;
    }

    uint8_t i = 0;
    while (i < t->n) {
        uint8_t cls = t->defs[i].cls;
        if (!class_has_info(cls)) {
            i++;
            continue;
        }
        uint8_t n = 0;
        while (i < t->n && t->defs[i].cls == cls && n < INFO_IDS_MAX) {
            idx[n] = i;
            ids[n] = t->defs[i].id; /* scaling lives on the high byte */
            n++;
            i++;
        }

        uint8_t ack = 0;
        geni_err_t e = geni_info(m, addr, cls, ids, n, infos, &ack);
        if (e == GENI_OK) {
            for (uint8_t k = 0; k < n; k++) {
                t->st[idx[k]].info = infos[k];
                t->st[idx[k]].info_state = GENI_INFO_VALID;
            }
        } else if (e == GENI_E_NACK && ack == GENI_ACK_ID_UNKNOWN && n > 1) {
            for (uint8_t k = 0; k < n; k++) {
                geni_item_state_t *s = &t->st[idx[k]];
                e = geni_info(m, addr, cls, &ids[k], 1, &s->info, &ack);
                if (e == GENI_OK)
                    s->info_state = GENI_INFO_VALID;
                else if (e == GENI_E_NACK)
                    info_mark_nack(s, ack);
                else
                    return e;
            }
        } else if (e == GENI_E_NACK) {
            for (uint8_t k = 0; k < n; k++)
                info_mark_nack(&t->st[idx[k]], ack);
        } else {
            return e;
        }
    }
    return GENI_OK;
}

/* GET one item on its own; marks it OK or NACK. */
static geni_err_t poll_single(geni_master_t *m, uint8_t addr, geni_table_t *t,
                              uint8_t i)
{
    const geni_item_def_t *d = &t->defs[i];
    geni_item_state_t *s = &t->st[i];
    uint8_t ids[4], ack = 0;
    for (uint8_t b = 0; b < d->size; b++)
        ids[b] = (uint8_t)(d->id + b);
    geni_err_t e = geni_get(m, addr, d->cls, ids, d->size, s->raw, &ack);
    if (e == GENI_OK) {
        s->state = GENI_ITEM_OK;
    } else if (e == GENI_E_NACK) {
        s->state = GENI_ITEM_NACK;
        s->ack = ack;
        if (ack == GENI_ACK_ID_UNKNOWN)
            s->unknown = 1;
        e = GENI_OK;
    }
    return e;
}

geni_err_t geni_poll(geni_master_t *m, uint8_t addr, geni_table_t *t)
{
    uint8_t ids[GENI_APDU_DATA_MAX], vals[GENI_APDU_DATA_MAX];
    uint8_t idx[GENI_APDU_DATA_MAX];
    const uint8_t lim = apdu_data_max(m);

    for (uint8_t i = 0; i < t->n; i++) {
        geni_item_state_t *s = &t->st[i];
        s->state = GENI_ITEM_NONE;
        s->ack = 0;
        memset(s->raw, 0, sizeof s->raw);
        if (s->unknown) { /* known-bad ID: report, don't ask again */
            s->state = GENI_ITEM_NACK;
            s->ack = GENI_ACK_ID_UNKNOWN;
        }
    }

    uint8_t i = 0;
    while (i < t->n) {
        const geni_item_def_t *d = &t->defs[i];
        if (!class_gettable(d->cls) || t->st[i].unknown || d->size < 1u ||
            d->size > 4u) {
            i++;
            continue;
        }
        uint8_t cls = d->cls, nb = 0, ni = 0;
        while (i < t->n) {
            const geni_item_def_t *di = &t->defs[i];
            if (di->cls != cls)
                break;
            if (t->st[i].unknown || di->size < 1u || di->size > 4u) {
                i++;
                continue;
            }
            if ((uint16_t)nb + di->size > lim)
                break;
            for (uint8_t b = 0; b < di->size; b++)
                ids[nb++] = (uint8_t)(di->id + b);
            idx[ni++] = i;
            i++;
        }

        uint8_t ack = 0;
        geni_err_t e = geni_get(m, addr, cls, ids, nb, vals, &ack);
        if (e == GENI_OK) {
            uint8_t p = 0;
            for (uint8_t k = 0; k < ni; k++) {
                geni_item_state_t *s = &t->st[idx[k]];
                uint8_t sz = t->defs[idx[k]].size;
                memcpy(s->raw, &vals[p], sz);
                p = (uint8_t)(p + sz);
                s->state = GENI_ITEM_OK;
            }
        } else if (e == GENI_E_NACK && ack == GENI_ACK_ID_UNKNOWN && ni > 1) {
            for (uint8_t k = 0; k < ni; k++) {
                e = poll_single(m, addr, t, idx[k]);
                if (e != GENI_OK)
                    return e;
            }
        } else if (e == GENI_E_NACK) {
            for (uint8_t k = 0; k < ni; k++) {
                geni_item_state_t *s = &t->st[idx[k]];
                s->state = GENI_ITEM_NACK;
                s->ack = ack;
                if (ack == GENI_ACK_ID_UNKNOWN)
                    s->unknown = 1;
            }
        } else {
            return e;
        }
    }
    return GENI_OK;
}

geni_itemv_t geni_item_value(const geni_table_t *t, uint8_t idx, double *value,
                             const char **unit, uint32_t *raw, uint8_t *ack)
{
    if (idx >= t->n)
        return GENI_ITEMV_NONE;
    const geni_item_def_t *d = &t->defs[idx];
    const geni_item_state_t *s = &t->st[idx];

    if (s->state == GENI_ITEM_NACK) {
        if (ack)
            *ack = s->ack;
        return GENI_ITEMV_NACK;
    }
    if (s->state != GENI_ITEM_OK)
        return GENI_ITEMV_NONE;
    if (raw)
        *raw = geni_raw_to_u32(s->raw, d->size);
    if (s->info_state != GENI_INFO_VALID)
        return GENI_ITEMV_RAW;

    double v = 0.0;
    switch (geni_scale(&s->info, s->raw, d->size, &v)) {
    case GENI_VAL_OK: {
        const geni_unit_info_t *u =
            geni_unit_lookup(geni_info_unit_index(&s->info));
        if (value)
            *value = v;
        if (unit)
            *unit = u ? u->unit : "";
        return GENI_ITEMV_SCALED;
    }
    case GENI_VAL_NA:
        return GENI_ITEMV_NA;
    default:
        return GENI_ITEMV_RAW;
    }
}

geni_err_t geni_set_value(geni_master_t *m, uint8_t addr, geni_table_t *t,
                          uint8_t idx, double value, uint8_t *raw_out,
                          uint8_t *ack)
{
    if (idx >= t->n)
        return GENI_E_BAD_ARG;
    const geni_item_def_t *d = &t->defs[idx];
    geni_item_state_t *s = &t->st[idx];
    if ((d->cls != GENI_CLASS_CONFIG && d->cls != GENI_CLASS_REFERENCE) ||
        s->info_state != GENI_INFO_VALID || d->size < 1u || d->size > 4u)
        return GENI_E_BAD_ARG;

    uint8_t raw[4], ids[4];
    geni_err_t e = geni_unscale(&s->info, value, d->size, raw);
    if (e != GENI_OK)
        return e;
    for (uint8_t b = 0; b < d->size; b++)
        ids[b] = (uint8_t)(d->id + b);
    if (raw_out)
        memcpy(raw_out, raw, d->size);
    return geni_set(m, addr, d->cls, ids, raw, d->size, ack);
}

int geni_table_find(const geni_table_t *t, const char *name)
{
    for (uint8_t i = 0; i < t->n; i++)
        if (t->defs[i].name && strcmp(t->defs[i].name, name) == 0)
            return i;
    return -1;
}
