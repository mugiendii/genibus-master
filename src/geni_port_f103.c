/*
 * geni_port_f103.c - GENIbus RS-485 transport (UART and pins: geni_config.h) (design C.3).
 *
 * TX blocks (HAL_UART_Transmit waits for TC, so DE drops after the last
 * stop bit). RX is interrupt driven with DWT timestamps for the
 * inter-byte gap and reply timeouts.
 */
#include "geni_port_f103.h"

#include "geni_config.h"
#include "stm32f1xx_hal.h"

#include <string.h>

static UART_HandleTypeDef s_uart;

static volatile uint8_t  s_rx[GENI_TELEGRAM_MAX];
static volatile uint16_t s_rx_len;
static volatile uint8_t  s_rx_err;
static volatile uint8_t  s_rx_ovf;
static volatile uint32_t s_last_cyc; /* last bus activity (DWT cycles) */
static volatile uint32_t s_last_ms;  /* same, HAL tick (for long gaps) */
static uint32_t          s_idle_req_us = GENI_IDLE_AFTER_MSG_US;
static uint32_t          s_cyc_per_us = 72;

static inline uint32_t cyc_now(void) { return DWT->CYCCNT; }

static uint32_t elapsed_us(uint32_t since_cyc)
{
    return (cyc_now() - since_cyc) / s_cyc_per_us;
}

void geni_port_delay_us(uint32_t us)
{
    uint32_t t0 = cyc_now();
    uint32_t c = us * s_cyc_per_us;
    while ((cyc_now() - t0) < c) {
    }
}

static void mark_activity(void)
{
    s_last_cyc = cyc_now();
    s_last_ms = HAL_GetTick();
}

static void rx_clear(void)
{
    HAL_NVIC_DisableIRQ(GENI_UART_IRQn);
    s_rx_len = 0;
    s_rx_err = 0;
    s_rx_ovf = 0;
    HAL_NVIC_EnableIRQ(GENI_UART_IRQn);
}

static void de(int on)
{
    HAL_GPIO_WritePin(GENI_DE_PORT, GENI_DE_PIN, (on != 0) == (GENI_DE_TX_HIGH != 0u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void geni_port_init(void)
{
    /* DWT cycle counter */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_cyc_per_us = SystemCoreClock / 1000000u;
    if (s_cyc_per_us == 0)
        s_cyc_per_us = 1;

    GENI_CLK_ENABLE();

    GPIO_InitTypeDef g = { 0 };
    de(0);
    g.Pin = GENI_DE_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GENI_DE_PORT, &g);

    g.Pin = GENI_TX_PIN;
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GENI_TX_PORT, &g);

    g.Pin = GENI_RX_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP; /* idle-high while the transceiver RO floats */
    HAL_GPIO_Init(GENI_RX_PORT, &g);

    s_uart.Instance = GENI_UART;
    s_uart.Init.BaudRate = GENI_BAUD;
    s_uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_uart.Init.StopBits = UART_STOPBITS_1;
    s_uart.Init.Parity = UART_PARITY_NONE;
    s_uart.Init.Mode = UART_MODE_TX_RX;
    s_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&s_uart);

    HAL_NVIC_SetPriority(GENI_UART_IRQn, 1, 0);
    rx_clear();
    __HAL_UART_ENABLE_IT(&s_uart, UART_IT_RXNE);
    HAL_NVIC_EnableIRQ(GENI_UART_IRQn);
    mark_activity();
}

void geni_port_irq(void)
{
    uint32_t sr = GENI_UART->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t b = (uint8_t)GENI_UART->DR; /* SR then DR read clears flags */
        /* Only a lost byte is fatal; the CRC judges noise/framing flags. */
        if (sr & USART_SR_ORE)
            s_rx_err = 1;
        if (s_rx_len < sizeof s_rx)
            s_rx[s_rx_len++] = b;
        else
            s_rx_ovf = 1;
        mark_activity();
    }
}

static void wait_bus_idle(void)
{
    for (;;) {
        if (HAL_GetTick() - s_last_ms > 100u)
            return; /* idle far longer than any requirement (and DWT wrap-safe) */
        if (elapsed_us(s_last_cyc) >= s_idle_req_us)
            return;
    }
}

geni_err_t geni_port_transact(void *ctx, const uint8_t *tx, uint16_t tx_len,
                              uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                              int expect_reply)
{
    (void)ctx;
    if (rx_len)
        *rx_len = 0;

    /* 1. bus idle */
    wait_bus_idle();

    /* 2. send */
    rx_clear();
    de(1);
    geni_port_delay_us(GENI_DE_SETUP_US);
    HAL_StatusTypeDef hs =
        HAL_UART_Transmit(&s_uart, (uint8_t *)(uintptr_t)tx, tx_len, 50u + tx_len * 2u);
    de(0);
    mark_activity();
    uint32_t tx_end = s_last_cyc;
    if (hs != HAL_OK) {
        s_idle_req_us = GENI_IDLE_AFTER_MSG_US;
        return GENI_E_UART;
    }

    /* 3. drop echo / turnaround glitches */
    geni_port_delay_us(GENI_TURNAROUND_US);
    rx_clear();

    /* 4. no reply expected */
    if (!expect_reply) {
        s_idle_req_us = GENI_IDLE_AFTER_MSG_US;
        return GENI_OK;
    }

    /* 5. receive */
    geni_err_t res;
    uint16_t n;
    for (;;) {
        n = s_rx_len;
        if (s_rx_err) { res = GENI_E_UART; break; }
        if (s_rx_ovf) { res = GENI_E_OVERFLOW; break; }
        if (n >= 2u && n >= (uint16_t)(s_rx[1] + 4u)) {
            n = (uint16_t)(s_rx[1] + 4u);
            res = GENI_OK;
            break;
        }
        if (n == 0u) {
            if (elapsed_us(tx_end) > GENI_REPLY_TIMEOUT_MS * 1000u) {
                res = GENI_E_TIMEOUT;
                break;
            }
        } else if (elapsed_us(s_last_cyc) > GENI_GAP_TIMEOUT_US) {
            res = GENI_E_TRUNCATED;
            break;
        }
    }

    if (n > rx_cap) {
        n = rx_cap;
        if (res == GENI_OK)
            res = GENI_E_OVERFLOW;
    }
    for (uint16_t i = 0; i < n; i++)
        rx[i] = s_rx[i];
    if (rx_len)
        *rx_len = n;

    if (res == GENI_E_TIMEOUT)
        mark_activity(); /* idle counts from the timeout */
    s_idle_req_us = (res == GENI_OK) ? GENI_IDLE_US : GENI_IDLE_AFTER_MSG_US;
    return res;
}
