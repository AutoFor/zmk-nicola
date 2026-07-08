/*
 * zmk-nicola: NICOLA (親指シフト) behavior for ZMK
 *
 * ロジックは eswai/qmk_firmware keyboards/crkbd/keymaps/nicola/nicola.c
 * (Copyright 2018-2019 eswai, GPL-2.0-or-later) のタイマーレス同時押し判定を移植。
 * モジュール構造は eswai/zmk-naginata の behavior 方式に倣う。
 *
 * 原典からの拡張:
 *  - 連続シフト(オプション, continuous-shift): 親指キーの物理押下状態を別管理し、
 *    オンの場合は解決後も押下中はシフトを維持する (既定オフ)
 *  - 親指キー単独タップ: そのキー自身を送出 (既定: 左=Space / 右=変換(INT4)、DTで変更可)
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

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <zmk_nicola/config.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---- 動作ログのストリーム出力 (Web設定ツールの表示用、set log 1 で有効) ---- */
static bool nc_log_on = false;
static nc_log_sink_t nc_log_sink = NULL;

void nc_cfg_set_log_sink(nc_log_sink_t sink) { nc_log_sink = sink; }

static void nc_emit(const char *fmt, ...) {
    char buf[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    LOG_INF("%s", buf);
    if (nc_log_on && nc_log_sink != NULL) {
        strcat(buf, "\n");
        nc_log_sink(buf);
    }
}

/* 親指シフトキー (DTの left-thumb / right-thumb で変更可)。
 * 単独タップ時はそのキー自身を送出する (既定: 左=Space, 右=INT4(変換)) */
static uint32_t nc_lthumb = SPACE;
static uint32_t nc_rthumb = INT4;

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

    /* NICOLA-J規格 D11: @キー 単独=、(読点)。シフト面(゛)はローマ字入力では表現不能のため未割当 */
    {LBKT, ",", "", ""},
};

/* ---- 設定 ---- */
struct behavior_nicola_config {
    int32_t range_pct;
    int32_t cont;
    uint32_t lthumb;
    uint32_t rthumb;
};

/* 連続シフト (DTの continuous-shift, 既定オフ)。オンの場合、親指キーを
 * 押している間は後続の文字キーにもシフトをかけ続ける */
static bool nc_cont = false;

/* 同時打鍵と判定される時間範囲 (%, 1-100)。やまぶきRと同方式:
 * 文字キーの押下時点を0、離す(または次のキーを押す)時点を100として、
 * その前半◯%以内に親指キーが押されたら同時打鍵とみなす。
 * 親指キー先行(連続シフト)には適用しない。DTの range-pct で設定。 */
static int32_t nc_range_pct = 65;

/* ---- 状態 ---- */
#define NC_BUF_MAX 8

static uint32_t nc_buf[NC_BUF_MAX]; /* 未解決の文字キー */
static int64_t nc_last_chr_ts;      /* 最後の文字キー押下時刻 */

/* 文字キー保留中に親指キーが押されたときの判定待ち状態。
 * 判定はインターバル終端(文字キーrelease/次キー押下)で行う */
static uint8_t nc_judge;     /* 0=なし 1=左親指 2=右親指 */
static int64_t nc_judge_ts;  /* 判定待ち親指キーの押下時刻 */
static int nc_chrcount;             /* 文字キーのカウンタ */
static int nc_keycount;             /* 親指キーも含めたカウンタ */
static bool nc_l_held, nc_r_held;   /* 親指キーの物理押下状態 */
static bool nc_l_used, nc_r_used;   /* この押下中に同時打鍵が成立したか */

/* 修飾キー押下中に素通ししたキー (releaseも素通しするため記録) */
static uint32_t nc_raw[NC_BUF_MAX];
static int nc_raw_n;

/* 連続シフトを考慮した実効シフト状態。連続シフトオフの場合、
 * 同時打鍵成立済み(used)の親指は以降の文字にはシフトをかけない */
static bool nc_l_eff(void) { return nc_l_held && (nc_cont || !nc_l_used); }
static bool nc_r_eff(void) { return nc_r_held && (nc_cont || !nc_r_used); }

static void nc_recount(void) {
    nc_keycount = (nc_l_eff() ? 1 : 0) + (nc_r_eff() ? 1 : 0) + nc_chrcount;
}

