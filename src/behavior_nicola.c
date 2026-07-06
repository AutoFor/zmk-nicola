/*
 * zmk-nicola: NICOLA (親指シフト) behavior for ZMK
 *
 * ロジックは eswai/qmk_firmware keyboards/crkbd/keymaps/nicola/nicola.c
 * (Copyright 2018-2019 eswai, GPL-2.0-or-later) のタイマーレス同時押し判定を移植。
 * モジュール構造は eswai/zmk-naginata の behavior 方式に倣う。
 *
 * 原典からの拡張:
 *  - 連続シフト: 親指キーの物理押下状態を別管理し、解決後も押下中はシフトを維持する
 *  - 親指キー単独タップ: 左=Space / 右=変換(INT4) を送出
 *  - Ctrl/Alt/GUI 押下中は素のキーコードを送出（ショートカット素通し）
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DT_DRV_COMPAT zmk_behavior_nicola

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 親指シフトキーとして扱うパラメータ */
#define NC_LTHUMB SPACE /* 左親指: 単独タップで Space */
#define NC_RTHUMB INT4  /* 右親指: 単独タップで 変換 */

/* NICOLA 配列テーブル (JIS / IMEローマ字入力想定) — nicola.c の nmap を移植 */
struct nc_map {
    uint32_t key;
    const char *t; /* 単独 */
    const char *l; /* 左親指シフト */
    const char *r; /* 右親指シフト */
};

static const struct nc_map nmap[] = {
    {Q, ".", "la", ""},      {W, "ka", "e", "ga"},    {E, "ta", "ri", "da"},
    {R, "ko", "lya", "go"},  {T, "sa", "re", "za"},

    {Y, "ra", "pa", "yo"},   {U, "ti", "di", "ni"},   {I, "ku", "gu", "ru"},
    {O, "tu", "du", "ma"},   {P, ",", "pi", "le"},

    {A, "u", "wo", "vu"},    {S, "si", "a", "zi"},    {D, "te", "na", "de"},
    {F, "ke", "lyu", "ge"},  {G, "se", "mo", "ze"},

    {H, "ha", "ba", "mi"},   {J, "to", "do", "o"},    {K, "ki", "gi", "no"},
    {L, "i", "po", "lyo"},   {SEMI, "nn", "", "ltu"},

    {Z, ".", "lu", ""},      {X, "hi", "-", "bi"},    {C, "su", "ro", "zu"},
    {V, "hu", "ya", "bu"},   {B, "he", "li", "be"},

    {N, "me", "pu", "nu"},   {M, "so", "zo", "yu"},   {COMMA, "ne", "pe", "mu"},
    {DOT, "ho", "bo", "wa"}, {SLASH, "/", "?", "lo"},
};

/* ---- 状態 ---- */
#define NC_BUF_MAX 8

static uint32_t nc_buf[NC_BUF_MAX]; /* 未解決の文字キー */
static int nc_chrcount;             /* 文字キーのカウンタ */
static int nc_keycount;             /* 親指キーも含めたカウンタ */
static bool nc_l_held, nc_r_held;   /* 親指キーの物理押下状態 */
static bool nc_l_used, nc_r_used;   /* この押下中に同時打鍵が成立したか */

/* 修飾キー押下中に素通ししたキー (releaseも素通しするため記録) */
static uint32_t nc_raw[NC_BUF_MAX];
static int nc_raw_n;

static const struct nc_map *nc_find(uint32_t key) {
    for (size_t i = 0; i < ARRAY_SIZE(nmap); i++) {
        if (nmap[i].key == key) {
            return &nmap[i];
        }
    }
    return NULL;
}

static void nc_tap(uint32_t encoded, int64_t ts) {
    raise_zmk_keycode_state_changed_from_encoded(encoded, true, ts);
    raise_zmk_keycode_state_changed_from_encoded(encoded, false, ts);
}

/* 解決済みローマ字列をHID送出 */
static void nc_send_romaji(const char *s, int64_t ts) {
    for (; *s != '\0'; s++) {
        uint32_t kc;
        switch (*s) {
        case '-':
            kc = MINUS;
            break;
        case ',':
            kc = COMMA;
            break;
        case '.':
            kc = DOT;
            break;
        case '/':
            kc = SLASH;
            break;
        case '?':
            kc = LS(SLASH); /* JIS: Shift+/ = ? */
            break;
        default:
            if (*s < 'a' || *s > 'z') {
                continue;
            }
            kc = A + (*s - 'a');
            break;
        }
        nc_tap(kc, ts);
    }
}

