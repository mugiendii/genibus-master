/*
 * Host unit tests for the portable GENIbus library.
 * Reproduces every telegram and scaling example from the spec (Part D)
 * bit for bit, and exercises the master against simulated slaves.
 *
 *   make -C test/host          run
 *   make -C test/host asan     run under ASan/UBSan
 */
#include "geni_master.h"
#include "geni_units.h"
#include "genibus.h"
#include "mp204.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static int bytes_eq(const uint8_t *a, uint16_t na, const uint8_t *b, uint16_t nb)
{
    if (na != nb || memcmp(a, b, na) != 0) {
        printf("  got:");
        for (uint16_t i = 0; i < na; i++) printf(" %02X", a[i]);
        printf("\n  exp:");
        for (uint16_t i = 0; i < nb; i++) printf(" %02X", b[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------ spec vectors */

static const uint8_t V1[] = { 0x27, 0x0E, 0xFE, 0x01, 0x00, 0x02, 0x02, 0x03,
                              0x04, 0x02, 0x2E, 0x2F, 0x02, 0x02, 0x94, 0x95,
                              0xA2, 0xAA };
static const uint8_t V2[] = { 0x24, 0x0E, 0x01, 0x20, 0x00, 0x02, 0x46, 0x0E,
                              0x04, 0x02, 0x20, 0xF7, 0x02, 0x02, 0x03, 0x01,
                              0x00, 0x04 };
static const uint8_t V3[] = { 0x27, 0x07, 0x20, 0x01, 0x02, 0xC3, 0x02, 0x10,
                              0x1A, 0x90, 0x1C };
static const uint8_t V4[] = { 0x24, 0x10, 0x01, 0x20, 0x02, 0x0C, 0x82, 0x3E,
                              0x00, 0x39, 0x82, 0x15, 0x00, 0x64, 0x82, 0x09,
                              0x00, 0xFA, 0x91, 0x0A };
static const uint8_t V5[] = { 0x27, 0x0F, 0x20, 0x01, 0x02, 0x04, 0x02, 0x10,
                              0x1A, 0x1B, 0x04, 0x02, 0x04, 0x05, 0x03, 0x81,
                              0x06, 0x80, 0x2A };
static const uint8_t V6[] = { 0x24, 0x0E, 0x01, 0x20, 0x02, 0x04, 0x7A, 0x42,
                              0x39, 0x80, 0x04, 0x02, 0xB5, 0xC8, 0x03, 0x00,
                              0xF2, 0xD7 };

/* Canned transport: records the request, returns queued replies. */
typedef struct {
    const uint8_t *replies[8];
    uint16_t       reply_len[8];
    int            n, next;
    uint8_t        last_tx[GENI_TELEGRAM_MAX];
    uint16_t       last_tx_len;
    uint8_t        first_tx[GENI_TELEGRAM_MAX];
    uint16_t       first_tx_len;
    int            calls;
} canned_t;

static geni_err_t canned_transact(void *ctx, const uint8_t *tx, uint16_t tx_len,
                                  uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                  int expect_reply)
{
    canned_t *c = ctx;
    memcpy(c->last_tx, tx, tx_len);
    c->last_tx_len = tx_len;
    if (c->calls++ == 0) {
        memcpy(c->first_tx, tx, tx_len);
        c->first_tx_len = tx_len;
    }
    if (!expect_reply)
        return GENI_OK;
    if (c->next >= c->n)
        return GENI_E_TIMEOUT;
    uint16_t n = c->reply_len[c->next];
    if (n > rx_cap)
        return GENI_E_OVERFLOW;
    memcpy(rx, c->replies[c->next], n);
    *rx_len = n;
    c->next++;
    return GENI_OK;
}

static void canned_push(canned_t *c, const uint8_t *r, uint16_t n)
{
    c->replies[c->n] = r;
    c->reply_len[c->n] = n;
    c->n++;
}

static geni_master_t g_m; /* big buffers: keep off the stack */

static void test_crc_vectors(void)
{
    const uint8_t *v[] = { V1, V2, V3, V4, V5, V6 };
    const size_t n[] = { sizeof V1, sizeof V2, sizeof V3, sizeof V4, sizeof V5,
                         sizeof V6 };
    for (int i = 0; i < 6; i++) {
        uint16_t crc = geni_crc(&v[i][1], n[i] - 3);
        CHECK(crc == (uint16_t)((v[i][n[i] - 2] << 8) | v[i][n[i] - 1]));
        geni_frame_t f;
        CHECK(geni_parse_frame(v[i], (uint16_t)n[i], &f) == GENI_OK);
        CHECK(f.le + 4u == n[i]);
    }
    /* known value for the CRC algorithm: "123456789" CCITT-FALSE ^ 0xFFFF */
    CHECK(geni_crc((const uint8_t *)"123456789", 9) == (uint16_t)~0x29B1u);
}

static void test_v1_v2_discovery(void)
{
    canned_t c;
    memset(&c, 0, sizeof c);
    canned_push(&c, V2, sizeof V2);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    uint8_t found = 0;
    CHECK(geni_discover(&g_m, 3, 20, NULL, NULL, &found) == GENI_OK);
    CHECK(bytes_eq(c.first_tx, c.first_tx_len, V1, sizeof V1));
    CHECK(found == 1);
    CHECK(g_m.n_units == 1);
    CHECK(g_m.units[0].addr == 0x20);
    CHECK(g_m.units[0].group == 0xF7);
    CHECK(g_m.units[0].family == 3);
    CHECK(g_m.units[0].type == 1);
    CHECK(g_m.units[0].df_buf_len == 0x46);
    CHECK(g_m.units[0].bus_mode == 0x0E);
    /* 1 conn req (reply), 1 direct GET (silence), 3 silent conn reqs */
    CHECK(c.calls == 5);
}

static void test_v3_v4_info(geni_info_t infos[3])
{
    canned_t c;
    memset(&c, 0, sizeof c);
    canned_push(&c, V4, sizeof V4);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    const uint8_t ids[] = { 2, 16, 26 };
    uint8_t ack = 9;
    CHECK(geni_info(&g_m, 0x20, GENI_CLASS_MEASURED, ids, 3, infos, &ack) == GENI_OK);
    CHECK(ack == 0);
    CHECK(bytes_eq(c.last_tx, c.last_tx_len, V3, sizeof V3));
    CHECK(infos[0].head == 0x82 && infos[0].sif == 2 && infos[0].vi == 0 &&
          infos[0].bo == 0);
    CHECK(infos[0].unit == 0x3E && infos[0].zero == 0 && infos[0].range == 0x39);
    CHECK(infos[1].unit == 0x15 && infos[1].range == 0x64);
    CHECK(infos[2].unit == 0x09 && infos[2].range == 0xFA);
}

static void test_v5_v6_transfer(const geni_info_t infos[3])
{
    canned_t c;
    memset(&c, 0, sizeof c);
    canned_push(&c, V6, sizeof V6);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    const uint8_t ids2[] = { 2, 16, 26, 27 }, ids4[] = { 4, 5 }, ids3[] = { 6 };
    uint8_t r2[8], r4[8];
    geni_op_t ops[3] = {
        { GENI_CLASS_MEASURED, GENI_OS_GET, ids2, 4, r2, sizeof r2, 0, 0 },
        { GENI_CLASS_CONFIG,   GENI_OS_GET, ids4, 2, r4, sizeof r4, 0, 0 },
        { GENI_CLASS_COMMAND,  GENI_OS_SET, ids3, 1, NULL, 0, 0, 0 },
    };
    CHECK(geni_transfer(&g_m, 0x20, ops, 3) == GENI_OK);
    CHECK(bytes_eq(c.last_tx, c.last_tx_len, V5, sizeof V5));
    CHECK(ops[0].resp_len == 4 && r2[0] == 0x7A && r2[1] == 0x42 &&
          r2[2] == 0x39 && r2[3] == 0x80);
    CHECK(ops[1].resp_len == 2 && r4[0] == 0xB5 && r4[1] == 0xC8);
    CHECK(ops[2].resp_len == 0 && ops[2].ack == 0);

    /* Combining #4 + #6 */
    double v;
    CHECK(geni_scale(&infos[0], &r2[0], 1, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 13.689, 0.0005);
    CHECK(geni_scale(&infos[1], &r2[1], 1, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 25.984, 0.0005);
    CHECK(geni_scale(&infos[2], &r2[2], 2, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 5659.45, 0.005);
    CHECK(strcmp(geni_unit_lookup(geni_info_unit_index(&infos[0]))->unit, "A") == 0);
    CHECK(strcmp(geni_unit_lookup(geni_info_unit_index(&infos[1]))->unit, "degC") == 0);
    CHECK(strcmp(geni_unit_lookup(geni_info_unit_index(&infos[2]))->unit, "W") == 0);
}

static void test_builder_direct(void)
{
    /* Rebuild every request with the raw builder too. */
    uint8_t buf[80];
    geni_builder_t b;
    uint16_t len = 0;

    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REQUEST, 0xFE, 0x01, 64);
    geni_tx_apdu(&b, 0, GENI_OS_GET, (const uint8_t[]){ 2, 3 }, 2);
    geni_tx_apdu(&b, 4, GENI_OS_GET, (const uint8_t[]){ 46, 47 }, 2);
    geni_tx_apdu(&b, 2, GENI_OS_GET, (const uint8_t[]){ 148, 149 }, 2);
    CHECK(geni_tx_end(&b, &len) == GENI_OK);
    CHECK(bytes_eq(buf, len, V1, sizeof V1));

    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REQUEST, 0x20, 0x01, 64);
    geni_tx_apdu(&b, 2, GENI_OS_INFO, (const uint8_t[]){ 2, 16, 26 }, 3);
    CHECK(geni_tx_end(&b, &len) == GENI_OK);
    CHECK(bytes_eq(buf, len, V3, sizeof V3));

    /* Build the replies as a "slave" would, too. */
    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REPLY, 0x01, 0x20, 64);
    geni_tx_apdu(&b, 0, GENI_ACK_OK, (const uint8_t[]){ 0x46, 0x0E }, 2);
    geni_tx_apdu(&b, 4, GENI_ACK_OK, (const uint8_t[]){ 0x20, 0xF7 }, 2);
    geni_tx_apdu(&b, 2, GENI_ACK_OK, (const uint8_t[]){ 3, 1 }, 2);
    CHECK(geni_tx_end(&b, &len) == GENI_OK);
    CHECK(bytes_eq(buf, len, V2, sizeof V2));

    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REPLY, 0x01, 0x20, 64);
    geni_tx_apdu(&b, 2, GENI_ACK_OK, (const uint8_t[]){ 0x7A, 0x42, 0x39, 0x80 }, 4);
    geni_tx_apdu(&b, 4, GENI_ACK_OK, (const uint8_t[]){ 0xB5, 0xC8 }, 2);
    geni_tx_apdu(&b, 3, GENI_ACK_OK, NULL, 0);
    CHECK(geni_tx_end(&b, &len) == GENI_OK);
    CHECK(bytes_eq(buf, len, V6, sizeof V6));

    /* Limits */
    uint8_t big[64] = { 0 };
    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REQUEST, 0x20, 0x01, 64);
    CHECK(geni_tx_apdu(&b, 2, GENI_OS_GET, big, 64) == GENI_E_BAD_ARG);
    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REQUEST, 0x20, 0x01, 64);
    CHECK(geni_tx_apdu(&b, 2, GENI_OS_GET, big, 62) == GENI_OK);
    CHECK(geni_tx_apdu(&b, 2, GENI_OS_GET, big, 1) == GENI_E_OVERFLOW);
    CHECK(geni_tx_end(&b, &len) == GENI_E_OVERFLOW); /* sticky */
}

static void test_parser(void)
{
    geni_frame_t f;
    uint8_t t[sizeof V6];
    memcpy(t, V6, sizeof t);
    CHECK(geni_parse_frame(t, sizeof t - 1, &f) == GENI_E_BAD_LENGTH);
    CHECK(geni_parse_frame(t, 3, &f) == GENI_E_BAD_LENGTH);
    t[6] ^= 0x01;
    CHECK(geni_parse_frame(t, sizeof t, &f) == GENI_E_CRC);

    /* APDU walk on V6 */
    CHECK(geni_parse_frame(V6, sizeof V6, &f) == GENI_OK);
    CHECK(f.sd == 0x24 && f.da == 0x01 && f.sa == 0x20 && !f.has_rfs);
    uint16_t pos = 0;
    geni_apdu_t a;
    CHECK(geni_next_apdu(&f, &pos, &a) == 1 && a.cls == 2 && a.len == 4);
    CHECK(geni_next_apdu(&f, &pos, &a) == 1 && a.cls == 4 && a.len == 2);
    CHECK(geni_next_apdu(&f, &pos, &a) == 1 && a.cls == 3 && a.len == 0);
    CHECK(geni_next_apdu(&f, &pos, &a) == 0);

    /* trailing RFS byte */
    uint8_t buf[32];
    geni_builder_t b;
    uint16_t len;
    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REPLY, 1, 0x20, 0);
    geni_tx_apdu(&b, 2, 0, (const uint8_t[]){ 5 }, 1);
    geni_tx_end(&b, &len);
    /* insert RFS before CRC and redo LE/CRC by hand */
    buf[len - 2] = 0x77;
    len = (uint16_t)(len - 1);
    buf[1] = (uint8_t)(buf[1] + 1);
    uint16_t crc = geni_crc(&buf[1], (size_t)(len - 1));
    buf[len++] = (uint8_t)(crc >> 8);
    buf[len++] = (uint8_t)crc;
    CHECK(geni_parse_frame(buf, len, &f) == GENI_OK);
    CHECK(f.has_rfs && f.rfs == 0x77 && f.pdu_len == 3);

    /* APDU length runs past the PDU */
    geni_tx_begin(&b, buf, sizeof buf, GENI_SD_REPLY, 1, 0x20, 0);
    geni_tx_apdu(&b, 2, 0, (const uint8_t[]){ 5, 6 }, 2);
    geni_tx_end(&b, &len);
    buf[5] = 0x05; /* claims 5 bytes, has 2 */
    crc = geni_crc(&buf[1], (size_t)(len - 3));
    buf[len - 2] = (uint8_t)(crc >> 8);
    buf[len - 1] = (uint8_t)crc;
    CHECK(geni_parse_frame(buf, len, &f) == GENI_E_BAD_APDU);
}

