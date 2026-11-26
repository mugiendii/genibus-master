/*
 * geni_units.h - GENIbus unit table (B.10): index -> factor + base unit.
 */
#ifndef GENI_UNITS_H
#define GENI_UNITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double      factor; /* one raw step in `unit`  */
    const char *unit;   /* ASCII base unit name    */
} geni_unit_info_t;

/* Returns NULL for an index that is not in the table (0, > 95, ...). */
const geni_unit_info_t *geni_unit_lookup(uint8_t index);

#ifdef __cplusplus
}
#endif
#endif
