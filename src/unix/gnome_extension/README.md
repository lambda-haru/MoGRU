# MoGRU Active App Tracker (GNOME Shell Extension)

GNOME + Wayland 環境において、現在フォーカス中のアプリケーション識別子を取得し、Session D-Bus 経由で `fcitx5-mogru` に供給するための GNOME Shell 拡張機能です（対応バージョン: GNOME Shell 45〜50）。

## D-Bus インターフェース

- **Bus Name**: `io.github.lambda_haru.MoGRU.AppTracker`
- **Object Path**: `/io/github/lambda_haru/MoGRU/AppTracker`
- **Interface**: `io.github.lambda_haru.MoGRU.AppTracker`

### メソッド

- `GetFocusedApp() -> (s appName)`
  - フォーカス中アプリの代表識別子を返します（Overview・デスクトップ等では `""`）。
- `GetFocusedAppInfo() -> (a{sv} info)`
  - 以下のメタデータ辞書を返します（プライバシー保護のためウィンドウタイトルは含めません）:
    - `app_name` (`s`): 正規化済みの代表アプリ名
    - `process_name` (`s`): `/proc/<pid>/exe`（読み取り不可時は `/proc/<pid>/comm`）ベース名
    - `wm_class` (`s`): `Meta.Window.get_wm_class()`
    - `wm_class_instance` (`s`): `Meta.Window.get_wm_class_instance()`
    - `app_id` (`s`): `Shell.App.get_id()`（`.desktop` 除去、`window:*` 除外）
    - `gtk_application_id` (`s`): `Meta.Window.get_gtk_application_id()`
    - `sandboxed_app_id` (`s`): `Meta.Window.get_sandboxed_app_id()`
    - `pid` (`i`): クライアント PID
    - `is_wayland` (`b`): Wayland ネイティブクライアントか否か

### シグナル

- `FocusedAppChanged(s appName, a{sv} info)`
  - フォーカス変更時や遅延属性更新時に発火します（同一状態への重複通知は抑止）。

## テスト・動作確認

```bash
# 単体テスト (GJS)
gjs -m src/unix/gnome_extension/app_resolver_test.js

# 稼働中の GNOME Shell での D-Bus 確認
gdbus call --session \
  --dest io.github.lambda_haru.MoGRU.AppTracker \
  --object-path /io/github/lambda_haru/MoGRU/AppTracker \
  --method io.github.lambda_haru.MoGRU.AppTracker.GetFocusedAppInfo

gdbus monitor --session \
  --dest io.github.lambda_haru.MoGRU.AppTracker \
  --object-path /io/github/lambda_haru/MoGRU/AppTracker
```
