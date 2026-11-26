/*
 * geni_config.h - every pin, UART and timing knob for the F103 master.
 */
#ifndef GENI_CONFIG_H
#define GENI_CONFIG_H

/* ---- Bus addresses ---- */
#define GENI_MASTER_ADDR   0x01u /* our own address (master range 0..231)    */
#define GENI_TARGET_ADDR   0x00u /* 0 = poll first discovered unit,
                                    e.g. 0x20 = fixed address (R100 "No. 1") */

/* ---- RS-485 (GENIbus) on USART3: PB10 TX, PB11 RX, PB1 DE+/RE ----
 * FlowHub F103 prototype (Flow_hub/include/board_pins.h). NOT the STM32F411
 * FlowHub PCB, which has no USART3. */
#define GENI_UART          USART3
#define GENI_UART_IRQn     USART3_IRQn
#define GENI_UART_IRQHandler USART3_IRQHandler
#define GENI_CLK_ENABLE()  do { __HAL_RCC_GPIOB_CLK_ENABLE(); \
                                __HAL_RCC_USART3_CLK_ENABLE(); } while (0)
#define GENI_BAUD          9600u
#define GENI_TX_PORT       GPIOB
#define GENI_TX_PIN        GPIO_PIN_10
#define GENI_RX_PORT       GPIOB
#define GENI_RX_PIN        GPIO_PIN_11
#define GENI_DE_PORT       GPIOB
#define GENI_DE_PIN        GPIO_PIN_1
#define GENI_DE_TX_HIGH    1u /* 1: high = transmit. Board polarity UNCONFIRMED */

/* ---- FlowHub MOSFET outputs, active high: held LOW (off) from boot ---- */
#define OUT_A_PINS         (GPIO_PIN_7)                        /* DO1 */
#define OUT_B_PINS         (GPIO_PIN_13 | GPIO_PIN_12 | \
                            GPIO_PIN_8 | GPIO_PIN_9)            /* DO2..DO5 */

/* ---- GENIbus timing (spec B.2 / design C.3) ---- */
#define GENI_REPLY_TIMEOUT_MS   60u    /* no first byte within this -> timeout */
#define GENI_GAP_TIMEOUT_US     4000u  /* gap inside a reply -> truncated      */
#define GENI_IDLE_US            3000u  /* bus idle before next request         */
#define GENI_IDLE_AFTER_MSG_US  50000u /* idle after no-reply telegram/error   */
#define GENI_DE_SETUP_US        20u    /* DE high -> first start bit           */
/* After DE low: discard echo/glitches. Must outlast a false start bit (one
 * byte = 1.04 ms) but stay below the slave's 3 ms minimum reply delay. */
#define GENI_TURNAROUND_US      2500u

/* ---- Console: JSON output + CLI on USART1: PA9 TX, PA10 RX ---- */
#define CONSOLE_UART       USART1
#define CONSOLE_UART_IRQn  USART1_IRQn
#define CONSOLE_BAUD       115200u

/* ---- LED (Blue Pill PC13, active low) ---- */
#define LED_PORT           GPIOC
#define LED_PIN            GPIO_PIN_13

/* ---- Application ---- */
#define APP_POLL_MS            1000u  /* poll period                          */
#define APP_OFFLINE_TIMEOUTS   5u     /* consecutive timeouts -> offline      */
#define APP_DISCOVERY_SILENT   3u     /* K silent Connection Requests -> done */
#define APP_DISCOVERY_MAX_REQ  100u   /* hard cap per scan (noise/collisions) */
#define APP_CONFIRM_MS         15000u /* `mp204 reset` -> `mp204 reset yes`    */
#define APP_RETRY_MS           2000u  /* retry delay when nothing answers     */
#define APP_RESCAN_MS          60000u /* periodic rescan for new units, 0=off */

#endif
