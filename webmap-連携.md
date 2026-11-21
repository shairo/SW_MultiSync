# WebMap との連携

SW_MultiSync（swhook.dll）は、WebMap の 3 つの部品から車両の情報を受け取り、車両同期の追加の情報源として使います。
この資料は、どの部品が何をどの経路で送り、SW_MultiSync がそれをどう使うかを、ソースに沿ってまとめたものです（2026-11-21 時点）。

| 部品 | 場所 | 役目 |
|---|---|---|
| WebMap アドオン | `Stormworks_WebMap` の `mission/WebMap/script.lua` | ゲームサーバーの中で全車両の位置・姿勢を読み、Bridge へ送る |
| WebMapBridge | `WebMapBridge` の `src/main/` | アドオンの送信を受け、SW_MultiSync の IPC へ流す。車両 XML から砲塔・砲の voxel を選ぶ |
| StormWorksMapViewer | `Stormworks_WebMap` の `server/StormWorksMapViewer.js` | 車両の Type（Tank など）を決め、Tank の一覧を Bridge 経由でアドオンへ返す |
| SW_MultiSync | この repo の `src/hook/` | 受け取った位置・姿勢で 0x81（車両の記録）を補い、書き換える |

## 全体の流れ

```
              (1) vpos  HTTP GET /server/post/{"type":"vpos",...}
 WebMap アドオン ───────────────────────────────▶ WebMapBridge
   ▲   │                                          │  │
   │   └─(返信) set_body_voxels / tank_vehicles ◀─┘  │ (2) IPC addon.poses（1 行の JSON）
   │                                                 ▼
   │                                       SW_MultiSync (127.0.0.1:28215)
   │                                                 │ (3) IPC peers.get を 1 秒ごと
   │   (返信) adc（クライアントの遅れのまとめ）◀──────┘    （Bridge が読みに行く）
   │
 StormWorksMapViewer ──(4) tank_vehicles──▶ Bridge の返信の列 ──▶ アドオン
```

- (1)〜(2) が本流です。アドオンが送った全車両の姿勢を、Bridge がほぼそのまま SW_MultiSync へ渡します。
- (3) は逆向きです。SW_MultiSync が測ったクライアントの遅れを Bridge が読み、アドオンへの返信に載せます（Anti Desync Command 用）。
- (4) は砲塔・砲の同期（試作）だけで使う経路です。

## (1) アドオン → Bridge：vpos

アドオンは `server.httpGet` で Bridge の `/server/post/{"type":"vpos",...}` を呼びます（`script.lua` の送信は `wm2` まわり）。

- 間隔: `vpos_interval`（既定 3 tick）。1 回に入りきらない車両は次の tick に続きを送り、溢れが続くと間隔を広げます。`?vpos interval <n>` で最小の間隔を決められます（管理者のみ）。
- 応答待ちは `vpos_max_in_flight`（既定 1）通まで。それ以上は送らずに待ちます。

SW_MultiSync が使う項目は次のとおりです。

| 項目 | 中身 | SW_MultiSync での使い道 |
|---|---|---|
| `ep` | アドオンの世代（読み込み直すたびに変わる） | 変わったら tick の対応（K）をやり直す。同じ `ep` で `tc` が戻った束は捨てる |
| `tc` | アドオンの tick 数（`?reload_scripts` で 0 に戻る） | 世界の tick = `tc + K` |
| `fl` | 送った時点の、アドオン側の応答待ちの数 | `fl == 0` の束は届いた時刻から K を推定する（arrival） |
| `data` | `[[車両ID, x, y, z, qx, qy, qz, qw], ...]` | body0 の姿勢。0x81 の body0 と同じ軸・同じ四元数の順 |
| `bp` | `[[車両ID, body の番号, x, y, z, qx, qy, qz, qw], ...]` | 砲塔・砲の body の姿勢（後述）。位置と回転は **body0 から見た相対** |
| `al` | `[[fake_missile の ID, 追従先の ID], ...]` | fake を追従先の位置に見せる |
| `fk` | fake_missile の全 ID | 追従していない fake を 500 m 下で待機させる |
| `pk` | `[[fake の ID, 船の ID], ...]` | fake を船の 300 m 下にいるように見せる |

`data` はアドオンが `server.getVehiclePos` で読んだ値で、近くにプレイヤーがいなくても全車両が入ります。
ゲームサーバー自身の 0x81 は最短でも 5 tick おきで、近くにプレイヤーがいる車両しか出ないので、そこを埋めるのが目的です。

## (2) Bridge → SW_MultiSync：IPC `addon.poses`

Bridge の `bridge-server.js` は vpos を受けると、まず `mapState.ingestVpos` で `ep`・`tc` を検査し、無視しなかった束だけを `swhook-feed.js` の `onVpos` に渡します。
`swhook-feed.js` は 127.0.0.1:28215（`WEBMAP_SWHOOK_PORT` で変更）へ 1 行の JSON を書きます。

```json
{"cmd":"addon.poses","ep":E,"tc":N,"fl":F,"data":[...],"al":[...],"fk":[...],"pk":[...],"bp":[...]}
```