static void test_scaling_examples(void)
{
    geni_info_t i;
    double v;
    uint8_t p[4];

    /* 8-bit std (UPE t_m): head 0x82, UNIT 21, ZERO 10, RANGE 90, raw 163 */
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 21, 10, 90 }, 4, &i) == 4);
    CHECK(geni_scale(&i, (const uint8_t[]){ 163 }, 1, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 67.76, 0.005);
    CHECK(lround(v) == 68);

    /* 16-bit std (CU3 power): 0x82, UNIT 44, ZERO 0, RANGE 120, raw 16,214 */
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 44, 0, 120 }, 4, &i) == 4);
    CHECK(geni_scale(&i, (const uint8_t[]){ 16, 214 }, 2, &v) == GENI_VAL_OK);
    CHECK_NEAR(v / 1000.0, 7.954, 0.0005); /* kW */

    /* 8-bit ext (MP 204 t_mo): 0x83, UNIT 21, ZERO 0/0, raw 87 */
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 21, 0, 0 }, 4, &i) == 4);
    CHECK(i.sif == 3);
    CHECK(geni_scale(&i, (const uint8_t[]){ 87 }, 1, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 87.0, 1e-9);

    /* 16-bit ext (SEE level): 0x83, UNIT 179 (neg, idx 51), ZERO 3/245 */
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 179, 3, 245 }, 4, &i) == 4);
    CHECK(geni_info_unit_index(&i) == 51 && geni_info_zero_negative(&i));
    CHECK(geni_scale(&i, (const uint8_t[]){ 18, 12 }, 2, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 3.607, 1e-9); /* bar = 3607 mbar */

    /* 24-bit ext (UPE power-on time): 0x83, UNIT 36, raw 7,108,32 */
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 36, 0, 0 }, 4, &i) == 4);
    CHECK(geni_scale(&i, (const uint8_t[]){ 7, 108, 32 }, 3, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 972864.0, 1e-6); /* min */

    /* 32-bit ext (DME dosing flow): 0x83, UNIT 69, raw 23,216,42,214 */
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 69, 0, 0 }, 4, &i) == 4);
    CHECK(geni_raw_to_u32((const uint8_t[]){ 23, 216, 42, 214 }, 4) == 400042710u);
    CHECK(geni_scale(&i, (const uint8_t[]){ 23, 216, 42, 214 }, 4, &v) == GENI_VAL_OK);
    CHECK_NEAR(v, 40004271.0, 1e-3); /* ml/h */

    /* Not available / raw */
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 21, 10, 90 }, 4, &i) == 4);
    CHECK(geni_scale(&i, (const uint8_t[]){ 255 }, 1, &v) == GENI_VAL_NA);
    CHECK(geni_info_decode((const uint8_t[]){ 0xA2, 21, 10, 90 }, 4, &i) == 4);
    CHECK(i.vi == 1);
    CHECK(geni_scale(&i, (const uint8_t[]){ 255 }, 1, &v) == GENI_VAL_OK);
    CHECK(geni_info_decode((const uint8_t[]){ 0x81 }, 1, &i) == 1);
    CHECK(i.sif == 1);
    CHECK(geni_scale(&i, (const uint8_t[]){ 5 }, 1, &v) == GENI_VAL_RAW);
    CHECK(geni_info_decode((const uint8_t[]){ 0x90 }, 1, &i) == 1);
    CHECK(i.bo == 1 && i.sif == 0);
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 1 }, 2, &i) == 0);
    CHECK(geni_info_decode((const uint8_t[]){ 0x42 }, 1, &i) == 0);
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 0x7F, 0, 1 }, 4, &i) == 4);
    CHECK(geni_scale(&i, (const uint8_t[]){ 1 }, 1, &v) == GENI_VAL_RAW); /* idx 127 */

    /* Inverse scaling */
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 21, 10, 90 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 67.76, 1, p) == GENI_OK && p[0] == 163);
    CHECK(geni_unscale(&i, 1000.0, 1, p) == GENI_OK && p[0] == 254);
    CHECK(geni_unscale(&i, -50.0, 1, p) == GENI_OK && p[0] == 0);
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 44, 0, 120 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 7954.0, 2, p) == GENI_OK && p[0] == 16 && p[1] == 214);
    CHECK(geni_unscale(&i, 1e9, 2, p) == GENI_OK && p[0] == 0xFE && p[1] == 0x00);
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 179, 3, 245 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 3.607, 2, p) == GENI_OK && p[0] == 18 && p[1] == 12);
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 36, 0, 0 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 972864.0, 3, p) == GENI_OK && p[0] == 7 && p[1] == 108 &&
          p[2] == 32);
    CHECK(geni_unscale(&i, 1e12, 3, p) == GENI_OK && p[0] == 0xFE && p[1] == 0xFF &&
          p[2] == 0xFF); /* VI = 0: hi byte <= 254 */
    CHECK(geni_info_decode((const uint8_t[]){ 0xA3, 36, 0, 0 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 1e12, 3, p) == GENI_OK && p[0] == 0xFF && p[1] == 0xFF &&
          p[2] == 0xFF); /* VI = 1: full range */
    CHECK(geni_info_decode((const uint8_t[]){ 0x83, 69, 0, 0 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 40004271.0, 4, p) == GENI_OK && p[0] == 23 && p[1] == 216 &&
          p[2] == 42 && p[3] == 214);
    CHECK(geni_unscale(&i, NAN, 1, p) == GENI_E_BAD_ARG);
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 21, 0, 0 }, 4, &i) == 4);
    CHECK(geni_unscale(&i, 1.0, 1, p) == GENI_E_BAD_ARG); /* RANGE 0 */

    /* Round trip over the whole 8-bit standard range */
    CHECK(geni_info_decode((const uint8_t[]){ 0x82, 0x80 | 27, 50, 200 }, 4, &i) == 4);
    int rt_ok = 1;
    for (int x = 0; x <= 254; x++) {
        uint8_t r = (uint8_t)x;
        geni_scale(&i, &r, 1, &v);
        if (geni_unscale(&i, v, 1, p) != GENI_OK || p[0] != r)
            rt_ok = 0;
    }
    CHECK(rt_ok);
}

