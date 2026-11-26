/*
 * genibus.c - portable GENIbus protocol core.
 */
#include "genibus.h"

#include "geni_units.h"

#include <math.h>
#include <string.h>

const char *geni_err_str(geni_err_t e)
{
    switch (e) {
    case GENI_OK:           return "ok";
    case GENI_E_TIMEOUT:    return "timeout";
    case GENI_E_TRUNCATED:  return "truncated";
    case GENI_E_UART:       return "uart";
    case GENI_E_CRC:        return "crc";
    case GENI_E_BAD_LENGTH: return "bad_length";
    case GENI_E_BAD_SD:     return "bad_sd";
    case GENI_E_BAD_ADDR:   return "bad_addr";
    case GENI_E_BAD_APDU:   return "bad_apdu";
    case GENI_E_NACK:       return "nack";
    case GENI_E_OVERFLOW:   return "overflow";
    case GENI_E_BAD_ARG:    return "bad_arg";
    default:                return "unknown";
    }
}

/* ------------------------------------------------------------------ CRC */

uint16_t geni_crc_update(uint16_t acc, uint8_t b)
{
    acc ^= (uint16_t)((uint16_t)b << 8);
    for (int i = 0; i < 8; i++)
        acc = (acc & 0x8000u) ? (uint16_t)((acc << 1) ^ 0x1021u)
                              : (uint16_t)(acc << 1);
    return acc;
}

uint16_t geni_crc(const uint8_t *data, size_t len)
{
    uint16_t acc = 0xFFFFu;
    for (size_t i = 0; i < len; i++)
        acc = geni_crc_update(acc, data[i]);
    return (uint16_t)~acc;
}

/* -------------------------------------------------------------- builder */

void geni_tx_begin(geni_builder_t *b, uint8_t *buf, uint16_t cap,
                   uint8_t sd, uint8_t da, uint8_t sa, uint8_t pdu_max)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->pdu_max = pdu_max;
    b->err = GENI_OK;
    if (buf == NULL || cap < 6u) {
        b->err = GENI_E_BAD_ARG;
        return;
    }
    buf[0] = sd;
    buf[1] = 0; /* LE, filled in by geni_tx_end */
    buf[2] = da;
    buf[3] = sa;
    b->len = 4;
}

uint16_t geni_tx_pdu_len(const geni_builder_t *b)
{
    return b->len >= 4u ? (uint16_t)(b->len - 4u) : 0u;
}

geni_err_t geni_tx_apdu(geni_builder_t *b, uint8_t cls, uint8_t os,
                        const uint8_t *data, uint8_t n)
{
    if (b->err != GENI_OK)
        return b->err;
    if (cls > 15u || os > 3u || n > GENI_APDU_DATA_MAX || (n && data == NULL)) {
        b->err = GENI_E_BAD_ARG;
        return b->err;
    }
    uint16_t pdu_after = (uint16_t)(geni_tx_pdu_len(b) + 2u + n);
    /* LE = DA + SA + PDU must fit a byte; total also needs 2 CRC bytes. */
    if ((b->pdu_max && pdu_after > b->pdu_max) || pdu_after + 2u > 255u ||
        (uint32_t)b->len + 2u + n + 2u > b->cap) {
        b->err = GENI_E_OVERFLOW;
        return b->err;
    }
    b->buf[b->len++] = (uint8_t)(cls & 0x0Fu);
    b->buf[b->len++] = (uint8_t)((os << 6) | n);
    if (n) {
        memcpy(&b->buf[b->len], data, n);
        b->len = (uint16_t)(b->len + n);
    }
    return GENI_OK;
}

geni_err_t geni_tx_end(geni_builder_t *b, uint16_t *out_len)
{
    if (b->err != GENI_OK)
        return b->err;
    if ((uint32_t)b->len + 2u > b->cap) {
        b->err = GENI_E_OVERFLOW;
        return b->err;
    }
    b->buf[1] = (uint8_t)(b->len - 2u);
    uint16_t crc = geni_crc(&b->buf[1], (size_t)(b->len - 1u));
    b->buf[b->len++] = (uint8_t)(crc >> 8);
    b->buf[b->len++] = (uint8_t)(crc & 0xFFu);
    if (out_len)
        *out_len = b->len;
    return GENI_OK;
}

/* --------------------------------------------------------------- parser */

