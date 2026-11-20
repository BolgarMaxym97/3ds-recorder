/*
 * The app's texts. STRINGS lists every key with its English text (the default and the fallback);
 * each other language is a table in lang/<code>.c, where a key left out shows its English text
 * (tools/i18n-check.mjs fails on one). Chosen in the language list (first run, Settings →
 * Language) and kept in config.ini as lang=<code> (LANGS).
 */
#ifndef I18N_H
#define I18N_H

#include <stdbool.h>

#define STRINGS(X) \
    X(S_STATE_READY, "Ready to record") \
    X(S_STATE_RECORDING, "Recording · %s") \
    X(S_STATE_SAVING, "Saving…") \
    X(S_STATE_OFF, "Recorder is off") \
    X(S_STATE_STARTING, "Starting…") \
    X(S_STATE_STOPPING, "Stopping…") \
    X(S_STATE_ERROR, "Recorder error") \
    X(S_PILL_ON, "Recorder on") \
    X(S_PILL_OFF, "Off") \
    X(S_PROFILE, "Profile") \
    X(S_PROFILE_STD, "Standard") \
    X(S_FPS, "Frame rate") \
    X(S_TARGET, "target") \
    X(S_SCREENS, "Screens") \
    X(S_BOTH, "Both") \
    X(S_TOP, "Top") \
    X(S_BOTTOM, "Bottom") \
    X(S_SOUND_ON, "sound on") \
    X(S_SOUND_OFF, "sound off") \
    X(S_SD_FREE, "SD free") \
    X(S_MINUTES, "≈ %u min") \
    X(S_FRAMES, "Frames") \
    X(S_SKIPPED, "skipped %u") \
    X(S_FILE, "File") \
    X(S_LAST, "Last recording") \
    X(S_NO_RECORDINGS, "No recordings yet") \
    X(S_N_REC_ONE, "%u recording") \
    X(S_N_REC_FEW, "%u recordings") \
    X(S_N_REC_MANY, "%u recordings") \
    X(S_NO_RECORDINGS_HINT, "In a game or HOME press %s") \
    X(S_WHOLE_CONSOLE, "The whole console is recorded") \
    X(S_WHOLE_CONSOLE_BODY, "Games, HOME, applets and switching between them go into one file. Stop with the hotkey or the button below. The LED breathes red while recording.") \
    X(S_TILE_RECORDER, "Recorder") \
    X(S_TILE_STOP, "Stop recording") \
    X(S_TILE_STOP_SUB, "Saved; the LED blinks green twice") \
    X(S_TILE_GALLERY, "Gallery") \
    X(S_TILE_PHONE, "Download") \
    X(S_TILE_SETTINGS, "Settings") \
    X(S_TILE_HELP, "Help") \
    X(S_LANG_CONTINUE, "Continue") \
    X(S_LANG_CHOOSE, "Choose your language") \
    X(S_GUIDE_OF, "How to use · %d of %d") \
    X(S_GUIDE_NEXT, "Next") \
    X(S_GUIDE_DONE, "Got it") \
    X(S_GUIDE_SKIP, "Skip") \
    X(S_GUIDE_OPEN, "How to use") \
    X(S_G1_TITLE, "Record anything") \
    X(S_G1_BODY, "In a game, in HOME, anywhere: press %s. The whole console is recorded, both screens and sound. The LED breathes red while it records.") \
    X(S_G2_TITLE, "Stop") \
    X(S_G2_BODY, "Press %s again. The recording is saved to the SD card and the LED blinks green twice.") \
    X(S_G3_TITLE, "Watch and download") \
    X(S_G3_BODY, "Open the Gallery: A plays a recording, Y downloads it to a phone over Wi-Fi through a QR code. Or take it straight from the SD card in a computer: /3ds/recorder/recordings (a converted recording has its MP4 next to it). While this app is open, the console converts recordings into small MP4 videos in the background (or only the one you download: Settings → App → Convert To MP4).") \
    X(S_G4_TITLE, "After turning the console on") \
    X(S_G4_BODY, "Open this app once: the recorder starts by itself and keeps running after you leave the app. When it is ready, the LED glows blue for a moment. When the console is turned off or restarts (leaving System Settings or starting a DS or GBA game restarts it), the recorder stops and a recording is saved: open this app again then. Or switch on Settings → Recorder → Launch On Startup: the recorder then starts with the console.") \
    X(S_TILE_HELP_SUB, "Recording, LED, download") \
    X(S_TILE_SETTINGS_SUB, "Recording, app, language") \
    X(S_START_STOP, "Start / stop:") \
    X(S_MSG_STARTED, "Recorder started: the LED glows blue.") \
    X(S_MSG_STOPPED, "Recorder stopped.") \
    X(S_MSG_STOP_FAILED, "The recorder did not stop.") \
    X(S_MSG_START_FAILED, "Could not start the recorder (%s, %08lX).") \
    X(S_DELETING, "Deleting…") \
    X(S_MSG_SYSTEM_STOP, "Recording of %s stopped because the console restarted or turned off (System Settings restart it on exit). It is saved.") \
    X(S_MSG_GAME_STOP, "Recording of %s is saved. On Old 3DS a game that needs extra memory restarts the console when it closes, and the recording stops there.") \
    X(S_MSG_RECOVERED, "Recording of %s was cut off (power or a crash): what reached the card is saved.") \
    X(S_MSG_SOON, "This comes in a later update.") \
    X(S_FR_EYEBROW, "Checks before recording") \
    X(S_FR_ALMOST, "Almost ready") \
    X(S_FR_LUMA, "Luma3DS") \
    X(S_FR_LUMA_OK, "v%lu.%lu · supported") \
    X(S_FR_LUMA_UNKNOWN, "not detected") \
    X(S_FR_OPTION, "Luma: external modules") \
    X(S_FR_ON, "on") \
    X(S_FR_OFF, "off") \
    X(S_FR_MODULE, "Recorder module") \
    X(S_FR_MODULE_OK, "installed") \
    X(S_FR_MODULE_UPDATE, "will be updated") \
    X(S_FR_MODULE_MISSING, "will be installed") \
    X(S_FR_RUNNING, "Recorder running") \
    X(S_FR_RUNNING_NO, "not started yet") \
    X(S_FR_SD, "Recordings on SD") \
    X(S_FR_SD_FREE, "%s free") \
    X(S_FR_START_TITLE, "Start the recorder") \
    X(S_FR_START_BODY, "The recorder is not running. Press Start. Then you can leave this app: the recorder keeps running until the console is turned off or restarts.") \
    X(S_FR_LED, "The LED glows blue for 1.5 s: ready") \
    X(S_FR_LUMA_TITLE, "Turn on a Luma option") \
    X(S_FR_LUMA_BODY, "Hold SELECT while turning the console on. In the Luma menu enable \"Enable loading external FIRMs and modules\", save with START, then open this app again.") \
    X(S_LATER, "Later") \
    X(S_START, "Start") \
    X(S_HELP_TITLE, "Help") \
    X(S_HELP_OF, "Help · %d of %d") \
    X(S_H1, "How to record") \
    X(S_H1_BODY, "1. Open this app: the recorder starts by itself, the LED glows blue for a moment.\n2. Go to HOME and start a game. You can close the app: the recorder keeps running.\n3. Press %s: recording starts, the LED breathes red.\n4. The whole console is recorded: game, HOME, other apps, with sound, into one file.\n5. Press %s again: the recording is saved, the LED blinks green twice.") \
    X(S_H2, "What the LED means") \
    X(S_H2_BODY, "Blue for 1.5 s: the recorder started and is ready.\nRed, breathing: recording.\nFaint blue, steady: Replay keeps the last 30 s.\nGreen twice: a recording or a clip is saved.\nRed three times: an error, open this app.\nNo light and the hotkey does nothing: the recorder is off, or the console was just turned on (see 3).") \
    X(S_H3, "After turning the console on") \
    X(S_H3_BODY, "With Settings → Recorder → Launch On Startup the recorder starts with the console by itself (see 9). Without it, the recorder starts when you open this app: after each power-on open the app once, that is all. Leaving the app does not stop it; to stop it until the app is opened again, switch Recorder off on the panel. Leaving System Settings or starting a DS or GBA game restarts the console like turning it off: a recording is saved and stops there, and the recorder starts again when you open this app (or by itself with Launch On Startup). Closing the lid saves a recording; it goes on when the lid opens and becomes one recording again (see 10).") \
    X(S_H4, "Getting a recording off") \
    X(S_H4_BODY, "Watch recordings in the Gallery (A) and download them to a phone over Wi-Fi (Y). While this app is open, the console converts each recording into a small MP4 in the background; it lies next to the recording in /3ds/recorder/recordings, so the SD card in a computer has it too. To convert on a computer instead (faster for long ones: bolgarmaxym97.github.io/3ds-recorder), set Settings → App → Convert To MP4 → On Download: then only the recording you download is converted.\nYou can close the lid while this app converts, joins recordings or saves clips: the screens go dark, the work goes on, and the console goes to sleep when all is done (Settings → App → Work With Lid Closed).") \
    X(S_H5, "Hotkeys and limits") \
    X(S_H5_BODY, "The recording hotkey is %s; Replay has its own for saving a clip (see 8). Change both in Settings → Recorder → Hotkeys: first the recording one, then the Replay one; START keeps the current one. Not allowed: L + R together (opens the Camera in HOME), L + ↓ + SELECT (the Rosalina menu) or a part of it, START, and one hotkey holding all the buttons of the other. A long recording goes on in a new file every 4 GB or every hour (a 1–2 s gap); Settings shows how many minutes one file holds.") \
    X(S_H6, "If something is wrong") \
    X(S_H6_BODY, "Red three times: the recording did not start or stopped, usually because the SD card is full. Skipped frames: the SD card could not keep up, the video is still fine. A 3D game looks jerky in the video: set Settings → Recording → Quality → Auto. The top screen runs short of frames while the bottom one shows a still menu: lower Settings → Recording → Bottom Screen FPS.\nA game does not start or freezes while the recorder runs: switch Recorder off on the panel and play without it. Please tell us in Issues at github.com/BolgarMaxym97/3ds-recorder: name the game and the console, and attach the logs /3ds/recorder/recorder.log and app.log (or Download → Log for the developer). With Settings → App → Share Statistics on, the logs reach us by themselves when this app opens with Wi-Fi.") \
    X(S_H7, "Games that need extra memory") \
    X(S_H7_BODY, "On Old 3DS some games need more memory: Smash Bros, Pokemon Sun / Moon and Ultra Sun / Ultra Moon, Super Mario Maker, Monster Hunter. The console restarts when they start and when they close. If the recorder runs, it comes back by itself inside the game through a tiny plugin of Luma: a recording goes on in a new file, Replay works too. Luma's Plugin Loader must be on (Rosalina menu, L + ↓ + SELECT); with Launch On Startup the recorder turns it on just for that game. The game has 2 MB less memory and the video is less smooth; Settings → Recording → Screens → Top makes it smoother. Monster Hunter 4 Ultimate, Generations and XX need all of their memory: no plugin there, so the recorder is in them only with Launch On Startup (see 9). When such a game closes, the recording is saved; open this app or use Launch On Startup to record again. New 3DS does not restart for these games.") \
    X(S_H8, "Replay: the last 30 seconds") \
    X(S_H8_BODY, "Switch Replay 30 s on (the panel). While a game or another program runs (not in the HOME Menu alone), the recorder keeps the last 30 seconds all the time, and the LED glows faint blue (from about 15 seconds after a game starts). Press %s (Settings → Recorder → Hotkeys changes it) in a game or HOME: the LED blinks green twice and those seconds (a few more at times) become a clip; red three times: Replay is not running there. Open this app: the clip turns up in the Gallery like any recording.\nThe recording hotkey works as usual: the replay pauses while a recording runs and while this app is open.\nThe replay writes to the SD card all the time (several GB an hour in a 3D game): the card wears. It keeps 512 MB of the card while it is on (700 MB must be free to switch it on). After the console has been turned off, Replay is off again; with Launch On Startup it stays as you left it. On Old 3DS, games that need extra memory get fewer frames a second in it.") \
    X(S_H10, "Applets and pauses") \
    X(S_H10_BODY, "An applet (Internet Browser, Game Notes, Friend List...) needs the memory the recorder holds. When one opens during a recording, the recording is saved first. On Old 3DS it goes on next to a small applet at a lower frame rate; nothing is recorded next to the Internet Browser (it needs nearly all the memory), and on New 3DS nothing next to any applet. When the applet closes, the recording goes on. Replay waits until then.\nThe parts of a recording the recorder paused by itself (an applet, the lid) become one recording again while this app is open: keep it open (the lid may be closed) until \"Joining recordings\" is gone. A pause shows as half a second of the last picture. Recordings you stop yourself stay apart.") \
    X(S_H9, "Launch on startup") \
    X(S_H9_BODY, "Settings → Recorder → Launch On Startup (off at first). The recorder then starts by itself each time the console is turned on or restarts: no need to open this app, and Replay stays as you left it. On Old 3DS it also comes back after a game that needs extra memory closes.\nIt needs two options in Luma's menu (hold SELECT while turning the console on): \"Enable loading external FIRMs and modules\" and \"Enable game patching\".\nIt adds one small file for Luma to the SD card; the console's own memory is not changed. If the console ever fails to start with it, turn the console on again: the recorder switches Launch On Startup off by itself. If that does not help, hold SELECT at power-on and turn \"Enable game patching\" off.\nBefore deleting the recorder's files by hand, switch this off or use Settings → App → Uninstall recorder.") \
    X(S_AUTOSTART, "Launch On Startup") \
    X(S_LID_WORK, "Work With Lid Closed") \
    X(S_LID_WORK_NOTE, "Work With Lid Closed: close the lid while this app converts, joins recordings or saves clips, and the work goes on with the screens dark; the console goes to sleep when all is done, or when the battery is nearly empty. Off: the console sleeps at once and the work waits.") \
    X(S_STATS, "Share Statistics") \
    X(S_STATS_NOTE, "Share Statistics: when this app opens with Wi-Fi, it quietly sends the new lines of its logs, new Luma crash reports, the settings, the console model and versions, so we see how the recorder works on consoles and in games we cannot test. No IP address, name or Friend Code. ID: %s") \
    X(S_AUTOSTART_NOTE, "Launch On Startup: the recorder starts with the console, also after it restarts, and Replay stays as you left it. Needs \"Enable game patching\" in Luma's menu too. More in Help, 9.") \
    X(S_MSG_AUTO_ON, "Launch On Startup is on: the recorder now starts with the console.") \
    X(S_MSG_AUTO_OFF, "Launch On Startup is off.") \
    X(S_MSG_AUTO_PATCHING, "Launch On Startup needs a Luma option: hold SELECT while turning the console on, enable \"Enable game patching\" and save with START.") \
    X(S_MSG_AUTO_SD_ERROR, "Launch On Startup could not be set up: the SD card refused the file (see /3ds/recorder/recorder.log). Nothing was changed.") \
    X(S_MSG_AUTO_UNSUPPORTED, "Launch On Startup could not be set up on this console (see /3ds/recorder/recorder.log). Nothing was changed.") \
    X(S_MSG_AUTO_FAILED, "The console did not start properly with Launch On Startup, so the recorder switched it off. Details are in /3ds/recorder/recorder.log.") \
    X(S_MSG_AUTO_LATER, "Launch On Startup is saved: it is set up once the recorder runs and no recording is under way.") \
    X(S_REPLAY, "Replay 30 s") \
    X(S_TILE_RECORDER_ON_SHORT, "Hotkey works") \
    X(S_TILE_RECORDER_OFF_SHORT, "Hotkey does nothing") \
    X(S_TILE_REPLAY_ON, "Clip: %s") \
    X(S_TILE_REPLAY_OFF, "Keeps the last 30 s") \
    X(S_MSG_REPLAY_ON, "Replay is on. It starts about 15 s after a game starts; then press %s in the game or HOME to save the last 30 seconds.") \
    X(S_MSG_REPLAY_OFF, "Replay is off.") \
    X(S_MSG_REPLAY_WRITE, "Replay stopped: the SD card refused its writes. Check the card, then turn Replay off and on again.") \
    X(S_MSG_REPLAY_SPACE, "Replay needs 700 MB free on the SD card.") \
    X(S_CLIP_PCT, "Saving clip %d%%") \
    X(S_GAL_CLIP, "Replay") \
    X(S_CLIP_WAIT, "Saving clip…") \
    X(S_CLIP_MORE, "%s · %d more") \
    X(S_JOIN_PCT, "Joining recordings %d%%") \
    X(S_JOIN_WAIT, "Joining recordings…") \
    X(S_MSG_CLIPS, "Clips are being saved: %d. Each appears in the Gallery when it is ready; keep this app open.") \
    X(S_SETTINGS, "Settings") \
    X(S_LANGUAGE, "Language") \
    X(S_UNINSTALL_ROW, "Uninstall recorder…") \
    X(S_QUALITY, "Quality") \
    X(S_Q_AUTO, "Auto") \
    X(S_Q_LOSSLESS, "Lossless") \
    X(S_SOUND, "Sound") \
    X(S_LED, "LED") \
    X(S_LED_PULSE, "Pulse") \
    X(S_LED_SOLID, "Solid") \
    X(S_LED_OFF, "Off") \
    X(S_HOTKEY, "Hotkeys") \
    X(S_GRP_RECORDING, "Recording") \
    X(S_GRP_RECORDER, "Recorder") \
    X(S_GRP_APP, "App") \
    X(S_BOTTOM_FPS, "Bottom Screen FPS") \
    X(S_BOTTOM_SAME, "As top") \
    X(S_BOTTOM_FPS_NOTE, "Bottom Screen FPS: with both screens, the bottom one can be captured less often. A still menu there then leaves more of the console to the top screen (smoother on Old 3DS); a game played on the bottom screen looks choppy at 5 or 1 fps. As top: the top screen's frame rate.") \
    X(S_MSG_BOTTOM_BOTH, "Bottom Screen FPS works when both screens are recorded.") \
    X(S_CONVERT, "Convert To MP4") \
    X(S_CONVERT_AUTO, "Automatically") \
    X(S_CONVERT_MANUAL, "On Download") \
    X(S_CONVERT_NOTE, "Convert To MP4: Automatically, every recording becomes an MP4 in the background while this app is open. On Download, only the recording you download to a phone is converted; the rest stay .r3d for a computer (bolgarmaxym97.github.io/3ds-recorder converts them in the browser).") \
    X(S_FILE_TITLE, "What goes into the file") \
    X(S_FRAME_IN_FILE, "Frame in the file") \
    X(S_TOP_BOX, "top 400×240") \
    X(S_BOTTOM_BOX, "bottom 320×240") \
    X(S_CODEC, "Codec") \
    X(S_CODEC_VAL, "XR32 · lossless") \
    X(S_CODEC_AUTO, "XR32 + C420") \
    X(S_STREAM, "To the SD card") \
    X(S_PER_MIN, "Per minute") \
    X(S_FITS, "Fits on the SD card") \
    X(S_SETTINGS_NOTE, "Frame rate, screens, sound and LED apply from the next recording, the hotkeys right away. One file holds up to 4 GB, about %u min.") \
    X(S_HK_SCREEN, "1 of 2 · Start and stop a recording") \
    X(S_HK_SCREEN2, "2 of 2 · Save the last 30 s (Replay)") \
    X(S_HK_ERR_OTHER, "It shares all its buttons with the recording hotkey: both would fire.") \
    X(S_HK_KEEP, "Keep") \
    X(S_HK_SAVED2, "Replay hotkey saved: %s. It works right away.") \
    X(S_HK_HOLD_TITLE, "Hold the new combination") \
    X(S_HK_HOLD_HINT, "Hold for 1 s to set it · 2–3 buttons") \
    X(S_HK_TOO_MANY, "At most 3 buttons") \
    X(S_HK_NOW, "Now") \
    X(S_HK_REVIEW_TITLE, "New combination") \
    X(S_HK_ERR_LR, "L + R together is not allowed: in HOME it opens the Camera.") \
    X(S_HK_ERR_ROSALINA, "This is part of L + ↓ + SELECT, the Rosalina menu.") \
    X(S_HK_ERR_FACE, "Only A / B / X / Y is too easy to press in a game. Add SELECT, L or R.") \
    X(S_HK_OK, "This combination can be used.") \
    X(S_HK_RULES, "Not allowed: L + R together (Camera in HOME), L + ↓ + SELECT (Rosalina), START.") \
    X(S_HK_KEYS_CAPTURE, "START: keep the current one") \
    X(S_HK_KEYS_RETRY, "B: again · START: keep the current one") \
    X(S_HK_KEYS_REVIEW, "A: save · B: again · START: keep") \
    X(S_RESET, "Reset") \
    X(S_SAVE, "Save") \
    X(S_HK_SAVED, "Hotkey saved: %s. It works right away.") \
    X(S_UN_EYEBROW, "Settings · Uninstall recorder") \
    X(S_UN_TITLE, "Full removal") \
    X(S_UN_MODULE, "Recorder module") \
    X(S_UN_MODULE_SUB, "stops and leaves Luma") \
    X(S_UN_SETTINGS, "Settings and log") \
    X(S_UN_APP, "This app") \
    X(S_UN_APP_CIA, "icon in HOME") \
    X(S_UN_RECORDINGS, "Recordings") \
    X(S_UN_RECORDINGS_KEEP, "kept unless ticked") \
    X(S_UN_ASK, "Uninstall the recorder?") \
    X(S_UN_ASK_BODY, "The console goes back to how it was without the recorder.") \
    X(S_UN_ALSO, "Also delete %s (%s)") \
    X(S_CANCEL, "Cancel") \
    X(S_PH_EYEBROW, "Phone and console on one Wi-Fi") \
    X(S_PH_TITLE, "Point the phone's camera at the code") \
    X(S_PH_BODY, "The console converts the recording into a small MP4 video, the phone downloads and saves it: nothing to install.") \
    X(S_PH_CONNECTED, "Phone connected") \
    X(S_PH_WAITING, "Waiting for the phone") \
    X(S_PH_IDLE, "Scan the code: the phone starts the download by itself.") \
    X(S_PH_SLOW, "Keep the lid open until the phone has the video.") \
    X(S_PH_SD, "Without Wi-Fi: turn the console off, take out the SD card and put it in a computer. Recordings are in /3ds/recorder/recordings.") \
    X(S_PH_STOP, "Stop") \
    X(S_PH_NO_WIFI, "Wi-Fi is off") \
    X(S_PH_NO_WIFI_BODY, "Turn Wi-Fi on (the switch on the console, or HOME → settings) and connect to the network the phone is on. Then press A.") \
    X(S_PH_FAILED, "Could not start sending") \
    X(S_PH_FAILED_BODY, "The network did not start. Check Wi-Fi and press A to try again.") \
    X(S_PH_RETRY, "Try again") \
    X(S_PH_LEFT, "≈ %u min left") \
    X(S_PH_SENDING, "Sending") \
    X(S_PH_HINT, "Download") \
    X(S_PH_READY, "Ready") \
    X(S_PH_PREPARING, "Converting") \
    X(S_PH_PREP_BODY, "The console converts the recording into a small video; the phone waits and then downloads it by itself.") \
    X(S_PH_PREP_FAILED, "Could not convert the recording: the phone gets the original.") \
    X(S_GAL_PREP_READY, "Ready for download") \
    X(S_GAL_PREP_PCT, "Converting %d%%") \
    X(S_GAL_PREP_FAILED, "Could not convert") \
    X(S_PLAY_FAILED, "Could not open the recording.") \
    X(S_MSG_NO_DSP, "No sound in this player: the console's DSP firmware has not been dumped. Run DSP1 once (it saves /3ds/dspfirm.cdc). The recordings have their sound.") \
    X(S_MSG_START_OLD_LUMA, "Could not start the recorder (%s, %08lX). Update Luma3DS to the latest release, then try again.") \
    X(S_PL_SEEK, "±10 s") \
    X(S_PL_PAUSE, "Pause") \
    X(S_PL_BACK, "Gallery") \
    X(S_PL_NO_DSP, "No sound: the DSP firmware is missing (run DSP1 once).") \
    X(S_PL_TOP_ONLY, "Only the top screen was recorded") \
    X(S_PL_BOTTOM_ONLY, "Only the bottom screen was recorded") \
    X(S_GAL_NTH, "Recording %d of %d") \
    X(S_DURATION, "Length") \
    X(S_SIZE, "Size") \
    X(S_DROPPED, "Frames skipped") \
    X(S_WITH_SOUND, "with sound") \
    X(S_NO_SOUND, "no sound") \
    X(S_BOTH_SCREENS, "both screens") \
    X(S_TOP_SCREEN, "top screen") \
    X(S_BOTTOM_SCREEN, "bottom screen") \
    X(S_NOT_CLOSED, "Cut off (power or a crash): what reached the card is saved.") \
    X(S_GAL_CUT, "Cut off") \
    X(S_UNREADABLE, "This file can't be read.") \
    X(S_PLAY, "Play") \
    X(S_DELETE, "Delete") \
    X(S_OPEN, "Open") \
    X(S_GAL_LOADING, "Reading the SD card…") \
    X(S_DEL_TITLE, "Delete this recording?") \
    X(S_DEL_BODY, "%s · %s. The file is removed from the SD card.") \
    X(S_DEL_BTN, "Delete") \
    X(S_DELETED, "Recording deleted.") \
    X(S_DEL_FAILED, "Could not delete the recording.") \
    X(S_UNINSTALL, "Uninstall") \
    X(S_UN_WORKING, "Uninstalling…") \
    X(S_UN_DONE, "Uninstalled") \
    X(S_UN_DONE_BODY, "3DS Recorder is removed. Luma settings were not changed: if you turned on external modules only for it, you can turn the option off.") \
    X(S_UN_PARTIAL, "Some files could not be removed") \
    X(S_UN_PARTIAL_BODY, "Delete what is left by hand: /luma/sysmodules/0004013000F1D302.cxi and this app (Data Management for the HOME icon).") \
    X(S_MB, "MB") \
    X(S_GB, "GB") \
    X(S_MBS, "MB/s") \
    X(S_EXIT, "Exit")

