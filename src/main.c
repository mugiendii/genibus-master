/*
 * main.c - GENIbus master on STM32F103C8 (Blue Pill).
 *
 * BOOT -> DISCOVER -> READ_INFO -> POLL (JSON line every APP_POLL_MS)
 *         ^---- offline after APP_OFFLINE_TIMEOUTS consecutive timeouts
 *
 * USART1 @115200: JSON output + line-based CLI (type `help`).
 */
#include "geni_config.h"
#include "geni_master.h"
#include "geni_port_f103.h"
#include "geni_units.h"
#include "mp204.h"
#include "poll_table.h"
#include "stm32f1xx_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW_NAME "genibus-f103-master"

/* ------------------------------------------------------------- console */

static UART_HandleTypeDef s_con;
static volatile uint8_t   s_con_rx[128];
static volatile uint8_t   s_con_head, s_con_tail;

static char   s_out[2048]; /* JSON line being built (kept off the stack) */
static size_t s_out_len;

static void con_write(const char *s, size_t n)
{
    HAL_UART_Transmit(&s_con, (uint8_t *)(uintptr_t)s, (uint16_t)n, 200u);
}

static void out_reset(void) { s_out_len = 0; s_out[0] = '\0'; }

static void out_printf(const char *fmt, ...)
{
    if (s_out_len >= sizeof s_out - 1u)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(&s_out[s_out_len], sizeof s_out - s_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s_out_len += (size_t)n;
        if (s_out_len > sizeof s_out - 1u)
            s_out_len = sizeof s_out - 1u;
    }
}

static void out_double(double v)
{
    char b[32];
    geni_fmt_double(b, sizeof b, v, 3);
    out_printf("%s", b);
}

static void out_flush(void)
{
    out_printf("\r\n");
    con_write(s_out, s_out_len);
    out_reset();
}

static void console_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    GPIO_InitTypeDef g = { 0 };
    g.Pin = GPIO_PIN_9;
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_10;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &g);

    s_con.Instance = CONSOLE_UART;
    s_con.Init.BaudRate = CONSOLE_BAUD;
    s_con.Init.WordLength = UART_WORDLENGTH_8B;
    s_con.Init.StopBits = UART_STOPBITS_1;
    s_con.Init.Parity = UART_PARITY_NONE;
    s_con.Init.Mode = UART_MODE_TX_RX;
    s_con.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_con.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&s_con);
    HAL_NVIC_SetPriority(CONSOLE_UART_IRQn, 2, 0);
    __HAL_UART_ENABLE_IT(&s_con, UART_IT_RXNE);
    HAL_NVIC_EnableIRQ(CONSOLE_UART_IRQn);
}

void USART1_IRQHandler(void)
{
    uint32_t sr = CONSOLE_UART->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t b = (uint8_t)CONSOLE_UART->DR;
        uint8_t next = (uint8_t)((s_con_head + 1u) % sizeof s_con_rx);
        if (next != s_con_tail) {
            s_con_rx[s_con_head] = b;
            s_con_head = next;
        }
    }
}

void GENI_UART_IRQHandler(void) { geni_port_irq(); }

/* Returns a complete line (without CR/LF) or NULL. */
static char *console_line(void)
{
    static char line[96];
    static uint8_t n;
    while (s_con_tail != s_con_head) {
        char c = (char)s_con_rx[s_con_tail];
        s_con_tail = (uint8_t)((s_con_tail + 1u) % sizeof s_con_rx);
        if (c == '\r' || c == '\n') {
            if (n == 0)
                continue;
            line[n] = '\0';
            n = 0;
            return line;
        }
        if ((c == 0x08 || c == 0x7F) && n) {
            n--;
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < sizeof line - 1u)
            line[n++] = c;
    }
    return NULL;
}

/* -------------------------------------------------------------- state */

typedef enum { ST_DISCOVER, ST_READ_INFO, ST_POLL } app_state_t;

static geni_master_t     g_m; /* ~800 bytes: static, not on the stack */
static geni_item_state_t g_st[32];
static geni_table_t      g_tab;

