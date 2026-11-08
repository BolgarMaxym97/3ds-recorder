/*
 * The app's texts in English (the default) and Ukrainian. One X-macro list, so a key without
 * both texts does not compile. Switched by the flag on the control panel or in Settings and
 * kept in config.ini as lang=en|uk.
 */
#ifndef I18N_H
#define I18N_H

#include <stdbool.h>

#define STRINGS(X) \
    X(S_STATE_READY, "Ready to record", "Готово до запису") \
    X(S_STATE_RECORDING, "Recording · %s", "Йде запис · %s") \
    X(S_STATE_SAVING, "Saving…", "Зберігаю…") \
    X(S_STATE_OFF, "Recorder is off", "Рекордер вимкнено") \
    X(S_STATE_STARTING, "Starting…", "Запускаю…") \
    X(S_STATE_STOPPING, "Stopping…", "Зупиняю…") \
    X(S_STATE_ERROR, "Recorder error", "Помилка рекордера") \
    X(S_PILL_ON, "Recorder on", "Рекордер працює") \
    X(S_PILL_OFF, "Off", "Вимкнено") \
    X(S_PROFILE, "Profile", "Профіль") \
    X(S_PROFILE_STD, "Standard", "Стандарт") \
    X(S_FPS, "Frame rate", "Частота") \
    X(S_TARGET, "target", "ціль") \
    X(S_SCREENS, "Screens", "Екрани") \
    X(S_BOTH, "Both", "Обидва") \
    X(S_TOP, "Top", "Верхній") \
    X(S_SOUND_ON, "sound on", "звук увімк.") \
    X(S_SOUND_OFF, "sound off", "звук вимк.") \
    X(S_SD_FREE, "SD free", "SD вільно") \
    X(S_MINUTES, "≈ %u min", "≈ %u хв") \
    X(S_FRAMES, "Frames", "Кадри") \
    X(S_SKIPPED, "skipped %u", "пропущено %u") \
    X(S_FILE, "File", "Файл") \
    X(S_LAST, "Last recording", "Останній запис") \
    X(S_NO_RECORDINGS, "No recordings yet", "Записів ще немає") \
    X(S_NO_RECORDINGS_HINT, "In a game or HOME press %s", "У грі чи HOME натисніть %s") \
    X(S_WHOLE_CONSOLE, "The whole console is recorded", "Записується вся консоль") \
    X(S_WHOLE_CONSOLE_BODY, "Games, HOME, applets and switching between them go into one file. Stop with the hotkey or the button below. The LED breathes red while recording.", "Гра, HOME, аплети й перемикання між програмами — в одному файлі. Зупинити: хоткей або кнопка нижче. LED «дихає» червоним, поки йде запис.") \
    X(S_TILE_RECORDER, "Recorder", "Рекордер") \
    X(S_TILE_RECORDER_ON, "Hotkey works in games and HOME", "Хоткей у грі й HOME працює") \
    X(S_TILE_RECORDER_OFF, "Hotkey does nothing", "Хоткей не працює") \
    X(S_TILE_STOP, "Stop recording", "Зупинити запис") \
    X(S_TILE_STOP_SUB, "Saved; the LED blinks green twice", "Файл збережеться, LED мигне зеленим") \
    X(S_TILE_GALLERY, "Gallery", "Галерея") \
    X(S_TILE_PHONE, "Download", "Завантажити") \
    X(S_TILE_PHONE_SUB, "over Wi-Fi", "по Wi-Fi") \
    X(S_SOON, "coming soon", "незабаром") \
    X(S_TILE_SETTINGS, "Settings", "Налаштування") \
    X(S_TILE_HELP, "Help", "Допомога") \
    X(S_LANG_CONTINUE, "Continue", "Далі") \
    X(S_GUIDE_OF, "How to use · %d of %d", "Як користуватися · %d з %d") \
    X(S_GUIDE_NEXT, "Next", "Далі") \
    X(S_GUIDE_DONE, "Got it", "Зрозуміло") \
    X(S_GUIDE_SKIP, "Skip", "Пропустити") \
    X(S_GUIDE_OPEN, "How to use", "Як користуватися") \
    X(S_G1_TITLE, "Record anything", "Записуйте будь-що") \
    X(S_G1_BODY, "In a game, in HOME, anywhere: press %s. The whole console is recorded, both screens and sound. The LED breathes red while it records.", "У грі, в HOME, будь-де: натисніть %s. Записується вся консоль — обидва екрани і звук. Поки йде запис, LED «дихає» червоним.") \
    X(S_G2_TITLE, "Stop", "Зупинка") \
    X(S_G2_BODY, "Press %s again. The recording is saved to the SD card and the LED blinks green twice.", "Натисніть %s ще раз. Запис збережеться на SD-карту, а LED двічі мигне зеленим.") \
    X(S_G3_TITLE, "Watch and download", "Перегляд і завантаження") \
    X(S_G3_BODY, "Open the Gallery: A plays a recording, Y downloads it to a phone over Wi-Fi through a QR code. Or take it straight from the SD card in a computer: /3ds/recorder/recordings (an MP4 lies next to each recording). While this app is open, the console converts recordings into small MP4 videos in the background.", "Відкрийте Галерею: A — переглянути запис, Y — завантажити на телефон по Wi-Fi через QR-код. Або заберіть напряму з SD-карти на комп'ютері: /3ds/recorder/recordings (поруч із кожним записом лежить MP4). Поки цей додаток відкритий, консоль у фоні конвертує записи в невеликі відео MP4.") \
    X(S_G4_TITLE, "After turning the console on", "Після ввімкнення консолі") \
    X(S_G4_BODY, "Open this app once: the recorder starts by itself and keeps running after you leave the app. When it is ready, the LED glows blue for a moment. Turning the console off, leaving System Settings or starting a DS or GBA game restarts the console: a recording is saved and stops, open this app again then.", "Відкрийте цей додаток один раз — рекордер запуститься сам і працюватиме далі, коли ви вийдете з додатка. Коли він готовий, LED на мить засвітиться синім. Вимкнення, вихід з Налаштувань консолі чи запуск DS- або GBA-гри перезапускають консоль: запис збережеться і зупиниться — тоді знову відкрийте додаток.") \
    X(S_TILE_HELP_SUB, "Recording, LED, download", "Запис, LED, завантаження") \
    X(S_TILE_SETTINGS_SUB, "fps, screens, hotkey", "частота, екрани, хоткей") \
    X(S_START_STOP, "Start / stop:", "Старт / стоп:") \
    X(S_HELP, "Help", "Допомога") \
    X(S_MSG_STARTED, "Recorder started: the LED glows blue.", "Рекордер запущено: LED світиться синім.") \
    X(S_MSG_STOPPED, "Recorder stopped.", "Рекордер зупинено.") \
    X(S_MSG_STOP_FAILED, "The recorder did not stop.", "Рекордер не зупинився.") \
    X(S_MSG_START_FAILED, "Could not start the recorder (%s, %08lX).", "Не вдалося запустити рекордер (%s, %08lX).") \
    X(S_DELETING, "Deleting…", "Видаляю…") \
    X(S_MSG_SYSTEM_STOP, "Recording of %s stopped because the console restarted or turned off (System Settings restart it on exit). It is saved.", "Запис %s зупинився, бо консоль перезапустилась чи вимкнулась (Налаштування консолі перезапускають її при виході). Його збережено.") \
    X(S_MSG_GAME_STOP, "Recording of %s is saved. On Old 3DS a game that needs extra memory restarts the console when it closes, and the recording stops there.", "Запис %s збережено. На Old 3DS гра, якій треба більше пам'яті, перезапускає консоль при виході, і запис на цьому зупиняється.") \
    X(S_MSG_RECOVERED, "Recording of %s was cut off (power or a crash): saved what was on the card.", "Запис %s обірвався (вимкнення чи збій): збережено все, що встигло записатися.") \
    X(S_MSG_SOON, "This comes in a later update.", "Це буде в одному з наступних оновлень.") \
    X(S_FR_EYEBROW, "Checks before recording", "Перевірка перед записом") \
    X(S_FR_ALMOST, "Almost ready", "Майже готово") \
    X(S_FR_LUMA, "Luma3DS", "Luma3DS") \
    X(S_FR_LUMA_OK, "v%lu.%lu · supported", "v%lu.%lu · підтримується") \
    X(S_FR_LUMA_UNKNOWN, "not detected", "не знайдено") \
    X(S_FR_OPTION, "Luma: external modules", "Luma: зовнішні модулі") \
    X(S_FR_ON, "on", "увімкнено") \
    X(S_FR_OFF, "off", "вимкнено") \
    X(S_FR_MODULE, "Recorder module", "Модуль рекордера") \
    X(S_FR_MODULE_OK, "installed", "встановлено") \
    X(S_FR_MODULE_UPDATE, "will be updated", "буде оновлено") \
    X(S_FR_MODULE_MISSING, "will be installed", "буде встановлено") \
    X(S_FR_RUNNING, "Recorder running", "Рекордер запущено") \
    X(S_FR_RUNNING_NO, "no — after power-on", "ні — після ввімкнення") \
    X(S_FR_SD, "Recordings on SD", "Запис на SD") \
    X(S_FR_SD_FREE, "%s free", "вільно %s") \
    X(S_FR_START_TITLE, "Start the recorder", "Запустіть рекордер") \
    X(S_FR_START_BODY, "Nothing starts on its own: you decide when recording is possible. After each power-on open this app and press Start. Then you can leave: the recorder keeps running until the console is turned off.", "Автозапуску немає — ви вирішуєте, чи можна записувати. Після кожного ввімкнення консолі відкрийте цей додаток і натисніть «Запустити». Потім можна виходити: рекордер працює до вимкнення консолі.") \
    X(S_FR_LED, "The LED glows blue for 1.5 s: ready", "LED засвітиться синім на 1.5 с — готово") \
    X(S_FR_LUMA_TITLE, "Turn on a Luma option", "Увімкніть опцію Luma") \
    X(S_FR_LUMA_BODY, "Hold SELECT while turning the console on. In the Luma menu enable \"Enable loading external FIRMs and modules\", save with START, then open this app again.", "Тримайте SELECT під час увімкнення консолі. У меню Luma увімкніть «Enable loading external FIRMs and modules», збережіть START і знову відкрийте цей додаток.") \
    X(S_LATER, "Later", "Пізніше") \
    X(S_START, "Start", "Запустити") \
    X(S_HELP_TITLE, "Help", "Допомога") \
    X(S_HELP_OF, "Help · %d of %d", "Допомога · %d з %d") \
    X(S_H1, "How to record", "Як записувати") \
    X(S_H1_BODY, "1. Open this app: the recorder starts by itself, the LED glows blue for a moment.\n2. Go to HOME and start a game. You can close the app: the recorder keeps running.\n3. Press %s: recording starts, the LED breathes red.\n4. The whole console is recorded: game, HOME, other apps, with sound, into one file.\n5. Press %s again: the recording is saved, the LED blinks green twice.", "1. Відкрийте цей додаток — рекордер запуститься сам, LED на мить засвітиться синім.\n2. Вийдіть у HOME і запустіть гру. Додаток можна закрити — рекордер працює далі.\n3. Натисніть %s — почнеться запис, LED «дихає» червоним.\n4. Пишеться вся консоль: гра, HOME, інші програми — в один файл, зі звуком.\n5. Ще раз %s — запис збережено, LED мигне зеленим двічі.") \
    X(S_H2, "What the LED means", "Що означає LED") \
    X(S_H2_BODY, "Blue for 1.5 s: the recorder started and is ready.\nRed, breathing: recording.\nGreen twice: the recording is saved.\nRed three times: an error, open this app.\nNo light and the hotkey does nothing: the recorder is off, or the console was just turned on (see 3).", "Синій 1.5 с — рекордер запущено, готовий.\nЧервоний «дихає» — іде запис.\nЗелений двічі — запис збережено.\nЧервоний тричі — помилка, відкрийте додаток.\nLED не горить і хоткей не реагує — рекордер вимкнено або консоль щойно ввімкнули (розділ 3).") \
    X(S_H3, "After turning the console on", "Після ввімкнення консолі") \
    X(S_H3_BODY, "The recorder starts when you open this app. After each power-on open the app once: that is all. Leaving the app does not stop it; to stop it until the app is opened again, switch Recorder off on the panel. Leaving System Settings and starting a DS or GBA game restart the console like turning it off: a recording is saved and stops there, and the recorder starts again when you open this app. Closing the lid saves a recording; it goes on in a new file when the lid opens.", "Рекордер запускається, коли ви відкриваєте цей додаток. Після кожного ввімкнення консолі відкрийте додаток один раз — і все. Вихід з додатка його не зупиняє; щоб вимкнути його до наступного відкриття додатка, вимкніть «Рекордер» на пульті. Вихід з Налаштувань консолі й запуск DS- чи GBA-гри перезапускають консоль, як вимкнення: запис там зберігається і зупиняється, а рекордер знову запуститься, коли ви відкриєте додаток. Якщо закрити кришку під час запису, він збережеться, а після відкриття продовжиться новим файлом.") \
    X(S_H4, "Getting a recording off", "Як забрати запис") \
    X(S_H4_BODY, "Watch recordings in the Gallery (A) and download them to a phone over Wi-Fi (Y). While this app is open, the console converts each recording into a small MP4 in the background; it lies next to the recording in /3ds/recorder/recordings, so the SD card in a computer has it too.", "Записи можна дивитися в Галереї (A) і завантажувати на телефон по Wi-Fi (Y). Поки цей додаток відкритий, консоль у фоні конвертує кожен запис у невелике MP4; воно лежить поруч із записом у /3ds/recorder/recordings, тож є й на SD-карті в комп'ютері.") \
    X(S_H5, "Hotkey and limits", "Хоткей і обмеження") \
    X(S_H5_BODY, "The hotkey is %s. Change it in Settings → Hotkey. Not allowed: L + R together (opens the Camera in HOME), L + ↓ + SELECT (the Rosalina menu), START and HOME. A long recording goes on in a new file every 4 GB (a 1-2 s gap); Settings shows how many minutes one file holds.", "Хоткей — %s. Змінити: Налаштування → Хоткей. Не можна: L + R разом (у HOME відкриває Камеру), L + ↓ + SELECT (меню Rosalina), START і HOME. Довгий запис продовжується в новому файлі кожні 4 GB (пауза 1–2 с); скільки хвилин уміщає файл, видно в налаштуваннях.") \
    X(S_H6, "If something is wrong", "Якщо щось не так") \
    X(S_H6_BODY, "Red three times: the recording did not start or stopped, usually because the SD card is full. Skipped frames: the SD card could not keep up, the video is still fine. Details are in /3ds/recorder/recorder.log.", "Червоний тричі — запис не почався або зупинився, найчастіше через заповнену SD-карту. Пропущені кадри — SD не встигала, відео при цьому ціле. Подробиці — в /3ds/recorder/recorder.log.") \
    X(S_H7, "Games that need extra memory", "Ігри, яким треба більше пам'яті") \
    X(S_H7_BODY, "On Old 3DS some games need more memory: Smash Bros, Pokemon Sun / Moon and Ultra Sun / Ultra Moon, Super Mario Maker, Monster Hunter. The console restarts when they start and when they close. If the recorder runs, it comes back by itself inside the game through a tiny plugin of Luma (if the plugin loader is off, it is on just for that game), and a recording goes on in a new file. The game has 2 MB less memory, and the video is less smooth: the console has less power and memory to spare there. For smoother video there, record only the top screen: Settings → Screens → Top. When such a game closes, the recording is saved; open this app to record again. New 3DS does not restart for these games.", "На Old 3DS деяким іграм треба більше пам'яті: Smash Bros, Pokemon Sun / Moon і Ultra Sun / Ultra Moon, Super Mario Maker, Monster Hunter. Консоль перезапускається, коли вони стартують і коли закриваються. Якщо рекордер працює, він сам повертається в гру через маленький плагін Luma (якщо завантажувач плагінів вимкнено, він вмикається лише на цю гру), а запис продовжується новим файлом. Грі лишається на 2 МБ менше пам'яті, а відео менш плавне: консолі там бракує потужності й пам'яті. Щоб відео було плавнішим, пишіть лише верхній екран: Налаштування → Екрани → Верхній. Коли така гра закривається, запис зберігається; щоб писати далі, відкрийте цей додаток. New 3DS для цих ігор не перезапускається.") \
    X(S_SETTINGS, "Settings", "Налаштування") \
    X(S_LANGUAGE, "Language · Мова", "Мова · Language") \
    X(S_UNINSTALL_ROW, "Uninstall recorder…", "Видалити рекордер…") \
    X(S_SOUND, "Sound", "Звук") \
    X(S_LED, "LED", "LED") \
    X(S_LED_PULSE, "Pulse", "Пульс") \
    X(S_LED_SOLID, "Solid", "Постійно") \
    X(S_LED_OFF, "Off", "Вимк.") \
    X(S_HOTKEY, "Hotkey", "Хоткей") \
    X(S_FILE_TITLE, "What goes into the file", "Що вийде у файлі") \
    X(S_FRAME_IN_FILE, "Frame in the file", "Кадр у файлі") \
    X(S_TOP_BOX, "top 400×240", "верхній 400×240") \
    X(S_BOTTOM_BOX, "bottom 320×240", "нижній 320×240") \
    X(S_CODEC, "Codec", "Кодек") \
    X(S_CODEC_VAL, "XR32 · lossless", "XR32 · без втрат") \
    X(S_STREAM, "To the SD card", "Потік на SD") \
    X(S_PER_MIN, "One minute", "Хвилина запису") \
    X(S_FITS, "Fits on the SD card", "Влізе на SD") \
    X(S_SETTINGS_NOTE, "Frame rate, screens, sound and LED apply from the next recording, the hotkey right away. One file holds up to 4 GB, about %u min.", "Частота, екрани, звук і LED діють з наступного запису, хоткей — одразу. Один файл — до 4 GB, це ≈ %u хв.") \
    X(S_HK_SCREEN, "Recording hotkey", "Хоткей запису") \
    X(S_HK_HOLD_TITLE, "Hold the new combination", "Затисніть нову комбінацію") \
    X(S_HK_HOLD_HINT, "Hold for 1 s to set it · 2–3 buttons", "Тримайте 1 с, щоб зафіксувати · 2–3 кнопки") \
    X(S_HK_TOO_MANY, "At most 3 buttons", "Не більше 3 кнопок") \
    X(S_HK_NOW, "Now", "Зараз") \
    X(S_HK_REVIEW_TITLE, "New combination", "Нова комбінація") \
    X(S_HK_ERR_LR, "L + R together is not allowed: in HOME it opens the Camera.", "L + R разом не можна: у HOME ця пара відкриває Камеру.") \
    X(S_HK_ERR_ROSALINA, "This is part of L + ↓ + SELECT, the Rosalina menu.", "Це частина L + ↓ + SELECT — меню Rosalina.") \
    X(S_HK_ERR_FACE, "Only A / B / X / Y is too easy to press in a game. Add SELECT, L or R.", "Лише A / B / X / Y легко натиснути в грі. Додайте SELECT, L або R.") \
    X(S_HK_OK, "This combination can be used.", "Цю комбінацію можна використати.") \
    X(S_HK_RULES, "Not allowed: L + R together (Camera in HOME), L + ↓ + SELECT (Rosalina), START.", "Не можна: L + R разом (Камера в HOME), L + ↓ + SELECT (Rosalina), START.") \
    X(S_HK_KEYS_CAPTURE, "START: cancel", "START: скасувати") \
    X(S_HK_KEYS_RETRY, "B: again · START: cancel", "B: ще раз · START: скасувати") \
    X(S_HK_KEYS_REVIEW, "A: save · B: again · START: cancel", "A: зберегти · B: ще раз · START: скасувати") \
    X(S_RESET, "Reset", "Скинути") \
    X(S_SAVE, "Save", "Зберегти") \
    X(S_HK_SAVED, "Hotkey saved: %s. It works right away.", "Хоткей збережено: %s. Він уже діє.") \
    X(S_UN_EYEBROW, "Settings · Uninstall recorder", "Налаштування · Видалити рекордер") \
    X(S_UN_TITLE, "Full removal", "Повне видалення") \
    X(S_UN_MODULE, "Recorder module", "Модуль рекордера") \
    X(S_UN_MODULE_SUB, "stops and leaves Luma", "зупиниться й зникне з Luma") \
    X(S_UN_SETTINGS, "Settings and log", "Налаштування і лог") \
    X(S_UN_APP, "This app", "Цей додаток") \
    X(S_UN_APP_CIA, "icon in HOME", "іконка в HOME") \
    X(S_UN_RECORDINGS, "Recordings", "Записи") \
    X(S_UN_RECORDINGS_KEEP, "kept unless ticked", "лишаються, якщо не обрано") \
    X(S_UN_ASK, "Uninstall the recorder?", "Видалити рекордер?") \
    X(S_UN_ASK_BODY, "The console goes back to how it was without the recorder.", "Консоль повернеться до стану без рекордера.") \
    X(S_UN_ALSO, "Also delete %lu recordings (%s)", "Також видалити записи: %lu (%s)") \
    X(S_CANCEL, "Cancel", "Скасувати") \
    X(S_PH_EYEBROW, "Phone and console on one Wi-Fi", "Телефон і консоль в одній Wi-Fi") \
    X(S_PH_TITLE, "Point the phone's camera at the code", "Наведіть камеру телефону на код") \
    X(S_PH_BODY, "The console converts the recording into a small MP4 video, the phone downloads and saves it: nothing to install.", "Консоль конвертує запис у невелике відео MP4, а телефон його завантажує й зберігає — програми ставити не треба.") \
    X(S_PH_CONNECTED, "Phone connected", "Телефон підключено") \
    X(S_PH_WAITING, "Waiting for the phone", "Чекаю на телефон") \
    X(S_PH_IDLE, "Scan the code: the phone starts the download by itself.", "Відскануйте код — телефон сам почне завантаження.") \
    X(S_PH_SLOW, "Keep the lid open until the phone has the video.", "Не закривайте кришку, поки телефон не отримає відео.") \
    X(S_PH_SD, "Without Wi-Fi: turn the console off, take out the SD card and put it in a computer. Recordings are in /3ds/recorder/recordings.", "Можна й без Wi-Fi: вимкніть консоль, вийміть SD-карту й підключіть до комп'ютера — записи в /3ds/recorder/recordings.") \
    X(S_PH_STOP, "Stop", "Зупинити") \
    X(S_PH_NO_WIFI, "Wi-Fi is off", "Wi-Fi вимкнено") \
    X(S_PH_NO_WIFI_BODY, "Turn Wi-Fi on (the switch on the console, or HOME → settings) and connect to the network the phone is on. Then press A.", "Увімкніть Wi-Fi (перемикач на консолі або HOME → налаштування) і підключіться до тієї ж мережі, що й телефон. Потім натисніть A.") \
    X(S_PH_FAILED, "Could not start sending", "Не вдалося почати передачу") \
    X(S_PH_FAILED_BODY, "The network did not start. Check Wi-Fi and press A to try again.", "Мережа не запустилась. Перевірте Wi-Fi і натисніть A, щоб спробувати ще раз.") \
    X(S_PH_RETRY, "Try again", "Ще раз") \
    X(S_PH_LEFT, "≈ %u min", "≈ %u хв") \
    X(S_PH_SENDING, "Sending", "Передача") \
    X(S_PH_PICK, "Pick a recording and press Y to download it to the phone.", "Виберіть запис і натисніть Y, щоб завантажити його на телефон.") \
    X(S_PH_HINT, "Download", "Завантажити") \
    X(S_PH_READY, "Ready", "Готово") \
    X(S_PH_PREPARING, "Converting", "Конвертую") \
    X(S_PH_PREP_BODY, "The console converts the recording into a small video; the phone waits and then downloads it by itself.", "Консоль конвертує запис у невелике відео; телефон зачекає й сам його завантажить.") \
    X(S_PH_PREP_FAILED, "Could not convert the recording: the phone gets the original.", "Не вдалося сконвертувати запис: телефон отримає оригінал.") \
    X(S_GAL_PREP_READY, "Ready for download", "Готово до завантаження") \
    X(S_GAL_PREP_PCT, "Converting %d%%", "Конвертую %d%%") \
    X(S_GAL_PREP_FAILED, "Not converted", "Не сконвертовано") \
    X(S_PLAY_FAILED, "Could not open the recording.", "Не вдалося відкрити запис.") \
    X(S_PL_SEEK, "±10 s", "±10 с") \
    X(S_PL_PAUSE, "Pause", "Пауза") \
    X(S_PL_BACK, "Gallery", "У галерею") \
    X(S_PL_SOUND, "sound", "звук") \
    X(S_PL_NO_DSP, "No sound: the DSP firmware is missing (run DSP1 once).", "Без звуку: немає прошивки DSP (запустіть DSP1 один раз).") \
    X(S_PL_TOP_ONLY, "Only the top screen was recorded", "Записано лише верхній екран") \
    X(S_GAL_NTH, "Recording %d of %d", "Запис %d з %d") \
    X(S_DURATION, "Length", "Тривалість") \
    X(S_SIZE, "Size", "Розмір") \
    X(S_DROPPED, "Frames skipped", "Пропущено кадрів") \
    X(S_WITH_SOUND, "with sound", "зі звуком") \
    X(S_NO_SOUND, "no sound", "без звуку") \
    X(S_BOTH_SCREENS, "both screens", "обидва екрани") \
    X(S_TOP_SCREEN, "top screen", "верхній екран") \
    X(S_NOT_CLOSED, "Cut off (power or a crash): what reached the card is saved.", "Обірвався (вимкнення чи збій): збережено, що встигло записатися.") \
    X(S_GAL_CUT, "Cut off", "Обірвався") \
    X(S_UNREADABLE, "This file can't be read.", "Цей файл не читається.") \
    X(S_PLAY, "Play", "Відтворити") \
    X(S_DELETE, "Delete", "Видалити") \
    X(S_OPEN, "Open", "Відкрити") \
    X(S_GAL_LOADING, "Reading the SD card…", "Читаю SD-картку…") \
    X(S_DEL_TITLE, "Delete this recording?", "Видалити цей запис?") \
    X(S_DEL_BODY, "%s · %s. The file is removed from the SD card.", "%s · %s. Файл буде видалено з SD-картки.") \
    X(S_DEL_BTN, "Delete", "Видалити") \
    X(S_DELETED, "Recording deleted.", "Запис видалено.") \
    X(S_DEL_FAILED, "Could not delete the recording.", "Не вдалося видалити запис.") \
    X(S_UNINSTALL, "Uninstall", "Видалити") \
    X(S_UN_WORKING, "Uninstalling…", "Видаляю…") \
    X(S_UN_DONE, "Uninstalled", "Видалено") \
    X(S_UN_DONE_BODY, "3DS Recorder is removed. Luma settings were not changed: if you turned on external modules only for it, you can turn the option off.", "3DS Recorder видалено. Налаштування Luma не змінювались: якщо ви вмикали зовнішні модулі лише для нього, опцію можна вимкнути.") \
    X(S_UN_PARTIAL, "Some files could not be removed", "Деякі файли не вдалося видалити") \
    X(S_UN_PARTIAL_BODY, "Delete what is left by hand: /luma/sysmodules/0004013000F1D302.cxi and this app (Data Management for the HOME icon).", "Видаліть залишки вручну: /luma/sysmodules/0004013000F1D302.cxi і цей додаток (іконку — в «Керуванні даними»).") \
    X(S_EXIT, "Exit", "Вийти") \
    X(S_BACK, "Back", "Назад") \
    X(S_OK, "OK", "OK")

typedef enum {
#define X(id, en, uk) id,
    STRINGS(X)
#undef X
    S_COUNT
} Str;

void i18n_set_ukrainian(bool on);
bool i18n_ukrainian(void);
const char *T(Str id);
/* "12 recordings" / "12 записів" with the Ukrainian plural forms. */
const char *i18n_recordings(unsigned n, char *buf, unsigned cap);

#endif