typedef enum {
#define X(id, en) id,
    STRINGS(X)
#undef X
    S_COUNT
} Str;

/* The languages, in the order the language list shows them. A new one: an entry here and in
   LANGS (i18n.c), its table in lang/<code>.c, a flag in ui_flag_sized, a TEXT table in web/app.js,
   then tools/i18n-check.mjs. */
typedef enum {
    LANG_EN, LANG_UK, LANG_DE, LANG_ES, LANG_FR, LANG_IT, LANG_PT, LANG_JA, LANG_ZH_HANS, LANG_ZH_HANT, LANG_COUNT
} Lang;

/* the font a language's text needs (ui.c loads it) */
typedef enum { SCRIPT_LATIN, SCRIPT_JA, SCRIPT_ZH_HANS, SCRIPT_ZH_HANT } Script;

typedef struct {
    const char *code;          /* config.ini lang=, the phone page's lang */
    const char *name;          /* in its own language */
    const char *const *text;   /* S_COUNT texts (NULL: English); NULL for English itself */
    Script script;
} LangInfo;

extern const LangInfo LANGS[LANG_COUNT];

void i18n_set_lang(Lang lang);
Lang i18n_lang(void);
/* lang= value -> Lang; LANG_EN for anything unknown */
Lang i18n_lang_from_code(const char *code);
/* the console's language (CFG_LANGUAGE_*: 0 JP, 1 EN, 2 FR, 3 DE, 4 IT, 5 ES, 6 ZH, 7 KO, 8 NL,
   9 PT, 10 RU, 11 TW) -> the language to offer first; LANG_EN when there is none */
Lang i18n_lang_from_system(int cfgLanguage);
const char *T(Str id);
/* the same text in a given language (the language list shows each in its own) */
const char *T_in(Lang lang, Str id);
/* "12 recordings" / "12 записів", with the plural rules of the language */
const char *i18n_recordings(unsigned n, char *buf, unsigned cap);
/* a recording's date: "Nov 3, 2026" / "03.11.2026" / "2026/11/03"; no year: "Nov 3" / "03.11" */
/* the decimal separator: '.' (en, ja, zh) or ',' */
char i18n_decimal(void);
const char *i18n_date(int y, int mo, int d, bool year, char *buf, unsigned cap);

#endif
