# zmk-nicola

NICOLA（親指シフト）入力を実現する ZMK behavior モジュール。

- ロジック: [eswai/qmk_firmware](https://github.com/eswai/qmk_firmware) の
  `keyboards/crkbd/keymaps/nicola/nicola.c`（タイマーレス同時押し判定）を移植
- モジュール構造: [eswai/zmk-naginata](https://github.com/eswai/zmk-naginata) の behavior 方式に倣う
- 前提: IME はローマ字入力モード。ファームは解決後のローマ字キー列を送出する（例: か → `k` `a`）

## 原典からの拡張

1. **連続シフト**: 親指キーの物理押下状態を別管理し、同時打鍵の解決後も
   押し続けている間はシフト状態を維持する
2. **親指キー単独タップ**: 左親指 = Space / 右親指 = 変換(INT4) を送出
3. **修飾キー素通し**: Ctrl/Alt/GUI/Shift 押下中は NICOLA 処理をせず素のキーを送出
   （NICOLA モードのままショートカットが使える）

## 使い方

### west.yml

```yaml
manifest:
  remotes:
    - name: AutoFor
      url-base: https://github.com/AutoFor
  projects:
    - name: zmk-nicola
      remote: AutoFor
      revision: main
      clone-depth: 1
```

### keymap

```dts
#include <behaviors/nicola.dtsi>

// NICOLAレイヤーの文字キーを &nc <キーコード> にバインドする
// 左親指シフト = &nc SPACE / 右親指シフト = &nc INT4
nicola_layer {
    bindings = <
        ... &nc Q  &nc W  &nc E ...
        ... &nc SPACE ... &nc INT4 ...
    >;
};
```

レイヤーの切替（`&tog` 等）はキーマップ側で行う。

## ライセンス

GPL-2.0-or-later（原典 nicola.c のライセンスを継承）
