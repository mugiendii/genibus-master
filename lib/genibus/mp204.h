/*
 * mp204.h - checked stop / trip / reset for the Grundfos MP 204.
 *
 * IDs and behaviour: "Operating the MP 204 ... via GENIbus or G100",
 * Ed. 2.28, May 2011, chapters 5 and 6.
 *
 * A command is sent once, never retried. The unit is identified and its state
 * checked first, then the state is read back: an acknowledge only says that
 * the telegram was accepted, not that the relay moved.
 */
#ifndef MP204_H
#define MP204_H

#include "geni_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP204_FAMILY 7u
#define MP204_TYPE   1u

typedef enum {
    MP204_STOP = 0, /* TRIP_A (3,95): relay opens, no alarm; -> Tripped 3   */
    MP204_TRIP,     /* TRIP (3,81): like the Test button; -> Tripped 2      */
    MP204_RESET     /* RESET_ALARM (3,2): clears AND STARTS the motor       */
} mp204_action_t;

enum { MP204_STARTED = 0, MP204_TRIPPED1, MP204_TRIPPED2, MP204_TRIPPED3 };
enum { MP204_SYS_POWER_UP = 0, MP204_SYS_OPERATING, MP204_SYS_FAULT };

typedef struct {
    uint8_t family, type;
    uint8_t act_mode1, act_mode2, setup, alarm_code; /* raw */
} mp204_status_t;

typedef enum {
    MP204_OK = 0,          /* the state read back is the requested one       */
    MP204_BUS,             /* nothing sent: state could not be read (*err)   */
    MP204_NOT_MP204,       /* nothing sent: other unit                       */
    MP204_NOT_OPERATING,   /* nothing sent: power-up, device fault, no data  */
    MP204_NOT_STARTED,     /* nothing sent: stop/trip of a tripped unit      */
    MP204_ALREADY_STARTED, /* nothing sent: reset of a started unit          */
    MP204_UNPROTECTED,     /* nothing sent: reset with protection disabled   */
    MP204_REJECTED,        /* sent, the unit refused it (*ack)               */
    MP204_STILL_TRIPPED,   /* reset sent, unit is in Tripped 1               */
    MP204_UNCONFIRMED      /* sent, effect not seen. State is UNKNOWN        */
} mp204_result_t;

typedef struct {
    mp204_status_t before, after;
    uint8_t        have_before, have_after, sent;
    geni_err_t     err; /* of the state read or of the command */
    uint8_t        ack;
} mp204_report_t;

geni_err_t mp204_read(geni_master_t *m, uint8_t addr, mp204_status_t *s);

/* delay_ms is called between readbacks (may be NULL). */
mp204_result_t mp204_command(geni_master_t *m, uint8_t addr, mp204_action_t act,
                             void (*delay_ms)(uint32_t), mp204_report_t *r);

static inline uint8_t mp204_mode(const mp204_status_t *s) { return s->act_mode1 & 7u; }
static inline uint8_t mp204_motor_off(const mp204_status_t *s) { return (s->act_mode1 >> 3) & 1u; }
static inline uint8_t mp204_relay1(const mp204_status_t *s) { return (s->act_mode1 >> 4) & 1u; }
static inline uint8_t mp204_relay2(const mp204_status_t *s) { return (s->act_mode1 >> 5) & 1u; }
static inline uint8_t mp204_unprotected(const mp204_status_t *s) { return (s->act_mode1 >> 7) & 1u; }
static inline uint8_t mp204_system(const mp204_status_t *s) { return s->act_mode2 & 3u; }
static inline uint8_t mp204_ready(const mp204_status_t *s) { return (s->act_mode2 >> 2) & 1u; }
static inline uint8_t mp204_pending(const mp204_status_t *s) { return (s->act_mode2 >> 3) & 1u; }
static inline uint8_t mp204_auto_restart(const mp204_status_t *s) { return (s->setup >> 5) & 1u; }
static inline uint8_t mp204_power_on_stop(const mp204_status_t *s) { return (s->setup >> 7) & 1u; }

const char *mp204_mode_str(const mp204_status_t *s);
const char *mp204_alarm_str(uint8_t code);
const char *mp204_result_str(mp204_result_t r);

#ifdef __cplusplus
}
#endif
#endif
