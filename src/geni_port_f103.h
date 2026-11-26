/*
 * geni_port_f103.h - RS-485 transport for the GENIbus master (STM32F1 HAL).
 */
#ifndef GENI_PORT_F103_H
#define GENI_PORT_F103_H

#include "genibus.h"

void       geni_port_init(void);
/* Matches geni_transact_fn; ctx is unused. */
geni_err_t geni_port_transact(void *ctx, const uint8_t *tx, uint16_t tx_len,
                              uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                              int expect_reply);
void       geni_port_delay_us(uint32_t us);
/* GENIbus UART IRQ entry, called from its IRQ handler. */
void       geni_port_irq(void);

#endif