geni_err_t geni_parse_frame(const uint8_t *buf, uint16_t len, geni_frame_t *f)
{
    if (buf == NULL || f == NULL)
        return GENI_E_BAD_ARG;
    if (len < 6u)
        return GENI_E_BAD_LENGTH;
    uint8_t le = buf[1];
    if (le < 2u || (uint16_t)(le + 4u) != len)
        return GENI_E_BAD_LENGTH;
    uint16_t crc = geni_crc(&buf[1], (size_t)(len - 3u));
    uint16_t rx_crc = (uint16_t)(((uint16_t)buf[len - 2u] << 8) | buf[len - 1u]);
    if (crc != rx_crc)
        return GENI_E_CRC;

    f->sd = buf[0];
    f->le = le;
    f->da = buf[2];
    f->sa = buf[3];
    f->pdu = &buf[4];
    f->pdu_len = (uint8_t)(le - 2u);
    f->has_rfs = 0;
    f->rfs = 0;

    /* Walk the APDUs once: a lone trailing byte is RFS. */
    uint16_t pos = 0;
    while (pos < f->pdu_len) {
        uint16_t remain = (uint16_t)(f->pdu_len - pos);
        if (remain == 1u) {
            f->has_rfs = 1;
            f->rfs = f->pdu[pos];
            f->pdu_len = (uint8_t)(f->pdu_len - 1u);
            break;
        }
        uint8_t alen = (uint8_t)(f->pdu[pos + 1u] & 0x3Fu);
        if ((uint16_t)(2u + alen) > remain)
            return GENI_E_BAD_APDU;
        pos = (uint16_t)(pos + 2u + alen);
    }
    return GENI_OK;
}

int geni_next_apdu(const geni_frame_t *f, uint16_t *pos, geni_apdu_t *a)
{
    if (*pos >= f->pdu_len)
        return 0;
    if ((uint16_t)(f->pdu_len - *pos) < 2u)
        return -1;
    const uint8_t *p = &f->pdu[*pos];
    uint8_t alen = (uint8_t)(p[1] & 0x3Fu);
    if ((uint16_t)(*pos + 2u + alen) > f->pdu_len)
        return -1;
    a->cls = (uint8_t)(p[0] & 0x0Fu);
    a->ack = (uint8_t)(p[1] >> 6);
    a->len = alen;
    a->data = &p[2];
    *pos = (uint16_t)(*pos + 2u + alen);
    return 1;
}

/* -------------------------------------------------------------- scaling */

uint8_t geni_info_decode(const uint8_t *p, uint8_t avail, geni_info_t *out)
{
    if (p == NULL || out == NULL || avail < 1u)
        return 0;
    uint8_t head = p[0];
    if ((head & 0xC0u) != 0x80u)
        return 0;
    memset(out, 0, sizeof *out);
    out->head = head;
    out->vi = (uint8_t)((head >> 5) & 1u);
    out->bo = (uint8_t)((head >> 4) & 1u);
    out->sif = (uint8_t)(head & 3u);
    if (out->sif < GENI_SIF_STANDARD)
        return 1;
    if (avail < 4u)
        return 0;
    out->unit = p[1];
    out->zero = p[2];
    out->range = p[3];
    return 4;
}

uint8_t geni_info_unit_index(const geni_info_t *i) { return (uint8_t)(i->unit & 0x7Fu); }
int geni_info_zero_negative(const geni_info_t *i) { return (i->unit & 0x80u) != 0; }

uint32_t geni_raw_to_u32(const uint8_t *raw, uint8_t n)
{
    uint32_t v = 0;
    for (uint8_t k = 0; k < n && k < 4u; k++)
        v = (v << 8) | raw[k];
    return v;
}

static double pow256(uint8_t e)
{
    double r = 1.0;
    while (e--)
        r *= 256.0;
    return r;
}

