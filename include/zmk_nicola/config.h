/*
 * zmk-nicola: 実行時設定API (USBシリアル設定コンソール用)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

struct nc_settings {
    int32_t timeout_ms; /* ms方式の判定窓 */
    int32_t range_pct;  /* %方式の判定範囲 */
    int32_t mode;       /* 判定方式: 0=ms方式 1=%方式(やまぶきR) */
    bool cont;          /* 連続シフト */
    bool log;           /* 動作ログのストリーム出力 (非永続) */
};

void nc_cfg_get(struct nc_settings *out);

/* key: "timeout" / "range" / "cont"。成功=0、未対応キー=-EINVAL。
 * 値は即時反映され、フラッシュに永続化される(再起動後も維持) */
int nc_cfg_set(const char *key, int32_t value);

/* 保存済み設定を削除し、次回起動時にkeymapの既定値へ戻す */
void nc_cfg_reset(void);

/* 動作ログの出力先 (設定コンソールが登録する)。lineは改行込み */
typedef void (*nc_log_sink_t)(const char *line);
void nc_cfg_set_log_sink(nc_log_sink_t sink);