/* バッファの文字キーを現在のシフト状態で確定する。
 * 原典の ncl_type() + ncl_clear() に相当するが、親指キーが物理的に
 * 押されている間はシフト状態(keycount=1)を維持する(連続シフト)。 */
static void nc_type(int64_t ts) {
    for (int i = 0; i < nc_chrcount; i++) {
        const struct nc_map *m = nc_find(nc_buf[i]);
        if (m == NULL) {
            continue;
        }
        if (nc_l_held) {
            nc_send_romaji(m->l, ts);
        } else if (nc_r_held) {
            nc_send_romaji(m->r, ts);
        } else {
            nc_send_romaji(m->t, ts);
        }
    }
    nc_chrcount = 0;
    nc_keycount = (nc_l_held || nc_r_held) ? 1 : 0;
}

static void nc_reset(void) {
    nc_chrcount = 0;
    nc_keycount = 0;
    nc_l_held = nc_r_held = false;
    nc_l_used = nc_r_used = false;
    nc_raw_n = 0;
}

static bool nc_mods_active(void) {
    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    return mods != 0;
}

static int on_nicola_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    const uint32_t key = binding->param1;
    const int64_t ts = event.timestamp;

    /* Ctrl/Alt/GUI/Shift 押下中はNICOLA処理せず素通し */
    if (nc_mods_active()) {
        nc_type(ts); /* 未解決分を吐き出してから */
        if (nc_raw_n < NC_BUF_MAX) {
            nc_raw[nc_raw_n++] = key;
        }
        raise_zmk_keycode_state_changed_from_encoded(key, true, ts);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (key == NC_LTHUMB) {
        nc_l_held = true;
        nc_l_used = false;
        nc_keycount++;
        if (nc_keycount > 1) { /* 保留中の文字キーと同時打鍵成立 */
            nc_type(ts);
            nc_l_used = true;
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (key == NC_RTHUMB) {
        nc_r_held = true;
        nc_r_used = false;
        nc_keycount++;
        if (nc_keycount > 1) {
            nc_type(ts);
            nc_r_used = true;
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (nc_find(key) != NULL) {
        if (nc_chrcount < NC_BUF_MAX) {
            nc_buf[nc_chrcount++] = key;
        }
        nc_keycount++;
        if (nc_keycount > 1) { /* 2打目以降は即時確定 (シフト中なら連続シフト) */
            nc_type(ts);
            if (nc_l_held) {
                nc_l_used = true;
            }
            if (nc_r_held) {
                nc_r_used = true;
            }
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* テーブル外のキーは素通し */
    nc_type(ts);
    if (nc_raw_n < NC_BUF_MAX) {
        nc_raw[nc_raw_n++] = key;
    }
    raise_zmk_keycode_state_changed_from_encoded(key, true, ts);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_nicola_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const uint32_t key = binding->param1;
    const int64_t ts = event.timestamp;

    /* 素通し中のキーはreleaseも素通し */
    for (int i = 0; i < nc_raw_n; i++) {
        if (nc_raw[i] == key) {
            for (int j = i; j < nc_raw_n - 1; j++) {
                nc_raw[j] = nc_raw[j + 1];
            }
            nc_raw_n--;
            raise_zmk_keycode_state_changed_from_encoded(key, false, ts);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }

    if (key == NC_LTHUMB) {
        bool tap = !nc_l_used && nc_chrcount == 0;
        nc_l_held = false;
        nc_l_used = false;
        if (tap) {
            nc_tap(SPACE, ts); /* 単独タップ = Space */
        } else if (nc_chrcount > 0) {
            nc_type(ts);
        }
        nc_keycount = (nc_r_held ? 1 : 0) + nc_chrcount;
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (key == NC_RTHUMB) {
        bool tap = !nc_r_used && nc_chrcount == 0;
        nc_r_held = false;
        nc_r_used = false;
        if (tap) {
            nc_tap(INT4, ts); /* 単独タップ = 変換 */
        } else if (nc_chrcount > 0) {
            nc_type(ts);
        }
        nc_keycount = (nc_l_held ? 1 : 0) + nc_chrcount;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (nc_find(key) != NULL) {
        if (nc_chrcount > 0) { /* 単独打鍵: releaseで確定 */
            nc_type(ts);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_nicola_init(const struct device *dev) {
    nc_reset();
    return 0;
}

static const struct behavior_driver_api behavior_nicola_driver_api = {
    .binding_pressed = on_nicola_pressed,
    .binding_released = on_nicola_released,
};

#define NC_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_nicola_init, NULL, NULL, NULL, POST_KERNEL,                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_nicola_driver_api);

DT_INST_FOREACH_STATUS_OKAY(NC_INST)
