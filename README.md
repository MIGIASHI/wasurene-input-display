# WASURENE. Input Display

FPS配信向けに、**押したボタン・キー・マウス操作を光らせて表示するOBSプラグイン**です。

SF6 Trackerから独立したツールです。パッドとキーボード／マウスに対応。SF6、別アプリ、Python、文字認識モデルは不要です。Windows 64bit・OBS 31以降向けです。

## ダウンロード・入れ方

[Releases](https://github.com/MIGIASHI/wasurene-input-display/releases/latest)のZIPから、配置先に合う**どちらか1つだけ**を選んでください。

| ZIP | 配置先 |
|---|---|
| `wasurene-input-display-windows.zip` | 中の `obs-plugins` と `data` をOBS本体フォルダ（通常 `C:\Program Files\obs-studio\`）へコピー |
| `wasurene-input-display-windows-programdata.zip` | 中の `wasurene-input-display` フォルダを `C:\ProgramData\obs-studio\plugins\` へコピー |

1. OBSを終了し、選んだZIPを展開して上記の場所へコピーします。DLLだけでなくデータも一緒に配置してください。
2. OBSを起動し、**ツール → WASURENE. Input Display** を開きます。設定画面は `http://127.0.0.1:17861/input?setup=1` でも開けます。
3. 「ゲームパッド」または「キーボード・マウス」を選び、表示する入力・配置・色・大きさを設定します。
4. 画面下に出るURLをコピーし、OBSの「ブラウザ」ソースに貼ります。最初は幅640・高さ480を目安に調整してください。

ゲームキャプチャのフィルター追加は不要です。OBS起動時に入力取得が始まり、OBS終了時に停止します。「ドック → カスタムブラウザドック」に設定画面のURLを登録するとOBS内から調整できます。

## 表示できるもの

- パッド：ボタン、十字キー、左右アナログスティック、L3/R3押し込み。Xbox系のボタン名が初期値で、名前は自由に変更できます。
- キーボード・マウス：任意キー、左右・中央・戻る・進むボタン、ホイール上下・左右チルト。
- キーは実際のキーボードを参考に配置し、Space・Shift・Ctrl・Enterなどは種類に合った幅で表示します。各キーの幅は設定から変更できます。
- マウスは専用の形で表示し、ボタン・ホイールの部分が点灯します。本体ごとドラッグして配置できます。
- 「マウスを振る方向を表示」を有効にすると、相対移動量から方向の矢印を表示します。停止すると矢印は消えます。感度も変更できます。
- ボタンは最大32個まで追加、個別削除、ドラッグで配置変更できます。割り当て・名前・色・大きさ・背景も変更できます。
- 設定を自動保存し、OBS再起動後に復元します。URLで明示した設定が優先されるため、見た目を変えたらURLも貼り直してください。
- ホイールは短時間点灯します。入力履歴の表示は任意で、ブラウザ内の一時表示です。

FPS向けのため、レバーレス・アケコンの表示モードは含みません。ゲームパッドはXInput / DirectInputから、キー・マウスボタンはWindowsから取得します。

## データと注意点

- 設定はOBSの設定フォルダに保存します。通常は `%APPDATA%\obs-studio\plugin_config\wasurene-input-display\settings.json` です。ポータブル版はそのOBSの設定フォルダ内です。
- 実際に入力した文字列やキー履歴はファイルに保存しません。SF6 Trackerの設定とは別に保存します。
- 通信はこのPCの `127.0.0.1:17861` に限定しています。ゲームのメモリの読み取り、入力の書き換え、マクロ、ゲームへの入力送信はしません。
- ゲーム・機器の制約により取得できない場合があります。すべてのFPSやアンチチートとの互換性は未確認です。各ゲームの利用条件を確認してください。
- マウス方向はWindows Raw Inputを使用し、カーソルが中央に固定されるFPSでも相対移動を取得する方式です。OBS内の別機能がマウスのRaw Inputを使用している場合は、その登録を置き換えず方向表示を利用不可にします。表示を無効にした後は購読を解除し、移動履歴は保存しません。
- 他のプラグインが同じポートを使用していると起動できません。SF6 Trackerの既定ポートとは別なので、同時に使えます。
- 設定画面が開かない場合は、配置とOBSのログを確認してください。2種類のZIPを同時に入れないでください。更新前にはOBSを終了してください。

## ライセンス・公式配布

コードはGPL v2以降です。ライセンスに従って改変・再配布できます。公式配布先はこのリポジトリのReleasesと[WASURENE. TOOL](https://wasurene.xyz/tools)です。改変版は変更内容を明記し、WASURENE.の公式版・公認版と誤認されない表示をお願いします。この案内はGPLの権利を制限するものではありません。

派生元と同梱ライブラリは [NOTICE.md](NOTICE.md) を参照してください。

## 開発

Visual Studio 2022のC++ツールとCMake 3.28以降を使用します。OBS用の依存物はビルド時に取得します。

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --prefix release --config RelWithDebInfo
```

API・HTTP・入力取得のテストは `cmake -S tests -B build-tests` で独立して実行できます。テスト用EXEは配布ZIPに含めません。LinuxはAPI開発テスト用で、配布対象はWindowsです。
