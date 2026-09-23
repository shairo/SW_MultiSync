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

### 既知の結論（session_20260913_214246_776_freeze、2026-09-22 再解析）
搭乗機 veh93 は搭乗者 peer `76561197994178477` に対し **開始〜墜落まで dETA=5・全ボディ健全**。サーバー送出は
4 ピアで同一内容・欠落なし（tick 連番・全 reliable）。**フリーズは t+42.31 s、機体が z=−2500 のタイル境界
を越えた瞬間**（タイル (3,−3)→(3,−2)）で、その直前 0.5 s にサーバーが送った非フラッド記録は皆無。
クライアント側の症状はワイヤ上にはっきり出る:

| 信号 | 凍結クライアント (8477) | 健全な 3 ピア |
|---|---|---|
| 0x2F 自機 pose | t+42.31 から末尾まで **完全静止**（サーバー側 veh93 は飛び続け着水） | 動く |
| 0x34 ハートビート / 0x66 視線 / 0x64 キー | **継続**（+60/s、マウスも動いている） | 継続 |
| 0x45 タイルロード → 0x28 ack | **継続**（88:88、凍結後も 100 ms で返す） | 同じ |
| 0x2B 配置 → 0x1B 定義要求 | **継続**（100 ms で返す） | 同じ |
| ch1 type12 定義 → **0x29 loaded ack** | **凍結後 12 件すべて未 ack**（80..85, 74..79） | 全件 60 ms で ack |
| サーバー → 0x2D/0x2F 状態 | 上記 12 車両分は **一度も送られない**（ack 待ち） | ack 直後に送られる |

= ネットワークスレッドは生きているが **ワールドシミュレーション（0x81 適用・車両生成）だけが止まる**。
サーバーが待たせている訳ではなく（0x2D/0x2F の保留は結果）、ワイヤから見える原因は無い＝クライアント内部
のタイル跨ぎ処理（タイル (3,−2) には veh93 以外の車両なし）。詳細: `protocol/lifecycle.md`（定義ハンド
シェイク）、`protocol/reference.md`（タイル集合はサーバー主導・全員同報・半径 2）。

**次の手（実装済み）**: DLL 内の凍結検出器 `src/hook/freeze.h`（pose 静止×自機車両移動 / 定義未 ack）が
`captures/*.log` に `FREEZE?` 行 + キャプチャマーカーを打ち、`swctl peers` の freeze 列と IPC
`peers[].freeze` に出る。緩和実験は `swctl nudge <peer> tp|tile`（0x5E テレポート / 0x45 再送を次 tick に
追加）、遮断実験は `swctl hold <peer> [ms]`（送出を丸ごと棄却。保留→一括放流版は tick 待ち＋早送りになるだけと判明）。オフライン再生は
`tools/rectest/test_freeze.cpp`（このキャプチャで t+44.5 s に 8477 のみ検出、健全 7 本で誤検出 0）。

### 実験結果（2026-09-22、peer 8477、単独）
- **送出保留→一括放流 3 s**（session_20260922_150005_474）: クライアントは 0x2F 送信を止め、hb の tick エコーが
  最後に受信した tick で固定（＝シミュレーションはサーバー tick 駆動）。放流後は早送りで追いつく。サーバーは
  hb 遅延に一切反応せず、再送記録もなし。同じ形は freeze キャプチャの t+27.75（車両 9 台の定義ロードで全員
  1.3 s 停止→0x29×9→早送り）にも自然発生している。
- **送出棄却 5 s**（session_20260922_153939_896）: 同じく 0x2F 停止・エコー固定。再開後は新しい tick を黙って
  受け入れ（要求記録なし・早送りなし）。クライアントに再送プロトコルは無い。
- **0x46 タイルアンロード → 操作 → 0x45 再ロード**（session_20260922_154320_984、搭乗中）: 0x46 直後から
  **0x2F は 12.5/s で送り続けるが値は静止、hb エコーは進み、0x64 キー入力も出る＝フリーズと同じ署名**。
  0x45 → 0x28 ack → 0x47 の後、pose はサーバー側で入力どおり動いた位置へジャンプ。つまり「自分の居る
  タイルがクライアント側で未ロード扱い」だと車両更新が捨てられ、フリーズそのものの見え方になる。
  回復操作の第一候補: `swctl nudge <peer> reload`（0x46→0.5 s 後 0x45）。

### フリーズ調査の追加レシピ
```bash
# 1 ピアのイベント列（非フラッド記録 + ch1 定義プッシュ）を t+ms で並べる（--all でフラッドも）
dotnet run -c Release -- events "$CAP" --peer <steamid> [--from MS --to MS]
# クライアント側（0x2F/0x1B/0x29/0x28）は Records.cs 未対応 → tools/rectest/test_freeze.exe か
# python（scratchpad の swcap.py 相当: msgType=3 の固定長表 client.md）で見る
build/test_freeze.exe "$CAP"      # 検出器の再生: FREEZE? 行が出る peer と時刻、末尾で未 ack の定義
```

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
| `events --peer S [--from MS] [--to MS] [--all]` | 1 受信者のイベント列（非フラッド記録 + ch1 定義プッシュ）を t+ms 順に |
| `when --tag N` / `marks` | タグ出現時刻 / キャプチャマーカー（`capture.mark`、再現用） |
| `validate [--peer S]` / `walk --seq N|--tag N` | レコード長ルールで走査 / 1メッセージの内訳 |
| `bw` / `shadow [--lod]` / `resend` | relay の帯域見積 / 因果sim / 再送痕跡 |
| `unz --id N` | 0x2F の zlib ペイロード展開 |

長さルールの本体は `Records.cs`（`Records.Decode`）、プロトコル詳細は `../../protocol.md`。