static app_state_t g_state = ST_DISCOVER;
static uint8_t     g_fixed_target = GENI_TARGET_ADDR;
static uint8_t     g_addr;           /* unit being polled, 0 = none */
static uint8_t     g_poll_on = 1;
static uint8_t     g_print = 0;      /* 1: JSON line per poll and auto-scan */
static uint8_t     g_link = 2;       /* last reported link: 0 down, 1 up, 2 none */
static geni_err_t  g_last_err = GENI_E_TIMEOUT; /* result of the latest poll */
static uint8_t     g_timeouts;
static uint32_t    g_next_try, g_last_poll, g_last_scan;

static void led_toggle(void) { HAL_GPIO_TogglePin(LED_PORT, LED_PIN); }

static void print_err(const char *what, geni_err_t e, uint8_t ack)
{
    out_printf("{\"%s\":{\"err\":\"%s\"", what, geni_err_str(e));
    if (e == GENI_E_NACK)
        out_printf(",\"ack\":%u", ack);
    out_printf("}}");
    out_flush();
}

static void print_unit(void *user, const geni_unit_t *u)
{
    (void)user;
    out_printf("{\"unit\":{\"addr\":%u,\"group\":%u,\"family\":%u,\"type\":%u,"
               "\"df_buf_len\":%u,\"bus_mode\":%u}}",
               u->addr, u->group, u->family, u->type, u->df_buf_len, u->bus_mode);
    out_flush();
}

static void print_info(const char *name, uint8_t cls, uint8_t id,
                       const geni_info_t *i)
{
    out_printf("{\"info\":{\"name\":\"%s\",\"class\":%u,\"id\":%u,\"head\":\"0x%02X\","
               "\"sif\":%u,\"vi\":%u,\"bo\":%u",
               name, cls, id, i->head, i->sif, i->vi, i->bo);
    if (i->sif >= GENI_SIF_STANDARD) {
        uint8_t idx = geni_info_unit_index(i);
        const geni_unit_info_t *u = geni_unit_lookup(idx);
        out_printf(",\"unit_idx\":%u,\"zero_neg\":%d", idx, geni_info_zero_negative(i));
        if (i->sif == GENI_SIF_STANDARD)
            out_printf(",\"zero\":%u,\"range\":%u", i->zero, i->range);
        else
            out_printf(",\"zero\":%u", (unsigned)(i->zero * 256u + i->range));
        if (u) {
            out_printf(",\"unit\":\"%s\",\"factor\":", u->unit);
            out_double(u->factor);
        } else {
            out_printf(",\"unit\":null");
        }
    }
    out_printf("}}");
    out_flush();
}

static void print_table_info(void)
{
    for (uint8_t k = 0; k < g_tab.n; k++) {
        const geni_item_def_t *d = &g_tab.defs[k];
        const geni_item_state_t *s = &g_tab.st[k];
        if (s->info_state == GENI_INFO_VALID) {
            print_info(d->name, d->cls, d->id, &s->info);
        } else if (s->info_state == GENI_INFO_NACK) {
            out_printf("{\"info\":{\"name\":\"%s\",\"class\":%u,\"id\":%u,\"ack\":%u}}",
                       d->name, d->cls, d->id, s->info_ack);
            out_flush();
        }
    }
}

static void print_poll(geni_err_t e)
{
    out_printf("{\"t\":%lu,\"addr\":%u,\"ok\":%s", (unsigned long)HAL_GetTick(),
               g_addr, e == GENI_OK ? "true" : "false");
    if (e != GENI_OK)
        out_printf(",\"err\":\"%s\"", geni_err_str(e));
    out_printf(",\"items\":{");
    for (uint8_t k = 0; k < g_tab.n; k++) {
        double v = 0;
        const char *u = "";
        uint32_t raw = 0;
        uint8_t ack = 0;
        out_printf("%s\"%s\":", k ? "," : "", g_tab.defs[k].name);
        switch (geni_item_value(&g_tab, k, &v, &u, &raw, &ack)) {
        case GENI_ITEMV_SCALED:
            out_printf("{\"v\":");
            out_double(v);
            out_printf(",\"u\":\"%s\"}", u);
            break;
        case GENI_ITEMV_RAW:
            out_printf("{\"raw\":%lu}", (unsigned long)raw);
            break;
        case GENI_ITEMV_NACK:
            out_printf("{\"ack\":%u}", ack);
            break;
        default:
            out_printf("null");
            break;
        }
    }
    out_printf("}}");
    out_flush();
}

