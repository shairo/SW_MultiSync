# swcap — レコード解析プレイブック

`.swcap`（`SendMessageToUser` フックが録画したサーバー→クライアント送出ストリーム）を解析するツール。
**新しいセッションはまずこのファイルを読めば解析を再現できる。**

## 実行方法（重要）

```bash
cd tools/swcap
dotnet run -c Release -- <command> "<絶対パス>.swcap" [opts]
```

- **キャプチャは必ず絶対パスで渡す**（`dotnet run` の作業ディレクトリは `tools/swcap`。
  `build/captures/...` の相対パスは `DirectoryNotFoundException` になる）。例:
  `C:/Users/Shairo/CommonProjects/sw-serverhook2/build/captures/session_XXXX.swcap`
- 全コマンド一覧は引数なしの `dotnet run -c Release --` で表示。

## 大前提（解析の勘所）

1. **キャプチャ＝クライアントに実際に届いたバイト列そのもの**（`SendMessageToUser` を素通り前にフック）。
   だから「届いたデータが健全か」を証明できる。健全なのに画面が固まるなら **クライアント側**の問題。
2. **必ず宛先（peer=受信者 SteamID）ごとに分けて見る**。マルチのキャプチャは複数受信者のストリームが
   インターリーブしており、**マージ表示は誤読の元**。ほぼ全コマンドが `--peer <SteamID>` を取る。
   - ある車両を `meanGap≈5`・高サンプル数で受信している peer が、その車両の**搭乗者/所有者**（近距離＝高頻度）。
   - 同じ車両を `meanGap` 数百で数件だけ受信している peer は**遠方観測者**（低頻度＝dETA大は正常）。
3. **0x81.time = 未来のワールドtick ETA**（そのposeに到達すべき時刻）。`dETA = time − envelope.tick`。
   近距離=+5、遠方=数百。クライアントは受信〜ETA間で補間し、ETA超過で次が来ないと停止（速度非搭載）。
4. 0x81 ボディ配置（`protocol.md` 参照）: body0 は必ず type1（`float[4] rot + double[3] pos`＝40B・世界座標）。
   type2 は `float[4] rot + float[3] pos`＝28B で**body0からの相対**（±数m の一定値になる）。

## 車両フリーズの切り分けレシピ

```bash
CAP="C:/.../build/captures/session_XXXX.swcap"
# 1) 末尾で止まった車両を洗い出す（"still syncing"=サーバー送出継続 / "went silent"=送出停止）
dotnet run -c Release -- frozen "$CAP"
# 2) 受信者を確認
dotnet run -c Release -- stats "$CAP"          # peers 行
# 3) 各 peer が meanGap≈5 で持つ車両＝その peer の自機。搭乗者peerを特定
dotnet run -c Release -- vel "$CAP" --peer <steamid>
# 4) 搭乗者peer視点で自機の 0x81 を時系列ダンプ（dETA が急増していないか、途切れていないか）
dotnet run -c Release -- vehrec "$CAP" --id <veh> --peer <搭乗者steamid> --last 30
# 5) その車両の全ボディ(type1/2)を健全性検査（NaN/非unit quat/座標爆発/移動スパイク）
dotnet run -c Release -- vehscan "$CAP" --id <veh> --peer <搭乗者steamid>
```

**判定:**
- 搭乗者peir宛の dETA が終始 5・pose健全・滑らかに推移 → **届いたデータは健全＝クライアント側フリーズ**
  （サーバー/relay無罪）。サーバー側キャプチャではこれ以上原因に踏み込めない。
- 途中で dETA が急増 / レコードが途切れる → サーバー送出側 or despawn を疑い、`vehrec`/`lifespan`/`attick` で追う。

### 既知の結論（session_20260913_214246_776_freeze）
搭乗機 veh93 は搭乗者 peer `76561197994178477` に対し **開始〜墜落まで dETA=5・全2753ボディ健全・滑らかに着水**。
→ このフリーズは**クライアント側確定**（バニラでも稀に起きる描画/物理適用の停止）。relay の ETA バグとは別問題。
大きな dETA(720等)は全て遠方観測者向けで正常。

## コマンド早見

| コマンド | 用途 |
|---|---|
| `stats` | チャネル/型/方向の内訳・**peers 一覧** |
| `frozen [--eps F] [--tail N]` | 末尾で停止した車両（still syncing / went silent） |
| `vel [--id N] [--peer S]` | 車両ごと meanGap（peer の自機特定に使う）・外挿誤差 |
| `vehrec --id N [--peer S] [--last K]` | 1車両の 0x81 時系列（tick, dETA, bodyCount, b0type, move, pos） |
| `vehscan --id N [--peer S] [--jump F]` | 全ボディの pose 健全性検査（type1/2） |
| `eta [--peer S]` | 0x81 の dETA 分布と実同期間隔 |
| `lifespan [--near T]` | 車両ごとの 0x81 first/last tick + 件数 |
| `attick --at T [--w N]` | tick窓内のレコード（フラッド系タグは除外） |
| `when --tag N` / `marks` | タグ出現時刻 / F8マーカー（再現用） |
| `validate [--peer S]` / `walk --seq N|--tag N` | レコード長ルールで走査 / 1メッセージの内訳 |
| `bw` / `shadow [--lod]` / `resend` | relay の帯域見積 / 因果sim / 再送痕跡 |
| `unz --id N` | 0x2F の zlib ペイロード展開 |

長さルールの本体は `Records.cs`（`Records.Decode`）、プロトコル詳細は `../../protocol.md`。