static void test_units(void)
{
    int all = 1;
    for (int k = 1; k <= 95; k++)
        if (geni_unit_lookup((uint8_t)k) == NULL)
            all = 0;
    CHECK(all);
    CHECK(geni_unit_lookup(0) == NULL);
    CHECK(geni_unit_lookup(96) == NULL);
    CHECK(geni_unit_lookup(27)->factor == 0.01 &&
          strcmp(geni_unit_lookup(27)->unit, "bar") == 0);
    CHECK(geni_unit_lookup(9)->factor == 100.0);
    CHECK(geni_unit_lookup(15)->factor == 1e-6);
}

static void test_fmt(void)
{
    char b[32];
    geni_fmt_double(b, sizeof b, 13.6890, 3);   CHECK(strcmp(b, "13.689") == 0);
    geni_fmt_double(b, sizeof b, 5659.4488, 3); CHECK(strcmp(b, "5659.449") == 0);
    geni_fmt_double(b, sizeof b, 87.0, 3);      CHECK(strcmp(b, "87") == 0);
    geni_fmt_double(b, sizeof b, -3.5, 3);      CHECK(strcmp(b, "-3.5") == 0);
    geni_fmt_double(b, sizeof b, -0.0001, 3);   CHECK(strcmp(b, "0") == 0);
    geni_fmt_double(b, sizeof b, 0.05, 3);      CHECK(strcmp(b, "0.05") == 0);
    geni_fmt_double(b, sizeof b, 40004271.0, 3); CHECK(strcmp(b, "40004271") == 0);
    geni_fmt_double(b, sizeof b, NAN, 3);       CHECK(strcmp(b, "null") == 0);
    geni_fmt_double(b, 4, 12345.0, 0);          CHECK(strcmp(b, "123") == 0);
}

