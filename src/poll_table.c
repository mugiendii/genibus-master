/*
 * poll_table.c - the data items to poll.  <-- EDIT THIS per device.
 *
 * Data item IDs are PER PRODUCT. Take them from the device's GENIbus
 * functional profile (class, ID, size). Do not guess.
 *
 *   { name, class, high-byte ID, size in bytes (1..4, IDs id, id+1, ...) }
 *
 * Keep items of the same class next to each other: they go out in one
 * telegram (max 62 value bytes per GET; this table uses 47).
 *
 * ---------------------------------------------------------------------
 * Device: Grundfos MP 204 motor protector (unit_family 7, unit_type 1).
 * Source: "Operating the MP 204 Electronic Motor Protection Unit via
 * GENIbus or G100", Edition 2.28, May 2011, chapters 7, 8, 14 and the
 * telegram example in 12.3 (which confirms hi-byte-first consecutive IDs).
 * Scaling is read from the device via INFO (extended precision);
 * the units below are what the profile documents.
 *
 * This table is set up for a 3-PHASE motor. For a 1-phase motor the line
 * voltages v12/v23/v31/v_line, i_asym and cos_phi1..3 read "NA" (null);
 * use v_phase (2, 27, 2 bytes) and c_start (2, 50, 2) / c_run (2, 52, 2).
 * ---------------------------------------------------------------------
 */
#include "poll_table.h"

const geni_item_def_t g_poll_items[] = {
    /* Voltages, 0.1 V, 16 bit */
    { "v12",          GENI_CLASS_MEASURED,  29, 2 }, /* line voltage L1-L2   */
    { "v23",          GENI_CLASS_MEASURED,  31, 2 }, /* line voltage L2-L3   */
    { "v31",          GENI_CLASS_MEASURED,  33, 2 }, /* line voltage L3-L1   */
    { "v_line",       GENI_CLASS_MEASURED,  35, 2 }, /* mean line voltage    */
    /* Currents, 0.1 A, 16 bit */
    { "i1",           GENI_CLASS_MEASURED,  37, 2 }, /* L1 line current      */
    { "i2",           GENI_CLASS_MEASURED,  39, 2 }, /* L2 line current      */
    { "i3",           GENI_CLASS_MEASURED,  41, 2 }, /* L3 line current      */
    { "i_line",       GENI_CLASS_MEASURED,  43, 2 }, /* mean line current    */
    /* Motor / supply, 8 bit */
    { "t_mo1",        GENI_CLASS_MEASURED,  47, 1 }, /* 1 degC, Tempcon (motor on) */
    { "t_mo2",        GENI_CLASS_MEASURED,  48, 1 }, /* 1 degC, PT sensor    */
    { "i_asym",       GENI_CLASS_MEASURED,  49, 1 }, /* 0.1 %, motor on      */
    { "f_line",       GENI_CLASS_MEASURED,  58, 1 }, /* 0.5 Hz               */
    { "cos_phi",      GENI_CLASS_MEASURED,  64, 1 }, /* 0.01, power factor   */
    /* Power / energy, 32 bit */
    { "p",            GENI_CLASS_MEASURED,  65, 4 }, /* 1 W                  */
    { "energy",       GENI_CLASS_MEASURED,  69, 4 }, /* 1 kWh                */
    /* Insulation: 10 kOhm steps, only with motor OFF; 101 = >1000 kOhm */
    { "r_insulation", GENI_CLASS_MEASURED,  94, 1 },
    /* Counters, 24 bit */
    { "t_run",        GENI_CLASS_MEASURED,  77, 3 }, /* running time, 1 min  */
    { "t_on",         GENI_CLASS_MEASURED,  80, 3 }, /* power-on time, 1 min */
    { "start_cnt",    GENI_CLASS_MEASURED,  86, 3 }, /* number of starts     */
    /* Status (bit/unscaled, decode per profile ch. 5/6) */
    { "act_mode1",    GENI_CLASS_MEASURED,  95, 1 }, /* modes set by commands */
    { "act_mode2",    GENI_CLASS_MEASURED,  96, 1 }, /* modes set by MP 204   */
    { "alarm_code",   GENI_CLASS_MEASURED, 144, 1 },
    { "warnings_1",   GENI_CLASS_MEASURED, 145, 1 },
    { "warnings_2",   GENI_CLASS_MEASURED, 146, 1 },
    { "warnings_3",   GENI_CLASS_MEASURED, 147, 1 },
    /* Identification: expect 7 / 1 for an MP 204 */
    { "unit_family",  GENI_CLASS_MEASURED, 148, 1 },
    { "unit_type",    GENI_CLASS_MEASURED, 149, 1 },
};

const uint8_t g_poll_count = (uint8_t)(sizeof g_poll_items / sizeof g_poll_items[0]);
