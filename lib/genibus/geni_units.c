/*
 * geni_units.c - GENIbus unit table, indices 1..95 (spec Aug 2005).
 * Newer products may define more indices: check the functional profile.
 */
#include "geni_units.h"

#include <stddef.h>

#define U(f, s) { (f), (s) }

static const geni_unit_info_t k_units[96] = {
    [0]  = U(0.0, NULL),
    /* current */
    [15] = U(1e-6, "A"), [1] = U(0.1, "A"), [42] = U(0.2, "A"),
    [62] = U(0.5, "A"),  [2] = U(5.0, "A"),
    /* voltage */
    [3] = U(0.1, "V"), [4] = U(1.0, "V"), [5] = U(5.0, "V"),
    /* resistance / capacitance */
    [6] = U(1.0, "ohm"), [43] = U(1e4, "ohm"), [89] = U(1e5, "ohm"),
    [10] = U(1e-6, "F"),
    /* power */
    [7] = U(1.0, "W"), [8] = U(10.0, "W"), [9] = U(100.0, "W"),
    [44] = U(1000.0, "W"), [45] = U(10000.0, "W"),
    /* frequency */
    [11] = U(0.5, "Hz"), [16] = U(1.0, "Hz"), [38] = U(2.0, "Hz"),
    [17] = U(2.5, "Hz"),
    /* speed */
    [18] = U(12.0, "rpm"), [19] = U(100.0, "rpm"), [34] = U(2.0, "rad/s"),
    /* temperature */
    [20] = U(0.1, "degC"), [21] = U(1.0, "degC"), [57] = U(1.0, "degF"),
    [84] = U(0.01, "K"),
    /* flow */
    [69] = U(0.1, "ml/h"),  [95] = U(0.01, "m3/h"), [22] = U(0.1, "m3/h"),
    [23] = U(1.0, "m3/h"),  [41] = U(5.0, "m3/h"),  [92] = U(10.0, "m3/h"),
    [93] = U(100.0, "m3/h"), [73] = U(0.5, "l/h"),  [52] = U(1.0, "l/s"),
    [63] = U(0.1, "l/s"),   [53] = U(1.0, "m3/s"),  [54] = U(1.0, "gpm"),
    [58] = U(10.0, "gpm"),  [82] = U(0.1, "l/min"),
    /* head / distance */
    [91] = U(0.0001, "m"), [83] = U(0.01, "m"), [24] = U(0.1, "m"),
    [25] = U(1.0, "m"),    [26] = U(10.0, "m"), [56] = U(1.0, "ft"),
    [59] = U(10.0, "ft"),
    /* pressure */
    [51] = U(0.001, "bar"), [27] = U(0.01, "bar"), [28] = U(0.1, "bar"),
    [29] = U(1.0, "bar"),   [61] = U(1.0, "kPa"),  [55] = U(1.0, "psi"),
    [60] = U(10.0, "psi"),
    /* percent */
    [12] = U(0.1, "%"), [30] = U(1.0, "%"), [76] = U(10.0, "%"),
    /* energy */
    [87] = U(1.0, "Ws"),     [94] = U(1.0, "Wh"),      [31] = U(1.0, "kWh"),
    [85] = U(2.0, "kWh"),    [32] = U(10.0, "kWh"),    [33] = U(100.0, "kWh"),
    [40] = U(512.0, "kWh"),  [46] = U(1000.0, "kWh"),  [47] = U(10000.0, "kWh"),
    [48] = U(100000.0, "kWh"),
    /* time */
    [39] = U(1024.0, "h"), [81] = U(100.0, "h"), [72] = U(1024.0, "min"),
    [13] = U(2.0, "h"),    [35] = U(1.0, "h"),   [36] = U(2.0, "min"),
    [80] = U(1.0, "min"),  [14] = U(30.0, "s"),  [78] = U(10.0, "s"),
    [37] = U(1.0, "s"),    [79] = U(0.1, "s"),
    /* volume */
    [71] = U(1e-6, "ml"), [70] = U(0.1, "ml"), [88] = U(1.0, "ml"),
    [64] = U(0.1, "m3"),  [86] = U(1.0, "m3"), [67] = U(256.0, "m3"),
    [65] = U(1000.0, "m3"),
    /* other */
    [90] = U(1.0, "mm/s"),     [49] = U(1.0, "deg"),     [77] = U(0.01, "gain"),
    [50] = U(1.0, "gain"),     [66] = U(10.0, "kWh/m3"), [74] = U(1.0, "Wh/m3"),
    [68] = U(1.0, "m2"),       [75] = U(1.0, "Nm"),
};

const geni_unit_info_t *geni_unit_lookup(uint8_t index)
{
    if (index >= sizeof k_units / sizeof k_units[0])
        return NULL;
    if (k_units[index].unit == NULL)
        return NULL;
    return &k_units[index];
}