- `data`・`tc` は必ず入れます。`ep`・`fl`・`al`・`fk`・`pk` はアドオンが付けたときだけ、`bp` は空でないときだけ入れます。
- つながらないときはためずに捨て、5 秒おきに接続し直します。応答のない書き込みが 200 を超えたら切ります。
- `WEBMAP_SWHOOK_FEED=0` で止まります。
- 返事 `{"ok":true,"records":n,"fed":m,"offsetKnown":..,"offset":K}` は Bridge の統計（`getStats`）にだけ使います。

SW_MultiSync 側は `dllmain.cpp` の IPC が `addon::on_batch`（`addon.h`）を呼びます。

1. `ep` が前回と違えば K を忘れてやり直す（`epChanges` を数える）。同じ `ep` で `tc` が戻った束は捨てる（`staleBatches`）。
2. `fl == 0` なら「今の世界の tick − `tc`」を K の候補にする（arrival）。
3. `data` の各車両を車両ごとのリング（24 件）に入れる。K が決まっていて `addonPoses=1`（既定）で、その車両の種類のチェックが入っていれば（下の「種類ごとの切り替え」）、`relay::observe_addon` で中継の情報源にする。
4. 自分の 0x81 と `data` の位置を突き合わせても K を確かめる（matched。動いている車両が要る）。arrival と食い違えば matched を使う。
5. `bp` は K が決まってからだけ `relay::body_sample` に入れる。
6. `fk`・`al`・`pk` で fake_missile の待機・追従の一覧を置き換える。アドオンの送信が `stale` tick（既定 300）途絶えたら一覧を捨てる。
7. 3600 tick 以上報告のない車両は忘れる。

K が決まる前は、受け取った姿勢を中継には使いません（位置がずれた記録を作らないため）。

## (3) SW_MultiSync → Bridge：IPC `peers.get`

Bridge の `swhook-lag.js` は同じポートへ 1 秒ごとに `{"cmd":"peers.get"}` を送り、各プレイヤーの心拍（0x34 の clientTick・TPS・framesBehind）を読みます。
これをまとめた `adc` を、vpos への返信に 1 秒に 1 回だけ載せます。返信は全アドオンに届くので、Anti Desync Command がこれで `?tw` を自動で調整します。
SW_MultiSync が居なければ何も載せません（`WEBMAP_SWHOOK_LAG=0` で止まります）。

開発者モードの Bridge は、ほかに `swhook-probe.js` で `vehicles.get`・`peers.get` を読み、記録を取ります（通常配布では動きません）。

## (4) 砲塔・砲の同期（試作）

0x81 の body0 以外（砲塔・砲）を、アドオンが読んだ本当の向きで書き換えます。詳しい経緯は `Stormworks_WebMap` の `docs/progress/turret-sync.md` です。

### どの voxel を読むか（Bridge）

車両が出たとき、Bridge の `vehicle-service.js` が車両 XML を `body-voxels.js` の `extractBodyVoxels` にかけ、
アドオンへ `{mode:"set_body_voxels", data:{vehicle_id, v:[[body の番号, x, y, z], ...]}}` を返します（同じ内容は送り直しません。ただし、ゲームのフォルダーを `working` と `working_server` の間で切り替えたときは、覚えた内容と送る前の `set_body_voxels` を捨て、切り替え先の車両の分を送り直します。砲の無い車両にも、空の一覧を一度送ります）。

- 砲・レーザーのある body から、関節（ピボット・ロボットヒンジなど 20 種）をたどって body0 まで戻る道を探し、砲の側から最大 3 個の body を選びます（全体で最大 15）。
- 関節の子は親の y 軸の向きに 1〜6 voxel 先にあります。ミラー（`<c t>` の 2 = y）なら反対側です。
- 各 body から 1 つ voxel を選びます（車輪・multibody・関節の部品は除く）。

### どの車両を送るか（MapViewer → アドオン）

アドオンは MapViewer で Type が **Tank** の車両だけ `bp` を作ります。

- MapViewer は vpos を受けるたびに Type が Tank の車両 ID を集め（最大 128）、変わったとき、または 30 秒ごとに `{mode:"tank_vehicles", data:{ids}}` を送ります。
- Bridge の `handleWebMapResponse` がこれを許可された命令として受け取り、アドオンへの返信の列に入れます。
- アドオンは一覧を丸ごと置き換え、`g_savedata.wm_tank_vehicles` に残します。
- Type は MapViewer の推定・ground_mode・手で決めた値（`changeIDTypeMap`）から決まります。手で決めた値はアドオンを読み込み直すと消えます。

### アドオンが送る `bp`

`wm2.bodyPoses` が、Tank かつ voxel を受け取った車両について、`server.getVehiclePos(id, x, y, z)` で各 voxel を読みます。

