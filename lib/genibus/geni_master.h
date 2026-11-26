/*
 * geni_master.h - portable GENIbus master: transfers, discovery,
 * INFO/scaling cache, batched polling and writes.
 *
 * The hardware is reached through a single transact() callback, so the
 * same code runs on the STM32 and against a simulated slave in host tests.
 */
#ifndef GENI_MASTER_H
#define GENI_MASTER_H

#include "genibus.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GENI_MAX_UNITS
#define GENI_MAX_UNITS 32u
#endif
#ifndef GENI_PDU_MAX_DEFAULT
#define GENI_PDU_MAX_DEFAULT 64u
#endif

/*
 * Send tx[0..tx_len-1]. If expect_reply, receive one telegram into rx and
 * set *rx_len. Must return GENI_OK, TIMEOUT, TRUNCATED, UART or OVERFLOW.
 * Frame validation (length, CRC, SD, addresses) is done by the master.
 */
typedef geni_err_t (*geni_transact_fn)(void *ctx, const uint8_t *tx,
                                       uint16_t tx_len, uint8_t *rx,
                                       uint16_t rx_cap, uint16_t *rx_len,
                                       int expect_reply);

typedef struct {
    uint8_t addr;
    uint8_t group;
    uint8_t family;
    uint8_t type;
    uint8_t df_buf_len;
    uint8_t bus_mode;
} geni_unit_t;

typedef struct {
    uint32_t requests;
    uint32_t err[GENI_E_COUNT]; /* err[GENI_OK] = good replies */
} geni_stats_t;

typedef struct {
    geni_transact_fn transact;
    void            *ctx;
    uint8_t          own_addr;
    uint8_t          pdu_max;  /* max request/reply PDU bytes */
    uint8_t          last_sa;  /* SA of the last valid reply  */
    geni_stats_t     stats;
    geni_unit_t      units[GENI_MAX_UNITS];
    uint8_t          n_units;
    uint8_t          tx[GENI_TELEGRAM_MAX];
    uint8_t          rx[GENI_TELEGRAM_MAX];
} geni_master_t;

void geni_master_init(geni_master_t *m, uint8_t own_addr,
                      geni_transact_fn transact, void *ctx);

/* ---- Generic multi-APDU transfer ---- */
typedef struct {
    uint8_t        cls;
    uint8_t        os;       /* GENI_OS_GET / SET / INFO             */
    const uint8_t *req;
    uint8_t        req_len;
    uint8_t       *resp;     /* may be NULL if resp_cap == 0         */
    uint8_t        resp_cap;
    uint8_t        resp_len; /* out                                  */
    uint8_t        ack;      /* out                                  */
} geni_op_t;

/*
 * One Data Request with n_ops APDUs to `da` (0xFE = Connection Request,
 * any SA accepted). Reply APDUs are matched to ops in order.
 * Returns GENI_E_NACK if the transfer worked but any op has ack != 0.
 */
geni_err_t geni_transfer(geni_master_t *m, uint8_t da, geni_op_t *ops,
                         uint8_t n_ops);

/* ---- Single-APDU helpers (ack may be NULL) ---- */
geni_err_t geni_get(geni_master_t *m, uint8_t addr, uint8_t cls,
                    const uint8_t *ids, uint8_t n, uint8_t *vals, uint8_t *ack);
geni_err_t geni_info(geni_master_t *m, uint8_t addr, uint8_t cls,
                     const uint8_t *ids, uint8_t n, geni_info_t *infos,
                     uint8_t *ack);
geni_err_t geni_cmd(geni_master_t *m, uint8_t addr, const uint8_t *ids,
                    uint8_t n, uint8_t *ack);
geni_err_t geni_set(geni_master_t *m, uint8_t addr, uint8_t cls,
                    const uint8_t *ids, const uint8_t *vals, uint8_t n,
                    uint8_t *ack);
geni_err_t geni_get_string(geni_master_t *m, uint8_t addr, uint8_t id,
                           char *out, uint8_t cap, uint8_t *ack);

/* ---- Discovery (B.8) ---- */
typedef void (*geni_unit_cb)(void *user, const geni_unit_t *u);

/*
 * Connection Request loop. Stops after `silent_rounds` consecutive
 * requests without any reply, or after `max_requests`. Each unit found is
 * addressed directly once so it stays silent for 20 s. Units are stored
 * in m->units (updated in place if already known); cb (may be NULL) is
 * called for every reply. *found (may be NULL) = replies received.
 */
geni_err_t geni_discover(geni_master_t *m, uint8_t silent_rounds,
                         uint16_t max_requests, geni_unit_cb cb, void *user,
                         uint8_t *found);

/* ---- Item table: INFO cache + polling ---- */
typedef struct {
    const char *name;
    uint8_t     cls;
    uint8_t     id;   /* high-byte ID; bytes use id, id+1, ... */
    uint8_t     size; /* 1..4 */
} geni_item_def_t;

enum { GENI_INFO_NONE = 0, GENI_INFO_VALID, GENI_INFO_NACK };
enum { GENI_ITEM_NONE = 0, GENI_ITEM_OK, GENI_ITEM_NACK };

typedef struct {
    geni_info_t info;
    uint8_t     info_state; /* GENI_INFO_*                      */
    uint8_t     info_ack;
    uint8_t     unknown;    /* device said "ID unknown": skip   */
    uint8_t     state;      /* GENI_ITEM_*, reset on every poll */
    uint8_t     ack;
    uint8_t     raw[4];
} geni_item_state_t;

typedef struct {
    const geni_item_def_t *defs;
    geni_item_state_t     *st;
    uint8_t                n;
} geni_table_t;

/* Read INFO for every item (batched by class, per-item retry on NACK). */
geni_err_t geni_read_info(geni_master_t *m, uint8_t addr, geni_table_t *t);
/*
 * GET every item. All item states are cleared first, so a failed batch
 * never leaves stale values behind. Returns the first transport error.
 */
geni_err_t geni_poll(geni_master_t *m, uint8_t addr, geni_table_t *t);

typedef enum {
    GENI_ITEMV_NONE = 0, /* no data this poll (error/skipped)        */
    GENI_ITEMV_SCALED,   /* *value valid, *unit is the unit name     */
    GENI_ITEMV_RAW,      /* *raw valid (no scaling known)            */
    GENI_ITEMV_NA,       /* device: "not available"                  */
    GENI_ITEMV_NACK      /* device rejected the item (*ack)          */
} geni_itemv_t;

geni_itemv_t geni_item_value(const geni_table_t *t, uint8_t idx, double *value,
                             const char **unit, uint32_t *raw, uint8_t *ack);

/* SET a class 4/5 table item in physical units (needs cached INFO). */
geni_err_t geni_set_value(geni_master_t *m, uint8_t addr, geni_table_t *t,
                          uint8_t idx, double value, uint8_t *raw_out,
                          uint8_t *ack);

int geni_table_find(const geni_table_t *t, const char *name);

#ifdef __cplusplus
}
#endif
#endif
