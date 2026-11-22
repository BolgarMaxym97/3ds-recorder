# r3d2video — 録画を PC で高画質な動画に

本体が作る MP4 はスマホ向け（400 × 480、強めの圧縮）です。録画ファイル（SD の
`/3ds/recorder/recordings/` にある `.r3d`）からもっと良い動画を作る方法は2つあります。

| | [変換ページ](https://bolgarmaxym97.github.io/3ds-recorder/) | r3d2video（このページ） |
|---|---|---|
| インストール | 不要：PC の Chrome・Edge・Safari | Windows 用 zip、または Python + ffmpeg |
| サイズ | 1×・2×・3×（くっきり） | 任意の整数倍（既定はプリセットごと） |
| 画面 | 録画のまま・上・下・横に並べる | 同じ＋画面ごとの別ファイル |
| 用途 | 共有・YouTube（高ビットレート）・編集（全フレームがキーフレーム）・Discord（10 MB 以下） | YouTube（H.264 CRF 16）・DaVinci Resolve（DNxHR HQ）・ロスレス（FFV1 + FLAC） |
| 押したボタン | **ボタン：表示する** | `--inputs` |
| 一部分だけ | 動画の下：ここから／ここまで | `--trim 開始 終了` |

**まずは変換ページを**：`.r3d` をドロップし、サイズ・画面・用途を選ぶだけ。ファイルは PC の外に出ません。
DNxHR、ロスレス、画面ごとの別ファイルが必要なときは r3d2video を使ってください。

## Windows

1. ffmpeg を一度だけ入れます：`winget install --id Gyan.FFmpeg -e`
   （または `ffmpeg.exe` を `r3d2video.exe` と同じフォルダに置く）
2. [`r3d2video-windows.zip`](https://github.com/BolgarMaxym97/3ds-recorder/releases/latest/download/r3d2video-windows.zip)（1.6.0 以降の各リリースに付属）をダウンロードし、好きな場所に展開します。
3. `.r3d`（複数可・フォルダも可）を **r3d2video.exe** にドラッグし、用途と画面の並べ方を選びます。
   動画は録画ファイルと同じフォルダにできます。

## 用途（プリセット）

| 用途 | ファイル | 映像 | 音声 | サイズ（上下2画面） |
|---|---|---|---|---|
| **youtube** | `.mp4` | H.264（CRF 16） | AAC 192 kb/s・48 kHz | 1200 × 1440（3倍） |
| **resolve** | `.mov` | DNxHR HQ（DaVinci Resolve 無料版でも読める） | PCM・48 kHz | 800 × 960（2倍、1080p に収まる） |
| **lossless** | `.mkv` | FFV1 RGB（全ピクセルそのまま） | FLAC（そのまま） | 400 × 480（等倍） |

拡大は整数倍・ニアレストネイバーなので、ドットがぼけません。しかも色を間引く（4:2:0 / 4:2:2）
**前に**拡大するので、細い色の線や文字がにじみません（等倍の 4:2:0 MP4 は、3DS の 2 × 2 ピクセルごとに
色が1つになります）。YouTube は 1440p 以上の動画に高いビットレートを割り当てるので、その点でも有利です。

## 画面の並べ方

`both` 上下（録画のまま）・`top` 上だけ・`bottom` 下だけ（320 × 240）・`side` 横並び・
`split` 画面ごとに別ファイル・`auto` 録画に入っている画面のまま（既定）

## オプション

    --preset youtube|resolve|lossless
    --layout auto|both|top|bottom|side|split
    --scale N        拡大倍率（整数）を指定
    --fps N          出力のフレームレート（既定は録画のもの）
    --no-eq          本体スピーカー風の高音カットをせず、DSP の音のまま
    --inputs         押したボタンを表示（1.6.0 以降の録画）。両画面なら下画面の横、それ以外は隅
    --trim 開始 終了  開始〜終了（秒）の部分だけを書き出す
    --out DIR        出力先フォルダ
    --ffmpeg PATH    使う ffmpeg を指定

---

r3d2video は [noctis1008-wq](https://github.com/noctis1008-wq) さんがフォークで作り、1.6.0 で本プロジェクトに取り込みました。ありがとうございます！