/* ----------------------------------------------------- simulated bus */

typedef struct {
    uint8_t  addr, group, family, type;
    uint8_t  known[8][256];
    uint8_t  val[8][256];
    uint8_t  info[8][256][4];
    uint8_t  info_len[8][256];
    char     str[256][32];
    int64_t  last_direct_ms;
    uint8_t  cmds[16];
    int      n_cmds;
} sim_slave_t;

typedef struct {
    sim_slave_t *s[4];
    int          n;
    int64_t      now_ms;
    int          collide_next; /* force N collisions on Connection Request */
    int          dead;         /* all slaves silent                        */
    int          requests;
    int          gets_by_class[8];
} sim_bus_t;

static void sim_known(sim_slave_t *s, uint8_t cls, uint8_t id, uint8_t v)
{
    s->known[cls][id] = 1;
    s->val[cls][id] = v;
}

static void sim_info(sim_slave_t *s, uint8_t cls, uint8_t id, uint8_t head,
                     uint8_t unit, uint8_t zero, uint8_t range)
{
    s->known[cls][id] = 1;
    s->info[cls][id][0] = head;
    s->info[cls][id][1] = unit;
    s->info[cls][id][2] = zero;
    s->info[cls][id][3] = range;
    s->info_len[cls][id] = (head & 3) >= 2 ? 4 : 1;
}

static uint16_t sim_reply(sim_bus_t *bus, sim_slave_t *s, const geni_frame_t *rq,
                          uint8_t *out, uint16_t cap)
{
    geni_builder_t b;
    uint16_t pos = 0, len = 0;
    geni_apdu_t a;
    geni_tx_begin(&b, out, cap, GENI_SD_REPLY, rq->sa, s->addr, 0);
    while (geni_next_apdu(rq, &pos, &a) == 1) {
        uint8_t d[64], n = 0, ack = 0;
        uint8_t c = a.cls;
        int class_ok = c == 0 || c == 2 || c == 3 || c == 4 || c == 5 || c == 7;
        if (!class_ok) {
            ack = GENI_ACK_CLASS_UNKNOWN;
        } else if (a.ack == GENI_OS_GET) {
            bus->gets_by_class[c]++;
            if (c == 7) {
                const char *str = s->str[a.data[0]];
                n = (uint8_t)(strlen(str) + 1);
                memcpy(d, str, n);
            } else {
                for (uint8_t k = 0; k < a.len; k++) {
                    if (!s->known[c][a.data[k]]) {
                        ack = GENI_ACK_ID_UNKNOWN;
                        d[0] = a.data[k];
                        n = 1;
                        break;
                    }
                    d[n++] = s->val[c][a.data[k]];
                }
            }
        } else if (a.ack == GENI_OS_INFO) {
            for (uint8_t k = 0; k < a.len; k++) {
                uint8_t id = a.data[k];
                if (!s->known[c][id] || !s->info_len[c][id]) {
                    ack = GENI_ACK_ID_UNKNOWN;
                    d[0] = id;
                    n = 1;
                    break;
                }
                memcpy(&d[n], s->info[c][id], s->info_len[c][id]);
                n = (uint8_t)(n + s->info_len[c][id]);
            }
        } else if (a.ack == GENI_OS_SET) {
            if (c == 3) {
                for (uint8_t k = 0; k < a.len && s->n_cmds < 16; k++)
                    s->cmds[s->n_cmds++] = a.data[k];
            } else if (c == 4 || c == 5) {
                for (uint8_t k = 0; k + 1 < a.len; k += 2)
                    s->val[c][a.data[k]] = a.data[k + 1];
            } else {
                ack = GENI_ACK_ILLEGAL;
            }
        }
        if (ack == GENI_ACK_CLASS_UNKNOWN || ack == GENI_ACK_ILLEGAL)
            n = 0;
        geni_tx_apdu(&b, c, ack, d, n);
    }
    geni_tx_end(&b, &len);
    return len;
}