static const struct nc_map *nc_find(uint32_t key) {
    for (size_t i = 0; i < ARRAY_SIZE(nmap); i++) {
        if (nmap[i].key == key) {
            return &nmap[i];
        }
    }
    return NULL;
}

/* ログ用: キーの単独面ローマ字を識別名として返す */
static const char *nc_name(uint32_t key) {
    const struct nc_map *m = nc_find(key);
    return m ? m->t : "?";
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

/* バッファの文字キーを指定シフト面で確定する。
 * 原典の ncl_type() + ncl_clear() に相当するが、親指キーが物理的に
 * 押されている間はシフト状態(keycount=1)を維持する(連続シフト)。 */
static void nc_type_with(int64_t ts, bool lsh, bool rsh) {
    for (int i = 0; i < nc_chrcount; i++) {
        const struct nc_map *m = nc_find(nc_buf[i]);
        if (m == NULL) {
            continue;
        }
        const char *out = lsh ? m->l : (rsh ? m->r : m->t);
        nc_emit("NC out '%s' plane=%c (key '%s')", out, lsh ? 'L' : (rsh ? 'R' : '-'), m->t);
        nc_send_romaji(out, ts);
    }
    nc_chrcount = 0;
    nc_keycount = (nc_l_eff() ? 1 : 0) + (nc_r_eff() ? 1 : 0);
}

/* 現在の実効シフト状態で確定 */
static void nc_type(int64_t ts) { nc_type_with(ts, nc_l_eff(), nc_r_eff()); }

/* 判定待ちの同時打鍵をインターバル終端 end_ts で確定する。
 * やまぶきR方式: 文字キー押下(0)〜終端(100)のうち、親指キー押下位置が
 * range-pct% 以内なら同時打鍵、外れたら文字キーは単独打ちで確定
 * (親指はそのまま保持され、以降の文字への連続シフトとして生きる)。 */
static void nc_judge_resolve(int64_t end_ts) {
    if (nc_judge == 0) {
        return;
    }
    const uint8_t judge = nc_judge;
    nc_judge = 0;
    if (nc_chrcount == 0) {
        return;
    }
    const int64_t total = end_ts - nc_last_chr_ts;
    const int64_t pos = nc_judge_ts - nc_last_chr_ts;
    const bool simul = (total <= 0) || (pos * 100 <= total * nc_range_pct);
    nc_emit("NC judge %cthumb: pos=%dms / total=%dms = %d%% (range=%d%%) -> %s",
            judge == 1 ? 'L' : 'R', (int32_t)pos, (int32_t)total,
            total > 0 ? (int32_t)(pos * 100 / total) : 0, nc_range_pct,
            simul ? "SIMUL" : "LATE(single)");
    if (simul) {
        nc_type_with(end_ts, judge == 1, judge == 2);
        if (judge == 1) {
            nc_l_used = true;
        } else {
            nc_r_used = true;
        }
        nc_recount();
    } else {
        nc_type_with(end_ts, false, false);
    }
}

/* 保留中をすべて吐き出す (判定待ちがあれば判定してから) */
static void nc_flush(int64_t ts) {
    if (nc_judge != 0) {
        nc_judge_resolve(ts);
    } else {
        nc_type(ts);
    }
}

static void nc_reset(void) {
    nc_chrcount = 0;
    nc_keycount = 0;
    nc_l_held = nc_r_held = false;
    nc_l_used = nc_r_used = false;
    nc_raw_n = 0;
    nc_judge = 0;
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
        nc_flush(ts); /* 未解決分を吐き出してから */
        if (nc_raw_n < NC_BUF_MAX) {
            nc_raw[nc_raw_n++] = key;
        }
        raise_zmk_keycode_state_changed_from_encoded(key, true, ts);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (key == nc_lthumb) {
        nc_l_held = true;
        nc_l_used = false;
        nc_keycount++;
        if (nc_chrcount > 0 && nc_judge == 0) {
            /* 文字キー保留中: 同時打鍵かどうかの判定はインターバル終端まで保留 */
            nc_judge = 1;
            nc_judge_ts = ts;
            nc_emit("NC Lthumb dn: +%dms after char, judge pending", (int32_t)(ts - nc_last_chr_ts));
        } else {
            nc_emit("NC Lthumb dn (no pending char)");
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (key == nc_rthumb) {
        nc_r_held = true;
        nc_r_used = false;
        nc_keycount++;
        if (nc_chrcount > 0 && nc_judge == 0) {
            nc_judge = 2;
            nc_judge_ts = ts;
            nc_emit("NC Rthumb dn: +%dms after char, judge pending", (int32_t)(ts - nc_last_chr_ts));
        } else {
            nc_emit("NC Rthumb dn (no pending char)");
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (nc_find(key) != NULL) {
        if (nc_judge != 0) { /* 次キー押下 = 前の文字のインターバル終端 */
            nc_judge_resolve(ts);
        }
        if (nc_chrcount < NC_BUF_MAX) {
            nc_buf[nc_chrcount++] = key;
        }
        nc_emit("NC dn '%s' pend=%d L=%d R=%d", nc_name(key), nc_chrcount, (int)nc_l_held,
                (int)nc_r_held);
        nc_last_chr_ts = ts;
        nc_keycount++;
        if (nc_keycount > 1) { /* 2打目以降は即時確定 (親指先行ならシフト) */
            nc_type(ts);
            if (nc_l_held) {
                nc_l_used = true;
            }
            if (nc_r_held) {
                nc_r_used = true;
            }
            nc_recount();
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* テーブル外のキーは素通し */
    nc_flush(ts);
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

    if (key == nc_lthumb) {
        if (nc_judge == 1) {
            /* 文字キーより先に親指が離れた = 押下が完全に重なっている → 同時打鍵 */
            nc_emit("NC Lthumb up before char up -> full overlap, SIMUL");
            nc_type_with(ts, true, false);
            nc_l_used = true;
            nc_judge = 0;
        }
        bool tap = !nc_l_used && nc_chrcount == 0;
        nc_l_held = false;
        nc_l_used = false;
        if (tap) {
            nc_emit("NC Lthumb up -> solo tap");
            nc_tap(nc_lthumb, ts); /* 単独タップ = そのキー自身 (既定Space) */
        } else if (nc_chrcount > 0) {
            nc_type(ts);
        }
        nc_keycount = (nc_r_eff() ? 1 : 0) + nc_chrcount;
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (key == nc_rthumb) {
        if (nc_judge == 2) {
            nc_emit("NC Rthumb up before char up -> full overlap, SIMUL");
            nc_type_with(ts, false, true);
            nc_r_used = true;
            nc_judge = 0;
        }
        bool tap = !nc_r_used && nc_chrcount == 0;
        nc_r_held = false;
        nc_r_used = false;
        if (tap) {
            nc_emit("NC Rthumb up -> solo tap");
            nc_tap(nc_rthumb, ts); /* 単独タップ = そのキー自身 (既定変換) */
        } else if (nc_chrcount > 0) {
            nc_type(ts);
        }
        nc_keycount = (nc_l_eff() ? 1 : 0) + nc_chrcount;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (nc_find(key) != NULL) {
        if (nc_judge != 0) { /* 文字キーrelease = インターバル終端: ここで判定 */
            nc_judge_resolve(ts);
        } else if (nc_chrcount > 0) { /* 単独打鍵: releaseで確定 */
            nc_type(ts);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_nicola_init(const struct device *dev) {
    const struct behavior_nicola_config *cfg = dev->config;
    nc_range_pct = CLAMP(cfg->range_pct, 1, 100);
    nc_cont = cfg->cont != 0;
    nc_lthumb = cfg->lthumb;
    nc_rthumb = cfg->rthumb;
    nc_reset();
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* ZMK Studio 用パラメータ一覧。ラベルは「単独 左親指 右親指 (物理キー)」。
 * nmap の全キー + 親指シフトキー2つを網羅すること
 * (リストにない値は Studio が不明値として扱うため)。 */
#define NC_VAL(_label, _key)                                                                       \
    { .display_name = _label, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = _key }

static const struct behavior_parameter_value_metadata param_values[] = {
    /* 上段 */
    NC_VAL("。 ぁ − (Q)", Q),
    NC_VAL("か え が (W)", W),
    NC_VAL("た り だ (E)", E),
    NC_VAL("こ ゃ ご (R)", R),
    NC_VAL("さ れ ざ (T)", T),
    NC_VAL("ら ぱ よ (Y)", Y),
    NC_VAL("ち ぢ に (U)", U),
    NC_VAL("く ぐ る (I)", I),
    NC_VAL("つ づ ま (O)", O),
    NC_VAL("、 ぴ ぇ (P)", P),
    /* 中段 */
    NC_VAL("う を ゔ (A)", A),
    NC_VAL("し あ じ (S)", S),
    NC_VAL("て な で (D)", D),
    NC_VAL("け ゅ げ (F)", F),
    NC_VAL("せ も ぜ (G)", G),
    NC_VAL("は ば み (H)", H),
    NC_VAL("と ど お (J)", J),
    NC_VAL("き ぎ の (K)", K),
    NC_VAL("い ぽ ょ (L)", L),
    NC_VAL("ん − っ (;)", SEMI),
    /* 下段 */
    NC_VAL("。 ぅ − (Z)", Z),
    NC_VAL("ひ ー び (X)", X),
    NC_VAL("す ろ ず (C)", C),
    NC_VAL("ふ や ぶ (V)", V),
    NC_VAL("へ ぃ べ (B)", B),
    NC_VAL("め ぷ ぬ (N)", N),
    NC_VAL("そ ぞ ゆ (M)", M),
    NC_VAL("ね ぺ む (,)", COMMA),
    NC_VAL("ほ ぼ わ (.)", DOT),
    NC_VAL("／ ？ ぉ (/)", SLASH),
    NC_VAL("、 − − (@)", LBKT),
    /* 親指シフトキー */
    NC_VAL("左親指シフト (Space)", SPACE),
    NC_VAL("右親指シフト (変換)", INT4),
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

/* ---- 実行時設定 (USBシリアル設定コンソール / 永続化) ---- */

void nc_cfg_get(struct nc_settings *out) {
    out->timeout_ms = -1; /* %方式ブランチでは未使用 */
    out->range_pct = nc_range_pct;
    out->cont = nc_cont;
    out->log = nc_log_on;
}

int nc_cfg_set(const char *key, int32_t value) {
    if (strcmp(key, "range") == 0) {
        nc_range_pct = CLAMP(value, 1, 100);
#if IS_ENABLED(CONFIG_SETTINGS)
        settings_save_one("nicola/rng", &nc_range_pct, sizeof(nc_range_pct));
#endif
        nc_emit("NC cfg: range=%d%% (saved)", nc_range_pct);
        return 0;
    }
    if (strcmp(key, "cont") == 0) {
        nc_cont = value != 0;
#if IS_ENABLED(CONFIG_SETTINGS)
        uint8_t v = nc_cont ? 1 : 0;
        settings_save_one("nicola/cnt", &v, sizeof(v));
#endif
        nc_emit("NC cfg: cont=%d (saved)", (int)nc_cont);
        return 0;
    }
    if (strcmp(key, "log") == 0) { /* 動作ログのストリーム (非永続) */
        nc_log_on = value != 0;
        return 0;
    }
    return -EINVAL;
}

void nc_cfg_reset(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_delete("nicola/rng");
    settings_delete("nicola/cnt");
#endif
    nc_emit("NC cfg: saved settings cleared");
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int nicola_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (settings_name_steq(name, "rng", NULL) && len == sizeof(nc_range_pct)) {
        read_cb(cb_arg, &nc_range_pct, len);
        nc_range_pct = CLAMP(nc_range_pct, 1, 100);
        nc_emit("NC cfg loaded: range=%d%%", nc_range_pct);
        return 0;
    }
    if (settings_name_steq(name, "cnt", NULL) && len == 1) {
        uint8_t v;
        read_cb(cb_arg, &v, 1);
        nc_cont = v != 0;
        nc_emit("NC cfg loaded: cont=%d", (int)nc_cont);
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(nicola, "nicola", NULL, nicola_settings_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

static const struct behavior_driver_api behavior_nicola_driver_api = {
    .binding_pressed = on_nicola_pressed,
    .binding_released = on_nicola_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define NC_INST(n)                                                                                 \
    static const struct behavior_nicola_config behavior_nicola_config_##n = {                      \
        .range_pct = DT_INST_PROP(n, range_pct),                                                   \
        .cont = DT_INST_PROP(n, continuous_shift),                                                 \
        .lthumb = DT_INST_PROP_OR(n, left_thumb, SPACE),                                           \
        .rthumb = DT_INST_PROP_OR(n, right_thumb, INT4),                                           \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_nicola_init, NULL, NULL, &behavior_nicola_config_##n,      \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_nicola_driver_api);

DT_INST_FOREACH_STATUS_OKAY(NC_INST)
