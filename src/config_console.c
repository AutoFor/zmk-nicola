/*
 * zmk-nicola: USBシリアル設定コンソール
 *
 * キーボード側dtsで chosen "zmk,nicola-cfg-uart" にCDC-ACMノードを指定すると
 * 有効になる。行単位のテキストプロトコル:
 *   get                 -> ok timeout=50 range=-1 cont=0
 *   set timeout 60      -> ok timeout=60   (即時反映+フラッシュ保存)
 *   set range 65        -> ok range=65
 *   set cont 1          -> ok cont=1
 *   reset               -> ok reset        (保存値を消去、再起動で既定値)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_CHOSEN(zmk_nicola_cfg_uart) && IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

#include <zmk_nicola/config.h>

static const struct device *cfg_uart = DEVICE_DT_GET(DT_CHOSEN(zmk_nicola_cfg_uart));

#define LINE_MAX 64
static char rx_line[LINE_MAX];
static size_t rx_len;
static char pending_line[LINE_MAX];

static void respond(const char *s) {
    for (; *s != '\0'; s++) {
        uart_poll_out(cfg_uart, *s);
    }
}

static void handle_line(struct k_work *work) {
    ARG_UNUSED(work);
    char buf[96];
    char *saveptr = NULL;
    char *cmd = strtok_r(pending_line, " \t", &saveptr);
    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "get") == 0) {
        struct nc_settings s;
        nc_cfg_get(&s);
        snprintf(buf, sizeof(buf), "ok timeout=%d range=%d mode=%d cont=%d log=%d\n", s.timeout_ms,
                 s.range_pct, s.mode, (int)s.cont, (int)s.log);
        respond(buf);
    } else if (strcmp(cmd, "set") == 0) {
        char *key = strtok_r(NULL, " \t", &saveptr);
        char *val = strtok_r(NULL, " \t", &saveptr);
        if (key == NULL || val == NULL) {
            respond("err usage: set <timeout|range|mode|cont|log> <value>\n");
            return;
        }
        if (nc_cfg_set(key, atoi(val)) == 0) {
            struct nc_settings s;
            nc_cfg_get(&s);
            snprintf(buf, sizeof(buf), "ok timeout=%d range=%d mode=%d cont=%d log=%d\n", s.timeout_ms,
                     s.range_pct, s.mode, (int)s.cont, (int)s.log);
            respond(buf);
        } else {
            respond("err unsupported key on this firmware\n");
        }
    } else if (strcmp(cmd, "reset") == 0) {
        nc_cfg_reset();
        respond("ok reset (reboot to load keymap defaults)\n");
    } else {
        respond("err commands: get / set <key> <val> / reset\n");
    }
}

static K_WORK_DEFINE(line_work, handle_line);

static void uart_cb(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t c;
        while (uart_fifo_read(dev, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (rx_len > 0) {
                    rx_line[rx_len] = '\0';
                    memcpy(pending_line, rx_line, rx_len + 1);
                    rx_len = 0;
                    k_work_submit(&line_work);
                }
            } else if (rx_len < LINE_MAX - 1) {
                rx_line[rx_len++] = (char)c;
            }
        }
    }
}

/* 動作ログ (set log 1) の出力先としてこのCDCを登録する */
static void console_log_sink(const char *line) { respond(line); }

static int nicola_console_init(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    /* 保存済み設定を反映 (keymap既定値の後に上書き) */
    settings_load_subtree("nicola");
#endif
    if (!device_is_ready(cfg_uart)) {
        LOG_WRN("NICOLA cfg uart not ready");
        return 0;
    }
    uart_irq_callback_set(cfg_uart, uart_cb);
    uart_irq_rx_enable(cfg_uart);
    nc_cfg_set_log_sink(console_log_sink);
    LOG_INF("NICOLA config console ready");
    return 0;
}

SYS_INIT(nicola_console_init, APPLICATION, 99);

#endif /* DT_HAS_CHOSEN(zmk_nicola_cfg_uart) && CONFIG_UART_INTERRUPT_DRIVEN */
