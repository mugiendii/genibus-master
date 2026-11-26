/*
 * mp204.c - checked stop / trip / reset for the Grundfos MP 204.
 */
#include "mp204.h"

#include <string.h>

#define ID_RESET_ALARM 2u
#define ID_TRIP        81u
#define ID_TRIP_A      95u

#define READBACKS      10u
#define READBACK_MS    200u
#define NA             0xFFu

geni_err_t mp204_read(geni_master_t *m, uint8_t addr, mp204_status_t *s)
{
    static const uint8_t ids[] = { 148, 149, 95, 96, 97, 144 };
    uint8_t v[sizeof ids];
    geni_err_t e = geni_get(m, addr, GENI_CLASS_MEASURED, ids, sizeof ids, v, NULL);
    if (e == GENI_OK) {
        s->family = v[0];
        s->type = v[1];
        s->act_mode1 = v[2];
        s->act_mode2 = v[3];
        s->setup = v[4];
        s->alarm_code = v[5];
    }
    return e;
}

static int usable(const mp204_status_t *s)
{
    return s->act_mode1 != NA && s->act_mode2 != NA &&
           mp204_mode(s) <= MP204_TRIPPED3 &&
           mp204_system(s) == MP204_SYS_OPERATING;
}

static mp204_result_t allowed(mp204_action_t act, const mp204_status_t *s)
{
    if (s->family != MP204_FAMILY || s->type != MP204_TYPE)
        return MP204_NOT_MP204;
    if (!usable(s))
        return MP204_NOT_OPERATING;
    if (act == MP204_RESET) {
        if (mp204_mode(s) == MP204_STARTED)
            return MP204_ALREADY_STARTED;
        if (mp204_unprotected(s))
            return MP204_UNPROTECTED;
    } else if (mp204_mode(s) != MP204_STARTED) {
        /* TRIP_A on a tripped unit would turn a protection trip into
         * Tripped 3, which shows no alarm. */
        return MP204_NOT_STARTED;
    }
    return MP204_OK;
}

static int reached(mp204_action_t act, const mp204_status_t *s)
{
    if (!usable(s))
        return 0;
    switch (act) {
    case MP204_STOP:
        return mp204_mode(s) == MP204_TRIPPED3 && !mp204_relay1(s);
    case MP204_TRIP:
        return mp204_mode(s) == MP204_TRIPPED2 && !mp204_relay1(s);
    default:
        return mp204_mode(s) == MP204_STARTED && mp204_relay1(s);
    }
}

mp204_result_t mp204_command(geni_master_t *m, uint8_t addr, mp204_action_t act,
                             void (*delay_ms)(uint32_t), mp204_report_t *r)
{
    uint8_t id;
    memset(r, 0, sizeof *r);
    switch (act) {
    case MP204_STOP:  id = ID_TRIP_A; break;
    case MP204_TRIP:  id = ID_TRIP; break;
    case MP204_RESET: id = ID_RESET_ALARM; break;
    default:
        r->err = GENI_E_BAD_ARG;
        return MP204_BUS;
    }

    r->err = mp204_read(m, addr, &r->before);
    if (r->err != GENI_OK)
        return MP204_BUS;
    r->have_before = 1;
    mp204_result_t ok = allowed(act, &r->before);
    if (ok != MP204_OK)
        return ok;

    r->sent = 1;
    r->err = geni_cmd(m, addr, &id, 1, &r->ack);
    /* On any other error the command may have been executed and only the
     * reply lost, so the readback decides. */

    for (unsigned i = 0; i < READBACKS; i++) {
        mp204_status_t s;
        if (delay_ms)
            delay_ms(READBACK_MS);
        if (mp204_read(m, addr, &s) != GENI_OK) {
            /* Report only what is current. */
            r->have_after = 0;
            continue;
        }
        r->after = s;
        r->have_after = 1;
        if (r->err == GENI_E_NACK)
            return MP204_REJECTED;
        if (s.family == MP204_FAMILY && s.type == MP204_TYPE && reached(act, &s))
            return MP204_OK;
    }
    if (r->err == GENI_E_NACK)
        return MP204_REJECTED;
    if (act == MP204_RESET && r->have_after && usable(&r->after) &&
        mp204_mode(&r->after) == MP204_TRIPPED1)
        return MP204_STILL_TRIPPED;
    return MP204_UNCONFIRMED;
}

const char *mp204_mode_str(const mp204_status_t *s)
{
    if (s->act_mode1 == NA)
        return "unknown";
    switch (mp204_mode(s)) {
    case MP204_STARTED:  return "started";
    case MP204_TRIPPED1: return "tripped1_protection";
    case MP204_TRIPPED2: return "tripped2_test";
    case MP204_TRIPPED3: return "tripped3_stop";
    default:             return "unknown";
    }
}

const char *mp204_alarm_str(uint8_t code)
{
    switch (code) {
    case 0:   return "none";
    case 2:   return "missing_phase";
    case 3:   return "ptc_external";
    case 4:   return "too_many_auto_restarts";
    case 9:   return "phase_sequence";
    case 15:  return "communication";
    case 18:  return "commanded_trip";
    case 20:  return "insulation_low";
    case 32:  return "overvoltage";
    case 40:  return "undervoltage";
    case 48:  return "overload";
    case 56:  return "underload";
    case 64:  return "overtemp_tempcon";
    case 71:  return "overtemp_pt";
    case 111: return "current_asymmetry";
    case 112: return "cos_phi_max";
    case 113: return "cos_phi_min";
    case 120: return "aux_winding";
    case 123: return "start_capacitor";
    case 124: return "run_capacitor";
    default:  return "unknown";
    }
}

const char *mp204_result_str(mp204_result_t r)
{
    static const char *const n[] = {
        "ok", "bus_error", "not_an_mp204", "not_operating", "not_started",
        "already_started", "protection_disabled", "rejected", "still_tripped",
        "unconfirmed",
    };
    return (unsigned)r < sizeof n / sizeof n[0] ? n[r] : "?";
}