static void forget_unit(uint8_t addr)
{
    for (uint8_t i = 0; i < g_m.n_units; i++) {
        if (g_m.units[i].addr == addr) {
            g_m.units[i] = g_m.units[--g_m.n_units];
            return;
        }
    }
}

/* verbose: always print the result (CLI). Automatic scans print only with
 * `print on`; a unit found is still reported by print_unit. */
static void run_scan(int verbose)
{
    uint8_t found = 0;
    geni_discover(&g_m, APP_DISCOVERY_SILENT, APP_DISCOVERY_MAX_REQ, print_unit,
                  NULL, &found);
    g_last_scan = HAL_GetTick();
    if (verbose || g_print) {
        out_printf("{\"scan\":{\"found\":%u,\"known\":%u}}", found, g_m.n_units);
        out_flush();
    }
}

/* One line when polling starts or stops working, instead of one per poll. */
static void report_link(geni_err_t e)
{
    uint8_t up = (e == GENI_OK);
    if (up == g_link)
        return;
    g_link = up;
    out_printf("{\"link\":{\"addr\":%u,\"up\":%s", g_addr, up ? "true" : "false");
    if (!up)
        out_printf(",\"err\":\"%s\"", geni_err_str(e));
    out_printf("}}");
    out_flush();
}

/* ----------------------------------------------------------- events
 * After each good poll, compare the MP 204 state items with the last poll and
 * print one {"event":...} line when any of them changes: mode, relays, motor
 * on/off (act_mode1), alarm code, warning bytes. The first good poll after
 * boot or a target change prints the baseline ("first":true).
 * Value 255 means "not available" and is ignored.
 */
enum { EV_MODE = 0, EV_ALARM, EV_W1, EV_W2, EV_W3, EV_N };
static const char *const s_ev_names[EV_N] = {
    "act_mode1", "alarm_code", "warnings_1", "warnings_2", "warnings_3"
};
static int8_t  s_ev_idx[EV_N];
static uint8_t s_ev_resolved;
static uint8_t s_ev_last[EV_N];
static uint8_t s_ev_have;  /* baseline valid */
static uint8_t s_ev_addr;  /* unit the baseline belongs to */

static void out_ev_state(const uint8_t *v)
{
    mp204_status_t s;
    memset(&s, 0, sizeof s);
    s.act_mode1 = v[EV_MODE];
    out_printf("\"mode\":\"%s\",\"relay1_closed\":%u,\"relay2_closed\":%u,"
               "\"motor_off\":%u,\"alarm\":%u,\"alarm_text\":\"%s\","
               "\"warnings\":[%u,%u,%u]",
               mp204_mode_str(&s), mp204_relay1(&s), mp204_relay2(&s),
               mp204_motor_off(&s), v[EV_ALARM], mp204_alarm_str(v[EV_ALARM]),
               v[EV_W1], v[EV_W2], v[EV_W3]);
}

static void report_events(void)
{
    uint8_t now[EV_N];

    if (!s_ev_resolved) {
        for (int k = 0; k < EV_N; k++)
            s_ev_idx[k] = (int8_t)geni_table_find(&g_tab, s_ev_names[k]);
        s_ev_resolved = 1;
    }
    for (int k = 0; k < EV_N; k++) {
        int i = s_ev_idx[k];
        if (i < 0 || g_tab.st[i].state != GENI_ITEM_OK || g_tab.st[i].raw[0] == 0xFFu)
            return; /* incomplete: decide on the next poll */
        now[k] = g_tab.st[i].raw[0];
    }
    if (s_ev_have && s_ev_addr == g_addr && !memcmp(now, s_ev_last, sizeof now))
        return;

    out_printf("{\"event\":{\"addr\":%u,\"t\":%lu,", g_addr,
               (unsigned long)HAL_GetTick());
    out_ev_state(now);
    if (s_ev_have && s_ev_addr == g_addr) {
        out_printf(",\"was\":{");
        out_ev_state(s_ev_last);
        out_printf("}");
    } else {
        out_printf(",\"first\":true");
    }
    out_printf("}}");
    out_flush();
    memcpy(s_ev_last, now, sizeof now);
    s_ev_have = 1;
    s_ev_addr = g_addr;
}