static geni_err_t sim_transact(void *ctx, const uint8_t *tx, uint16_t tx_len,
                               uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                               int expect_reply)
{
    sim_bus_t *bus = ctx;
    geni_frame_t rq;
    bus->now_ms += 70;
    bus->requests++;
    if (geni_parse_frame(tx, tx_len, &rq) != GENI_OK || tx[0] != GENI_SD_REQUEST)
        return GENI_E_TIMEOUT;
    if (!expect_reply)
        return GENI_OK;
    if (bus->dead)
        return GENI_E_TIMEOUT;

    if (rq.da == GENI_ADDR_CONNREQ) {
        sim_slave_t *resp[4];
        int nr = 0;
        for (int i = 0; i < bus->n; i++)
            if (bus->now_ms - bus->s[i]->last_direct_ms >= 20000)
                resp[nr++] = bus->s[i];
        if (nr == 0)
            return GENI_E_TIMEOUT;
        *rx_len = sim_reply(bus, resp[0], &rq, rx, rx_cap);
        if (nr > 1 || bus->collide_next > 0) {
            if (bus->collide_next > 0)
                bus->collide_next--;
            rx[6] ^= 0x5A; /* overlapping replies: garbage */
        }
        return GENI_OK;
    }
    for (int i = 0; i < bus->n; i++) {
        sim_slave_t *s = bus->s[i];
        if (s->addr == rq.da) {
            s->last_direct_ms = bus->now_ms;
            *rx_len = sim_reply(bus, s, &rq, rx, rx_cap);
            return GENI_OK;
        }
    }
    return GENI_E_TIMEOUT;
}

static sim_slave_t g_s1, g_s2;
static sim_bus_t g_bus;

static void sim_setup(void)
{
    memset(&g_s1, 0, sizeof g_s1);
    memset(&g_s2, 0, sizeof g_s2);
    memset(&g_bus, 0, sizeof g_bus);
    g_s1.addr = 0x20; g_s1.group = 0xF7; g_s1.family = 3; g_s1.type = 1;
    g_s2.addr = 0x21; g_s2.group = 0xF7; g_s2.family = 7; g_s2.type = 2;
    g_s1.last_direct_ms = g_s2.last_direct_ms = -100000;
    sim_slave_t *ss[2] = { &g_s1, &g_s2 };
    for (int i = 0; i < 2; i++) {
        sim_slave_t *s = ss[i];
        sim_known(s, 0, 2, 0x46);
        sim_known(s, 0, 3, 0x0E);
        sim_known(s, 4, 46, s->addr);
        sim_known(s, 4, 47, s->group);
        sim_known(s, 2, 148, s->family);
        sim_known(s, 2, 149, s->type);
        sim_info(s, 2, 148, 0x80, 0, 0, 0);
        sim_info(s, 2, 149, 0x80, 0, 0, 0);
    }
    /* s1 = CU3-like from vectors #4/#6 */
    sim_info(&g_s1, 2, 2, 0x82, 0x3E, 0, 0x39);
    sim_info(&g_s1, 2, 16, 0x82, 0x15, 0, 0x64);
    sim_info(&g_s1, 2, 26, 0x82, 0x09, 0, 0xFA);
    sim_info(&g_s1, 2, 27, 0x90, 0, 0, 0);
    sim_known(&g_s1, 2, 2, 0x7A);
    sim_known(&g_s1, 2, 16, 0x42);
    sim_known(&g_s1, 2, 26, 0x39);
    sim_known(&g_s1, 2, 27, 0x80);
    /* class 5 setpoint, 8-bit std 0..100 % */
    sim_info(&g_s1, 5, 1, 0x82, 30, 0, 100);
    sim_known(&g_s1, 5, 1, 127);
    /* class 4 16-bit ext, 0.01 bar */
    sim_info(&g_s1, 4, 10, 0x83, 27, 0, 0);
    sim_info(&g_s1, 4, 11, 0x90, 0, 0, 0);
    sim_known(&g_s1, 4, 10, 0);
    sim_known(&g_s1, 4, 11, 0);
    strcpy(g_s1.str[1], "CU3 SIM");
    g_bus.s[0] = &g_s1;
    g_bus.s[1] = &g_s2;
    g_bus.n = 2;
}

static void test_sim_discovery(void)
{
    sim_setup();
    g_bus.collide_next = 2;
    geni_master_init(&g_m, 0x01, sim_transact, &g_bus);
    uint8_t found = 0;
    CHECK(geni_discover(&g_m, 3, 50, NULL, NULL, &found) == GENI_OK);
    /* both slaves unsilenced at start -> collisions; the spec's algorithm
     * needs the slaves' random delay to separate them, which the sim
     * models by giving the first responder the bus once only one remains.
     * Here two always collide, so nothing is found... */
    CHECK(g_m.stats.err[GENI_E_CRC] > 0);

    /* ...make s2 recently-addressed so only s1 answers, then s2 later. */
    sim_setup();
    g_bus.collide_next = 2;
    g_s2.last_direct_ms = 0;
    geni_master_init(&g_m, 0x01, sim_transact, &g_bus);
    CHECK(geni_discover(&g_m, 3, 50, NULL, NULL, &found) == GENI_OK);
    CHECK(found == 1 && g_m.n_units == 1 && g_m.units[0].addr == 0x20);
    CHECK(g_m.units[0].family == 3 && g_m.units[0].type == 1);
    CHECK(g_m.stats.err[GENI_E_CRC] == 2);
    /* 20 s later s2 has been quiet; s1 is still silenced only if polled. */
    g_bus.now_ms += 20000;
    g_s1.last_direct_ms = g_bus.now_ms;
    CHECK(geni_discover(&g_m, 3, 50, NULL, NULL, &found) == GENI_OK);
    CHECK(found == 1 && g_m.n_units == 2 && g_m.units[1].addr == 0x21);
    CHECK(g_m.units[1].family == 7);
    /* re-discovery of a known unit updates in place */
    g_bus.now_ms += 20000;
    g_s2.last_direct_ms = g_bus.now_ms;
    CHECK(geni_discover(&g_m, 3, 50, NULL, NULL, &found) == GENI_OK);
    CHECK(found == 1 && g_m.n_units == 2);

    /* empty bus */
    g_bus.dead = 1;
    int before = g_bus.requests;
    CHECK(geni_discover(&g_m, 3, 50, NULL, NULL, &found) == GENI_OK);
    CHECK(found == 0 && g_bus.requests - before == 3);
}

