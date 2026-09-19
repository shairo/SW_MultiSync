# SW MultiSync

[Stormworks](https://store.steampowered.com/app/573090/Stormworks_Build_and_Rescue/) 専用サーバー
(`server64.exe`) に組み込んで、遠くの車両の位置同期（カクつき・遅延）を改善するツールです。
サーバー側だけに導入すれば効き、参加者側の導入は不要です。

何をどう解決しているかは [目的と設計.md](目的と設計.md)、フックの仕組みは [hooking.md](hooking.md)、
通信プロトコルの解析結果は [protocol.md](protocol.md) を参照してください。

## 構成

- `SWMultiSync.exe` — 操作用 GUI
- `swctl.exe` — CLI（引数なし実行で一覧表示）
- `swhook.dll` — サーバーに注入される本体
- 操作は全て `127.0.0.1` のローカルTCP経由で統一（[ipc-protocol.md](ipc-protocol.md)）

## ビルド

MSVC BuildTools のみで外部依存なし。既定では
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` を使うので、パスが違う場合は
[build.bat](build.bat) 冒頭の `VS` 変数を書き換えてください。

```bash
build.bat
```

`build\` に各exe/dllが生成されます。配布ZIP (`dist\SW_MultiSync_v<version>.zip`) を作る場合は
`package.bat` を実行してください。バージョンは [src/common/version.h](src/common/version.h) が
情報源です。

## 使い方

`SWMultiSync.exe` を起動（管理者権限の確認は「はい」）→ いつも通りサーバーを起動、の順で
自動検出・自動注入されます。詳しい手順やトラブルシューティングは配布ZIP同梱の
[packaging/README_はじめにお読みください.txt](packaging/README_はじめにお読みください.txt)
を参照してください。

## その他ドキュメント

- [ipc-protocol.md](ipc-protocol.md) — 制御プレーン仕様
- [protocol.md](protocol.md) / [protocol/](protocol/) — 通信プロトコル解析

## 注意

DLLインジェクションを行うためAV/SmartScreenの警告が出ることがあります。自分が管理する
（または許可を得た）サーバーでのみ使用してください。

## ライセンス

[MIT](LICENSE)
