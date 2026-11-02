# [MoGRU - Neural Language Model Integrated Mozc](https://github.com/lambda-haru/MoGRU)

[![Linux](https://github.com/lambda-haru/MoGRU/actions/workflows/linux.yaml/badge.svg)](https://github.com/lambda-haru/MoGRU/actions/workflows/linux.yaml)
[![macOS](https://github.com/lambda-haru/MoGRU/actions/workflows/macos.yaml/badge.svg)](https://github.com/lambda-haru/MoGRU/actions/workflows/macos.yaml)
[![Windows](https://github.com/lambda-haru/MoGRU/actions/workflows/windows.yaml/badge.svg)](https://github.com/lambda-haru/MoGRU/actions/workflows/windows.yaml)
[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](LICENSE)

**MoGRU** は、オープンソースかな漢字変換エンジン [Mozc](https://github.com/google/mozc) に、**文字単位 GRU ニューラル言語モデル（GRU Scorer）** と **リアルタイム・ライブ変換** を統合した日本語入力システムです。

---

## 主な特徴

- **ライブ変換**: 入力と同時にリアルタイムでインライン自動変換。スペースキーを押す手間なく思考の流れのままタイピング可能。
- **GRU言語モデルによる高精度変換**: 前後の文脈を文字単位GRUモデル（大域的ビームサーチ $B=8$）で評価し、文脈に応じた適切な同音異義語を自動選択。
- **インプロセス・オンライン継続学習**: 変換確定履歴からリアルタイム（< 1ms）に重みを自己微調整。一般的な日本語理解を保ったまま、ユーザー固有の言い回しや好みに自動適応。
- **高精度モデル同梱**: 超軽量な標準モデル（$H=128$）に加え、表現力豊かな高精度モデル（$H=256$）を同梱。
- **GUI設定 & 独立動作**: システム標準の Mozc と共存可能。専用GUI設定画面から各種パラメータやモデルを再起動なしで調整可能。
- **マルチプラットフォーム**: Linux (Fcitx5), macOS, Windows をサポート。

---

## インストール

[Releases](https://github.com/lambda-haru/MoGRU/releases) からお使いの OS に合わせたインストーラをダウンロードして実行してください（ビルド不要・言語モデル同梱）。

### Windows
`MoGRU-windows-x64.msi`（ARM64 環境は `MoGRU-windows-arm64.msi`）を実行してインストールしてください（または `MoGRU-windows-*.zip` を展開して `install.ps1` を実行することも可能です）。

### macOS
`MoGRU-macOS-arm64.pkg` を実行してインストールしてください（または `MoGRU-macOS-arm64.zip` を展開して `install.sh` を実行することも可能です）。

### Linux (Fcitx5)
`MoGRU-linux.zip` を展開し、付属の `install.sh` を実行してください（`sudo` 不要・`~/.local` に導入されます）。
```bash
unzip MoGRU-linux.zip
cd MoGRU-linux
./install.sh
```

<details>
<summary>ソースコードから自前ビルドする場合（開発者向け）</summary>

```bash
# Linux (Fcitx5)
cd src
bazelisk build -c opt //unix/fcitx5:fcitx5-mogru.so //gui/tool:mozc_tool --//unix/fcitx5:use_server=False

# macOS
cd src
bazelisk build package --config release_build

# Windows (MSVC)
cd src
bazelisk build package --config release_build
```
</details>

---

## 設定 & モデル切り替え

GUI設定ツール（Linux: Fcitx5設定、macOS: 環境設定、Windows: プロパティ）の「**MoGRU**」タブから設定できます。

- **ライブ変換**: ON / OFF の切り替え
- **高精度モデル ($H=256$) への切り替え**:
  「モデルファイル設定」で以下をセットで指定（再起動不要で即座に反映）：
  - ベースモデル: `grumodel_new.bin`
  - 文字辞書: `chardata_new.bin`
  *(モデルファイルはインストール時に各 OS の設定ディレクトリへ自動配置されます)*

詳細な学習パラメータ等は `~/.config/mogru/gru_config.ini`（Windows: `%USERPROFILE%\AppData\LocalLow\MoGRU\gru_config.ini`）からカスタマイズ可能です。

---

## ライセンス & 謝辞

- 本プロジェクトは [Google Mozc](https://github.com/google/mozc) をベースにしています。
- **MoGRU 拡張コード**: Copyright (c) 2026 Haruhiro Obora ([BSD 3-Clause](LICENSE))
- **Mozc オリジナルコード**: Google LLC ([BSD 3-Clause](LICENSE))
- **Fcitx5 モジュール**: CSSlayer and The Fcitx Team ([LGPL-2.1+ / GPL-2.0+](src/unix/fcitx5/org.fcitx.Fcitx5.Addon.Mozc.metainfo.xml.in))