static const geni_item_def_t k_items[] = {
    { "i_rst",  GENI_CLASS_MEASURED, 2, 1 },
    { "t_mo",   GENI_CLASS_MEASURED, 16, 1 },
    { "bogus",  GENI_CLASS_MEASURED, 99, 1 }, /* unknown to the slave */
    { "p",      GENI_CLASS_MEASURED, 26, 2 },
    { "family", GENI_CLASS_MEASURED, 148, 1 },
    { "ref",    GENI_CLASS_REFERENCE, 1, 1 },
    { "pmax",   GENI_CLASS_CONFIG, 10, 2 },
};
#define N_ITEMS ((uint8_t)(sizeof k_items / sizeof k_items[0]))
static geni_item_state_t g_st[N_ITEMS];

static void test_sim_poll(void)
{
    sim_setup();
    geni_master_init(&g_m, 0x01, sim_transact, &g_bus);
    geni_table_t t = { k_items, g_st, N_ITEMS };
    memset(g_st, 0, sizeof g_st);

    CHECK(geni_read_info(&g_m, 0x20, &t) == GENI_OK);
    CHECK(g_st[0].info_state == GENI_INFO_VALID && g_st[0].info.unit == 0x3E);
    CHECK(g_st[2].info_state == GENI_INFO_NACK && g_st[2].unknown);
    CHECK(g_st[3].info_state == GENI_INFO_VALID && g_st[3].info.range == 0xFA);
    CHECK(g_st[4].info_state == GENI_INFO_VALID && g_st[4].info.sif == 0);
    CHECK(g_st[5].info_state == GENI_INFO_VALID);
    CHECK(g_st[6].info_state == GENI_INFO_VALID && g_st[6].info.sif == 3);

    g_bus.gets_by_class[2] = 0;
    CHECK(geni_poll(&g_m, 0x20, &t) == GENI_OK);
    /* the bogus item is skipped up front: class 2 goes in ONE telegram */
    CHECK(g_bus.gets_by_class[2] == 1);

    double v;
    const char *u;
    uint32_t raw;
    uint8_t ack;
    CHECK(geni_item_value(&t, 0, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    CHECK_NEAR(v, 13.689, 0.0005);
    CHECK(strcmp(u, "A") == 0);
    CHECK(geni_item_value(&t, 1, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    CHECK_NEAR(v, 25.984, 0.0005);
    CHECK(geni_item_value(&t, 2, &v, &u, &raw, &ack) == GENI_ITEMV_NACK && ack == 2);
    CHECK(geni_item_value(&t, 3, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    CHECK_NEAR(v, 5659.45, 0.005);
    CHECK(geni_item_value(&t, 4, &v, &u, &raw, &ack) == GENI_ITEMV_RAW && raw == 3);
    CHECK(geni_item_value(&t, 5, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    CHECK_NEAR(v, 50.0, 0.0001);

    /* Batch rejection on an ID the master did not know was bad:
     * the slave forgets ID 16 -> batch NACK -> per-item retry. */
    g_s1.known[2][16] = 0;
    g_bus.gets_by_class[2] = 0;
    CHECK(geni_poll(&g_m, 0x20, &t) == GENI_OK);
    CHECK(g_bus.gets_by_class[2] == 1 + 4); /* batch + 4 singles */
    CHECK(geni_item_value(&t, 0, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    CHECK(geni_item_value(&t, 1, &v, &u, &raw, &ack) == GENI_ITEMV_NACK && ack == 2);
    CHECK(g_st[1].unknown);
    CHECK(geni_item_value(&t, 3, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    g_bus.gets_by_class[2] = 0;
    CHECK(geni_poll(&g_m, 0x20, &t) == GENI_OK);
    CHECK(g_bus.gets_by_class[2] == 1);
    g_s1.known[2][16] = 1;

    /* Not available */
    g_s1.val[2][2] = 0xFF;
    CHECK(geni_poll(&g_m, 0x20, &t) == GENI_OK);
    CHECK(geni_item_value(&t, 0, &v, &u, &raw, &ack) == GENI_ITEMV_NA);
    g_s1.val[2][2] = 0x7A;

    /* Timeout: no stale values */
    g_bus.dead = 1;
    CHECK(geni_poll(&g_m, 0x20, &t) == GENI_E_TIMEOUT);
    int none = 1;
    for (uint8_t k = 0; k < N_ITEMS; k++) {
        geni_itemv_t r = geni_item_value(&t, k, &v, &u, &raw, &ack);
        if (r != GENI_ITEMV_NONE && r != GENI_ITEMV_NACK)
            none = 0;
    }
    CHECK(none);
    CHECK(g_m.stats.err[GENI_E_TIMEOUT] == 1);
    g_bus.dead = 0;

    /* Wrong unit address -> timeout too */
    CHECK(geni_poll(&g_m, 0x55, &t) == GENI_E_TIMEOUT);

    /* SET in physical units */
    uint8_t rawb[4];
    CHECK(geni_set_value(&g_m, 0x20, &t, 5, 75.0, rawb, &ack) == GENI_OK);
    CHECK(rawb[0] == 191 && g_s1.val[5][1] == 191);
    CHECK(geni_set_value(&g_m, 0x20, &t, 6, 4.56, rawb, &ack) == GENI_OK);
    CHECK(g_s1.val[4][10] == 0x01 && g_s1.val[4][11] == 0xC8); /* 456 */
    CHECK(geni_poll(&g_m, 0x20, &t) == GENI_OK);
    CHECK(geni_item_value(&t, 6, &v, &u, &raw, &ack) == GENI_ITEMV_SCALED);
    CHECK_NEAR(v, 4.56, 1e-9);
    CHECK(strcmp(u, "bar") == 0);
    CHECK(geni_set_value(&g_m, 0x20, &t, 0, 1.0, rawb, &ack) == GENI_E_BAD_ARG);

    /* raw SET, command, string */
    CHECK(geni_set(&g_m, 0x20, 5, (const uint8_t[]){ 1 }, (const uint8_t[]){ 9 }, 1,
                   &ack) == GENI_OK);
    CHECK(g_s1.val[5][1] == 9);
    CHECK(geni_cmd(&g_m, 0x20, (const uint8_t[]){ 6 }, 1, &ack) == GENI_OK);
    CHECK(g_s1.n_cmds == 1 && g_s1.cmds[0] == 6);
    char str[32];
    CHECK(geni_get_string(&g_m, 0x20, 1, str, sizeof str, &ack) == GENI_OK);
    CHECK(strcmp(str, "CU3 SIM") == 0);
    CHECK(geni_get(&g_m, 0x20, 1, (const uint8_t[]){ 1 }, 1, rawb, &ack) == GENI_E_NACK);
    CHECK(ack == GENI_ACK_CLASS_UNKNOWN);
    CHECK(geni_set(&g_m, 0x20, 2, (const uint8_t[]){ 1 }, (const uint8_t[]){ 9 }, 1,
                   &ack) == GENI_E_NACK && ack == GENI_ACK_ILLEGAL);
}

static void test_reply_validation(void)
{
    canned_t c;
    uint8_t vals[4];

    /* SA mismatch */
    memset(&c, 0, sizeof c);
    canned_push(&c, V6, sizeof V6);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    CHECK(geni_get(&g_m, 0x21, 2, (const uint8_t[]){ 2, 16, 26, 27 }, 4, vals, NULL) ==
          GENI_E_BAD_ADDR);
    /* DA mismatch (own address 2) */
    memset(&c, 0, sizeof c);
    canned_push(&c, V6, sizeof V6);
    geni_master_init(&g_m, 0x02, canned_transact, &c);
    CHECK(geni_get(&g_m, 0x20, 2, (const uint8_t[]){ 2, 16, 26, 27 }, 4, vals, NULL) ==
          GENI_E_BAD_ADDR);
    /* SD of a request echoed back */
    memset(&c, 0, sizeof c);
    canned_push(&c, V5, sizeof V5);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    CHECK(geni_get(&g_m, 0x20, 2, (const uint8_t[]){ 2 }, 1, vals, NULL) == GENI_E_BAD_SD);
    /* APDU count/class mismatch: V6 has 3 APDUs, we asked for 1 */
    memset(&c, 0, sizeof c);
    canned_push(&c, V6, sizeof V6);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    CHECK(geni_get(&g_m, 0x20, 2, (const uint8_t[]){ 2, 16, 26, 27 }, 4, vals, NULL) ==
          GENI_E_BAD_APDU);
    /* GET reply length != number of IDs */
    memset(&c, 0, sizeof c);
    canned_push(&c, V4, sizeof V4);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    uint8_t v12[12];
    CHECK(geni_get(&g_m, 0x20, 2, (const uint8_t[]){ 2, 16, 26 }, 3, v12, NULL) ==
          GENI_E_OVERFLOW || g_m.stats.err[GENI_E_BAD_APDU] == 1);
    /* broadcast needs no reply: rejected for transfers */
    CHECK(geni_transfer(&g_m, 0xFF, NULL, 0) == GENI_E_BAD_ARG);
    CHECK(strcmp(geni_err_str(GENI_E_CRC), "crc") == 0);
    CHECK(strcmp(geni_err_str(GENI_E_BAD_ARG), "bad_arg") == 0);
}

/* ------------------------------------------------------------- MP 204 */

/* MP 204 behind the simulated bus: executes class 3 per profile figure 5. */
static struct {
    int     inert;        /* acknowledges but does nothing               */
    int     lose_reply;   /* executes the command, reply is lost         */
    int     nack;         /* refuses commands                            */
    int     alarm;        /* alarm condition present: reset -> Tripped 1 */
    int     die_after;    /* bus goes silent after the command           */
    int     n_cmd;        /* class 3 telegrams on the wire               */
    uint8_t last;
    int     slow;         /* readbacks before the state changes          */
    uint8_t want1;
    int     delays;
} g_mp;

static void mp_delay(uint32_t ms) { (void)ms; g_mp.delays++; }

static void mp_set(uint8_t m1)
{
    g_s1.val[2][95] = m1;
}

static geni_err_t mp_transact(void *ctx, const uint8_t *tx, uint16_t tx_len,
                              uint8_t *rx, uint16_t cap, uint16_t *rx_len, int er)
{
    geni_frame_t rq;
    geni_apdu_t a;
    uint16_t pos = 0;
    CHECK(geni_parse_frame(tx, tx_len, &rq) == GENI_OK);
    CHECK(geni_next_apdu(&rq, &pos, &a) == 1);
    if (a.cls != 3) {
        if (g_mp.slow && g_mp.n_cmd && --g_mp.slow == 0)
            mp_set(g_mp.want1);
        return sim_transact(ctx, tx, tx_len, rx, cap, rx_len, er);
    }
    g_mp.n_cmd++;
    CHECK(a.ack == GENI_OS_SET && a.len == 1);
    g_mp.last = a.data[0];
    if (g_mp.nack) {
        geni_builder_t b;
        geni_tx_begin(&b, rx, cap, GENI_SD_REPLY, rq.sa, rq.da, 0);
        geni_tx_apdu(&b, 3, GENI_ACK_ILLEGAL, NULL, 0);
        return geni_tx_end(&b, rx_len);
    }
    if (!g_mp.inert) {
        uint8_t to = g_s1.val[2][95];
        if (a.data[0] == 95) to = 0x0B;
        else if (a.data[0] == 81) to = 0x2A;
        else if (a.data[0] == 2) to = g_mp.alarm ? 0x29 : 0x10;
        if (g_mp.slow) g_mp.want1 = to;
        else mp_set(to);
    }
    if (g_mp.die_after) g_bus.dead = 1;
    if (g_mp.lose_reply || g_mp.die_after) return GENI_E_TIMEOUT;
    return sim_transact(ctx, tx, tx_len, rx, cap, rx_len, er);
}

static void mp_setup(uint8_t m1)
{
    sim_setup();
    memset(&g_mp, 0, sizeof g_mp);
    sim_known(&g_s1, 2, 148, 7);
    sim_known(&g_s1, 2, 149, 1);
    sim_known(&g_s1, 2, 95, m1);
    sim_known(&g_s1, 2, 96, 0x05);
    sim_known(&g_s1, 2, 97, 0x22);
    sim_known(&g_s1, 2, 144, 0);
    geni_master_init(&g_m, 0x01, mp_transact, &g_bus);
}

static void test_mp204(void)
{
    mp204_report_t r;

    /* Profile figure 12.3, both telegrams bit for bit */
    static const uint8_t TX[] = { 0x27, 0x05, 0x20, 0x01, 0x03, 0x81, 0x5F, 0x16, 0x0D };
    static const uint8_t RX[] = { 0x24, 0x04, 0x01, 0x20, 0x03, 0x00, 0xC2, 0xD4 };
    canned_t c;
    uint8_t id = 95, ack = 9;
    memset(&c, 0, sizeof c);
    canned_push(&c, RX, sizeof RX);
    geni_master_init(&g_m, 0x01, canned_transact, &c);
    CHECK(geni_cmd(&g_m, 0x20, &id, 1, &ack) == GENI_OK && ack == 0);
    CHECK(bytes_eq(g_m.tx, sizeof TX, TX, sizeof TX));

    /* Normal operation: right ID, one telegram, read back */
    mp_setup(0x10);
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, mp_delay, &r) == MP204_OK);
    CHECK(g_mp.n_cmd == 1 && g_mp.last == 95 && r.sent && g_mp.delays == 1);
    CHECK(mp204_mode(&r.before) == MP204_STARTED && mp204_relay1(&r.before));
    CHECK(mp204_mode(&r.after) == MP204_TRIPPED3 && !mp204_relay1(&r.after));
    CHECK(mp204_motor_off(&r.after) && !mp204_relay2(&r.after));
    CHECK(mp204_auto_restart(&r.after) && !mp204_power_on_stop(&r.after));
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, mp_delay, &r) == MP204_OK);
    CHECK(g_mp.n_cmd == 2 && g_mp.last == 2 && mp204_relay1(&r.after));
    CHECK(mp204_command(&g_m, 0x20, MP204_TRIP, NULL, &r) == MP204_OK);
    CHECK(g_mp.n_cmd == 3 && g_mp.last == 81 && mp204_relay2(&r.after));
    /* Motor stopped by an external contactor is still "started" */
    mp_setup(0x18);
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_OK);

    /* Refused: nothing may reach the wire */
    for (uint8_t m1 = 0x29; m1 <= 0x2B; m1++) {
        mp_setup(m1);
        CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_NOT_STARTED);
        CHECK(mp204_command(&g_m, 0x20, MP204_TRIP, NULL, &r) == MP204_NOT_STARTED);
        CHECK(g_mp.n_cmd == 0 && !r.sent);
        CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_OK);
    }
    mp_setup(0x10);
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_ALREADY_STARTED);
    CHECK(g_mp.n_cmd == 0);
    mp_setup(0xA9);
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_UNPROTECTED);
    CHECK(g_mp.n_cmd == 0);
    mp_setup(0x90); /* stopping an unprotected motor is fine */
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_OK);
    static const uint8_t bad2[] = { 0x00, 0x02, 0x03, 0xFF };
    for (unsigned i = 0; i < sizeof bad2; i++) {
        mp_setup(0x10);
        g_s1.val[2][96] = bad2[i];
        CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_NOT_OPERATING);
        CHECK(g_mp.n_cmd == 0);
    }
    mp_setup(0xFF);
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_NOT_OPERATING);
    mp_setup(0x14); /* undefined mode */
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_NOT_OPERATING);
    CHECK(g_mp.n_cmd == 0);
    mp_setup(0x10);
    g_s1.val[2][149] = 2;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_NOT_MP204);
    mp_setup(0x10);
    g_s1.val[2][148] = 3;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_NOT_MP204);
    CHECK(g_mp.n_cmd == 0);
    mp_setup(0x10);
    g_bus.dead = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_BUS);
    CHECK(g_mp.n_cmd == 0 && !r.have_before && r.err == GENI_E_TIMEOUT);
    mp_setup(0x10);
    CHECK(mp204_command(&g_m, 0x20, (mp204_action_t)7, NULL, &r) == MP204_BUS);
    CHECK(g_mp.n_cmd == 0);

    /* An acknowledge is not proof */
    mp_setup(0x10);
    g_mp.inert = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, mp_delay, &r) == MP204_UNCONFIRMED);
    CHECK(r.err == GENI_OK && g_mp.n_cmd == 1 && g_mp.delays == 10);
    /* Mode changed but the relay is still closed */
    mp_setup(0x10);
    g_mp.inert = 1; g_mp.slow = 1; g_mp.want1 = 0x13;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_UNCONFIRMED);
    /* Relay takes a few readbacks */
    mp_setup(0x10);
    g_mp.slow = 4;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, mp_delay, &r) == MP204_OK);
    CHECK(g_mp.delays == 4);
    /* Reply lost: never resent, the readback decides */
    mp_setup(0x10);
    g_mp.lose_reply = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_OK);
    CHECK(g_mp.n_cmd == 1 && r.err == GENI_E_TIMEOUT);
    mp_setup(0x10);
    g_mp.die_after = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_UNCONFIRMED);
    CHECK(g_mp.n_cmd == 1 && !r.have_after);
    mp_setup(0x10);
    g_mp.nack = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_STOP, NULL, &r) == MP204_REJECTED);
    CHECK(r.ack == GENI_ACK_ILLEGAL && g_mp.n_cmd == 1);
    /* Reset with the alarm still there */
    mp_setup(0x29);
    g_mp.alarm = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_STILL_TRIPPED);
    mp_setup(0x2A);
    g_mp.alarm = 1;
    CHECK(mp204_command(&g_m, 0x20, MP204_RESET, NULL, &r) == MP204_STILL_TRIPPED);
    CHECK(g_mp.n_cmd == 1);

    CHECK(strcmp(mp204_alarm_str(56), "underload") == 0);
    CHECK(strcmp(mp204_alarm_str(255), "unknown") == 0);
    CHECK(strcmp(mp204_result_str(MP204_UNCONFIRMED), "unconfirmed") == 0);
}

int main(void)
{
    geni_info_t infos[3];
    test_crc_vectors();
    test_builder_direct();
    test_parser();
    test_v1_v2_discovery();
    test_v3_v4_info(infos);
    test_v5_v6_transfer(infos);
    test_scaling_examples();
    test_units();
    test_fmt();
    test_sim_discovery();
    test_sim_poll();
    test_reply_validation();
    test_mp204();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