- 位置と回転は body0 から見た相対に直します（車体が走るだけでは変わらないので、砲塔・砲が動いたときだけ送ります）。
- 送るのは前回から変わった body だけで、60 tick ごとには必ず送ります。
- `bp` を送る車両は、車体が止まっていても `data` に body0 を載せます（SW_MultiSync は body0 で車両の間隔を測るため）。
- 負荷: `getVehiclePos` は計算が重めで、Tank の車両ごとに voxel の数だけ呼び出しが増えます。2026-11-21 の測定では、止まった戦車 10 両（body 2 つずつ）を Tank にすると、vpos は 1 tick あたり約 1.5 倍、TPS は約 65 → 約 57 になりました。詳しくは `Stormworks_WebMap` の `docs/progress/turret-sync.md` の「通信量と TPS の測定」です（減らす案は書いてありますが、作っていません）。

### SW_MultiSync が書き換える

- `body_sample` が body ごとのリングに入れ、`body_fill` が中継の 0x81 の type 2 の body を、予測した相対の姿勢で書き換えます。足りないときは `body_build` が type 0 の body を type 2 に広げます。
- 近くのクライアント（中継しない人）には `bodynear_pass` が、見せた向きとアドオンの向きが離れたときだけ type 0 の body を type 2 にします。
- 最後の `bp` から `stale` tick（既定 300）たった body は書き換えず、ゲームサーバーの記録のままにします。

### 誰に効くか

- `swhook.ini` / `config.set` の `bodySync` が全体のスイッチです（既定 1。管理者が止めたいときに 0 にする。GUI の「WebMap」タブのチェック）。
- そのうえで、プレイヤーごとにゲームのチャットで切り替えます（既定 OFF、保存しません）。

| チャット | 動き |
|---|---|
| `?bs` | 今の状態を本人にだけ表示 |
| `?bs on` | 自分に届く記録だけ、全 Tank の砲塔・砲を書き換える |
| `?bs off` | 自分の分を止める |

`swctl status` の `addon.bodies` に、追っている body の一覧（`list`）・見せた向き（`view`・`shown`）・ON の人（`players`）が出ます。

## 種類ごとの切り替え

アドオンの位置で body0 を書き換えるかを、車両の種類ごとに選べます。種類は MapViewer の Type です。

- MapViewer は種類ごとの車両 ID を `vehicle_kinds` で Bridge に送ります（変わったときと 30 秒ごと）。Bridge は `{"cmd":"addon.kinds","Missile":[...],"Ship":[...],"Tank":[...],"SubMarine":[...],"AirCraft":[...]}` を SW_MultiSync に送ります。最後の一覧を覚えておき、つなぎ直すたびに送り直します。アドオンへは送りません。
- SW_MultiSync は一覧を丸ごと置き換えます。どの一覧にも無い車両は「そのほか」です。
- 一覧が一度も届いていないあいだ（MapViewer が古いときなど）は、種類によらず全部の車両に使います。
- チェックを外すと、その種類でアドオンから来ていた車両の位置をすぐ捨て、次の 0x81 からゲームサーバーの記録に戻します（`kindDropped`）。
- 絞るのはアドオン → body0 の経路だけです。ほかのプレイヤーの細かい記録から作る中継、K を決めるための記録、`bp`（砲塔・砲）は絞りません。

| 種類 | 設定 | 既定 |
|---|---|---|
| ミサイル（Missile） | `addonMissile` | 1 |
| 船（Ship） | `addonShip` | 0 |
| 戦車（Tank） | `addonTank` | 0（車体だけ。砲塔・砲は `bodySync`） |
| 潜水艦（SubMarine） | `addonSub` | 0 |
| 航空機（AirCraft） | `addonAir` | 0 |
| そのほか（Torpedo・Bullet・Type なし） | `addonOther` | 0 |

ミサイル以外は、まだ確かめていないので既定で外しています。GUI の「WebMap」タブで切り替えられます（その場で効き、「設定」タブの「iniに保存」で残ります）。経緯は Stormworks_WebMap の `docs/progress/sync-kind-select.md` にあります。

## 関係するコマンド・設定

| 場所 | 名前 | 意味 |
|---|---|---|
| アドオン | `?vpos on\|off\|interval <n>\|<0-10>` | vpos の送信の切り替え・間隔・応答待ちの許容数（管理者のみ） |
| Bridge | `WEBMAP_SWHOOK_FEED=0` | `addon.poses` を送らない |
| Bridge | `WEBMAP_SWHOOK_LAG=0` | `peers.get` を読まない |
| Bridge | `WEBMAP_SWHOOK_PORT` | SW_MultiSync の IPC のポート（既定 28215） |
| SW_MultiSync | `addonPoses` | 1 = アドオンの姿勢を中継に使う（既定 1） |
| SW_MultiSync | `addonMissile` `addonShip` `addonTank` `addonSub` `addonAir` `addonOther` | 1 = その種類にアドオンの姿勢を使う（既定はミサイルだけ 1） |
| SW_MultiSync | `bodySync` | 1 = 砲塔・砲の書き換えを許す（既定 1。全員分のスイッチ） |
| SW_MultiSync | `stale` | アドオンの情報が途絶えてから、ゲームサーバーの記録に戻すまでの tick（既定 300） |
| SW_MultiSync | `ipcPort` | IPC のポート（既定 28215。起動時だけ読む） |
