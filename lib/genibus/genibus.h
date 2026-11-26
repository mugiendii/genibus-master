/*
 * genibus.h - portable GENIbus protocol core (C99, no HAL).
 *
 * CRC-16/CCITT, telegram builder/parser, INFO decoding and value scaling
 * per the Grundfos GENIbus Protocol Specification (Aug 2005).
 */
#ifndef GENIBUS_H
#define GENIBUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Start delimiters (B.4) ---- */
#define GENI_SD_REQUEST   0x27u /* Data Request, slave replies         */
#define GENI_SD_MESSAGE   0x26u /* Data Message, no reply              */
#define GENI_SD_REPLY     0x24u /* Data Reply                          */

/* ---- Addresses (B.3) ---- */
#define GENI_ADDR_CONNREQ   0xFEu
#define GENI_ADDR_BROADCAST 0xFFu
#define GENI_ADDR_SLAVE_MIN 32u
#define GENI_ADDR_SLAVE_MAX 231u

/* ---- Data classes (B.7) ---- */
#define GENI_CLASS_PROTOCOL  0u
#define GENI_CLASS_MEASURED  2u
#define GENI_CLASS_COMMAND   3u
#define GENI_CLASS_CONFIG    4u
#define GENI_CLASS_REFERENCE 5u
#define GENI_CLASS_STRING    7u

/* ---- APDU operation (requests) / acknowledge (replies) (B.6) ---- */
#define GENI_OS_GET  0u
#define GENI_OS_SET  2u
#define GENI_OS_INFO 3u

#define GENI_ACK_OK            0u
#define GENI_ACK_CLASS_UNKNOWN 1u
#define GENI_ACK_ID_UNKNOWN    2u
#define GENI_ACK_ILLEGAL       3u

#define GENI_APDU_DATA_MAX 63u
/* SD + LE + max LE (255) + CRC(2) */
#define GENI_TELEGRAM_MAX  (2u + 255u + 2u)

/* ---- Error codes (C.8) ---- */
typedef enum {
    GENI_OK = 0,
    GENI_E_TIMEOUT,
    GENI_E_TRUNCATED,
    GENI_E_UART,
    GENI_E_CRC,
    GENI_E_BAD_LENGTH,
    GENI_E_BAD_SD,
    GENI_E_BAD_ADDR,
    GENI_E_BAD_APDU,
    GENI_E_NACK,
    GENI_E_OVERFLOW,
    GENI_E_BAD_ARG,
    GENI_E_COUNT
} geni_err_t;

const char *geni_err_str(geni_err_t e);

/* ---- CRC (B.5) ---- */
uint16_t geni_crc_update(uint16_t acc, uint8_t b);
/* Final (inverted) CRC over data[0..len-1]. */
uint16_t geni_crc(const uint8_t *data, size_t len);

/* ---- Telegram builder ---- */
typedef struct {
    uint8_t   *buf;
    uint16_t   cap;
    uint16_t   len;     /* bytes written so far                       */
    uint8_t    pdu_max; /* max PDU bytes (APDUs) allowed, 0 = no limit */
    geni_err_t err;     /* sticky error                               */
} geni_builder_t;

void geni_tx_begin(geni_builder_t *b, uint8_t *buf, uint16_t cap,
                   uint8_t sd, uint8_t da, uint8_t sa, uint8_t pdu_max);
/* Append one APDU. os is GENI_OS_*; data may be NULL when n == 0. */
geni_err_t geni_tx_apdu(geni_builder_t *b, uint8_t cls, uint8_t os,
                        const uint8_t *data, uint8_t n);
/* Fill in LE and CRC. On success *out_len is the total telegram length. */
geni_err_t geni_tx_end(geni_builder_t *b, uint16_t *out_len);
/* PDU bytes currently used (APDUs only). */
uint16_t geni_tx_pdu_len(const geni_builder_t *b);

/* ---- Telegram parser ---- */
typedef struct {
    uint8_t        sd, le, da, sa;
    const uint8_t *pdu;     /* first APDU byte                        */
    uint8_t        pdu_len; /* APDU bytes (RFS excluded)              */
    uint8_t        has_rfs;
    uint8_t        rfs;
} geni_frame_t;

typedef struct {
    uint8_t        cls;
    uint8_t        ack; /* OS in requests, ACK in replies */
    uint8_t        len;
    const uint8_t *data;
} geni_apdu_t;

/*
 * Check length (== LE + 4) and CRC, and split the telegram.
 * Does NOT check SD or addresses: that is up to the caller.
 */
geni_err_t geni_parse_frame(const uint8_t *buf, uint16_t len, geni_frame_t *f);
/*
 * Iterate APDUs. *pos starts at 0. Returns 1 when an APDU was produced,
 * 0 at the end, -1 on a malformed APDU.
 */
int geni_next_apdu(const geni_frame_t *f, uint16_t *pos, geni_apdu_t *a);

/* ---- INFO / scaling (B.9) ---- */
#define GENI_SIF_NONE     0u
#define GENI_SIF_BITWISE  1u
#define GENI_SIF_STANDARD 2u
#define GENI_SIF_EXTENDED 3u

typedef struct {
    uint8_t head;
    uint8_t sif;
    uint8_t vi;    /* 1: 0..255 all valid; 0: 255 = not available       */
    uint8_t bo;    /* 1: low-order byte of a multi-byte value           */
    uint8_t unit;  /* raw UNIT byte (bit 7 = sign of ZERO)              */
    uint8_t zero;  /* standard: ZERO     extended: ZERO_hi              */
    uint8_t range; /* standard: RANGE    extended: ZERO_lo              */
} geni_info_t;

/*
 * Decode one INFO entry from p[0..avail-1].
 * Returns bytes consumed (1 or 4), or 0 if malformed / truncated.
 */
uint8_t geni_info_decode(const uint8_t *p, uint8_t avail, geni_info_t *out);
uint8_t geni_info_unit_index(const geni_info_t *i);
int     geni_info_zero_negative(const geni_info_t *i);

typedef enum {
    GENI_VAL_OK = 0, /* *out is a physical value                         */
    GENI_VAL_NA,     /* "data not available" (high byte 255, VI = 0)    */
    GENI_VAL_RAW,    /* item has no scaling (SIF 0/1) or unknown unit   */
    GENI_VAL_BAD     /* bad arguments                                   */
} geni_val_t;

/* Combine raw[0..n-1] (high byte first) into an unsigned integer. */
uint32_t geni_raw_to_u32(const uint8_t *raw, uint8_t n);

/* Scale n raw bytes (1..4, high byte first) into physical units. */
geni_val_t geni_scale(const geni_info_t *info, const uint8_t *raw, uint8_t n,
                      double *out);
/* Inverse: physical value -> n raw bytes (rounded, clamped). */
geni_err_t geni_unscale(const geni_info_t *info, double value, uint8_t n,
                        uint8_t *raw_out);

/*
 * Format a double with up to `decimals` decimals, trailing zeros trimmed.
 * Writes "null" for NaN/inf. Returns the number of characters written.
 * Avoids printf("%f"), which newlib-nano does not support.
 */
int geni_fmt_double(char *buf, size_t cap, double v, int decimals);

#ifdef __cplusplus
}
#endif
#endif
