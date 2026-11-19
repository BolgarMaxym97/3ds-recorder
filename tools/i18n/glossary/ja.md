# Glossary: Japanese (ja)

Sources used (all text-extracted and searched):

- **[M]** ニンテンドー3DS 取扱説明書 (CTR-001(JPN)): https://support.nintendo.com/jp/3ds/pdf/3dsmanual.pdf
- **[S]** 本体設定 電子説明書: https://support.nintendo.com/jp/manual/3ds/hardware/pdf/manual_hasj.pdf
- **[N]** おしらせリスト 電子説明書: https://support.nintendo.com/jp/manual/3ds/hardware/pdf/manual_hcrj.pdf
- **[G]** 3DS Hacks Guide, Japanese translation (partly translated; much text is still English):
  https://github.com/hacks-guide/Guide_3DS/tree/master/docs/ja_JP

## Conventions

- **Register.** Explanations and messages use です/ます. Instructions end in 〜してください
  ([M]: 「HOMEメニューで「おわる」をタッチすると…」, 「電源ボタンを押して、電源をONにしてください。」). Nintendo
  does not address the reader with a pronoun. あなた appears only in health and safety text, and お客様 only in
  privacy and service text ([M]). Leave out the pronoun.
- **Button and menu labels.** These are short and plain: a dictionary-form verb, often in hiragana, or a noun,
  with no です/ます. From [M]: 「はじめる」「おわる」「ひらく」「同意する」「電源を切る」「今は設定しない」「あとで」「設定」
  「再開」「OK」. Use 〜する or a plain noun on buttons. Use 〜します / 〜してください in sentences.
  Headings are noun phrases or 〜する phrases ([M] TOC: 「ソフトで遊ぶ」「本体を設定する」).
- **Punctuation.** Full-width 、。「」（）. Quote a label in 「」: 「データ管理」から…. Put a page or note in
  （）: スリープモード（本体を閉じた状態）. Use full-width ：！？ if needed. Use … (U+2026).
- **Latin text and numbers.** Write them with **no spaces** next to kana and kanji: 3DSソフト, SDカード,
  HOMEメニュー, HOMEボタン, DS/DSiソフト, Wi-Fi, 3D映像 ([M]). Use half-width digits (5分, 22 ページ in
  [M] is only page-reference layout). Brand words stay Latin: HOME, Miiverse, Nintendo eShop.
  Units: 30 fps and 4 GB are fine with a space, as in technical text. Keep `fps` and `MB` as they are (style.md rule 4).
- **Button letters.** Use the Latin letter, as [M] does: Aボタン, Lボタン, HOMEボタン, SELECTボタン. Write a
  combination as `L + Y`.

## Terms