static void set_target(uint8_t addr)
{
    g_addr = addr;
    g_timeouts = 0;
    g_state = ST_READ_INFO;
    g_next_try = HAL_GetTick();
}

static void go_offline(void)
{
    out_printf("{\"offline\":{\"addr\":%u}}", g_addr);
    out_flush();
    g_timeouts = 0;
    g_next_try = HAL_GetTick() + APP_RETRY_MS;
    if (g_fixed_target) {
        g_state = ST_READ_INFO;
    } else {
        forget_unit(g_addr);
        g_addr = 0;
        g_state = ST_DISCOVER;
    }
}

static void note_result(geni_err_t e)
{
    if (e == GENI_E_TIMEOUT) {
        if (++g_timeouts >= APP_OFFLINE_TIMEOUTS)
            go_offline();
    } else if (e == GENI_OK || e == GENI_E_NACK) {
        g_timeouts = 0;
    }
}

static void app_step(void)
{
    uint32_t now = HAL_GetTick();
    switch (g_state) {
    case ST_DISCOVER:
        if ((int32_t)(now - g_next_try) < 0)
            break;
        if (g_fixed_target) {
            set_target(g_fixed_target);
            break;
        }
        run_scan(0);
        if (g_m.n_units) {
            set_target(g_m.units[0].addr);
        } else {
            g_next_try = HAL_GetTick() + APP_RETRY_MS;
        }
        break;

    case ST_READ_INFO: {
        if ((int32_t)(now - g_next_try) < 0)
            break;
        geni_err_t e = geni_read_info(&g_m, g_addr, &g_tab);
        print_table_info();
        if (e == GENI_OK) {
            g_state = ST_POLL;
            g_last_poll = HAL_GetTick() - APP_POLL_MS;
            g_timeouts = 0;
        } else {
            print_err("read_info", e, 0);
            g_next_try = HAL_GetTick() + APP_RETRY_MS;
            note_result(e);
        }
        break;
    }

    case ST_POLL:
        if (g_poll_on && now - g_last_poll >= APP_POLL_MS) {
            g_last_poll = now;
            geni_err_t e = geni_poll(&g_m, g_addr, &g_tab);
            g_last_err = e;
            if (g_print)
                print_poll(e);
            report_link(e);
            if (e == GENI_OK)
                report_events();
            if (e == GENI_OK)
                led_toggle();
            note_result(e);
        }
        if (g_state == ST_POLL && !g_fixed_target && APP_RESCAN_MS &&
            now - g_last_scan >= APP_RESCAN_MS)
            run_scan(0); /* finds units added/powered on; polled unit stays quiet */
        break;
    }
}

/* ----------------------------------------------------------------- CLI */

static int parse_u(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    if (s == NULL || *s == '\0')
        return 0;
    unsigned long v = strtoul(s, &end, 0);
    if (*end != '\0' || v > max)
        return 0;
    *out = v;
    return 1;
}

