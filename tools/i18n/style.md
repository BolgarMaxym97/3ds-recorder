# Translating 3DS Recorder

The app's texts are `app/source/i18n.h` (keys and English) and `app/source/lang/<code>.c`; the phone page's are
`TEXT` in `web/app.js`. `tools/i18n.py context` lists every key with where it is drawn; `tools/azahar/tour.sh`
makes pictures of every screen in a language. Each language keeps a glossary in `glossary/<code>.md`.

## What the app is

A recorder for a modded Nintendo 3DS (Luma3DS custom firmware). It runs in the background and records the whole
console (both screens and sound) into a file on the SD card; the app turns recordings into MP4 and sends them to a
phone over Wi-Fi. The people reading it play games on a 3DS and know its own menus in their language, so the
console's things are named **as the console itself names them** in that language (System Settings, HOME Menu,
Sleep Mode, SD card…). Luma3DS, Rosalina, homebrew terms stay as the 3DS homebrew community writes them.

## Rules

1. Meaning first, then natural wording: write what a native UI writer would, not a word-for-word copy. Short:
   the screens are 400 x 240 and 320 x 240 pixels, much of the text sits in buttons and narrow cards.
2. Placeholders stay, in the same order, unchanged: `%s` `%u` `%d` `%lu` `%08lX` `%%` (app); `%1` `%2` (phone page).
   What goes into them is in the context (`%s` in a hotkey sentence is the button combination, like `L + Y`).
3. `\n` stays where it is: some texts are drawn line by line (the LED colours in Help 2 have a lamp per line).
4. Kept as they are: `3DS Recorder`, `Luma3DS`, `Luma`, `Rosalina`, `HOME`, `SELECT`, `START`, button letters
   (`A B X Y L R`), `Wi-Fi`, `SD`, `MP4`, `.r3d`, `XR32`, `C420`, `fps`, file paths (`/3ds/recorder/...`),
   URLs, `t.me/...`. Size units are their own keys (`S_MB`, `S_GB`, `S_MBS`, web `mb`/`gb`/`mbs`): the language's usual
   abbreviation there (French Mo/Go, Ukrainian МБ/ГБ), and the same inside sentences. Decimal numbers: the app writes
   the language's separator itself; inside texts write it the same way (1,5 s).
5. Feature names are translated once and then used the same everywhere: Recorder, Replay (30 s), Launch On
   Startup, Convert To MP4, Work With Lid Closed, Share Statistics, Gallery, Download, Settings, Help.
6. English captions are Title Case; other languages use their own convention (sentence case in most; German
   capitalises nouns anyway).
7. Typography of the language: its quotation marks, its spacing before punctuation (French: a no-break space,
   U+00A0, before `: ; ! ?` and inside « »), its ellipsis (`…` U+2026 is in every font), its full-width punctuation
   in Japanese and Chinese (、。「」（）：！？), no spaces between Japanese/Chinese words, a space between a
   number and its unit where the language wants one.
8. Address the user the way Nintendo's 3DS software does in that language (the glossary says which).
9. The Ukrainian text is a second source: where English is terse or ambiguous it shows what is meant.