| # | concept | term | source | note |
|---|---|---|---|---|
| 1 | the console / system | 本体 | [M] (329 occurrences: 本体設定, 本体を閉じる, 3DS本体) | Nintendo never says ゲーム機 for the user's own unit |
| 2 | Old 3DS / New 3DS | Old 3DS / New 3DS | community: [G] ja_JP (Latin "Old 3DS"/"New 3DS" throughout) | Nintendo product names: ニンテンドー3DS / Newニンテンドー3DS ([S] mentions Newニンテンドー3DS). Japanese blogs also say 旧3DS and New3DS; 旧3DS is the natural short form if Latin looks foreign |
| 3 | HOME Menu | HOMEメニュー | [M] (44x) | no space, no ホーム |
| 4 | Sleep Mode | スリープモード | [M] p.22 「スリープモードにする」 | |
| 5 | close the system / lid | 本体を閉じる | [M] 「電源がONの状態で本体を閉じると…スリープモードになります」 | not ふたを閉じる (0 hits) |
| 6 | System Settings | 本体設定 | [M], [S] title | |
| 7 | Data Management | データ管理 | [M] 「本体設定の「データ管理」」, [S] | in full: ニンテンドー3DSデータ管理 / DSiウェアデータ管理 ([M], [G]) |
| 8 | SD Card | SDカード | [M] (108x) | no space |
| 9 | Touch Screen | タッチスクリーン | [M] 「下画面（タッチスクリーン）」 | |
| 10 | upper / lower screen | 上画面 / 下画面 | [M] 「上画面（3Dスクリーン）」「下画面（タッチスクリーン）」 | |
| 11 | notification LED | おしらせランプ | [M] (in hiragana; お知らせランプ has 0 hits), [N] | other lamps: 電源ランプ, 充電ランプ, 無線ランプ, 3Dランプ ([M]) |
| 12 | POWER Button / turn off / restart | 電源ボタン / 電源を切る (電源をOFFにする) / 再起動 | [M] 「電源ボタンを押すと表示される電源メニューで「電源を切る」をタッチ」; 再起動: community: [G] ja_JP 「(Start) を押して再起動します」 | 再起動 is not in [M] or [S]. It is the standard Japanese word, so this is a low-risk choice |
| 13 | software / game / title | ソフト | [M] (394x: 3DSソフト, ダウンロードソフト, ソフトを中断/終了する) | ゲーム is for "the game in progress" (ゲームを一時中断); avoid アプリ (0 hits) |
| 14 | applet | UNCONFIRMED: avoid the word; name the thing, or HOMEメニューの機能 | [M] lists ゲームメモ・フレンドリスト・おしらせリスト・インターネットブラウザー…, things you can use while a title is suspended, without a generic word | アプレット is homebrew jargon (3dbrew). Users won't recognize it |
| 15 | Wi-Fi / internet connection | Wi-Fi / インターネット (インターネットに接続) | [M] (インターネット 86x, 無線LANアクセスポイント, インターネット設定) | "Wi-Fi" stays Latin per style.md. 無線通信 = wireless communication ([M]) |
| 16 | download (to a phone) | ダウンロード | [M] (69x, ダウンロードソフト) | project rule: "Download", not "To phone". UI label: ダウンロード |
| 17 | save (file) / saved | 保存 / 保存する / 保存しました | [M] 「写真をSDカードに保存したり」 | セーブ is only for game save data (セーブデータ). Don't use it for files |
| 18 | recording (n.), record (v.) | 録画 / 録画する | UNCONFIRMED (3DS manuals have no screen recording. [M] uses 撮影 for camera, 録音 for audio) | 録画 is the everyday word for recording what is on a screen. Use 撮影 only for camera |
| 19 | clip | クリップ | UNCONFIRMED | an alternative is 動画. Nintendo Switch saves 30-second 動画, which suits "Replay (30 s)": リプレイ（30秒） |
| 20 | frame rate / fps, frame | フレームレート / fps, フレーム | UNCONFIRMED (general video term) | keep `fps` |
| 21 | button combination / hotkey | ボタンの組み合わせ (同時押し) | UNCONFIRMED; [M] describes it as 「HOMEメニュー画面で…と…を同時に押す」 | short form on buttons: 同時押し |
| 22 | screenshot (Rosalina) | スクリーンショット | community: Rosalina menu item "Screenshot" is English-only in Luma; Japanese users write スクリーンショット / スクショ | Nintendo calls screen images 画面写真 ([S] 「本書内の画面写真」). Use that only for Nintendo's own captures |
| 23 | settings / options | 設定 | [M] (244x, 「設定」をタッチ) | |
| 24 | Gallery | ギャラリー | UNCONFIRMED | 3DS has no "gallery". The Switch equivalent is アルバム. ギャラリー keeps the feature name distinct |
| 25 | Help | ヘルプ | UNCONFIRMED (0 hits in [M]/[S]. The 3DS uses 説明書 for its manuals) | ヘルプ is standard on Japanese UIs. 説明書 would suggest Nintendo's manual |
| 26 | Uninstall | アンインストール | UNCONFIRMED | Nintendo says ソフトを消去する in データ管理 ([M] 「データ管理…から不要なソフトを消去」). For removing our sysmodule, アンインストール is clearer. Use 消去 for deleting recordings |
| 27 | sound / audio | 音声 (audio track) / サウンド (sound setting) | [M] 「写真や音声データ」, サウンド (21x) | 音声あり / 音声なし for the audio switch |
| 28 | firmware / custom firmware / homebrew / sysmodule / plugin | ファームウェア / カスタムファームウェア (CFW) / 自作ソフト (homebrew) / システムモジュール / プラグイン | community: [G] ja_JP (カスタムファームウェア, CFW 45x; "Homebrew" left in Latin) | 自作ソフト is the common Japanese homebrew word but is UNCONFIRMED by [G]. システムモジュール and プラグイン are UNCONFIRMED (transliterations of Luma's English terms). Nintendo's own word for firmware is 本体の更新 / システムバージョン ([S]) |
| 29 | Rosalina menu | Rosalinaメニュー | community: [G] ja_JP 「Rosalinaメインメニューに戻ります」 | [G] also has "Rosalina menu" in Latin |
| 30 | QR code, scan | QRコード / 読み取る | [M] 「QRコードの読み取り」 | phone side: スマートフォンで読み取る / スマホのカメラで読み取る |
| 31 | lossless / quality | ロスレス (可逆) / 画質 | UNCONFIRMED (general video terms) | |
| 32 | converting / conversion | 変換 / 変換中 / MP4に変換 | UNCONFIRMED (in [M] only as 予測変換) | from project wording: 変換中 N% ("Converting N%"), ダウンロードできます ("Ready for download") |

## App terms (as translated)

Final wording in `app/source/lang/ja.c` and the phone page. Messages and help use です/ます; buttons, rows and
labels are plain nouns or dictionary-form verbs. No spaces between Japanese and Latin text or numbers
(SDカード, 録画5件, 30秒); units keep a space after a number in technical text (4 GB, 700 MB, 5 fps).
Paths in settings are written 設定→レコーダー→自動起動 (no spaces around →). A switch named in a sentence
is quoted: パネルで「レコーダー」をOFFにします.

| English | Japanese | note |
|---|---|---|
| Recorder (module, tile, settings group) | **レコーダー** | states: レコーダーはOFFです / レコーダーON / 動作中 |
| Replay 30 s (tile) / Replay (feature, gallery tag) | **リプレイ30秒** / **リプレイ** | 30秒 kept in the tile so it fits beside the switch |
| Launch On Startup | **自動起動** | |
| Convert To MP4 · Automatically · On Download | **MP4に変換** · **自動** · **ダウンロード時** | |
| Work With Lid Closed | **本体を閉じても処理を続ける** | lid = 本体を閉じる (term 5) |
| Share Statistics | **統計情報を送信** | |
| Gallery / Download / Settings / Help | **ギャラリー / ダウンロード / 設定 / ヘルプ** | Help tile subtitle uses 転送 for room: 録画・ランプ・転送 |
| How to use (guide) | **使いかた** | |
| Settings groups: Recording / Recorder / App / Language | **録画 / レコーダー / ソフト / 言語** | |
| this app | **このソフト** | Nintendo's ソフト, not アプリ (term 13) |
| applets | **HOMEメニューの機能** (インターネットブラウザー、ゲームメモ、フレンドリスト) | term 14 |
| recording / record | **録画 / 録画する** | counter: 録画%u件 (one form for every number) |
| clip | **クリップ** | |
| hotkey(s) | **ボタンの組み合わせ** (row); in text 録画の組み合わせ / リプレイの組み合わせ | L + R together: L + Rの同時押し |
| LED | **おしらせランプ** (row, help); **ランプ** in tight labels | Pulse / Solid / Off: **明滅 / 点灯 / OFF**; breathes red: 赤くゆっくり明滅; blinks green twice: 緑に2回点滅; glows blue: 青く点灯; faint blue: うす青く点灯; red three times: 赤に3回点滅 |
| on / off | **ON / OFF** | as Nintendo writes switches |
| frame rate (label) / in text | **FPS** / フレームレート | Bottom Screen FPS: **下画面のFPS**, As top: **上画面と同じ** |
| frames / skipped frames | **フレーム / コマ落ち** | |
| top / bottom screen | **上画面 / 下画面** | Both: 両方 |
| sound on / off | **音声あり / 音声なし** | Sound row: 音声 |
| Quality: Auto / Lossless | **画質: 自動 / ロスレス** | |
| converting / Ready for download / Could not convert | **変換中%d% / ダウンロードできます / 変換できませんでした** | |
| Joining recordings / Saving clip | **録画を結合中 / クリップを保存中** | |
| Cut off (power or a crash) | **途切れ**; 途中で途切れました（電源断かクラッシュ） | |
| delete / uninstall | **消去 / アンインストール** | Nintendo's 消去 for recordings; uninstall for the recorder |
| logs / Log for the developer | **ログ / 開発者向けログ** | |
| phone / computer | **スマートフォン / パソコン** | |
| restart / turn off / turn on | **再起動 / 電源を切る / 電源を入れる** | |
| Sleep Mode / System Settings / internal memory | **スリープモード / 本体設定 / 本体保存メモリー** | |
| memory (RAM) | **メモリー** | |
| Old 3DS / New 3DS | **Old 3DS / New 3DS** | |
| Luma option names | kept in English, in 「」 | |
| games named in Help 7 | 大乱闘スマッシュブラザーズ, ポケットモンスター サン・ムーン, ウルトラサン・ウルトラムーン, スーパーマリオメーカー, モンスターハンター, モンスターハンター4G / X / XX | |