/* Small decimal parser ([-]digits[.digits]), avoids pulling in strtod. */
static int parse_d(const char *s, double *out)
{
    double v = 0.0, scale = 1.0;
    int neg = 0, digits = 0, frac = 0;
    if (s == NULL)
        return 0;
    if (*s == '-' || *s == '+')
        neg = (*s++ == '-');
    for (; *s; s++) {
        if (*s == '.' && !frac) {
            frac = 1;
        } else if (*s >= '0' && *s <= '9') {
            digits++;
            if (frac) {
                scale /= 10.0;
                v += (*s - '0') * scale;
            } else {
                v = v * 10.0 + (*s - '0');
            }
        } else {
            return 0;
        }
    }
    if (!digits)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

static int need_target(void)
{
    if (g_addr)
        return 1;
    out_printf("{\"err\":\"no_target\"}");
    out_flush();
    return 0;
}

static void cli_help(void)
{
    static const char *const lines[] = {
        "commands (numbers accept 0x..):",
        "  scan                     Connection Request discovery",
        "  units                    list discovered units",
        "  target <addr>            fix target (0 = auto/discover), re-read INFO",
        "  get <cls> <id> [n]       raw GET of n consecutive IDs",
        "  info <cls> <id>          INFO of one item (decoded)",
        "  probe <cls> <from> <to>  GET each ID, list the ones the unit knows",
        "  cmd <id>                 class 3 command (IDs from the profile!)",
        "  set <cls> <id> <byte>    raw SET (class 4/5)",
        "  setv <name> <value>      SET a class 4/5 poll item in physical units",
        "  mp204                    MP 204 mode, relays, alarm, alarm log",
        "  mp204 stop               open the motor relay, no alarm (TRIP_A)",
        "  mp204 trip               trip like the Test button (TRIP)",
        "  mp204 reset, then mp204 reset yes   clear AND START THE MOTOR",
        "  str <id>                 class 7 string",
        "  reinfo | poll on|off | stats",
        "  print on|off             JSON line per poll (default off; link changes always shown)",
        "  show                     print the latest poll once",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        con_write(lines[i], strlen(lines[i]));
        con_write("\r\n", 2);
    }
}

static void out_mp204(const char *key, const mp204_status_t *s)
{
    out_printf(",\"%s\":{\"mode\":\"%s\",\"relay1_closed\":%u,\"relay2_closed\":%u,"
               "\"motor_off\":%u,\"ready\":%u,\"pending_alarm\":%u,"
               "\"alarm\":%u,\"alarm_text\":\"%s\",\"raw\":[%u,%u,%u]}",
               key, mp204_mode_str(s), mp204_relay1(s), mp204_relay2(s),
               mp204_motor_off(s), mp204_ready(s), mp204_pending(s),
               s->alarm_code, mp204_alarm_str(s->alarm_code),
               s->act_mode1, s->act_mode2, s->setup);
}

static void mp204_show(void)
{
    static const uint8_t log_ids[] = { 153, 157, 161, 165, 169 };
    uint8_t log[sizeof log_ids], ack;
    mp204_status_t s;
    geni_err_t e = mp204_read(&g_m, g_addr, &s);
    if (e != GENI_OK) {
        print_err("mp204", e, 0);
        return;
    }
    if (s.family != MP204_FAMILY || s.type != MP204_TYPE) {
        out_printf("{\"mp204\":{\"err\":\"not_an_mp204\",\"family\":%u,\"type\":%u}}",
                   s.family, s.type);
        out_flush();
        return;
    }
    out_printf("{\"mp204\":{\"addr\":%u,\"protection\":%u,\"system\":%u,"
               "\"auto_restart\":%u,\"power_on_stop\":%u",
               g_addr, !mp204_unprotected(&s), mp204_system(&s),
               mp204_auto_restart(&s), mp204_power_on_stop(&s));
    out_mp204("state", &s);
    if (geni_get(&g_m, g_addr, GENI_CLASS_MEASURED, log_ids, sizeof log_ids, log,
                 &ack) == GENI_OK) {
        out_printf(",\"log\":[");
        for (size_t k = 0; k < sizeof log; k++)
            out_printf("%s%u", k ? "," : "", log[k]);
        out_printf("]");
    }
    out_printf("}}");
    out_flush();
}

static void mp204_run(mp204_action_t act, const char *name)
{
    mp204_report_t r;
    mp204_result_t res = mp204_command(&g_m, g_addr, act, HAL_Delay, &r);
    out_printf("{\"mp204\":{\"cmd\":\"%s\",\"addr\":%u,\"result\":\"%s\",\"sent\":%s,"
               "\"bus\":\"%s\",\"ack\":%u",
               name, g_addr, mp204_result_str(res), r.sent ? "true" : "false",
               geni_err_str(r.err), r.ack);
    if (r.have_before)
        out_mp204("before", &r.before);
    if (r.have_after)
        out_mp204("after", &r.after);
    out_printf("}}");
    out_flush();
}

/* argc/argv as in cli(). Returns 0 if the arguments make no sense. */
static int cli_mp204(int argc, char **argv)
{
    static uint8_t  armed_addr; /* 0 = reset not armed */
    static uint32_t armed_at;
    const char *act = argc >= 2 ? argv[1] : "";
    int confirm = argc == 3 && !strcmp(act, "reset") && !strcmp(argv[2], "yes");
    uint8_t armed = armed_addr;

    armed_addr = 0; /* any other mp204 command cancels a pending reset */
    if (argc > 2 && !confirm)
        return 0;
    if (argc == 2 && strcmp(act, "stop") && strcmp(act, "trip") && strcmp(act, "reset"))
        return 0;
    if (!need_target())
        return 1;
    if (argc == 1) {
        mp204_show();
    } else if (!strcmp(act, "stop")) {
        mp204_run(MP204_STOP, "stop");
    } else if (!strcmp(act, "trip")) {
        mp204_run(MP204_TRIP, "trip");
    } else if (confirm) {
        if (armed == g_addr && HAL_GetTick() - armed_at <= APP_CONFIRM_MS) {
            mp204_run(MP204_RESET, "reset");
        } else {
            out_printf("{\"mp204\":{\"cmd\":\"reset\",\"result\":\"not_armed\",\"sent\":false}}");
            out_flush();
        }
    } else {
        mp204_show();
        armed_addr = g_addr;
        armed_at = HAL_GetTick();
        out_printf("{\"mp204\":{\"cmd\":\"reset\",\"armed_s\":%u,\"warning\":"
                   "\"MOTOR WILL START. Send: mp204 reset yes\"}}",
                   (unsigned)(APP_CONFIRM_MS / 1000u));
        out_flush();
    }
    return 1;
}

static void cli(char *line)
{
    char *argv[6];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 6; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    if (argc == 0)
        return;

    unsigned long a = 0, b = 0, c = 0;
    uint8_t ack = 0;
    geni_err_t e;
    const char *cmd = argv[0];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        cli_help();
    } else if (!strcmp(cmd, "scan")) {
        run_scan(1);
    } else if (!strcmp(cmd, "units")) {
        for (uint8_t i = 0; i < g_m.n_units; i++)
            print_unit(NULL, &g_m.units[i]);
        out_printf("{\"units\":%u,\"target\":%u}", g_m.n_units, g_addr);
        out_flush();
    } else if (!strcmp(cmd, "target") && argc == 2 && parse_u(argv[1], 231, &a)) {
        g_fixed_target = (uint8_t)a;
        if (a) {
            set_target((uint8_t)a);
        } else {
            g_addr = 0;
            g_state = ST_DISCOVER;
            g_next_try = HAL_GetTick();
        }
        out_printf("{\"target\":%lu}", a);
        out_flush();
    } else if (!strcmp(cmd, "get") && (argc == 3 || argc == 4) &&
               parse_u(argv[1], 15, &a) && parse_u(argv[2], 255, &b) &&
               (argc == 3 || parse_u(argv[3], 62, &c))) {
        if (!need_target())
            return;
        uint8_t n = argc == 4 ? (uint8_t)c : 1u, ids[62], vals[62];
        if (n == 0 || b + n > 256u) {
            print_err("get", GENI_E_BAD_ARG, 0);
            return;
        }
        for (uint8_t k = 0; k < n; k++)
            ids[k] = (uint8_t)(b + k);
        e = geni_get(&g_m, g_addr, (uint8_t)a, ids, n, vals, &ack);
        if (e != GENI_OK) {
            print_err("get", e, ack);
            return;
        }
        out_printf("{\"get\":{\"addr\":%u,\"class\":%lu,\"id\":%lu,\"vals\":[", g_addr, a, b);
        for (uint8_t k = 0; k < n; k++)
            out_printf("%s%u", k ? "," : "", vals[k]);
        out_printf("]}}");
        out_flush();
    } else if (!strcmp(cmd, "info") && argc == 3 && parse_u(argv[1], 15, &a) &&
               parse_u(argv[2], 255, &b)) {
        if (!need_target())
            return;
        uint8_t id = (uint8_t)b;
        geni_info_t inf;
        e = geni_info(&g_m, g_addr, (uint8_t)a, &id, 1, &inf, &ack);
        if (e != GENI_OK)
            print_err("info", e, ack);
        else
            print_info("-", (uint8_t)a, id, &inf);
    } else if (!strcmp(cmd, "probe") && argc == 4 && parse_u(argv[1], 15, &a) &&
               parse_u(argv[2], 255, &b) && parse_u(argv[3], 255, &c) && c >= b) {
        if (!need_target())
            return;
        out_printf("{\"probe\":{\"class\":%lu,\"known\":[", a);
        int first = 1;
        for (unsigned long id = b; id <= c; id++) {
            uint8_t i8 = (uint8_t)id, v;
            e = geni_get(&g_m, g_addr, (uint8_t)a, &i8, 1, &v, &ack);
            if (e == GENI_OK) {
                out_printf("%s[%lu,%u]", first ? "" : ",", id, v);
                first = 0;
                if (s_out_len > sizeof s_out - 64u) { /* flush long lists */
                    out_printf("]}}");
                    out_flush();
                    out_printf("{\"probe\":{\"class\":%lu,\"known\":[", a);
                    first = 1;
                }
            } else if (e != GENI_E_NACK) {
                out_printf("],\"err\":\"%s\",\"at\":%lu}}", geni_err_str(e), id);
                out_flush();
                return;
            }
        }
        out_printf("]}}");
        out_flush();
    } else if (!strcmp(cmd, "cmd") && argc == 2 && parse_u(argv[1], 255, &a)) {
        if (!need_target())
            return;
        uint8_t id = (uint8_t)a;
        e = geni_cmd(&g_m, g_addr, &id, 1, &ack);
        print_err("cmd", e, ack);
    } else if (!strcmp(cmd, "set") && argc == 4 && parse_u(argv[1], 15, &a) &&
               parse_u(argv[2], 255, &b) && parse_u(argv[3], 255, &c)) {
        if (!need_target())
            return;
        if (a != GENI_CLASS_CONFIG && a != GENI_CLASS_REFERENCE) {
            print_err("set", GENI_E_BAD_ARG, 0);
            return;
        }
        uint8_t id = (uint8_t)b, v = (uint8_t)c;
        e = geni_set(&g_m, g_addr, (uint8_t)a, &id, &v, 1, &ack);
        print_err("set", e, ack);
    } else if (!strcmp(cmd, "setv") && argc == 3) {
        double v;
        int idx = geni_table_find(&g_tab, argv[1]);
        if (!need_target())
            return;
        if (idx < 0 || !parse_d(argv[2], &v)) {
            print_err("setv", GENI_E_BAD_ARG, 0);
            return;
        }
        uint8_t raw[4] = { 0 };
        e = geni_set_value(&g_m, g_addr, &g_tab, (uint8_t)idx, v, raw, &ack);
        if (e == GENI_OK) {
            out_printf("{\"setv\":{\"err\":\"ok\",\"name\":\"%s\",\"raw\":[", argv[1]);
            for (uint8_t k = 0; k < g_tab.defs[idx].size; k++)
                out_printf("%s%u", k ? "," : "", raw[k]);
            out_printf("]}}");
            out_flush();
        } else {
            print_err("setv", e, ack);
        }
    } else if (!strcmp(cmd, "str") && argc == 2 && parse_u(argv[1], 255, &a)) {
        if (!need_target())
            return;
        char s[64];
        e = geni_get_string(&g_m, g_addr, (uint8_t)a, s, sizeof s, &ack);
        if (e != GENI_OK) {
            print_err("str", e, ack);
            return;
        }
        out_printf("{\"str\":{\"id\":%lu,\"s\":\"", a);
        for (char *p = s; *p; p++)
            out_printf((*p == '"' || *p == '\\') ? "\\%c" : "%c", *p);
        out_printf("\"}}");
        out_flush();
    } else if (!strcmp(cmd, "mp204") && cli_mp204(argc, argv)) {
        /* handled */
    } else if (!strcmp(cmd, "reinfo")) {
        if (need_target())
            set_target(g_addr);
    } else if (!strcmp(cmd, "poll") && argc == 2 &&
               (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        g_poll_on = !strcmp(argv[1], "on");
        out_printf("{\"poll\":%s}", g_poll_on ? "true" : "false");
        out_flush();
    } else if (!strcmp(cmd, "print") && argc == 2 &&
               (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        g_print = !strcmp(argv[1], "on");
        out_printf("{\"print\":%s}", g_print ? "true" : "false");
        out_flush();
    } else if (!strcmp(cmd, "show")) {
        if (need_target())
            print_poll(g_last_err);
    } else if (!strcmp(cmd, "stats")) {
        out_printf("{\"stats\":{\"requests\":%lu", (unsigned long)g_m.stats.requests);
        for (int k = 0; k < GENI_E_COUNT; k++)
            out_printf(",\"%s\":%lu", geni_err_str((geni_err_t)k),
                       (unsigned long)g_m.stats.err[k]);
        out_printf("}}");
        out_flush();
    } else {
        out_printf("{\"err\":\"bad_cmd\",\"hint\":\"help\"}");
        out_flush();
    }
}

/* ---------------------------------------------------------- clock/boot */

static void clock_init(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };

    /* 8 MHz HSE x 9 = 72 MHz */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        /* No crystal: HSI/2 x 16 = 64 MHz */
        memset(&osc, 0, sizeof osc);
        osc.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE;
        osc.HSEState = RCC_HSE_OFF;
        osc.HSIState = RCC_HSI_ON;
        osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        osc.PLL.PLLState = RCC_PLL_ON;
        osc.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
        osc.PLL.PLLMUL = RCC_PLL_MUL16;
        HAL_RCC_OscConfig(&osc);
    }
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);
}