geni_val_t geni_scale(const geni_info_t *info, const uint8_t *raw, uint8_t n,
                      double *out)
{
    if (info == NULL || raw == NULL || out == NULL || n < 1u || n > 4u)
        return GENI_VAL_BAD;
    if (!info->vi && raw[0] == 0xFFu)
        return GENI_VAL_NA;
    if (info->sif < GENI_SIF_STANDARD)
        return GENI_VAL_RAW;
    const geni_unit_info_t *u = geni_unit_lookup(geni_info_unit_index(info));
    if (u == NULL)
        return GENI_VAL_RAW;

    double x = (double)geni_raw_to_u32(raw, n);
    double sign = geni_info_zero_negative(info) ? -1.0 : 1.0;

    if (info->sif == GENI_SIF_STANDARD) {
        double z = sign * (double)info->zero;
        double r = (double)info->range;
        double full = 254.0 * pow256((uint8_t)(n - 1u));
        *out = (z + x * r / full) * u->factor;
    } else {
        double z16 = sign * (double)((uint16_t)info->zero * 256u + info->range);
        double mult = n > 2u ? pow256((uint8_t)(n - 2u)) : 1.0;
        *out = (z16 * mult + x) * u->factor;
    }
    return GENI_VAL_OK;
}

geni_err_t geni_unscale(const geni_info_t *info, double value, uint8_t n,
                        uint8_t *raw_out)
{
    if (info == NULL || raw_out == NULL || n < 1u || n > 4u || !isfinite(value))
        return GENI_E_BAD_ARG;
    if (info->sif < GENI_SIF_STANDARD)
        return GENI_E_BAD_ARG;
    const geni_unit_info_t *u = geni_unit_lookup(geni_info_unit_index(info));
    if (u == NULL || u->factor == 0.0)
        return GENI_E_BAD_ARG;

    double sign = geni_info_zero_negative(info) ? -1.0 : 1.0;
    double lo_scale = pow256((uint8_t)(n - 1u)); /* weight of the high byte */
    double x, max;

    if (info->sif == GENI_SIF_STANDARD) {
        if (info->range == 0u)
            return GENI_E_BAD_ARG;
        double z = sign * (double)info->zero;
        double full = 254.0 * lo_scale;
        x = (value / u->factor - z) * full / (double)info->range;
        max = full; /* 254, 0xFE00, ... */
    } else {
        double z16 = sign * (double)((uint16_t)info->zero * 256u + info->range);
        double mult = n > 2u ? pow256((uint8_t)(n - 2u)) : 1.0;
        x = value / u->factor - z16 * mult;
        max = info->vi ? (lo_scale * 256.0 - 1.0)       /* full n bytes */
                       : (255.0 * lo_scale - 1.0);      /* hi byte <= 254 */
    }

    x = floor(x + 0.5);
    if (x < 0.0)
        x = 0.0;
    if (x > max)
        x = max;
    uint32_t v = (uint32_t)x;
    for (int k = (int)n - 1; k >= 0; k--) {
        raw_out[k] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
    return GENI_OK;
}

/* ----------------------------------------------------------- formatting */

int geni_fmt_double(char *buf, size_t cap, double v, int decimals)
{
    char tmp[32];
    int n = 0;

    if (buf == NULL || cap == 0)
        return 0;
    if (decimals < 0)
        decimals = 0;
    if (decimals > 6)
        decimals = 6;

    uint64_t pow10 = 1;
    for (int i = 0; i < decimals; i++)
        pow10 *= 10u;

    if (!isfinite(v) || fabs(v) * (double)pow10 >= 9.0e18) {
        n = 0;
        memcpy(tmp, "null", 4);
        n = 4;
    } else {
        int neg = v < 0.0;
        uint64_t s = (uint64_t)floor(fabs(v) * (double)pow10 + 0.5);
        if (s == 0)
            neg = 0;
        uint64_t ip = s / pow10;
        uint64_t fp = s % pow10;
        char rev[24];
        int r = 0;
        do {
            rev[r++] = (char)('0' + (int)(ip % 10u));
            ip /= 10u;
        } while (ip && r < (int)sizeof rev);
        if (neg)
            tmp[n++] = '-';
        while (r)
            tmp[n++] = rev[--r];
        if (decimals && fp) {
            char fr[8];
            for (int i = decimals - 1; i >= 0; i--) {
                fr[i] = (char)('0' + (int)(fp % 10u));
                fp /= 10u;
            }
            int last = decimals;
            while (last > 0 && fr[last - 1] == '0')
                last--;
            tmp[n++] = '.';
            for (int i = 0; i < last; i++)
                tmp[n++] = fr[i];
        }
    }
    if ((size_t)n >= cap)
        n = (int)cap - 1;
    memcpy(buf, tmp, (size_t)n);
    buf[n] = '\0';
    return n;
}
