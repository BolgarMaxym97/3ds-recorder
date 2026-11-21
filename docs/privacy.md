# Anonymous statistics

[← README](../README.md) · [Українська](privacy.uk.md)

There are many consoles and games we cannot test ourselves, the New 3DS above all. So when the app opens and the
console is on Wi-Fi, it quietly sends what its logs gained since the last time.

Settings → App → **Share Statistics** is on at first and can be switched off at any time. With it off, or without
Wi-Fi, the app does not even try to connect. Nothing is shown and nothing waits for it: if sending fails, the same
lines go next time.

**What is sent:** the new lines of `app.log` and `recorder.log` (at most 48 KB per launch), the newest Luma crash report
from `/luma/dumps/arm11/` that was not sent yet (and the dates of the unsent ones), `config.ini`, the app's language and the console's, the console model, region, system, Luma and kernel
versions, the app version and a random console ID. The logs say which games ran (title IDs), how recordings went
(frame rate, dropped frames, errors), what the console did (HOME, sleep, power-off), and, when you downloaded a
recording to a phone, the browser that opened the page (its user agent: browser, system and often the phone model,
screen size, language) and the recording's name (its date).

**What is not:** IP address (it is not stored, and LAN addresses in the logs are blanked on the console), country,
serial number, Friend Code, name, Mii.

**How long:** log text and crash reports are deleted after 180 days. The short figures taken from them (console
model, versions, settings, title IDs, frame counts) stay for the statistics.

**How it travels:** over plain HTTP, the way the console's own network stack reaches the server, so it is not
encrypted on the way.

**Your ID** is shown in Settings → App next to the switch. To have your records deleted, write it in an
[issue](https://github.com/BolgarMaxym97/3ds-recorder/issues/new) or on Telegram (t.me/side_quest_ua). Deleting
`/3ds/recorder/stats.ini` makes a new ID.
