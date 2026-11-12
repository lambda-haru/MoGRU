# [MoGRU - Neural Language Model Integrated Mozc](https://github.com/lambda-haru/MoGRU)

[![Linux](https://github.com/lambda-haru/MoGRU/actions/workflows/linux.yaml/badge.svg)](https://github.com/lambda-haru/MoGRU/actions/workflows/linux.yaml)
[![macOS](https://github.com/lambda-haru/MoGRU/actions/workflows/macos.yaml/badge.svg)](https://github.com/lambda-haru/MoGRU/actions/workflows/macos.yaml)
[![Windows](https://github.com/lambda-haru/MoGRU/actions/workflows/windows.yaml/badge.svg)](https://github.com/lambda-haru/MoGRU/actions/workflows/windows.yaml)
[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](LICENSE)

**MoGRU** は、オープンソースかな漢字変換エンジン [Mozc](https://github.com/google/mozc) に、**文字単位 GRU ニューラル言語モデル（GRU Scorer）** と **リアルタイム・ライブ変換** を統合した日本語入力システムです。

---

## 主な特徴

- **ライブ変換 & 高度な入力制御**: 入力と同時にリアルタイムでインライン自動変換。長文入力時の**前文節固定**（計算量抑制）、**段階的自動確定（Auto-Commit）**、**末尾ひらがな除外（遅延変換）**、文節区切り記号 `/` の表示にも対応しています。
- **GRU 言語モデルによる高精度変換**: 前後の文脈や先行入力テキスト（最大32文字）を文字単位 GRU モデル（大域的ビームサーチ $B=8$）で評価し、同音異義語選択・**文節区切り再編成（Resegmentation）**・候補変更時の**後続文節インタラクティブ再評価**を行います。
- **インプロセス・オンライン継続学習 & インスタント速習モード**: 変換確定履歴からリアルタイム（< 1ms）に重みを微調整。L2アンカー正則化による忘却防止に加え、1回の確定で即座にトップ候補へ学習させる**インスタント速習モード**や、直後の取消操作による**学習ロールバック（Revert）**を備えています。
- **文脈プロファイル & アプリ連動自動切り替え**: 用途別の6種類の組み込みプロファイル（`default` / `business` / `coding` / `casual` / `creative` / `instant`）に加え、**プロファイルの自由な新規追加・複製・削除**に対応。プロファイルごとに GRU 重みだけでなく Mozc の予測・変換・文節境界履歴 DB も完全に分離され、アクティブアプリに応じて自動で切り替わります。
- **インラインコマンド候補**: 通常変換中にキーワード（`ぷろふぁいる` / `もぐら` / `いんすたんと` 等）を変換するだけで、プロファイル切替・アプリ紐付け・インスタント速習モードの ON/OFF をキーボード操作のみで完結できます。
- **英語自動フォールバック**: 日本語モードのまま英単語（例: `mergesuru` → `merge` + `する`）を入力した場合でも、内蔵英単語辞書とユーザー登録単語から英単語候補を自動復元します（ライブ変換中も動作）。
- **高精度モデル同梱 & SIMD / FP16 高速化**: 超軽量な標準モデル（$H=128$）に加え、高精度モデル（$H=256$）を同梱。x86_64（AVX2 + FMA）および ARM64（NEON + FP16）向け最適化により低遅延で推論します。
- **変換統計・マルチプラットフォーム・独立共存**: ライブ変換 / Top-1 正確率などの統計計測、macOS 専用 SwiftUI UI、Linux（Fcitx5）サーバーレス直結動作に対応し、システム標準の Mozc とも設定・バイナリ単位で完全に共存可能です。

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

### GUI 設定ツール（3タブ構成）

GUI 設定ツール（Linux: Fcitx5 設定、macOS: 環境設定、Windows: プロパティ）には、MoGRU 専用の 3 つのタブがあります（再起動不要で即座に反映）。

1. **MoGRU設定** (`MoGRU`): ライブ変換の ON/OFF と詳細動作、プロファイルの選択・追加・削除・アプリ連動設定、ユーザー追加英単語、モデルファイル指定、統計閲覧・リセット
2. **ビームサーチ・ペナルティ** (`Beam Search & Penalty`): GRU スコア重み、文字種別ペナルティ、文節再分割、英語フォールバック閾値、GRU スコア表示切替
3. **モデル・学習** (`Model & Learning`): オンライン継続学習の学習率・学習モード・L2正則化、インスタント速習モード、個人学習モデルのリセット

### 高精度モデル ($H=256$) への切り替え

「MoGRU設定」タブのモデルファイル設定で以下をセットで指定します（再起動不要で即座に反映）：
- ベースモデル: `grumodel_new.bin`
- 文字辞書: `chardata_new.bin`

### 設定ファイル・モデル配置先 & 共存仕様

| 項目 | Linux | macOS | Windows |
| :--- | :--- | :--- | :--- |
| 設定・学習データ保存先 | `~/.config/mogru/`<br>(`$XDG_CONFIG_HOME/mogru/`) | `~/Library/Application Support/MoGRU/` | `%USERPROFILE%\AppData\LocalLow\MoGRU\` |
| 設定ファイル | `gru_config.ini` | `gru_config.ini` | `gru_config.ini` |
| 同梱モデル配置先 | `~/.local/share/mogru/models/` | `/Library/Input Methods/MoGRU.app/Contents/Resources/models/`<br>*(または `~/Library/Input Methods/...`)* | `C:\Program Files\MoGRU\models\` |

- **ホットリロード**: `gru_config.ini` の更新は最短1秒間隔で自動検知して再読み込みされます。
- **システム標準 Mozc との完全共存**: バイナリ名・macOS Bundle ID・Windows COM CLSID / レジストリ・設定ディレクトリが標準 Mozc から分離されているため、既存環境と併用できます。
- **旧環境からの自動マイグレーション**: 旧 Mozc / 旧 MoGRU ディレクトリからの設定・学習ファイルの自動引き継ぎに対応しています。

---

## 詳細機能ガイド

### 1. ライブ変換の高度な制御

- **前文節固定（計算量抑制）** (`enable_live_conversion_prefix_freezing = true`, `live_conversion_length_threshold = 24`): 確定ひらがな文字数が閾値に達すると前半分程度の文節を固定し、長文入力時の変換コストを抑えます。固定境界には全角スラッシュ `／` を表示でき、バックスペース等で固定部へ戻ると自動的に固定が解除され再変換されます。
- **段階的自動確定（Auto-Commit）** (`enable_live_conversion_auto_commit = false`): 前文節が固定された状態で次のキー入力を行うと、固定済み文節を自動で確定し、ユーザー履歴・GRU オンライン学習・変換統計へも反映します。
- **末尾ひらがな除外（遅延変換）** (`live_conversion_excluded_hiragana_characters = 0`, GUI 範囲 `0`〜`10`): 入力中ローマ字末尾だけでなく、直近 N 文字のひらがなをライブ変換対象から除外し、入力途中のちらつきを抑えます。
- **文節区切り `/` 表示**: 通常変換時（`show_segment_separator_in_conversion = false`）とライブ変換時（`show_segment_separator_in_live_conversion = false`）のそれぞれで文節区切り表示を切り替えられます。
- **通常変換への即時移行**: ライブ変換中に `Space` キーを押すと、即座に候補ウィンドウを開いて通常変換へ移行します。

### 2. GRU ニューラル変換・ペナルティ・再評価

- **文節区切り再編成（Resegmentation）** (`enable_resegmentation = true`, `resegment_threshold = 600.0`): 3〜60文字の変換に対し、形態素解析（Viterbi）の不自然な文節境界を GRU の文全体スコアで評価・再分割します。
- **文節移動・候補変更時の動的再評価（Focus 再ランキング）**: 複数文節の変換中に前の文節で別候補を選択すると、選択内容を文脈として後続文節の候補順位および文節区切りを動的に再計算します。先行確定テキスト（最大32文字）も文脈に利用されます。
- **文字種ペナルティ調整**: 変換の傾向を細かく調整できます（`katakana_multiplier = 1.30`, `hiragana_multiplier = 1.10`, `hiragana_length_power = 1.00`, `alphabet_multiplier = 2.00`, `emoji_penalty = 15000.0`）。なお、1文字のひらがな助詞はひらがなペナルティから自動免除されます。
- **GRU スコア内訳表示** (`show_gru_score = true`): 候補ウィンドウの説明欄に `GRU:<score>[E:<combined_cost>]*` などのスコア内訳を表示します。

### 3. 英語自動フォールバック

- **英単語の自動復元**: 日本語ローマ字入力のまま英単語を打って変換コストが閾値以上（`english_fallback_cost_threshold = 15000.0`, 最小単語長 `english_fallback_min_word_length = 3`）になった場合、バイナリ内蔵の英単語辞書（`google-10000-english.txt`、外部ファイル利用可）から最長一致で英単語候補を生成し、必要に応じて文節の統合・分割を行います（例: `mergesuru` → `merge` + `する`）。
- **ライブ変換対応** (`enable_live_conversion_english_fallback = true`): ライブ変換中でも英単語フォールバックが機能します。
- **ユーザー追加英単語 & 大文字小文字保持**: GUI・`gru_config.ini`（`user_english_words`）・`user_english_words.txt`・環境変数 `MOZC_GRU_USER_ENGLISH_WORDS` で独自の英単語（社内用語や固有名詞など）を追加でき、大文字・小文字を含んだ表記も候補として保持されます。

### 4. オンライン継続学習 & インスタント速習モード

- **学習モード選択** (`online_learning_mode`):
  - `output_only`（デフォルト）: 出力射影層のみを更新します。高速かつ一般的な日本語知識が壊れにくい方式です。
  - `full` / `full_bptt`: Embedding や GRU セルを含めた全層を BPTT で更新します。
- **インスタント速習モード** (`enable_instant_mode = false`, `instant_learning_rate = 0.80`, `instant_max_weight_delta = 2.0`):
  - 出力層更新（`output_only`）への固定・重み変化量クランプ・即時ファイル保存を組み合わせることで、既存の言語能力を壊さずに**1回の確定だけで即座に次回から最上位候補へ昇格**させやすくする機能です。
- **忘却防止と安全な運用**:
  - **L2 アンカー正則化**（`online_learning_weight_decay = 0.001`）と勾配ノルムクリッピング、定期保存（`online_learning_save_interval = 5` 回ごと＋Sync 時・アトミック保存）により重み崩壊を防止します。
  - **誤確定時の学習取り消し（Revert ロールバック）**: 確定直後に取消操作（`Revert`）を行うと、直前学習ステップのスナップショットを自動復元して誤学習を打ち消します。
  - **ベースモデルハッシュ照合**: 64-bit FNV-1a ハッシュにより、別のベースモデルへ切り替えた際に非互換な個人重みファイルが誤読込されるのを防ぎます。
  - **シークレットモード保護**: シークレットモード時はオンライン学習を自動でスキップします。

### 5. コンテキストプロファイル & アプリ連動自動切り替え

用途ごとにパラメータや学習状態を切り替えられる **6種類の組み込みプロファイル** に加え、**プロファイルの自由な新規追加・複製・削除**に対応しています。

| プロファイル名 | 用途・特徴 | 主なプリセット調整内容 |
| :--- | :--- | :--- |
| `default` | 標準設定 | バランス重視のデフォルト値 |
| `business` | ビジネス文書向け | `emoji_penalty=30000.0`, `katakana_multiplier=1.40`, `alphabet_multiplier=2.00`, `online_learning_rate=0.02`, `online_learning_weight_decay=0.005`, `resegment_threshold=800.0` |
| `coding` | 開発・技術文書向け | `alphabet_multiplier=1.00`, `katakana_multiplier=1.10`, `english_fallback_cost_threshold=10000.0`, `online_learning_rate=0.08`, `resegment_threshold=500.0` |
| `casual` (`sns`) | チャット・SNS 向け | `emoji_penalty=0.0`, `katakana_multiplier=1.00`, `hiragana_multiplier=1.00`, `online_learning_rate=0.10` |
| `creative` | 創作・文芸向け | `gru_score_weight=1200.0`, `resegment_threshold=400.0`, `katakana_multiplier=1.20` |
| `instant` | 即時適応重視 | `enable_instant_mode=true`, `instant_learning_rate=0.80`, `instant_max_weight_delta=2.0`, `online_learning_rate=0.80`, `online_learning_mode=output_only` |

- **プロファイルの自由な追加 & 複製（カスタムプロファイル作成）**:
  - GUI 設定ツールの「MoGRU設定」タブにある「**追加...**」ボタンから、半角英数字（`_`, `-` 含む）で任意のプロファイルを自由に追加できます。
  - 新規プロファイル作成時は、**現在表示中のプロファイルの設定値がそのまま初期値として複製・引き継がれる**ため、好みのプリセット（`coding` や `business` 等）をベースにした独自の派生プロファイルを簡単に作成可能です。
  - 設定ファイル（`gru_config.ini`）に `[profile.<名前>]` セクション（または `[<名前>]`）を直接追記することでも作成できます。
- **プロファイルの安全な削除 & データクリーンアップ**:
  - 不要になったプロファイルは「**削除**」ボタンから削除できます（`default` プロファイルは保護されており削除不可）。
  - 削除確認時に「**このプロファイル専用の学習データ・変換履歴ファイルも完全に削除する**」を有効にすると、ディスク上に保存されている専用の GRU 個人学習モデル（`grumodel_user_<profile>.bin`）および各種履歴 DB（`segment_<profile>.db`, `boundary_<profile>.db`, `.history_<profile>.db`）もまとめて安全に消去されます。
  - 削除したプロファイルがアプリ自動切り替え（`app_profile_map`）に登録されていた場合は関連付けが自動的に解除され、削除対象が現在のアクティブプロファイルだった場合は即座に `default` へ安全にフォールバックします。
  - `business` や `creative` などの組み込みプロファイルも不要であれば削除（非表示）でき、設定ファイルの `deleted_profiles` に記録されます（再度「追加...」で同じ名前を指定することでいつでも初期設定値で復元可能）。
- **学習・変換履歴の完全分離**:
  - プロファイルごとに GRU 個人重み（`grumodel_user_<profile>.bin`）だけでなく、Mozc 本体の変換履歴・文節境界・予測履歴 DB（`segment_<profile>.db`, `boundary_<profile>.db`, `.history_<profile>.db` 等）も個別に分離保存されるため、用途ごとの変換癖が混ざりません。
- **アクティブアプリ連動切り替え** (`enable_app_profile_switching = true`, デフォルト `app_profile_map = "code:coding,cursor:coding,slack:business,discord:casual"`):
  - Fcitx5 等のアクティブアプリ名（`gnome-shell` / `kwin_wayland` / `mutter` / `weston` / `xwayland` 等のコンポジタは自動除外）や環境変数 `MOGRU_APP_NAME` / `MOZC_CURRENT_APP` に応じてプロファイルを自動切替します。追加したカスタムプロファイルも紐付け可能です。

### 6. インラインコマンド候補（変換操作だけで設定変更）

通常変換中（誤確定防止のためライブ変換中は非表示）に以下の読みを入力して変換・確定すると、GUI を開かずに即時実行・`gru_config.ini` 保存が行われます。

| 読み・キーワード例 | 生成されるコマンド候補 | 動作 |
| :--- | :--- | :--- |
| `ぷろふぁいる` / `プロファイル` / `profile` / `purofairu` / `::profile`<br>`もぐら` / `モグラ` / `mogru` / `::mogru` | `MoGRU: プロファイル切替 [<profile>]`<br>*(有効な全プロファイル：組み込み & 追加したカスタムプロファイル)* | アクティブプロファイルを切り替え（カスタムプロファイル名も候補に自動列挙、削除済みプロファイルは自動除外） |
| 同上（アクティブアプリ名取得可能時） | `MoGRU: 現在のアプリ (<app>) を [<profile>] に登録` | 現在のアプリを指定プロファイルへ自動登録 |
| `いんすたんと` / `インスタント` / `instant` / `insutanto` / `::instant`<br>`もぐら` / `モグラ` / `mogru` / `::mogru` | `MoGRU: インスタント速習モード ON` / `OFF` | インスタント速習モードのトグル |

### 7. 変換正確率統計 & 詳細履歴ログ

- **変換正確率統計** (`enable_conversion_stats = true`): ライブ変換正確率、Space 変換の Top-1 正確率（文単位 / 文節単位）、文節リサイズ回数、総確定回数・総文字数を集計し、設定ディレクトリ内の `conversion_stats.json` に自動保存します。GUI の「MoGRU設定」タブから閲覧・最新化・リセットが可能です。
- **詳細変換履歴ログ** (`enable_conversion_history_log = false`): 有効化すると確定ごとの変換履歴を設定ディレクトリ内の `conversion_history.tsv` へ追記保存します。

### 8. 入力体験の改善とプラットフォーム・高速化

- **Shift キー解放時の即座なローマ字復帰**: Shift キーを押して一時的に半角英数（ASCII）モードへ入った後、Shift を離して次の英字を入力すると即座に元のローマ字入力モードへ復帰します。
- **句読点の統一と学習正規化**: カスタムローマ字テーブル使用時も句読点設定（`、` / `。`・`，` / `．` 等）に準拠し、辞書参照時は数値間を除いて出力スタイルを維持します。GRU 学習・推論内部（`CharIndex`）でも `,`/`.`/`，`/`．`/`､`/`｡` を `、` / `。` へ統一正規化します。
- **macOS SwiftUI 専用 UI**: macOS 版では SwiftUI ベースのネイティブ候補ウィンドウおよび用例情報（Infolist）ウィンドウを備えています。
- **Linux (Fcitx5) サーバーレス直結動作**: `use_server=False` により別プロセスの `mozc_server` を立てずにインプロセスで動作し、`config1.db` および `gru_config.ini` のタイムスタンプ（mtime）監視により設定変更を自動リロードします。
- **SIMD / FP16 高速化**: x86_64（AVX2 + FMA）および ARM64（NEON + ARMv8.2-A FP16）によるベクトル演算・FP16 推論・入力ゲート事前計算に対応しています（環境変数 `MOGRU_DISABLE_FP16=1` で FP16 推論を無効化可能）。

<details>
<summary>環境変数オーバーライド（MOZC_GRU_* 等）について</summary>

テストや一時的な運用向けに、`gru_config.ini` の各設定項目を環境変数 `MOZC_GRU_*` で上書きできます（例: `MOZC_GRU_USER_ENGLISH_WORDS`, `MOZC_GRU_ENABLE_CONVERSION_STATS`, `MOZC_GRU_CONVERSION_STATS_PATH`, `MOZC_GRU_ENABLE_CONVERSION_HISTORY_LOG` など）。また、`MOGRU_APP_NAME` または `MOZC_CURRENT_APP` でアプリ連動プロファイル用のアプリ名を指定でき、`MOGRU_DISABLE_FP16=1` で ARM64 FP16 推論を無効化できます。

</details>

---

## ライセンス & 謝辞

- 本プロジェクトは [Google Mozc](https://github.com/google/mozc) をベースにしています。
- **MoGRU 拡張コード**: Copyright (c) 2026 Haruhiro Obora ([BSD 3-Clause](LICENSE))
- **Mozc オリジナルコード**: Google LLC ([BSD 3-Clause](LICENSE))
- **Fcitx5 モジュール**: CSSlayer and The Fcitx Team ([LGPL-2.1+ / GPL-2.0+](src/unix/fcitx5/org.fcitx.Fcitx5.Addon.Mozc.metainfo.xml.in))