static void led_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = { 0 };
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET); /* off */
    g.Pin = LED_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);
}

/* Outputs off before anything else. Gates still float from reset until here,
 * so each MOSFET needs its own hardware pulldown. */
static void outputs_safe(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA, OUT_A_PINS, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, OUT_B_PINS, GPIO_PIN_RESET);
    GPIO_InitTypeDef g = { 0 };
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = OUT_A_PINS;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = OUT_B_PINS;
    HAL_GPIO_Init(GPIOB, &g);
}

int main(void)
{
    HAL_Init();
    outputs_safe();
    clock_init();
    led_init();
    console_init();
    geni_port_init();

    geni_master_init(&g_m, GENI_MASTER_ADDR, geni_port_transact, NULL);
    g_tab.defs = g_poll_items;
    g_tab.st = g_st;
    g_tab.n = g_poll_count <= sizeof g_st / sizeof g_st[0]
                  ? g_poll_count
                  : (uint8_t)(sizeof g_st / sizeof g_st[0]);

    out_reset();
    out_printf("{\"boot\":{\"fw\":\"" FW_NAME "\",\"master\":%u,\"target\":%u,"
               "\"items\":%u,\"sysclk\":%lu}}",
               GENI_MASTER_ADDR, GENI_TARGET_ADDR, g_tab.n,
               (unsigned long)SystemCoreClock);
    out_flush();

    g_next_try = HAL_GetTick();
    for (;;) {
        char *line = console_line();
        if (line)
            cli(line);
        app_step();
    }
}

/* ------------------------------------------------------------ handlers */

void SysTick_Handler(void) { HAL_IncTick(); }
void NMI_Handler(void) {}
void HardFault_Handler(void) { for (;;) {} }
void MemManage_Handler(void) { for (;;) {} }
void BusFault_Handler(void) { for (;;) {} }
void UsageFault_Handler(void) { for (;;) {} }
