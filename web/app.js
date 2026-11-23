/*
 * The phone page for one recording (?f=<name>). The console makes an MP4 of it (app/source/prepare.c):
 * the page shows how far that is, downloads the MP4 in pieces and offers it to save. Where the console
 * could not make one, the page downloads the recording itself, turns it into a video in the browser
 * and offers that:
 *   fast  WebCodecs H.264 + AAC (Opus where the browser has no AAC encoder: Firefox) -> MP4 (mp4-muxer):
 *         Safari and every browser on iOS, also on http
 *   live  MediaRecorder on a canvas + Web Audio, in real time (Chromium and Firefox give WebCodecs
 *         only to https pages; the console serves http): MP4 where the browser writes it, else WebM
 *   none  only the original .r3d
 * The download runs ahead of the conversion; both pause for each other, so memory stays small.
 *
 * Local mode (<html data-mode="local">, the GitHub Pages build of tools/build-web.py --local): no
 * console. A .r3d from the SD card is dropped on the page or picked, read from the disk instead of
 * downloaded and turned into a video the same way. The file never leaves the computer. Two options
 * there, for editing: the size (1x, or 2x / 3x with sharp pixels: players smooth a small video
 * when they stretch it) and the sound as recorded (no treble cut). The live way stays 1x.
 */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const TEXT = {
    en: {
      mb: 'MB', gb: 'GB', length: 'Length', size: 'Size', eta: 'Transfer time', s1: 'Download from the console', s2: 'Make the video',
      s3: 'Save', waiting: 'waiting', fast: 'fast: the phone\'s video encoder',
      live: 'in real time: keep this page open and the screen on', none: 'this browser cannot make videos',
      start: 'Start', startHint: 'Tap Start: the browser lets a page use sound only after a tap.',
      keep: 'Keep this page open and the screen on until the video is ready. If the page is hidden, the video waits.',
      long: 'A long recording: about %1 over the 3DS Wi-Fi. For long ones the SD card is faster.',
      save: 'Save the video', share: 'Share / save to Photos', raw: 'or the original .r3d, for a computer',
      sd: 'Recordings can also be taken from the SD card: turn the console off and put the card into a computer, they are in /3ds/recorder/recordings.',
      howIos: 'Tap “Save the video”, then open Files → Downloads → the video → Share → Save Video: it goes to Photos.',
      howAndroid: 'Tap “Save the video”: it goes to Downloads and shows up in the Gallery.',
      howOther: 'Tap “Save the video”: it goes to your downloads folder.',
      ready: 'ready', retry: 'Try again', lost: 'The connection to the console was lost. Is the console still on the “Download” screen, lid open?',
      notFound: 'This recording is not offered any more. Pick it on the console again (Gallery → Y).',
      noFile: 'No recording in the link. Scan the code on the console again.',
      failed: 'Could not make the video (%1). The original .r3d is below.', paused: 'paused: waiting for the download',
      hidden: 'paused: the page is hidden', min: 'min', sec: 's', left: '%1 left', mbs: 'MB/s',
      c1: 'Converting on the console', c1x: 'converting into a small MP4: the 3DS Wi-Fi sends it quickly', c1q: 'waiting its turn',
      c2: 'Download to the phone', direct: 'Download directly', directLink: 'or download directly',
      big: 'A long video (%1): the phone\'s browser downloads it by itself, look in its downloads. Keep the console on the “Download” screen, lid open.',
      diag: 'Log for the developer (.txt)',
      pick: 'Drop a .r3d here or click to choose', pickHint: 'From the SD card: /3ds/recorder/recordings',
      privacy: 'The file stays on your computer: the browser makes the video, nothing is uploaded.',
      read: 'Read the file', notR3d: 'This is not a 3DS Recorder recording (.r3d).',
      rpl: 'This is a Replay buffer, not a recording. Save the clip on the console (Replay hotkey) and convert the .r3d it makes.',
      optScale: 'Scale', optAudio: 'Sound', eqOn: 'Like the console', eqOff: 'As recorded',
      scaleHint: '1× is the 3DS\'s own size (400×480). 2× and 3× keep the pixels sharp in editors and on big screens; the file is bigger.',
      optLayout: 'Screens', layAuto: 'As recorded', layTop: 'Top', layBottom: 'Bottom', laySide: 'Side by side', optQuality: 'For', qNormal: 'Sharing', qHigh: 'YouTube', qEdit: 'Editing', qualityHint: 'YouTube: a higher bitrate (3× makes 1440p, which YouTube streams in better quality). Editing: every frame a keyframe at a high bitrate, for DaVinci Resolve and other editors; the file is much bigger.', pcTool: 'Need DNxHR for DaVinci Resolve, a lossless file or one file per screen?', pcToolLink: 'r3d2video for Windows',
      fastPc: 'fast: the computer\'s video encoder', readErr: 'Could not read the file (%1).', failedLocal: 'Could not make the video (%1).', again: 'Convert another file',
      webm: 'This browser can only make WebM videos. For an MP4, open this page in Chrome, Edge or Safari.',
      optInputs: "Buttons", inOff: "Hide", inOn: "Show", inputsHint: "The buttons pressed, beside the bottom screen or in a corner (recordings from 1.6.0 on).", qDiscord: "Discord", discordHint: "Discord: under 10 MB, the size Discord takes without Nitro.", discordLong: "Too long for 10 MB: the picture will be blurry. Below the video you can cut a shorter part.", noInputs: "This recording has no button presses: it was made before 1.6.0 or with Record Buttons off.", trimTitle: "Cut a part", trimStart: "Start here", trimEnd: "End here", trimGo: "Make this part", trimHint: "Play the video and pause where the part should start: “Start here”. Then where it should end: “End here”.", trimLabel: "part %1–%2",
    },
    uk: {
      mb: 'МБ', gb: 'ГБ', length: 'Тривалість', size: 'Розмір', eta: 'Час передачі', s1: 'Завантаження з консолі', s2: 'Робимо відео',
      s3: 'Зберегти', waiting: 'чекає', fast: 'швидко: відеокодер телефона',
      live: 'у реальному часі: тримайте сторінку відкритою, а екран увімкненим', none: 'цей браузер не вміє робити відео',
      start: 'Почати', startHint: 'Натисніть «Почати»: браузер дозволяє сторінці працювати зі звуком лише після дотику.',
      keep: 'Тримайте сторінку відкритою, а екран увімкненим, доки відео не буде готове. Якщо сторінку сховати, відео чекає.',
      long: 'Довгий запис: ≈ %1 через Wi-Fi 3DS. Довгі записи швидше забрати з SD-картки.',
      save: 'Зберегти відео', share: 'Поділитися / зберегти у Фото', raw: 'або оригінал .r3d — для комп\'ютера',
      sd: 'Записи можна забрати й через SD-картку: вимкніть консоль і вставте картку в комп\'ютер — вони в /3ds/recorder/recordings.',
      howIos: 'Натисніть «Зберегти відео», потім Файли → Завантаження → відео → Поділитися → Зберегти відео: воно потрапить у Фото.',
      howAndroid: 'Натисніть «Зберегти відео»: воно потрапить у «Завантаження» і з\'явиться в Галереї.',
      howOther: 'Натисніть «Зберегти відео»: воно потрапить у теку завантажень.',
      ready: 'готово', retry: 'Спробувати ще раз', lost: 'Зв\'язок з консоллю втрачено. Консоль досі на екрані «Завантажити», кришка відкрита?',
      notFound: 'Цей запис уже не пропонується. Виберіть його на консолі ще раз (Галерея → Y).',
      noFile: 'У посиланні немає запису. Відскануйте код на консолі ще раз.',
      failed: 'Не вдалося зробити відео (%1). Нижче — оригінал .r3d.', paused: 'пауза: чекаємо завантаження',
      hidden: 'пауза: сторінку сховано', min: 'хв', sec: 'с', left: 'ще %1', mbs: 'МБ/с',
      c1: 'Конвертація на консолі', c1x: 'конвертую в невелике MP4: Wi-Fi 3DS передасть його швидко', c1q: 'чекає черги',
      c2: 'Завантаження на телефон', direct: 'Завантажити напряму', directLink: 'або завантажити напряму',
      big: 'Довге відео (%1): браузер телефона сам його завантажить — дивіться в його завантаженнях. Тримайте консоль на екрані «Завантажити» з відкритою кришкою.',
      diag: 'Журнал для розробника (.txt)',
      pick: 'Перетягніть сюди .r3d або натисніть, щоб вибрати', pickHint: 'З SD-картки: /3ds/recorder/recordings',
      privacy: 'Файл лишається на вашому комп\'ютері: відео робить браузер, нічого не вивантажується.',
      read: 'Читаємо файл', notR3d: 'Це не запис 3DS Recorder (.r3d).',
      rpl: 'Це буфер Повтору, а не запис. Збережіть кліп на консолі (комбінація Повтору) і сконвертуйте .r3d, який вона створить.',
      optScale: 'Масштаб', optAudio: 'Звук', eqOn: 'Як на консолі', eqOff: 'Як записано',
      scaleHint: '1× — рідний розмір 3DS (400×480). 2× і 3× зберігають чіткі пікселі в редакторах і на великих екранах; файл більший.',
      optLayout: 'Екрани', layAuto: 'Як записано', layTop: 'Верхній', layBottom: 'Нижній', laySide: 'Поруч', optQuality: 'Для чого', qNormal: 'Поділитися', qHigh: 'YouTube', qEdit: 'Монтаж', qualityHint: 'YouTube: вищий бітрейт (3× дає 1440p, яке YouTube показує в кращій якості). Монтаж: кожен кадр ключовий і високий бітрейт, для DaVinci Resolve та інших редакторів; файл значно більший.', pcTool: 'Потрібен DNxHR для DaVinci Resolve, файл без втрат чи окремий файл для кожного екрана?', pcToolLink: 'r3d2video для Windows',
      fastPc: 'швидко: відеокодер комп\'ютера', readErr: 'Не вдалося прочитати файл (%1).', failedLocal: 'Не вдалося зробити відео (%1).', again: 'Сконвертувати інший файл',
      webm: 'Цей браузер уміє робити лише відео WebM. Для MP4 відкрийте цю сторінку в Chrome, Edge чи Safari.',
      optInputs: "Кнопки", inOff: "Сховати", inOn: "Показати", inputsHint: "Натиснуті кнопки збоку від нижнього екрана або в куті (записи з 1.6.0).", qDiscord: "Discord", discordHint: "Discord: до 10 МБ, скільки Discord приймає без Nitro.", discordLong: "Задовге для 10 МБ: картинка буде розмита. Під відео можна вирізати коротший фрагмент.", noInputs: "У цьому записі немає натискань: його зроблено до 1.6.0 або з вимкненим «Записувати кнопки».", trimTitle: "Вирізати фрагмент", trimStart: "Початок тут", trimEnd: "Кінець тут", trimGo: "Зробити фрагмент", trimHint: "Увімкніть відео й поставте на паузу там, де має початися фрагмент: «Початок тут». Потім там, де має закінчитися: «Кінець тут».", trimLabel: "фрагмент %1–%2",
    },
    es: {
      mb: 'MB', gb: 'GB', length: 'Duración', size: 'Tamaño', eta: 'Tiempo de transferencia', s1: 'Descarga desde la consola', s2: 'Creación del video',
      s3: 'Guardar', waiting: 'en espera', fast: 'rápido: codificador de video del teléfono',
      live: 'en tiempo real: mantén esta página abierta y la pantalla encendida', none: 'este navegador no puede crear videos',
      start: 'Empezar', startHint: 'Toca “Empezar”: el navegador solo deja que una página use el sonido después de un toque.',
      keep: 'Mantén esta página abierta y la pantalla encendida hasta que el video esté listo. Si la página queda oculta, el video espera.',
      long: 'Grabación larga: tardará unos %1 por la conexión Wi-Fi de la 3DS. Para las largas, la tarjeta SD es más rápida.',
      save: 'Guardar el video', share: 'Compartir / guardar en Fotos', raw: 'o el .r3d original, para PC',
      sd: 'Las grabaciones también se pueden sacar de la tarjeta SD: apaga la consola y pon la tarjeta en tu PC; están en /3ds/recorder/recordings.',
      howIos: 'Toca “Guardar el video” y luego abre Archivos → Descargas → el video → Compartir → Guardar video: irá a Fotos.',
      howAndroid: 'Toca “Guardar el video”: irá a Descargas y aparecerá en la Galería.',
      howOther: 'Toca “Guardar el video”: irá a tu carpeta de descargas.',
      ready: 'listo', retry: 'Reintentar', lost: 'Se perdió la conexión con la consola. ¿Sigue la consola abierta y en la pantalla “Descargar”?',
      notFound: 'Esta grabación ya no está disponible. Vuelve a elegirla en la consola (Galería → Y).',
      noFile: 'El enlace no incluye ninguna grabación. Vuelve a escanear el código en la consola.',
      failed: 'No se pudo crear el video (%1). Abajo tienes el .r3d original.', paused: 'en pausa: esperando la descarga',
      hidden: 'en pausa: la página está oculta', min: 'min', sec: 's', left: 'faltan %1', mbs: 'MB/s',
      c1: 'Conversión en la consola', c1x: 'convirtiendo en un MP4 pequeño, que la conexión Wi-Fi de la 3DS envía rápido', c1q: 'esperando su turno',
      c2: 'Descarga al teléfono', direct: 'Descargar directamente', directLink: 'o descargar directamente',
      big: 'Video largo (%1): el navegador del teléfono lo descarga solo; búscalo en sus descargas. Mantén la consola abierta y en la pantalla “Descargar”.',
      diag: 'Registro para el desarrollador (.txt)',
      pick: 'Suelta aquí un .r3d o haz clic para elegirlo', pickHint: 'De la tarjeta SD: /3ds/recorder/recordings',
      privacy: 'El archivo no sale de tu PC: el navegador crea el video y no se sube nada.',
      read: 'Lectura del archivo', notR3d: 'Esto no es una grabación de 3DS Recorder (.r3d).',
      rpl: 'Esto es un búfer de la repetición, no una grabación. Guarda el clip en la consola (combinación de la repetición) y convierte el .r3d que se crea.',
      optScale: 'Escala', optAudio: 'Sonido', eqOn: 'Como en la consola', eqOff: 'Tal como se grabó',
      scaleHint: '1× es el tamaño original de la 3DS (400×480). 2× y 3× mantienen los píxeles nítidos en editores y pantallas grandes; el archivo ocupa más.',
      optLayout: 'Pantallas', layAuto: 'Como se grabó', layTop: 'Superior', layBottom: 'Inferior', laySide: 'Lado a lado', optQuality: 'Para', qNormal: 'Compartir', qHigh: 'YouTube', qEdit: 'Edición', qualityHint: 'YouTube: más bitrate (3× da 1440p, que YouTube reproduce con mejor calidad). Edición: cada fotograma es clave y con bitrate alto, para DaVinci Resolve y otros editores; el archivo ocupa mucho más.', pcTool: '¿Necesitas DNxHR para DaVinci Resolve, un archivo sin pérdida o un archivo por pantalla?', pcToolLink: 'r3d2video para Windows',
      fastPc: 'rápido: codificador de video de tu PC', readErr: 'No se pudo leer el archivo (%1).', failedLocal: 'No se pudo crear el video (%1).', again: 'Convertir otro archivo',
      webm: 'Este navegador solo puede crear videos WebM. Para obtener un MP4, abre esta página en Chrome, Edge o Safari.',
      optInputs: "Botones", inOff: "Ocultar", inOn: "Mostrar", inputsHint: "Los botones pulsados, junto a la pantalla inferior o en una esquina (grabaciones desde la 1.6.0).", qDiscord: "Discord", discordHint: "Discord: menos de 10 MB, lo que Discord acepta sin Nitro.", discordLong: "Demasiado largo para 10 MB: la imagen se verá borrosa. Debajo del video puedes recortar una parte más corta.", noInputs: "Esta grabación no tiene pulsaciones: se hizo antes de la 1.6.0 o con Grabar botones desactivado.", trimTitle: "Recortar una parte", trimStart: "Empieza aquí", trimEnd: "Termina aquí", trimGo: "Crear esta parte", trimHint: "Reproduce el video y pausa donde debe empezar la parte: «Empieza aquí». Luego donde debe terminar: «Termina aquí».", trimLabel: "parte %1–%2",
    },
    pt: {
      mb: 'MB', gb: 'GB', length: 'Duração', size: 'Tamanho', eta: 'Tempo de transferência', s1: 'Download do console', s2: 'Criação do vídeo',
      s3: 'Salvar', waiting: 'aguardando', fast: 'rápido: codificador de vídeo do celular',
      live: 'em tempo real: mantenha esta página aberta e a tela ligada', none: 'este navegador não consegue criar vídeos',
      start: 'Começar', startHint: 'Toque em “Começar”: o navegador só deixa uma página usar o som depois de um toque.',
      keep: 'Mantenha esta página aberta e a tela ligada até o vídeo ficar pronto. Se a página ficar oculta, o vídeo espera.',
      long: 'Gravação longa: vai levar cerca de %1 pelo Wi-Fi do 3DS. Para as longas, o cartão SD é mais rápido.',
      save: 'Salvar o vídeo', share: 'Compartilhar / salvar em Fotos', raw: 'ou o original .r3d, para o computador',
      sd: 'As gravações também podem ser tiradas do cartão SD: desligue o console e coloque o cartão no computador; elas estão em /3ds/recorder/recordings.',
      howIos: 'Toque em “Salvar o vídeo” e depois abra Arquivos → Downloads → o vídeo → Compartilhar → Salvar Vídeo: ele vai para Fotos.',
      howAndroid: 'Toque em “Salvar o vídeo”: ele vai para Downloads e aparece na Galeria.',
      howOther: 'Toque em “Salvar o vídeo”: ele vai para a sua pasta de downloads.',
      ready: 'pronto', retry: 'Tentar de novo', lost: 'A conexão com o console caiu. O console ainda está aberto e na tela “Baixar”?',
      notFound: 'Esta gravação não está mais disponível. Escolha-a de novo no console (Galeria → Y).',
      noFile: 'Não há gravação no link. Escaneie o código no console de novo.',
      failed: 'Não foi possível criar o vídeo (%1). O original .r3d está abaixo.', paused: 'pausado: aguardando o download',
      hidden: 'pausado: a página está oculta', min: 'min', sec: 's', left: 'faltam %1', mbs: 'MB/s',
      c1: 'Conversão no console', c1x: 'convertendo em um MP4 pequeno: o Wi-Fi do 3DS o envia rápido', c1q: 'aguardando a vez',
      c2: 'Download para o celular', direct: 'Baixar diretamente', directLink: 'ou baixar diretamente',
      big: 'Um vídeo longo (%1): o navegador do celular o baixa sozinho; procure nos downloads dele. Mantenha o console aberto e na tela “Baixar”.',
      diag: 'Registro para o desenvolvedor (.txt)',
      pick: 'Solte um .r3d aqui ou clique para escolher', pickHint: 'Do cartão SD: /3ds/recorder/recordings',
      privacy: 'O arquivo fica no seu computador: o navegador cria o vídeo, nada é enviado.',
      read: 'Leitura do arquivo', notR3d: 'Isto não é uma gravação do 3DS Recorder (.r3d).',
      rpl: 'Isto é um buffer da repetição, não uma gravação. Salve o clipe no console (combinação da repetição) e converta o .r3d criado.',
      optScale: 'Escala', optAudio: 'Som', eqOn: 'Como no console', eqOff: 'Como gravado',
      scaleHint: '1× é o tamanho original do 3DS (400×480). 2× e 3× mantêm os pixels nítidos em editores e telas grandes; o arquivo fica maior.',
      optLayout: 'Telas', layAuto: 'Como gravado', layTop: 'Superior', layBottom: 'Inferior', laySide: 'Lado a lado', optQuality: 'Para', qNormal: 'Compartilhar', qHigh: 'YouTube', qEdit: 'Edição', qualityHint: 'YouTube: bitrate maior (3× gera 1440p, que o YouTube exibe com mais qualidade). Edição: todo quadro é chave e com bitrate alto, para o DaVinci Resolve e outros editores; o arquivo fica bem maior.', pcTool: 'Precisa de DNxHR para o DaVinci Resolve, de um arquivo sem perdas ou de um arquivo por tela?', pcToolLink: 'r3d2video para Windows',
      fastPc: 'rápido: codificador de vídeo do computador', readErr: 'Não foi possível ler o arquivo (%1).', failedLocal: 'Não foi possível criar o vídeo (%1).', again: 'Converter outro arquivo',
      webm: 'Este navegador só consegue criar vídeos WebM. Para um MP4, abra esta página no Chrome, Edge ou Safari.',
      optInputs: "Botões", inOff: "Ocultar", inOn: "Mostrar", inputsHint: "Os botões apertados, ao lado da tela de baixo ou num canto (gravações a partir da 1.6.0).", qDiscord: "Discord", discordHint: "Discord: menos de 10 MB, o que o Discord aceita sem Nitro.", discordLong: "Longo demais para 10 MB: a imagem vai ficar borrada. Abaixo do vídeo você pode cortar um trecho menor.", noInputs: "Esta gravação não tem botões: foi feita antes da 1.6.0 ou com Gravar botões desligado.", trimTitle: "Cortar um trecho", trimStart: "Começa aqui", trimEnd: "Termina aqui", trimGo: "Fazer este trecho", trimHint: "Reproduza o vídeo e pause onde o trecho deve começar: “Começa aqui”. Depois onde deve terminar: “Termina aqui”.", trimLabel: "trecho %1–%2",
    },
    it: {
      mb: 'MB', gb: 'GB', length: 'Durata', size: 'Dimensione', eta: 'Tempo di trasferimento', s1: 'Download dalla console', s2: 'Creazione del video',
      s3: 'Salvataggio', waiting: 'in attesa', fast: 'veloce: codificatore video del telefono',
      live: 'in tempo reale: tieni aperta questa pagina e lo schermo acceso', none: 'questo browser non può creare video',
      start: 'Inizia', startHint: 'Tocca “Inizia”: il browser permette a una pagina di usare l\'audio solo dopo un tocco.',
      keep: 'Tieni aperta questa pagina e lo schermo acceso finché il video non è pronto. Se la pagina è nascosta, il video resta in attesa.',
      long: 'Registrazione lunga: ci vorranno circa %1 con il Wi-Fi del 3DS. Per quelle lunghe la scheda SD è più veloce.',
      save: 'Salva il video', share: 'Condividi / salva in Foto', raw: 'oppure il file .r3d originale, per il computer',
      sd: 'Le registrazioni si possono prendere anche dalla scheda SD: spegni la console e inserisci la scheda in un computer; si trovano in /3ds/recorder/recordings.',
      howIos: 'Tocca “Salva il video”, poi apri File → Download → il video → Condividi → Salva video: finirà in Foto.',
      howAndroid: 'Tocca “Salva il video”: finirà in Download e comparirà nella galleria del telefono.',
      howOther: 'Tocca “Salva il video”: finirà nella tua cartella dei download.',
      ready: 'pronto', retry: 'Riprova', lost: 'La connessione con la console si è interrotta. La console è ancora aperta e sulla schermata “Scarica”?',
      notFound: 'Questa registrazione non è più disponibile. Selezionala di nuovo sulla console (Galleria → Y).',
      noFile: 'Il link non contiene nessuna registrazione. Scansiona di nuovo il codice sulla console.',
      failed: 'Impossibile creare il video (%1). Qui sotto trovi il file .r3d originale.', paused: 'in pausa: in attesa del download',
      hidden: 'in pausa: la pagina è nascosta', min: 'min', sec: 's', left: 'mancano %1', mbs: 'MB/s',
      c1: 'Conversione sulla console', c1x: 'conversione in un piccolo MP4, che il Wi-Fi del 3DS invia rapidamente', c1q: 'in attesa del suo turno',
      c2: 'Download sul telefono', direct: 'Scarica direttamente', directLink: 'oppure scarica direttamente',
      big: 'Video lungo (%1): il browser del telefono lo scarica da solo, cercalo tra i suoi download. Tieni la console aperta e sulla schermata “Scarica”.',
      diag: 'Log per lo sviluppatore (.txt)',
      pick: 'Trascina qui un file .r3d o fai clic per sceglierlo', pickHint: 'Dalla scheda SD: /3ds/recorder/recordings',
      privacy: 'Il file resta sul tuo computer: il video lo crea il browser e non viene caricato nulla.',
      read: 'Lettura del file', notR3d: 'Questa non è una registrazione di 3DS Recorder (.r3d).',
      rpl: 'Questo è il buffer del replay, non una registrazione. Salva la clip sulla console (combinazione del replay) e converti il file .r3d che viene creato.',
      optScale: 'Scala', optAudio: 'Audio', eqOn: 'Come sulla console', eqOff: 'Come registrato',
      scaleHint: '1× è la dimensione originale del 3DS (400×480). 2× e 3× mantengono i pixel nitidi negli editor e sugli schermi grandi; il file è più pesante.',
      optLayout: 'Schermi', layAuto: 'Come registrato', layTop: 'Superiore', layBottom: 'Inferiore', laySide: 'Affiancati', optQuality: 'Per', qNormal: 'Condividere', qHigh: 'YouTube', qEdit: 'Montaggio', qualityHint: 'YouTube: bitrate più alto (3× dà 1440p, che YouTube riproduce in qualità migliore). Montaggio: ogni fotogramma è chiave, con bitrate alto, per DaVinci Resolve e altri editor; il file è molto più pesante.', pcTool: 'Ti serve DNxHR per DaVinci Resolve, un file senza perdita o un file per schermo?', pcToolLink: 'r3d2video per Windows',
      fastPc: 'veloce: codificatore video del computer', readErr: 'Impossibile leggere il file (%1).', failedLocal: 'Impossibile creare il video (%1).', again: 'Converti un altro file',
      webm: 'Questo browser può creare solo video WebM. Per un MP4, apri questa pagina in Chrome, Edge o Safari.',
      optInputs: "Pulsanti", inOff: "Nascondi", inOn: "Mostra", inputsHint: "I pulsanti premuti, accanto allo schermo inferiore o in un angolo (registrazioni dalla 1.6.0).", qDiscord: "Discord", discordHint: "Discord: sotto i 10 MB, quanto Discord accetta senza Nitro.", discordLong: "Troppo lungo per 10 MB: l’immagine sarà sfocata. Sotto il video puoi tagliare una parte più breve.", noInputs: "Questa registrazione non ha pulsanti: è stata fatta prima della 1.6.0 o con Registra pulsanti disattivato.", trimTitle: "Taglia una parte", trimStart: "Inizia qui", trimEnd: "Finisci qui", trimGo: "Crea questa parte", trimHint: "Riproduci il video e metti in pausa dove deve iniziare la parte: «Inizia qui». Poi dove deve finire: «Finisci qui».", trimLabel: "parte %1–%2",
    },
    fr: {
      mb: 'Mo', gb: 'Go', length: 'Durée', size: 'Taille', eta: 'Durée du transfert', s1: 'Téléchargement depuis la console',
      s2: 'Création de la vidéo', s3: 'Sauvegarde', waiting: 'en attente',
      fast: 'rapide : encodeur vidéo du téléphone',
      live: 'en temps réel : laissez cette page ouverte et l’écran allumé',
      none: 'ce navigateur ne peut pas créer de vidéos', start: 'Commencer',
      startHint: 'Appuyez sur « Commencer » : le navigateur n’autorise une page à utiliser le son qu’après un appui.',
      keep: 'Laissez cette page ouverte et l’écran allumé jusqu’à ce que la vidéo soit prête. Si la page est masquée, la vidéo attend.',
      long: 'Enregistrement long : environ %1 de transfert via le Wi-Fi de la 3DS. Pour les enregistrements longs, passer par la carte SD est plus rapide.',
      save: 'Sauvegarder la vidéo', share: 'Partager / ajouter à Photos',
      raw: 'ou le .r3d original, pour un ordinateur',
      sd: 'Les enregistrements peuvent aussi être récupérés sur la carte SD : éteignez la console et insérez la carte dans un ordinateur, ils sont dans /3ds/recorder/recordings.',
      howIos: 'Appuyez sur « Sauvegarder la vidéo », puis ouvrez Fichiers → Téléchargements → la vidéo → Partager → Enregistrer la vidéo : elle arrive dans Photos.',
      howAndroid: 'Appuyez sur « Sauvegarder la vidéo » : elle arrive dans Téléchargements et apparaît dans votre galerie.',
      howOther: 'Appuyez sur « Sauvegarder la vidéo » : elle arrive dans votre dossier de téléchargements.',
      ready: 'terminé', retry: 'Réessayer',
      lost: 'La connexion avec la console a été perdue. La console est-elle toujours ouverte, sur l’écran « Télécharger » ?',
      notFound: 'Cet enregistrement n’est plus proposé. Sélectionnez-le de nouveau sur la console (Galerie → Y).',
      noFile: 'Aucun enregistrement dans le lien. Scannez de nouveau le code sur la console.',
      failed: 'Impossible de créer la vidéo (%1). Le .r3d original est ci-dessous.',
      paused: 'en pause : en attente du téléchargement', hidden: 'en pause : la page est masquée', min: 'min',
      sec: 's', left: 'encore %1', mbs: 'Mo/s', c1: 'Conversion sur la console',
      c1x: 'conversion en petit MP4 : le Wi-Fi de la 3DS l’envoie vite', c1q: 'en attente de son tour',
      c2: 'Téléchargement sur le téléphone', direct: 'Télécharger directement',
      directLink: 'ou télécharger directement',
      big: 'Vidéo volumineuse (%1) : le navigateur du téléphone la télécharge tout seul, regardez dans ses téléchargements. Laissez la console ouverte, sur l’écran « Télécharger ».',
      diag: 'Journal pour le développeur (.txt)', pick: 'Déposez un .r3d ici ou cliquez pour en choisir un',
      pickHint: 'Sur la carte SD : /3ds/recorder/recordings',
      privacy: 'Le fichier reste sur votre ordinateur : c’est le navigateur qui crée la vidéo, rien n’est mis en ligne.',
      read: 'Lecture du fichier', notR3d: 'Ce n’est pas un enregistrement de 3DS Recorder (.r3d).',
      rpl: 'C’est une mémoire tampon de la relecture, pas un enregistrement. Sauvegardez le clip sur la console (combinaison de la relecture) et convertissez le .r3d ainsi créé.',
      optScale: 'Échelle', optAudio: 'Son', eqOn: 'Comme sur la console', eqOff: 'Tel qu’enregistré',
      scaleHint: '1× est la taille d’origine de la 3DS (400×480). 2× et 3× gardent des pixels nets dans les logiciels de montage et sur les grands écrans ; le fichier est plus lourd.',
      optLayout: 'Écrans', layAuto: 'Comme enregistré', layTop: 'Haut', layBottom: 'Bas', laySide: 'Côte à côte', optQuality: 'Pour', qNormal: 'Partager', qHigh: 'YouTube', qEdit: 'Montage', qualityHint: 'YouTube : un débit plus élevé (3× donne du 1440p, que YouTube diffuse en meilleure qualité). Montage : chaque image est une image clé, à haut débit, pour DaVinci Resolve et d’autres logiciels ; le fichier est bien plus lourd.', pcTool: 'Besoin de DNxHR pour DaVinci Resolve, d’un fichier sans perte ou d’un fichier par écran ?', pcToolLink: 'r3d2video pour Windows',
      fastPc: 'rapide : encodeur vidéo de l’ordinateur', readErr: 'Impossible de lire le fichier (%1).',
      failedLocal: 'Impossible de créer la vidéo (%1).', again: 'Convertir un autre fichier',
      webm: 'Ce navigateur ne peut créer que des vidéos WebM. Pour un MP4, ouvrez cette page dans Chrome, Edge ou Safari.',
      optInputs: "Boutons", inOff: "Masquer", inOn: "Afficher", inputsHint: "Les boutons appuyés, à côté de l’écran du bas ou dans un coin (enregistrements depuis la 1.6.0).", qDiscord: "Discord", discordHint: "Discord : moins de 10 Mo, ce que Discord accepte sans Nitro.", discordLong: "Trop long pour 10 Mo : l’image sera floue. Sous la vidéo, vous pouvez découper un passage plus court.", noInputs: "Cet enregistrement n’a pas de boutons : il a été fait avant la 1.6.0 ou avec Enregistrer les boutons désactivé.", trimTitle: "Découper un passage", trimStart: "Début ici", trimEnd: "Fin ici", trimGo: "Créer ce passage", trimHint: "Lancez la vidéo et mettez-la en pause là où le passage doit commencer : « Début ici ». Puis là où il doit finir : « Fin ici ».", trimLabel: "passage %1–%2",
    },
    de: {
      mb: 'MB', gb: 'GB', length: 'Dauer', size: 'Größe', eta: 'Übertragungsdauer', s1: 'Download von der Konsole', s2: 'Video erstellen',
      s3: 'Speichern', waiting: 'wartet', fast: 'schnell: Video-Encoder des Handys',
      live: 'in Echtzeit: Lass diese Seite offen und den Bildschirm an', none: 'Dieser Browser kann keine Videos erstellen',
      start: 'Starten', startHint: 'Tippe auf „Starten“: Der Browser lässt eine Seite Ton erst nach einem Tippen nutzen.',
      keep: 'Lass diese Seite offen und den Bildschirm an, bis das Video fertig ist. Ist die Seite verborgen, wartet das Video.',
      long: 'Eine lange Aufnahme: Über das Wi-Fi des 3DS dauert es etwa %1. Bei langen Aufnahmen ist die SD Card schneller.',
      save: 'Video speichern', share: 'Teilen / in Fotos sichern', raw: 'oder die Original-.r3d für den Computer',
      sd: 'Aufnahmen kannst du auch von der SD Card holen: Schalte die Konsole aus und stecke die Karte in einen Computer, sie liegen in /3ds/recorder/recordings.',
      howIos: 'Tippe auf „Video speichern“, dann öffne Dateien → Downloads → das Video → Teilen → Video sichern: Es landet in Fotos.',
      howAndroid: 'Tippe auf „Video speichern“: Es landet in Downloads und erscheint in der Galerie.',
      howOther: 'Tippe auf „Video speichern“: Es landet in deinem Download-Ordner.',
      ready: 'fertig', retry: 'Erneut versuchen', lost: 'Die Verbindung zur Konsole ist abgebrochen. Ist die Konsole noch aufgeklappt und auf dem Bildschirm „Herunterladen“?',
      notFound: 'Diese Aufnahme wird nicht mehr angeboten. Wähle sie auf der Konsole erneut aus (Galerie → Y).',
      noFile: 'Der Link enthält keine Aufnahme. Scanne den Code auf der Konsole erneut.',
      failed: 'Das Video konnte nicht erstellt werden (%1). Die Original-.r3d findest du unten.', paused: 'pausiert: wartet auf den Download',
      hidden: 'pausiert: Die Seite ist verborgen', min: 'Min.', sec: 's', left: 'noch %1', mbs: 'MB/s',
      c1: 'Umwandlung auf der Konsole', c1x: 'wird in eine kleine MP4-Datei umgewandelt, die das Wi-Fi des 3DS schnell sendet', c1q: 'in der Warteschlange',
      c2: 'Download aufs Handy', direct: 'Direkt herunterladen', directLink: 'oder direkt herunterladen',
      big: 'Ein langes Video (%1): Der Browser des Handys lädt es von selbst herunter, sieh in seinen Downloads nach. Lass die Konsole aufgeklappt auf dem Bildschirm „Herunterladen“.',
      diag: 'Protokoll für den Entwickler (.txt)',
      pick: 'Ziehe eine .r3d hierher oder klicke zum Auswählen', pickHint: 'Von der SD Card: /3ds/recorder/recordings',
      privacy: 'Die Datei bleibt auf deinem Computer: Der Browser erstellt das Video, nichts wird hochgeladen.',
      read: 'Datei lesen', notR3d: 'Das ist keine Aufnahme von 3DS Recorder (.r3d).',
      rpl: 'Das ist ein Replay-Puffer, keine Aufnahme. Speichere den Clip auf der Konsole (Replay-Kombination) und wandle die dabei erstellte .r3d um.',
      optScale: 'Skalierung', optAudio: 'Ton', eqOn: 'Wie auf der Konsole', eqOff: 'Wie aufgenommen',
      scaleHint: '1× ist die Originalgröße des 3DS (400×480). 2× und 3× halten die Pixel in Schnittprogrammen und auf großen Bildschirmen scharf; die Datei wird größer.',
      optLayout: 'Bildschirme', layAuto: 'Wie aufgenommen', layTop: 'Oben', layBottom: 'Unten', laySide: 'Nebeneinander', optQuality: 'Für', qNormal: 'Teilen', qHigh: 'YouTube', qEdit: 'Schnitt', qualityHint: 'YouTube: höhere Bitrate (3× ergibt 1440p, das YouTube in besserer Qualität abspielt). Schnitt: jedes Bild ein Keyframe mit hoher Bitrate, für DaVinci Resolve und andere Schnittprogramme; die Datei wird deutlich größer.', pcTool: 'DNxHR für DaVinci Resolve, eine verlustfreie Datei oder eine Datei pro Bildschirm?', pcToolLink: 'r3d2video für Windows',
      fastPc: 'schnell: Video-Encoder des Computers', readErr: 'Die Datei konnte nicht gelesen werden (%1).', failedLocal: 'Das Video konnte nicht erstellt werden (%1).', again: 'Weitere Datei umwandeln',
      webm: 'Dieser Browser kann nur WebM-Videos erstellen. Für eine MP4-Datei öffne diese Seite in Chrome, Edge oder Safari.',
      optInputs: "Tasten", inOff: "Ausblenden", inOn: "Zeigen", inputsHint: "Die gedrückten Tasten, neben dem unteren Bildschirm oder in einer Ecke (Aufnahmen ab 1.6.0).", qDiscord: "Discord", discordHint: "Discord: unter 10 MB, so viel nimmt Discord ohne Nitro.", discordLong: "Zu lang für 10 MB: Das Bild wird unscharf. Unter dem Video kannst du einen kürzeren Teil ausschneiden.", noInputs: "Diese Aufnahme hat keine Tastendrücke: Sie wurde vor 1.6.0 oder mit ausgeschaltetem „Tasten aufzeichnen“ gemacht.", trimTitle: "Teil ausschneiden", trimStart: "Hier anfangen", trimEnd: "Hier enden", trimGo: "Diesen Teil erstellen", trimHint: "Spiel das Video ab und halte dort an, wo der Teil anfangen soll: „Hier anfangen“. Dann dort, wo er enden soll: „Hier enden“.", trimLabel: "Teil %1–%2",
    },
    'zh-Hans': {
      mb: 'MB', gb: 'GB', length: '时长', size: '大小', eta: '传输时间', s1: '从主机下载', s2: '生成视频',
      s3: '保存', waiting: '等待中', fast: '快速：使用手机的视频编码器',
      live: '实时：请保持此页面打开、屏幕常亮', none: '此浏览器无法生成视频',
      start: '开始', startHint: '请点按“开始”：浏览器只允许页面在点按后使用声音。',
      keep: '视频完成之前，请保持此页面打开、屏幕常亮。页面被隐藏时，视频会暂停等待。',
      long: '录像较长：通过3DS的Wi-Fi传输约需%1。长录像用SD卡更快。',
      save: '保存视频', share: '共享／存储到“照片”', raw: '或下载原始.r3d文件，供电脑使用',
      sd: '也可以通过SD卡获取录像：关闭主机，把SD卡插到电脑上，录像位于/3ds/recorder/recordings。',
      howIos: '点按“保存视频”，然后打开“文件”→“下载”→该视频→“共享”→“存储视频”：视频会存入“照片”。',
      howAndroid: '点按“保存视频”：视频会存入“下载”文件夹，并出现在相册中。',
      howOther: '点按“保存视频”：视频会存入下载文件夹。',
      ready: '完成', retry: '重试', lost: '与主机的连接已断开。主机是否仍停留在“下载”画面，且没有合上？',
      notFound: '此录像已不再提供。请在主机上重新选择（图库→Y）。',
      noFile: '链接中没有录像。请在主机上重新扫描二维码。',
      failed: '无法生成视频（%1）。原始.r3d文件在下方。', paused: '已暂停：等待下载',
      hidden: '已暂停：页面被隐藏', min: '分钟', sec: '秒', left: '剩余%1', mbs: 'MB/s',
      c1: '在主机上转换', c1x: '正在转换为体积较小的MP4：3DS的Wi-Fi能很快传完', c1q: '排队等待中',
      c2: '下载到手机', direct: '直接下载', directLink: '或直接下载',
      big: '视频较大（%1）：手机浏览器会自动下载，请在浏览器的下载内容中查找。请让主机停留在“下载”画面，不要合上主机。',
      diag: '开发者日志（.txt）',
      pick: '将.r3d文件拖放到此处，或点击选择', pickHint: '位于SD卡：/3ds/recorder/recordings',
      privacy: '文件不会离开您的电脑：视频由浏览器生成，不会上传任何内容。',
      read: '读取文件', notR3d: '这不是3DS Recorder的录像（.r3d）。',
      rpl: '这是即时回放的缓存，不是录像。请在主机上保存片段（即时回放组合键），然后转换由此生成的.r3d文件。',
      optScale: '缩放', optAudio: '声音', eqOn: '模拟主机音色', eqOff: '原声',
      scaleHint: '1×是3DS的原始尺寸（400×480）。2×和3×可在剪辑软件和大屏幕上保持像素清晰，但文件更大。',
      optLayout: '画面', layAuto: '按录制', layTop: '上屏', layBottom: '下屏', laySide: '并排', optQuality: '用途', qNormal: '分享', qHigh: 'YouTube', qEdit: '剪辑', qualityHint: 'YouTube：更高码率（3×为1440p，YouTube会以更好的画质播放）。剪辑：每帧都是关键帧且码率高，适合DaVinci Resolve等剪辑软件；文件会大很多。', pcTool: '需要用于DaVinci Resolve的DNxHR、无损文件或每个屏幕单独的文件？', pcToolLink: 'Windows版r3d2video',
      fastPc: '快速：使用电脑的视频编码器', readErr: '无法读取文件（%1）。', failedLocal: '无法生成视频（%1）。', again: '转换其他文件',
      webm: '此浏览器只能生成WebM视频。如需MP4，请在Chrome、Edge或Safari中打开此页面。',
      optInputs: "按键", inOff: "隐藏", inOn: "显示", inputsHint: "按下的按键，显示在下屏旁边或角落（1.6.0起的录像）。", qDiscord: "Discord", discordHint: "Discord：小于10 MB，即没有Nitro时Discord接受的大小。", discordLong: "对10 MB来说太长：画面会模糊。可在视频下方剪出较短的片段。", noInputs: "这段录像没有按键记录：录于1.6.0之前，或“记录按键”已关闭。", trimTitle: "剪出片段", trimStart: "从这里开始", trimEnd: "到这里结束", trimGo: "生成此片段", trimHint: "播放视频，在片段开始处暂停，点“从这里开始”；再在结束处暂停，点“到这里结束”。", trimLabel: "片段 %1–%2",
    },
    'zh-Hant': {
      mb: 'MB', gb: 'GB', length: '長度', size: '大小', eta: '傳輸時間', s1: '從主機下載', s2: '製作影片',
      s3: '儲存', waiting: '等待中', fast: '快速：使用手機的影片編碼器',
      live: '即時處理：請保持此頁面開啟並讓螢幕亮著', none: '此瀏覽器無法製作影片',
      start: '開始', startHint: '請點一下「開始」：瀏覽器只允許網頁在點觸後使用聲音。',
      keep: '影片完成前，請保持此頁面開啟並讓螢幕亮著。頁面被隱藏時，影片製作會暫停。',
      long: '較長的錄影：透過 3DS 的 Wi-Fi 約需 %1。這類錄影從 SD 卡取出會更快。',
      save: '儲存影片', share: '分享／儲存到「照片」', raw: '或下載原始 .r3d 檔，供電腦使用',
      sd: '也可以從 SD 卡取出錄影：關閉主機電源，將 SD 卡插入電腦，錄影位於 /3ds/recorder/recordings。',
      howIos: '點一下「儲存影片」，然後開啟「檔案」→「下載項目」→ 影片 →「分享」→「儲存影片」：影片會存到「照片」。',
      howAndroid: '點一下「儲存影片」：影片會存到「下載」資料夾，並出現在圖庫中。',
      howOther: '點一下「儲存影片」：影片會存到您的下載資料夾。',
      ready: '完成', retry: '重試', lost: '與主機的連線已中斷。主機是否仍停留在「下載」畫面，且沒有合上？',
      notFound: '已無法提供此錄影。請在主機上重新選擇（圖庫 → Y）。',
      noFile: '連結中沒有錄影。請重新掃描主機上的 QR Code。',
      failed: '無法製作影片（%1）。原始 .r3d 檔在下方。', paused: '已暫停：等待下載',
      hidden: '已暫停：頁面已隱藏', min: '分鐘', sec: '秒', left: '剩餘 %1', mbs: 'MB/s',
      c1: '在主機上轉換', c1x: '正在轉換為較小的 MP4：3DS 的 Wi-Fi 能很快傳完', c1q: '排隊等候中',
      c2: '下載到手機', direct: '直接下載', directLink: '或直接下載',
      big: '較長的影片（%1）：手機的瀏覽器會自動下載，請到瀏覽器的下載項目中查看。請讓主機停留在「下載」畫面，不要合上。',
      diag: '給開發者的記錄檔（.txt）',
      pick: '將 .r3d 拖放到這裡，或點擊選擇檔案', pickHint: '位於 SD 卡：/3ds/recorder/recordings',
      privacy: '檔案只會留在您的電腦上：影片由瀏覽器製作，不會上傳任何資料。',
      read: '讀取檔案', notR3d: '這不是 3DS Recorder 的錄影（.r3d）。',
      rpl: '這是即時重播的暫存檔，不是錄影。請在主機上儲存片段（即時重播快捷鍵），再轉換它產生的 .r3d。',
      optScale: '縮放', optAudio: '聲音', eqOn: '與主機相同', eqOff: '原始錄音',
      scaleHint: '1× 為 3DS 的原始尺寸（400×480）。2× 和 3× 可在編輯軟體和大螢幕上保持像素清晰；檔案會較大。',
      optLayout: '畫面', layAuto: '依錄製', layTop: '上螢幕', layBottom: '下螢幕', laySide: '並排', optQuality: '用途', qNormal: '分享', qHigh: 'YouTube', qEdit: '剪輯', qualityHint: 'YouTube：較高位元率（3× 為 1440p，YouTube 會以較佳畫質播放）。剪輯：每格都是關鍵影格且位元率高，適合 DaVinci Resolve 等剪輯軟體；檔案會大很多。', pcTool: '需要給 DaVinci Resolve 用的 DNxHR、無損檔案，或每個螢幕各一個檔案？', pcToolLink: 'Windows 版 r3d2video',
      fastPc: '快速：使用電腦的影片編碼器', readErr: '無法讀取檔案（%1）。', failedLocal: '無法製作影片（%1）。', again: '轉換其他檔案',
      webm: '此瀏覽器只能製作 WebM 影片。若要 MP4，請在 Chrome、Edge 或 Safari 中開啟此頁面。',
      optInputs: "按鍵", inOff: "隱藏", inOn: "顯示", inputsHint: "按下的按鍵，顯示在下螢幕旁邊或角落（1.6.0 起的錄影）。", qDiscord: "Discord", discordHint: "Discord：小於 10 MB，即沒有 Nitro 時 Discord 接受的大小。", discordLong: "對 10 MB 來說太長：畫面會模糊。可在影片下方剪出較短的片段。", noInputs: "這段錄影沒有按鍵記錄：錄於 1.6.0 之前，或「記錄按鍵」已關閉。", trimTitle: "剪出片段", trimStart: "從這裡開始", trimEnd: "到這裡結束", trimGo: "產生此片段", trimHint: "播放影片，在片段開始處暫停，點「從這裡開始」；再在結束處暫停，點「到這裡結束」。", trimLabel: "片段 %1–%2",
    },
    ja: {
      mb: 'MB', gb: 'GB', length: '長さ', size: 'サイズ', eta: '転送時間', s1: '本体からダウンロード', s2: '動画を作成',
      s3: '保存', waiting: '待機中', fast: '高速：スマートフォンの動画エンコーダー',
      live: 'リアルタイム：このページを開いたまま、画面をつけておいてください', none: 'このブラウザーでは動画を作成できません',
      start: '開始', startHint: '「開始」をタップしてください。ブラウザーは、タップされたあとでないとページに音声を使わせません。',
      keep: '動画ができるまで、このページを開いたまま画面をつけておいてください。ページが隠れると、動画の作成は待機します。',
      long: '長い録画です：3DSのWi-Fiで約%1かかります。長い録画はSDカードから取り出すほうが速いです。',
      save: '動画を保存', share: '共有／「写真」に保存', raw: 'または元の.r3d（パソコン用）',
      sd: '録画はSDカードからも取り出せます。本体の電源を切ってカードをパソコンに入れてください。録画は/3ds/recorder/recordingsにあります。',
      howIos: '「動画を保存」をタップし、「ファイル」→「ダウンロード」→動画→共有→「ビデオを保存」の順に進むと、「写真」に保存されます。',
      howAndroid: '「動画を保存」をタップすると「ダウンロード」に保存され、ギャラリーにも表示されます。',
      howOther: '「動画を保存」をタップすると、ダウンロードフォルダーに保存されます。',
      ready: '完了', retry: 'もう一度', lost: '本体との接続が切れました。本体は開いたまま「ダウンロード」の画面になっていますか？',
      notFound: 'この録画はもうダウンロードできません。本体でもう一度選んでください（ギャラリー→Y）。',
      noFile: 'リンクに録画が含まれていません。本体のコードをもう一度読み取ってください。',
      failed: '動画を作成できませんでした（%1）。元の.r3dは下にあります。', paused: '一時停止中：ダウンロードを待っています',
      hidden: '一時停止中：ページが隠れています', min: '分', sec: '秒', left: '残り%1', mbs: 'MB/s',
      c1: '本体で変換', c1x: '小さなMP4に変換中：3DSのWi-Fiでもすばやく送れます', c1q: '順番待ち',
      c2: 'スマートフォンにダウンロード', direct: '直接ダウンロード', directLink: 'または直接ダウンロード',
      big: '長い動画です（%1）：スマートフォンのブラウザーが自動でダウンロードするので、ブラウザーのダウンロードを確認してください。本体は開いたまま「ダウンロード」の画面にしておいてください。',
      diag: '開発者向けログ（.txt）',
      pick: '.r3dをここにドロップするか、クリックして選択', pickHint: 'SDカード内：/3ds/recorder/recordings',
      privacy: 'ファイルはパソコンから出ません。動画はブラウザーが作成し、何もアップロードされません。',
      read: 'ファイルを読み込み中', notR3d: '3DS Recorderの録画（.r3d）ではありません。',
      rpl: 'これはリプレイのバッファーで、録画ではありません。本体でクリップを保存し（リプレイの組み合わせ）、作られた.r3dを変換してください。',
      optScale: '倍率', optAudio: '音声', eqOn: '本体スピーカー風', eqOff: '録音のまま',
      scaleHint: '1×は3DS本来のサイズ（400×480）です。2×と3×では、編集ソフトや大きな画面でもドットがくっきりしますが、ファイルが大きくなります。',
      optLayout: '画面', layAuto: '録画のまま', layTop: '上画面', layBottom: '下画面', laySide: '横に並べる', optQuality: '用途', qNormal: '共有', qHigh: 'YouTube', qEdit: '編集', qualityHint: 'YouTube：ビットレートを上げます（3×で1440p、YouTubeでより高画質に再生されます）。編集：全フレームをキーフレームにし高ビットレートで、DaVinci Resolveなどの編集ソフト向け。ファイルはかなり大きくなります。', pcTool: 'DaVinci Resolve用のDNxHR、ロスレス、画面ごとの別ファイルが必要ですか？', pcToolLink: 'Windows版 r3d2video',
      fastPc: '高速：パソコンの動画エンコーダー', readErr: 'ファイルを読み込めませんでした（%1）。', failedLocal: '動画を作成できませんでした（%1）。', again: 'ほかのファイルを変換',
      webm: 'このブラウザーではWebM動画しか作成できません。MP4にするには、このページをChrome、Edge、Safariで開いてください。',
      optInputs: "ボタン", inOff: "表示しない", inOn: "表示する", inputsHint: "押したボタンを下画面の横か隅に表示します（1.6.0以降の録画）。", qDiscord: "Discord", discordHint: "Discord：Nitroなしで送れる10 MB以下にします。", discordLong: "10 MBには長すぎます：画質がぼやけます。動画の下で短い部分を切り出せます。", noInputs: "この録画にはボタンの記録がありません（1.6.0より前、または「ボタンを記録」がオフ）。", trimTitle: "部分を切り出す", trimStart: "ここから", trimEnd: "ここまで", trimGo: "この部分を作る", trimHint: "動画を再生し、部分の始まりで一時停止して「ここから」、終わりで一時停止して「ここまで」を押します。", trimLabel: "部分 %1–%2",
    },
  };
  let L = TEXT.en;
  let locale = 'en-GB';
  // the dates' form for each language (Intl), the TEXT key for each
  const LOCALE = { en: 'en-GB', uk: 'uk-UA', de: 'de-DE', es: 'es-ES', fr: 'fr-FR', it: 'it-IT', pt: 'pt-BR',
    ja: 'ja-JP', 'zh-Hans': 'zh-CN', 'zh-Hant': 'zh-TW' };
  function setLang(code) {
    if (!code || !TEXT[code]) return false;
    L = TEXT[code];
    locale = LOCALE[code] || code;
    document.documentElement.lang = code;
    return true;
  }
  // the browser's languages, in its order: zh-TW / zh-HK / zh-MO / zh-Hant -> zh-Hant, other zh -> zh-Hans
  function browserLang() {
    for (const tag of navigator.languages || [navigator.language || '']) {
      const l = String(tag).toLowerCase();
      const code = /^zh-(tw|hk|mo|hant)/.test(l) ? 'zh-Hant' : /^zh/.test(l) ? 'zh-Hans' : l.split('-')[0];
      if (TEXT[code]) return code;
    }
    return 'en';
  }
  const t = (k, ...a) => a.reduce((s, v, i) => s.replace('%' + (i + 1), v), L[k] || TEXT.en[k] || k);

  const CHUNK = 512 << 10;      // bytes per request (~1-2 s each on the 3DS Wi-Fi: progress moves)
  const PARALLEL = 3;           // requests at once (+15-25% on the 3DS Wi-Fi)
  const STALL_MS = 20000;       // a request with no byte for this long is asked again (weak signal)
  const BIG_MP4 = 150 << 20;    // larger MP4s are not put together in the page (phone memory)
  const FEED = 128 << 10;       // bytes handed to the decoder at a time (bounds the frames in flight)
  const WIFI_KBS = 420;         // for the first estimate (O3DS measured 350-490 KB/s)
  const AAC_RATE = 48000;
  const ua = navigator.userAgent;
  const isIOS = /iPad|iPhone|iPod/.test(ua) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const isAndroid = /Android/.test(ua);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const LOCAL = document.documentElement.dataset.mode === 'local';

  // the QR code names the recording; the address typed by hand (shown under the code) does not:
  // then it is the one the console offers
  let name = new URLSearchParams(location.search).get('f');
  let info = null;
  let localFile = null; // local mode: the File being converted
  const opts = { scale: 1, eq: true, layout: 'auto', quality: 'normal', inputs: false }; // local mode: size, sound, screens, purpose, buttons (remembered in the browser)
  let trim = null; // local mode: { from, to } in seconds of the recording, the part being made (not remembered)
  let lastFast = false; // the video on the page was made the fast way (a part of it can be cut)

  // Local mode: which screens go into the video and where (the composite picture is 400 wide: the top
  // screen above, the bottom one centred under it). blits: [sx, sy, sw, sh, dx, dy] from it; null: as it is.
  // A screen the recording does not have: as recorded. (From r3d2video's layouts, noctis1008-wq.)
  function viewFor(hdr, layout) {
    const hasTop = !hdr.bottomOnly, hasBottom = hdr.both || hdr.bottomOnly, by = hdr.both ? 240 : 0;
    if (layout === 'top' && hasTop && hasBottom) return { w: 400, h: 240, blits: [[0, 0, 400, 240, 0, 0]] };
    if (layout === 'bottom' && hasBottom) return { w: 320, h: 240, blits: [[40, by, 320, 240, 0, 0]] };
    if (layout === 'side' && hdr.both) return { w: 720, h: 240, blits: [[0, 0, 400, 240, 0, 0], [40, 240, 320, 240, 400, 0]] };
    return { w: 400, h: hdr.height, blits: null };
  }
  // the bitrate for a size and purpose: the upscaled pixels are flat blocks, they need far less than
  // scale^2 times the bits; YouTube re-encodes what it gets (more in, better out); editing: all keyframes
  const QUALITY = { normal: 1, high: 1.75, edit: 4, discord: 1 };
  // Discord: the whole file under 10 MB (its limit without Nitro), the sound at 96 kbit/s, a margin for
  // the encoder's swings; at least 250 kbit/s (a long video gets blurry: the page says so)
  const DISCORD_BYTES = 10 * 1000 * 1000, DISCORD_AUDIO = 96000, DISCORD_MIN = 250000;
  const discordRate = (seconds) => Math.floor((DISCORD_BYTES * 8 * 0.85) / Math.max(1, seconds) - DISCORD_AUDIO);
  function bitrateFor(view, scale, quality, seconds = 0) {
    const base = [0, 3500000, 8000000, 12000000][scale] * (view.w * view.h) / (400 * 480);
    const rate = Math.round(Math.max(1500000, base) * (QUALITY[quality] || 1));
    if (quality === 'discord') return Math.max(DISCORD_MIN, Math.min(rate, discordRate(seconds)));
    return rate;
  }

  // One line into the console's app.log: what the phone did, so a test far away needs no
  // questions afterwards (the "Log for the developer" file has it). Never fails the page.
  let reports = 0;
  function report(msg) {
    if (LOCAL || ++reports > 200) return; // local: no console to tell
    try {
      fetch('/api/log?m=' + encodeURIComponent(String(msg).slice(0, 300)), { cache: 'no-store' }).catch(() => {});
    } catch (e) { /* no fetch */ }
  }
  const why = (e) => (e && (e.name ? e.name + ': ' : '') + (e.message || '')) || String(e);
  window.addEventListener('error', (e) => report('js error: ' + e.message + ' (' + (e.filename || '').split('/').pop() + ':' + e.lineno + ')'));
  // where a rejection came from: "Error: Na" alone (Chrome on iPhone, 2026-11-23) told nothing
  const at = (e) => {
    const line = e && typeof e.stack === 'string' ? e.stack.split('\n').find((l) => /\d+:\d+/.test(l)) : '';
    return line ? ' @ ' + line.trim().split('/').pop() : '';
  };
  window.addEventListener('unhandledrejection', (e) => report('js promise: ' + why(e.reason) + at(e.reason)));
  document.addEventListener('visibilitychange', () => report('page ' + document.visibilityState));

  /* ------------------------------------------------------------ UI */

  function clock(sec) {
    sec = Math.max(0, Math.round(sec));
    const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60, s = sec % 60;
    return (h ? h + ':' + String(m).padStart(2, '0') : String(m).padStart(2, '0')) + ':' + String(s).padStart(2, '0');
  }
  function mb(bytes) {
    return bytes >= 1 << 30 ? (bytes / (1 << 30)).toLocaleString(locale, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) + ' ' + t('gb')
      : Math.round(bytes / (1 << 20)) + ' ' + t('mb');
  }
  function duration(sec) { return sec >= 90 ? '≈ ' + Math.round(sec / 60) + ' ' + t('min') : '≈ ' + Math.round(sec) + ' ' + t('sec'); }
  function when(n) { // yyyymmdd_hhmmss.r3d
    const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})/.exec(n || '');
    if (!m) return n || '';
    const d = new Date(+m[1], m[2] - 1, +m[3], +m[4], +m[5]);
    return d.toLocaleDateString(locale, { day: 'numeric', month: 'long', year: 'numeric' }) +
      ' · ' + m[4] + ':' + m[5];
  }
  function step(i, state, pct, text, extra) {
    const el = $('s' + i);
    el.classList.toggle('on', state === 'on');
    el.classList.toggle('done', state === 'done');
    if (pct !== undefined && $('b' + i)) $('b' + i).style.width = Math.min(100, pct) + '%';
    if (text !== undefined && $('p' + i)) $('p' + i).textContent = text;
    if (extra !== undefined) $('s' + i + 'x').textContent = extra;
  }
  function show(id, text) { const el = $(id); if (text !== undefined) el.textContent = text; el.hidden = false; }
  function fail(text, retry) {
    report('error shown: ' + text.slice(0, 80));
    show('error', text);
    if (retry) {
      const b = $('retry');
      b.textContent = t('retry');
      b.hidden = false;
      b.onclick = () => { b.hidden = true; $('error').hidden = true; retry(); };
    }
  }

  /* ------------------------------------------------------------ download */

  // Pieces of the file in parallel, handed on in order. feed(u8) may wait (the consumer is behind).
  class Download {
    constructor(url, size, feed, progress) {
      this.url = url; this.size = size; this.feed = feed; this.progress = progress;
      this.next = 0;          // next offset to request
      this.fed = 0;           // handed on up to here
      this.got = 0;           // bytes received
      this.ready = new Map(); // offset -> data, received out of order
      this.redo = [];         // offsets whose request failed
      this.t0 = performance.now();
      this.feeding = Promise.resolve();
    }
    async run() {
      this.error = null;
      const workers = [];
      for (let i = 0; i < PARALLEL; i++) workers.push(this.worker());
      await Promise.all(workers);
      await this.feeding;
      if (this.error) throw this.error;
    }
    async worker() {
      while (!this.error) {
        let off;
        if (this.redo.length) off = this.redo.shift();
        else if (this.next < this.size) { off = this.next; this.next += CHUNK; }
        else return;
        // not too far ahead of what has been handed on
        while (!this.error && off - this.fed > CHUNK * (PARALLEL + 3)) await sleep(50);
        try {
          const data = await this.fetch(off, Math.min(off + CHUNK, this.size) - 1);
          this.ready.set(off, data);
          this.drain();
        } catch (e) {
          this.redo.push(off);
          this.error = e;
        }
      }
    }
    // Bytes a..b. With a weak signal a request can stall: it is given up after STALL_MS without
    // a single byte (not after a fixed time: slow is fine) and asked again from where it stopped.
    async fetch(a, b) {
      const out = new Uint8Array(b - a + 1);
      let have = 0, lastErr, fails = 0;
      while (have < out.length) {
        const ctl = new AbortController();
        let timer = setTimeout(() => ctl.abort(), STALL_MS);
        const moved = () => { clearTimeout(timer); timer = setTimeout(() => ctl.abort(), STALL_MS); };
        const before = have;
        try {
          const r = await fetch(this.url, { headers: { Range: `bytes=${a + have}-${b}` }, signal: ctl.signal, cache: 'no-store' });
          if (r.status === 404) { const e = new Error('404'); e.notFound = true; throw e; }
          if (r.status !== 206 && r.status !== 200) throw new Error('HTTP ' + r.status);
          if (r.body && r.body.getReader) {
            const rd = r.body.getReader();
            for (;;) {
              const { done, value } = await rd.read();
              if (done) break;
              moved();
              if (have + value.length > out.length) throw new Error('long read');
              out.set(value, have);
              have += value.length;
              this.count(value.length);
            }
          } else {
            const buf = new Uint8Array(await r.arrayBuffer());
            if (have + buf.length > out.length) throw new Error('long read');
            out.set(buf, have);
            have += buf.length;
            this.count(buf.length);
          }
          if (have < out.length) throw new Error('short read');
        } catch (e) {
          if (e.notFound) throw e;
          lastErr = e;
          fails = have > before ? 0 : fails + 1; // progress resets the count: only a dead link gives up
          report(`retry ${this.url.split('/').pop()} @${a + have} (${have - before} B moved): ${why(e)}, fail ${fails}`);
          if (fails >= 6) throw lastErr;
          await sleep(1000 * fails); // the console may be busy (503) or the signal weak
        } finally {
          clearTimeout(timer);
        }
      }
      return out;
    }
    count(n) {
      this.got += n;
      this.progress(this.got, this.size, this.got / ((performance.now() - this.t0) / 1000));
    }
    drain() {
      this.feeding = this.feeding.then(async () => {
        while (this.ready.has(this.fed)) {
          const d = this.ready.get(this.fed);
          this.ready.delete(this.fed);
          for (let p = 0; p < d.length; p += FEED) await this.feed(d.subarray(p, p + FEED));
          this.fed += d.length;
        }
      });
    }
  }

  // Local mode: the same interface as Download, the bytes read from a File in order.
  class FileSource {
    constructor(file, feed, progress, stop) {
      this.file = file; this.feed = feed; this.progress = progress; this.stop = stop || (() => false);
      this.got = 0;
      this.t0 = performance.now();
    }
    async run() {
      const size = this.file.size;
      for (let off = 0; off < size && !this.stop(); off += CHUNK) {
        const d = new Uint8Array(await this.file.slice(off, Math.min(off + CHUNK, size)).arrayBuffer());
        for (let p = 0; p < d.length; p += FEED) await this.feed(d.subarray(p, p + FEED));
        this.got += d.length;
        this.progress(this.got, size, this.got / ((performance.now() - this.t0) / 1000));
      }
    }
  }

  /* ------------------------------------------------------------ fast: WebCodecs */

  // Safari (iOS 18) gives the AAC decoderConfig.description as a whole ES_Descriptor (tag 0x03)
  // instead of the 2-byte AudioSpecificConfig; the muxer then writes a broken esds (no sound).
  // Take the DecoderSpecificInfo (tag 0x05) out of it (tests/webcodecs, iPhone 2026-11-03).
  function asc(desc) {
    const b = desc instanceof ArrayBuffer ? new Uint8Array(desc) : new Uint8Array(desc.buffer, desc.byteOffset, desc.byteLength);
    if (b[0] !== 0x03) return b;
    const len = (i) => { let n = 0, k = 0; do { n = (n << 7) | (b[i + k] & 0x7f); } while (b[i + k++] & 0x80 && k < 4); return [n, k]; };
    const find = (i, end, tag) => {
      while (i < end) {
        const tg = b[i], [n, k] = len(i + 1), body = i + 1 + k;
        if (tg === tag) return b.slice(body, body + n);
        if (tg === 0x03) {
          const flags = b[body + 2]; let j = body + 3;
          if (flags & 0x80) j += 2; if (flags & 0x40) j += 1 + b[j]; if (flags & 0x20) j += 2;
          const r = find(j, body + n, tag); if (r) return r;
        }
        if (tg === 0x04) { const r = find(body + 13, body + n, tag); if (r) return r; }
        i = body + n;
      }
      return null;
    };
    return find(0, b.length, 0x05) || b;
  }

  async function fastSupport(width, height, fps, audio, bitrate = 2500000) {
    if (typeof VideoEncoder === 'undefined' || typeof VideoFrame === 'undefined') return null;
    if (audio && (typeof AudioEncoder === 'undefined' || typeof AudioData === 'undefined')) return null;
    if (width > 400 && typeof OffscreenCanvas === 'undefined') return null; // the upscale draws on one
    // AAC where the browser has an encoder for it (Safari, Chrome and Edge on Windows and macOS); else
    // Opus, which every browser with WebCodecs encodes (Firefox, Chrome on Linux): still an MP4, made
    // as fast, where it used to be a real-time WebM (issue #17)
    const want = new URLSearchParams(location.search).get('audio'); // tests: ?audio=opus
    let sound = null;
    if (typeof AudioEncoder !== 'undefined') {
      for (const a of want === 'opus' ? ['opus'] : ['mp4a.40.2', 'opus']) {
        try {
          const r = await AudioEncoder.isConfigSupported({ codec: a, sampleRate: AAC_RATE, numberOfChannels: 2, bitrate: a === 'opus' ? 128000 : 160000 });
          if (r.supported) { sound = a; break; }
        } catch (e) { /* next */ }
      }
    }
    if (audio && !sound) return null;
    // H.264 Main may come with B-frames, whose pictures leave the encoder out of order, and the muxer
    // stops at the first time going back ("Timestamps must be monotonically increasing", Firefox on
    // Windows, issue #28). Constrained Baseline has none: first where there is no AAC (Firefox); the
    // AAC browsers keep Main, as tested
    // level 3.1 up to 1280x720, 4.0 above; 5.1 for the bitrates of the YouTube / editing purposes (4.0 stops at 20 Mbit/s)
    const lv = bitrate > 20000000 ? '33' : Math.ceil(width / 16) * Math.ceil(height / 16) > 3600 ? '28' : '1f';
    const order = sound === 'mp4a.40.2' ? ['avc1.4d40' + lv, 'avc1.42e0' + lv, 'avc1.640028'] : ['avc1.42e0' + lv];
    for (const c of order) {
      try {
        const r = await VideoEncoder.isConfigSupported({ codec: c, width, height, bitrate, framerate: fps });
        if (r.supported) return { video: c, audio: audio ? sound : null };
      } catch (e) { /* next */ }
    }
    return null;
  }

  function opusHead(desc) {
    const b = desc instanceof ArrayBuffer ? new Uint8Array(desc) : new Uint8Array(desc.buffer, desc.byteOffset, desc.byteLength);
    return b.length >= 19 && String.fromCharCode(...b.subarray(0, 8)) === 'OpusHead';
  }

  // Streaming resampler, stereo s16 at `from` Hz -> stereo s16 at `to` Hz (Catmull-Rom).
  class Resampler {
    constructor(from, to) { this.step = from / to; this.pos = 0; this.hist = new Float32Array(8); this.have = 0; }
    run(inp) { // Int16Array stereo -> Int16Array stereo
      const n = inp.length / 2;
      const src = new Float32Array((n + 4) * 2); // 3 frames of history, then the new ones
      src.set(this.hist.subarray(0, 6));
      for (let i = 0; i < inp.length; i++) src[6 + i] = inp[i];
      const total = n + 3;
      const out = [];
      let p = this.pos;
      while (p + 2 < total) {
        const i = Math.floor(p), f = p - i;
        for (let ch = 0; ch < 2; ch++) {
          const y0 = src[2 * i + ch], y1 = src[2 * (i + 1) + ch], y2 = src[2 * (i + 2) + ch], y3 = src[2 * (i + 3) + ch];
          const v = y1 + 0.5 * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
          out.push(v > 32767 ? 32767 : v < -32768 ? -32768 : Math.round(v));
        }
        p += this.step;
      }
      this.hist.set(src.subarray(2 * n, 2 * n + 6));
      this.pos = p - n;
      return Int16Array.from(out);
    }
  }

  // more: { conv (R3D.Converter: its button presses), inputs (draw them), bitrate }
  function fastConvert(hdr, codecs, scale, layout = 'auto', quality = 'normal', more = {}) {
    const view = viewFor(hdr, layout);
    const I = typeof R3DInputs !== 'undefined' ? R3DInputs : null;
    const inputsOn = !!(more.inputs && I && more.conv && more.conv.inputs);
    const inLayout = inputsOn ? I.layout(I.screensFor(hdr, layout)) : null;
    let inState = null, inPatches = [];
    const W = view.w * scale, H = view.h * scale;
    const codec = codecs.video, opus = codecs.audio === 'opus';
    const withAudio = !!(hdr.audio && codecs.audio); // the codec was chosen from the file's details: they agree, but in case
    // the MP4 in 16 MB pieces, not one ArrayBuffer: Firefox makes no Blob of a buffer over 2 GB (issue #35:
    // 3x for editing). With fastStart 'in-memory' the muxer writes them in order, the same bytes as before
    const parts = [];
    let partsEnd = 0, partsOk = true;
    const target = new Mp4Muxer.StreamTarget({ chunked: true, onData: (data, pos) => {
      if (pos !== partsEnd) partsOk = false;
      parts.push(data);
      partsEnd = pos + data.byteLength;
    } });
    const muxer = new Mp4Muxer.Muxer({
      target,
      video: { codec: 'avc', width: W, height: H, frameRate: hdr.fps },
      audio: withAudio ? { codec: opus ? 'opus' : 'aac', numberOfChannels: 2, sampleRate: AAC_RATE } : undefined,
      fastStart: 'in-memory',
      firstTimestampBehavior: 'offset',
    });
    let error = null;
    // a muxer error inside the callback (a timestamp going back) is kept, not lost: the file would be broken
    const venc = new VideoEncoder({ output: (c, m) => { try { muxer.addVideoChunk(c, m); } catch (e) { error = error || e; } }, error: (e) => { error = e; } });
    // Discord: a size to keep, a constant bit rate (Chrome went 34% over a variable one there, 2026-11-22)
    venc.configure({ codec, width: W, height: H, bitrate: more.bitrate || bitrateFor(view, scale, quality), framerate: hdr.fps, latencyMode: 'quality',
      ...(quality === 'discord' ? { bitrateMode: 'constant' } : {}), avc: { format: 'avc' } });
    // 2x / 3x, another layout or the buttons: each picture drawn (bigger) with no smoothing (nearest neighbour), on the GPU
    const drawn = scale > 1 || view.blits || inputsOn;
    const small = drawn ? new OffscreenCanvas(400, hdr.height) : null;
    const big = drawn ? new OffscreenCanvas(W, H) : null;
    const blits = view.blits || [[0, 0, 400, hdr.height, 0, 0]];
    const allKeys = quality === 'edit';
    const bigCtx = big && big.getContext('2d');
    if (bigCtx) bigCtx.imageSmoothingEnabled = false;
    let aenc = null, rs = null, aOut = 0, pend = [], pendN = 0;
    if (withAudio) {
      aenc = new AudioEncoder({
        output: (c, m) => {
          if (!opus && m && m.decoderConfig && m.decoderConfig.description)
            m = { ...m, decoderConfig: { ...m.decoderConfig, description: asc(m.decoderConfig.description) } };
          // Opus: the muxer copies an OpusHead from it (and throws on a short one); without it, it writes its own
          if (opus && m && m.decoderConfig && m.decoderConfig.description && !opusHead(m.decoderConfig.description))
            m = { ...m, decoderConfig: { ...m.decoderConfig, description: undefined } };
          try { muxer.addAudioChunk(c, m); } catch (e) { error = error || e; }
        },
        error: (e) => { error = e; },
      });
      const aRate = quality === 'discord' ? DISCORD_AUDIO : opus ? 128000 : 160000;
      aenc.configure(opus ? { codec: 'opus', sampleRate: AAC_RATE, numberOfChannels: 2, bitrate: aRate }
        : { codec: 'mp4a.40.2', sampleRate: AAC_RATE, numberOfChannels: 2, bitrate: aRate });
      rs = new Resampler(hdr.audioRate / 1000, AAC_RATE);
    }
    let picture = -1, base = null;
    const usPerFrame = 1e6 / hdr.fps;
    const audioBlock = (s) => { // f32-planar: the format checked on the iPhone (tests/webcodecs)
      const frames = s.length / 2;
      const data = new Float32Array(frames * 2);
      for (let i = 0; i < frames; i++) { data[i] = s[2 * i] / 32768; data[frames + i] = s[2 * i + 1] / 32768; }
      const ad = new AudioData({ format: 'f32-planar', sampleRate: AAC_RATE, numberOfFrames: frames, numberOfChannels: 2, timestamp: Math.round(aOut * 1e6 / AAC_RATE), data });
      aOut += frames;
      aenc.encode(ad);
      ad.close();
    };
    return {
      // atUs: the recording's time of this frame (the buttons then); timeUs: its time in the video
      frame(rgba, index, pic, timeUs, atUs) {
        if (error) throw error;
        let inNew = false;
        if (inputsOn) {
          const st = more.conv.inputs.at(atUs === undefined ? index * usPerFrame : atUs);
          if (!I.same(st, inState)) {
            inState = { ...st };
            inNew = true;
            inPatches = I.patches(inLayout, inState, view.w, view.h, scale).map((p) => {
              const c = new OffscreenCanvas(p.w, p.h);
              c.getContext('2d').putImageData(new ImageData(p.data, p.w, p.h), 0, 0);
              return { c, x: p.x, y: p.y };
            });
          }
        }
        if (pic !== picture || !base || inNew) { // a new picture; repeats share its memory
          if (base) base.close();
          if (big) {
            small.getContext('2d').putImageData(new ImageData(rgba, 400, hdr.height), 0, 0);
            for (const [sx, sy, sw, sh, dx, dy] of blits) bigCtx.drawImage(small, sx, sy, sw, sh, dx * scale, dy * scale, sw * scale, sh * scale);
            for (const p of inPatches) bigCtx.drawImage(p.c, p.x, p.y);
            base = new VideoFrame(big, { timestamp: 0 });
          } else {
            base = new VideoFrame(rgba, { format: 'RGBA', codedWidth: 400, codedHeight: hdr.height, timestamp: 0 });
          }
          picture = pic;
        }
        // timeUs: the picture's capture time (R3D.Converter vfr); without it the frame sits on the fps grid
        const f = timeUs === undefined
          ? new VideoFrame(base, { timestamp: Math.round(index * usPerFrame), duration: Math.round(usPerFrame) })
          : new VideoFrame(base, { timestamp: Math.round(timeUs), duration: Math.round(usPerFrame) });
        venc.encode(f, { keyFrame: allKeys || index % (hdr.fps * 2) === 0 });
        f.close();
      },
      audio(s) {
        if (!rs) return; // no audio track
        const r = rs.run(s);
        pend.push(r); pendN += r.length;
        if (pendN >= 2048 * 2) {
          const all = new Int16Array(pendN); let o = 0;
          for (const p of pend) { all.set(p, o); o += p.length; }
          pend = []; pendN = 0;
          audioBlock(all);
        }
      },
      async wait() { // keeps the encoders' queues short
        while (!error && (venc.encodeQueueSize > 24 || (aenc && aenc.encodeQueueSize > 24))) await sleep(4);
        if (error) throw error;
      },
      async finish() {
        if (aenc && pendN) {
          const all = new Int16Array(pendN); let o = 0;
          for (const p of pend) { all.set(p, o); o += p.length; }
          audioBlock(all);
        }
        if (base) base.close();
        await venc.flush();
        if (aenc) await aenc.flush();
        if (error) throw error;
        muxer.finalize();
        if (!partsOk) throw new Error('MP4 written out of order');
        return new Blob(parts, { type: 'video/mp4' });
      },
    };
  }

  /* ------------------------------------------------------------ live: MediaRecorder */

  function liveType() {
    if (typeof MediaRecorder === 'undefined' || !HTMLCanvasElement.prototype.captureStream) return null;
    const types = ['video/mp4;codecs=avc1.42E01E,mp4a.40.2', 'video/mp4;codecs=avc1,mp4a.40.2', 'video/mp4;codecs=avc1,opus',
      'video/mp4', 'video/webm;codecs=vp9,opus', 'video/webm;codecs=vp8,opus', 'video/webm'];
    const want = new URLSearchParams(location.search).get('type'); // tests: ?type=webm
    return types.filter((m) => !want || m.indexOf(want) >= 0)
      .find((m) => { try { return MediaRecorder.isTypeSupported(m); } catch (e) { return false; } }) || null;
  }

  // Plays the recording onto a canvas in real time and records the canvas and the sound. While the
  // download is behind (or the page is hidden) the recording pauses and so does the media clock.
  // The AudioContext itself keeps running (no suspend()), sound goes out in short pieces a little
  // ahead; a pause stops them and the rest is played again after it.
  function liveConvert(hdr, mime, onTime, ctx, conv, inputs) {
    const I = typeof R3DInputs !== 'undefined' ? R3DInputs : null;
    const inputsOn = !!(inputs && I && conv && conv.inputs);
    const inLayout = inputsOn ? I.layout(I.screensFor(hdr, 'auto')) : null;
    let inState = null;
    const canvas = $('canvas');
    canvas.height = hdr.height;
    const g = canvas.getContext('2d');
    const dest = ctx.createMediaStreamDestination();
    // A constant silent source: with nothing playing (silence is not stored, and nothing plays
    // while paused) Chrome's MP4 recorder stops taking sound for good (headless Chrome
    // 2026-11-05: 2-26 s of sound in a 36 s video). With it, pauses are fine (12 in a row).
    const keep = ctx.createConstantSource ? ctx.createConstantSource() : ctx.createOscillator();
    if (keep.offset) keep.offset.value = 0;
    const mute = ctx.createGain();
    mute.gain.value = keep.offset ? 1 : 0;
    keep.connect(mute);
    mute.connect(dest);
    keep.start();
    const stream = new MediaStream([...canvas.captureStream(hdr.fps).getVideoTracks(), ...dest.stream.getAudioTracks()]);
    const rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: 1500000, audioBitsPerSecond: 128000 });
    const parts = [];
    rec.ondataavailable = (e) => { if (e.data && e.data.size) parts.push(e.data); };
    const rate = hdr.audioRate / 1000;
    const PIECE = 4096;       // samples per scheduled piece (~0.125 s)
    const LOOKAHEAD = 0.4;    // seconds of sound scheduled ahead
    const pics = [];          // { index, img } distinct pictures, in order
    let lastIndex = -1, picture = -1;
    let audioIn = 0;          // samples received
    const queue = [];         // { pos, data } not scheduled yet, in order
    let playing = [];         // { src, at, pos, data } scheduled
    let t0 = 0, pausedTotal = 0, pausedAt = 0, started = false, paused = true, ended = false, done = false;
    const media = () => (!started ? 0 : (paused ? pausedAt : ctx.currentTime) - t0 - pausedTotal);
    const ctxTime = (m) => t0 + pausedTotal + m;
    const aheadVideo = () => (lastIndex + 1) / hdr.fps - media();
    // sound is known up to what arrived, and up to ~0.5 s before the last picture: in the file it
    // lags its video by < 0.3 s, and where nothing arrived the recording was silent (not stored)
    const aheadAudio = () => (hdr.audio ? Math.max(audioIn / rate, (lastIndex + 1) / hdr.fps - 0.5) - media() : 99);
    const scheduleAhead = () => {
      const until = media() + LOOKAHEAD;
      while (queue.length && queue[0].pos / rate < until) {
        const q = queue.shift();
        const n = q.data.length / 2;
        const buf = ctx.createBuffer(2, n, rate);
        const l = buf.getChannelData(0), r = buf.getChannelData(1);
        for (let i = 0; i < n; i++) { l[i] = q.data[2 * i] / 32768; r[i] = q.data[2 * i + 1] / 32768; }
        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(dest);
        const at = ctxTime(q.pos / rate), now = ctx.currentTime;
        if (at >= now) src.start(at);
        else src.start(now, Math.min(now - at, buf.duration)); // late: keep it in step with the picture
        playing.push({ src, at, pos: q.pos, data: q.data });
      }
      playing = playing.filter((p) => p.at + p.data.length / 2 / rate > ctx.currentTime - 1);
    };
    const pause = () => {
      if (paused || !started) return; // before the start nothing runs yet
      paused = true;
      pausedAt = ctx.currentTime;
      rec.pause();
      const back = [];
      for (const p of playing) { // what is not heard yet goes back to the queue
        const end = p.at + p.data.length / 2 / rate;
        if (end <= pausedAt) continue;
        try { p.src.stop(); } catch (e) { /* already stopped */ }
        const played = p.at < pausedAt ? Math.floor((pausedAt - p.at) * rate) : 0;
        back.push({ pos: p.pos + played, data: p.data.subarray(played * 2) });
      }
      playing = [];
      queue.unshift(...back);
    };
    const resume = () => {
      if (!paused || document.hidden) return;
      if (!started) {
        started = true;
        t0 = ctx.currentTime + 0.15;
        pausedTotal = 0;
        rec.start(1000);
      } else {
        pausedTotal += ctx.currentTime - pausedAt;
        rec.resume();
      }
      paused = false;
      scheduleAhead();
    };
    let finish = null;
    const tick = () => {
      if (done) return;
      const ok = ended || (aheadVideo() > 0.25 && aheadAudio() > 0.25);
      if (!paused && !ok) pause();
      if (paused && !document.hidden && (ended || (aheadVideo() > 0.8 && aheadAudio() > 0.6))) resume();
      if (!paused) {
        scheduleAhead();
        const idx = Math.floor(media() * hdr.fps);
        let k = -1;
        for (let i = 0; i < pics.length && pics[i].index <= idx; i++) k = i;
        if (k >= 0) { g.putImageData(pics[k].img, 0, 0); pics.splice(0, k); } // keep the shown one
        if (ended && idx > lastIndex + 2 && !queue.length) finish();
      }
      onTime(media(), paused);
      requestAnimationFrame(tick);
    };
    document.addEventListener('visibilitychange', () => { if (document.hidden) pause(); });
    const result = new Promise((resolve, reject) => {
      finish = () => {
        if (done) return;
        done = true;
        rec.onstop = () => { ctx.close(); resolve(new Blob(parts, { type: mime.split(';')[0] })); };
        rec.onerror = (e) => reject(e.error || new Error('MediaRecorder'));
        setTimeout(() => rec.stop(), 200); // the last sound piece reaches the recorder
      };
    });
    requestAnimationFrame(tick);
    let pend = [], pendN = 0;
    const pushAudio = (force) => {
      while (pendN >= PIECE * 2 || (force && pendN)) {
        const n = Math.min(pendN, PIECE * 2);
        const piece = new Int16Array(n);
        let o = 0;
        while (o < n) {
          const h = pend[0], k = Math.min(h.length, n - o);
          piece.set(h.subarray(0, k), o);
          o += k;
          if (k === h.length) pend.shift(); else pend[0] = h.subarray(k);
        }
        pendN -= n;
        queue.push({ pos: audioIn - (pendN + n) / 2, data: piece });
      }
    };
    return {
      frame(rgba, index, pic) {
        let inNew = false;
        if (inputsOn) {
          const st = conv.inputs.at((index * 1e6) / hdr.fps);
          if (!I.same(st, inState)) { inState = { ...st }; inNew = true; }
        }
        if (pic !== picture || inNew) {
          const copy = new Uint8ClampedArray(rgba);
          if (inputsOn) I.drawRgba(copy, 400, hdr.height, inLayout, inState);
          pics.push({ index, img: new ImageData(copy, 400, hdr.height) });
          picture = pic;
        }
        lastIndex = index;
      },
      audio(s) {
        pend.push(s);
        pendN += s.length;
        audioIn += s.length / 2;
        pushAudio(false);
      },
      hungry() { // decoded pictures are large: keep about a second ahead of what is shown
        return !done && (aheadVideo() < 1.2 || (aheadAudio() < 1.2 && aheadVideo() < 8));
      },
      async finish() {
        pushAudio(true);
        ended = true;
        return result;
      },
    };
  }

  /* ------------------------------------------------------------ flow */

  // mode: { fast, label, make(hdr, onTime, conv), inputs (draw the buttons), trim ({ from, to } s, fast only) }
  async function run(mode) {
    const url = '/rec/' + encodeURIComponent(name);
    let enc = null, hdr = null, firstErr = null;
    // a part (trim): the frames before it are decoded but not made, the video starts at its start; the
    // reading stops after its end. The sound by its samples (the decoder places them from time 0).
    const cut = mode.fast && mode.trim ? { from: mode.trim.from * 1e6, to: mode.trim.to * 1e6 } : null;
    let firstIndex = -1, aPos = 0;
    const conv = new R3D.Converter({
      // the encoder at once: the first piece's frames and sound (~1 s) were decoded before it existed and lost
      onHeader: (h) => { hdr = h; ensureEnc(); },
      onFrame: (rgba, i, pic, timeUs) => {
        if (!enc) return;
        const at = timeUs === undefined ? (i * 1e6) / hdr.fps : timeUs;
        if (cut) {
          if (at < cut.from) return;
          if (at >= cut.to) { conv.done = true; return; }
          if (firstIndex < 0) firstIndex = i;
        }
        enc.frame(rgba, cut ? i - firstIndex : i, pic, cut && timeUs !== undefined ? timeUs - cut.from : timeUs, at);
        if (mode.fast && i % 15 === 0) { // live mode reports its own clock
          const sec = (at - (cut ? cut.from : 0)) / 1e6;
          step(2, 'on', totalSec ? (sec / totalSec) * 100 : 0, clock(sec));
        }
      },
      onAudio: (s) => {
        if (!enc) return;
        if (cut) {
          const hz = hdr.audioRate / 1000, n = s.length / 2;
          const a = Math.max(0, Math.round((cut.from / 1e6) * hz) - aPos), b = Math.min(n, Math.round((cut.to / 1e6) * hz) - aPos);
          aPos += n;
          if (b <= a) return;
          s = s.subarray(a * 2, b * 2);
        }
        enc.audio(s);
      },
    }, { vfr: !!mode.fast, eq: !LOCAL || opts.eq, inputs: !!mode.inputs }); // the fast way stamps frames itself; the live one draws on its own clock
    const totalSec = cut ? (cut.to - cut.from) / 1e6 : info.ms / 1000;
    step(1, 'on', 0, '0%');
    step(2, 'on', 0, '', mode.label);
    const onTime = (sec, paused) => {
      step(2, 'on', totalSec ? (sec / totalSec) * 100 : 0, clock(sec), paused ? (document.hidden ? t('hidden') : t('paused')) : mode.label);
    };
    let dlDone = false;
    const ensureEnc = () => {
      if (enc || !hdr) return;
      if (mode.inputs && !hdr.inputs) show('long', t('noInputs'));
      enc = mode.make(hdr, onTime, conv);
    };
    const stash = []; // live: downloaded, not decoded yet
    const feed = mode.fast
      ? async (piece) => {
        try {
          conv.push(piece);
          ensureEnc();
          if (enc) await enc.wait();
        } catch (e) { firstErr = firstErr || e; }
      }
      : async (piece) => { // a fast link fills it faster than real time: keep it bounded
        while (!firstErr && stash.reduce((n, p) => n + p.length, 0) > (16 << 20)) await sleep(100);
        stash.push(piece);
      };
    const pump = mode.fast ? Promise.resolve() : (async () => {
      for (;;) {
        if (firstErr) return;
        if (stash.length && (!enc || enc.hungry())) {
          try {
            conv.push(stash.shift());
            ensureEnc();
          } catch (e) { firstErr = firstErr || e; }
          continue;
        }
        if (dlDone && !stash.length) return;
        await sleep(20);
      }
    })();
    const progress = (g, size, rate) => {
      const left = rate ? (size - g) / rate : 0;
      step(1, g >= size ? 'done' : 'on', (g / size) * 100, Math.floor((g / size) * 100) + '%',
        `${mb(g)} / ${mb(size)} · ${(rate / 1048576).toFixed(2)} ${t('mbs')}` + (g < size ? ' · ' + t('left', duration(left)) : ''));
    };
    const dl = LOCAL ? new FileSource(localFile, feed, progress, () => conv.done) : new Download(url, info.size, feed, progress);
    const go = async () => {
      try {
        await dl.run();
      } catch (e) {
        report('r3d download failed: ' + why(e));
        if (LOCAL) fail(t('readErr', why(e)));
        else fail(e.notFound ? t('notFound') : t('lost'), e.notFound ? null : go);
        return;
      }
      report(`r3d downloaded: ${dl.got} B in ${((performance.now() - dl.t0) / 1000).toFixed(1)} s`);
      dlDone = true;
      step(1, 'done', 100, t('ready'));
      try {
        await pump;
        if (firstErr) throw firstErr;
        conv.finish();
        if (!enc) throw new Error('no video');
        const blob = await enc.finish();
        // Discord: still too big (the encoder kept over its rate): once more, the rate cut by as much
        if (mode.fit && blob.size > mode.fit.bytes && !mode.fit.again) {
          report(`discord: ${blob.size} B over ${mode.fit.bytes}, again at a lower rate`);
          mode.fit.again = true;
          mode.fit.bitrate = Math.max(DISCORD_MIN, Math.floor(mode.fit.bitrate * (mode.fit.bytes / blob.size) * 0.9));
          run(mode);
          return;
        }
        step(2, 'done', 100, t('ready'), mode.label);
        lastFast = !!mode.fast;
        offer(blob);
      } catch (e) {
        report('browser video failed: ' + why(e));
        fail(t(LOCAL ? 'failedLocal' : 'failed', e && e.message ? e.message : e));
      }
    };
    go();
  }

  function offer(blob) {
    step(3, 'on');
    const ext = blob.type.indexOf('webm') >= 0 ? 'webm' : 'mp4';
    const href = URL.createObjectURL(blob);
    const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/.exec(name) || [];
    const fname = m[0] ? `3DS ${m[1]}-${m[2]}-${m[3]} ${m[4]}-${m[5]}-${m[6]}.${ext}` : `${name.replace(/\.[^.]*$/, '')}.${ext}`;
    $('preview').src = href;
    const save = $('save');
    save.href = href;
    save.download = fname;
    save.textContent = t('save') + ' · ' + mb(blob.size);
    $('howto').textContent = (isIOS ? t('howIos') : isAndroid ? t('howAndroid') : t('howOther')) + (ext === 'webm' ? ' ' + t('webm') : '');
    $('keep').hidden = true;
    $('result').hidden = false;
    document.querySelectorAll('#result .share').forEach((b) => b.remove()); // from the video made before (a part)
    $('trim').hidden = !(LOCAL && lastFast);
    if (LOCAL && lastFast) trimSetup();
    report(`offer ${fname}: ${blob.type} ${blob.size} B, share ${!!navigator.canShare}`);
    // Web Share with files exists only on https pages; offer it where it does
    const file = typeof File !== 'undefined' ? new File([blob], fname, { type: blob.type }) : null;
    if (file && navigator.canShare && navigator.canShare({ files: [file] })) {
      const b = document.createElement('button');
      b.className = 'secondary share';
      b.textContent = t('share');
      b.onclick = () => navigator.share({ files: [file] }).then(() => report('shared'), (e) => report('share: ' + why(e)));
      save.after(b);
    }
    save.addEventListener('click', () => { report('save tapped'); step(3, 'done'); });
    if (LOCAL) $('again').hidden = false;
  }

  async function main() {
    if (LOCAL) { localMain(); return; }
    document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
    report(`open ${name} | ${ua} | screen ${screen.width}x${screen.height}@${devicePixelRatio} | ${navigator.language}` +
      ` | mem ${navigator.deviceMemory || '?'} GB | secure ${window.isSecureContext} | VideoEncoder ${typeof VideoEncoder}` +
      ` | MediaRecorder ${typeof MediaRecorder}`);
    try {
      const r = await fetch('/api/rec', { cache: 'no-store' });
      info = await r.json();
    } catch (e) { fail(t('lost'), main); return; }
    if (!name) name = info.name;
    if (!name) { fail(t('noFile')); return; }
    report(`rec ${info.name} ${info.size} B ${info.ms} ms ok ${info.ok} closed ${info.closed} | prep ${info.prep} ${info.prepPct} mp4 ${info.mp4}`);
    setLang(info.lang); // the console's language
    document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
    if (info.name !== name || !info.ok) { fail(t('notFound')); return; }
    $('console').textContent = info.console || '';
    $('when').textContent = when(name);
    $('fname').textContent = name;
    $('len').textContent = clock(info.ms / 1000);
    const raw = $('raw');
    raw.href = '/rec/' + encodeURIComponent(name);
    raw.download = name;
    raw.textContent = t('raw');
    $('sd').textContent = t('sd');
    $('diag').addEventListener('click', () => report('diagnostics file saved'));
    step(3, '');
    if (info.prep && info.prep !== 'failed') consoleVideo();
    else browserVideo();
  }

  /* ------------------------------------------------------------ local mode: a file on the computer */

  function localMain() {
    setLang(browserLang());
    document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
    for (const id of ['card', 'steps', 'raw', 'sd', 'diag']) $(id).hidden = true;
    $('s1').querySelector('b').textContent = t('read');
    $('pick').hidden = false;
    localOptions();
    const drop = $('drop'), input = $('file');
    input.addEventListener('change', () => { if (input.files[0]) openLocal(input.files[0]); });
    drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
    drop.addEventListener('dragleave', () => drop.classList.remove('over'));
    drop.addEventListener('drop', (e) => {
      e.preventDefault();
      drop.classList.remove('over');
      if (e.dataTransfer.files[0]) openLocal(e.dataTransfer.files[0]);
    });
  }

  // The two segmented choices; remembered per browser (storage may be off: then the defaults)
  function localOptions() {
    try { Object.assign(opts, JSON.parse(localStorage.getItem('r3d-opts') || '{}')); } catch (e) { /* defaults */ }
    if (![1, 2, 3].includes(opts.scale)) opts.scale = 1;
    const seg = (id, get, set) => {
      const btns = [...$(id).querySelectorAll('button')];
      const mark = () => btns.forEach((b) => b.classList.toggle('on', b.dataset.v === String(get())));
      btns.forEach((b) => b.addEventListener('click', () => {
        set(b.dataset.v);
        mark();
        try { localStorage.setItem('r3d-opts', JSON.stringify(opts)); } catch (e) { /* not kept */ }
      }));
      mark();
    };
    seg('scale', () => opts.scale, (v) => { opts.scale = Number(v); });
    seg('eq', () => (opts.eq ? 1 : 0), (v) => { opts.eq = v === '1'; });
    if (!['auto', 'top', 'bottom', 'side'].includes(opts.layout)) opts.layout = 'auto';
    if (!(opts.quality in QUALITY)) opts.quality = 'normal';
    seg('layout', () => opts.layout, (v) => { opts.layout = v; });
    seg('quality', () => opts.quality, (v) => { opts.quality = v; });
    opts.inputs = opts.inputs === true;
    seg('inputs', () => (opts.inputs ? 1 : 0), (v) => { opts.inputs = v === '1'; });
  }

  // Cutting a part of the video made from a file: play it, mark the start and the end, make it again
  // from the recording (the times are the recording's: the video on the page may be a part already)
  function trimSetup() {
    const v = $('preview'), base = trim ? trim.from : 0;
    let a = trim ? trim.from : 0, b = trim ? trim.to : 0;
    const show = () => {
      const end = b || base + (v.duration || 0);
      $('trimRange').textContent = clock(a) + ' – ' + clock(end);
      $('trimGo').disabled = !(end > a + 0.2) || (trim && a === trim.from && end === trim.to);
    };
    v.onloadedmetadata = show;
    $('trimStart').onclick = () => { a = base + v.currentTime; if (b && b <= a) b = 0; show(); };
    $('trimEnd').onclick = () => { const e = base + v.currentTime; if (e > a + 0.2) b = e; show(); };
    $('trimGo').onclick = () => {
      trim = { from: a, to: b || base + v.duration };
      report(`trim ${trim.from.toFixed(2)}-${trim.to.toFixed(2)} s`);
      $('result').hidden = true;
      $('again').hidden = true;
      $('long').hidden = true;
      URL.revokeObjectURL(v.src);
      browserVideo();
    };
    show();
  }

  async function openLocal(file) {
    $('error').hidden = true;
    let hdr = null;
    try { // the header (32 bytes) through the decoder itself: it knows every version
      const head = new Uint8Array(await file.slice(0, 32).arrayBuffer());
      if (head.length >= 4 && new DataView(head.buffer).getUint32(0, true) === 0x52443352) { fail(t('rpl')); return; } // R3DR
      new R3D.Converter({ onHeader: (h) => { hdr = h; } }).push(head);
    } catch (e) { /* not a .r3d */ }
    if (!hdr) { fail(t('notR3d')); return; }
    localFile = file;
    name = file.name;
    info = { name, size: file.size, ms: 0, both: hdr.both, bottomOnly: hdr.bottomOnly, fps: hdr.fps, audio: hdr.audio, inputs: hdr.inputs };
    trim = null;
    $('pick').hidden = true;
    $('card').hidden = false;
    $('steps').hidden = false;
    $('when').textContent = when(name);
    $('fname').textContent = name + ' · ' + mb(file.size);
    // the length from the end record (or the last record's time): a progress bar, and the Discord size
    try {
      info.ms = Math.round((await R3D.probeLength(file.size, async (o, n) => new Uint8Array(await file.slice(o, o + n).arrayBuffer()))) / 1000);
    } catch (e) { info.ms = 0; }
    $('stats').hidden = true;
    if (info.ms) $('fname').textContent += ' · ' + clock(info.ms / 1000);
    $('b2').parentNode.hidden = !info.ms; // without the length, step 2 shows the video's time without a bar
    browserVideo();
  }

  /* ------------------------------------------------------------ the console's MP4 */

  async function consoleVideo() {
    $('s1').querySelector('b').textContent = t('c1');
    $('s2').querySelector('b').textContent = t('c2');
    $('error').hidden = true;
    let st = info, fails = 0, seen = null, left = 0;
    const since = performance.now();
    step(2, '', 0, t('waiting'), '');
    while (st.prep !== 'ready') {
      if (st.prep === 'failed' || (st.prep === 'none' && performance.now() - since > 20000)) {
        $('s1').querySelector('b').textContent = t('s1');
        $('s2').querySelector('b').textContent = t('s2');
        report(`console MP4 ${st.prep}: the browser makes the video`);
        browserVideo(); // the console could not make it: the browser does
        return;
      }
      const pct = (st.prepPct || 0) / 10, now = performance.now();
      if (!seen && pct > 0) seen = { pct, at: now };
      else if (seen && pct > seen.pct + 1) left = ((100 - pct) * (now - seen.at)) / (pct - seen.pct) / 1000;
      step(1, 'on', pct, Math.floor(pct) + '%',
        (st.prep === 'working' ? t('c1x') : t('c1q')) + (left ? ' · ' + t('left', duration(left)) : ''));
      await sleep(1000);
      try {
        st = await (await fetch('/api/rec', { cache: 'no-store' })).json();
        fails = 0;
      } catch (e) {
        if (++fails >= 5) { fail(t('lost'), consoleVideo); return; }
      }
    }
    step(1, 'done', 100, t('ready'), '');
    report(`console MP4 ready: ${st.mp4} B after ${Math.round((performance.now() - since) / 1000)} s on the page`);
    $('size').textContent = mb(st.mp4);
    $('eta').textContent = duration(st.mp4 / (WIFI_KBS * 1024));
    const stem = name.replace(/\.r3d$/, '');
    const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/.exec(name) || [];
    const direct = $('direct');
    direct.href = '/rec/' + encodeURIComponent(stem + '.mp4');
    direct.download = `3DS ${m[1]}-${m[2]}-${m[3]} ${m[4]}-${m[5]}-${m[6]}.mp4`;
    if (st.mp4 > BIG_MP4) { // too big to hold in a phone's memory: the browser's own download
      direct.textContent = t('direct') + ' · ' + mb(st.mp4);
      direct.hidden = false;
      show('long', t('big', mb(st.mp4)));
      step(2, 'on', 0, '', '');
      direct.addEventListener('click', () => { report('big MP4: direct download tapped'); step(2, 'done', 100, t('ready')); });
      report('big MP4: direct link offered');
      return;
    }
    const parts = [];
    const dl = new Download('/rec/' + encodeURIComponent(stem + '.mp4'), st.mp4, async (piece) => { parts.push(piece); },
      (g, size, rate) => {
        const rest = rate ? (size - g) / rate : 0;
        step(2, g >= size ? 'done' : 'on', (g / size) * 100, Math.floor((g / size) * 100) + '%',
          `${mb(g)} / ${mb(size)} · ${(rate / 1048576).toFixed(2)} ${t('mbs')}` + (g < size ? ' · ' + t('left', duration(rest)) : ''));
      });
    try {
      await dl.run();
    } catch (e) {
      report('MP4 download failed: ' + why(e));
      fail(e.notFound ? t('notFound') : t('lost'), e.notFound ? null : consoleVideo);
      return;
    }
    const secs = (performance.now() - dl.t0) / 1000;
    report(`MP4 downloaded: ${dl.got} B in ${secs.toFixed(1)} s (${Math.round(dl.got / 1024 / secs)} KB/s)`);
    step(2, 'done', 100, t('ready'));
    offer(new Blob(parts, { type: 'video/mp4' }));
    direct.addEventListener('click', () => report('direct link tapped after the page download'));
    direct.className = 'btn link'; // if saving from the page does not work on some phone
    direct.textContent = t('directLink');
    direct.hidden = false;
  }

  /* ------------------------------------------------------------ the browser makes it */

  async function browserVideo() {
    if (!LOCAL) {
      $('size').textContent = mb(info.size);
      const etaSec = info.size / (WIFI_KBS * 1024);
      $('eta').textContent = duration(etaSec);
      if (etaSec > 10 * 60) show('long', t('long', duration(etaSec)));
    }
    step(1, 'on', 0, t('waiting'));
    step(2, '', 0, t('waiting'));

    const height = info.both ? 480 : 240;
    const canDraw = typeof OffscreenCanvas !== 'undefined'; // upscales and layouts are drawn on one
    let scale = LOCAL && canDraw ? opts.scale : 1;
    const layout = LOCAL && canDraw ? opts.layout : 'auto';
    let quality = LOCAL ? opts.quality : 'normal';
    // the buttons: the page's choice on a computer; on the phone, the console's Buttons In Video setting
    const inputs = LOCAL ? opts.inputs : info.keys === true; // the console's Buttons In Video, for a recording that kept them
    const view = viewFor({ both: info.both, bottomOnly: info.bottomOnly, height }, layout);
    const fps = info.fps || 30;
    const seconds = trim ? trim.to - trim.from : info.ms / 1000;
    // Discord: under ~3 Mbit/s a bigger picture does not fit (the encoder stays over its rate): 1x then
    if (quality === 'discord' && seconds && discordRate(seconds) < 3000000) scale = 1;
    if (quality === 'discord' && seconds && discordRate(seconds) < DISCORD_MIN) show('long', t('discordLong'));
    let codec = await fastSupport(view.w * scale, view.h * scale, fps, info.audio, bitrateFor(view, scale, quality, seconds));
    if (!codec && quality !== 'normal' && quality !== 'discord') { quality = 'normal'; codec = await fastSupport(view.w * scale, view.h * scale, fps, info.audio, bitrateFor(view, scale, quality)); }
    if (!codec && scale > 1) { scale = 1; codec = await fastSupport(view.w, view.h, fps, info.audio, bitrateFor(view, 1, quality, seconds)); } // too big for this encoder
    if (codec) {
      const bitrate = bitrateFor(view, scale, quality, seconds);
      report(`browser video: fast, ${JSON.stringify(codec).slice(0, 120)}, ${Math.round(bitrate / 1000)} kbit/s` +
        (inputs ? ', buttons' : '') + (trim ? `, part ${trim.from.toFixed(1)}-${trim.to.toFixed(1)} s` : ''));
      const size = view.w * scale !== 400 || view.h * scale !== height ? ` · ${view.w * scale}×${view.h * scale}` : '';
      const purpose = { high: 'qHigh', edit: 'qEdit', discord: 'qDiscord' }[quality];
      const fit = quality === 'discord' ? { bytes: DISCORD_BYTES, bitrate, again: false } : null;
      run({ fast: true, inputs, trim, fit,
        label: t(LOCAL ? 'fastPc' : 'fast') + size + (purpose ? ' · ' + t(purpose) : '') + (trim ? ' · ' + t('trimLabel', clock(trim.from), clock(trim.to)) : ''),
        make: (hdr, onTime, conv) => fastConvert(hdr, codec, scale, layout, quality, { conv, inputs, bitrate: fit ? fit.bitrate : bitrate }) });
      return;
    }
    const mime = liveType();
    report('browser video: ' + (mime ? 'live, ' + mime : 'none possible'));
    if (!mime) {
      step(2, '', 0, '', t('none'));
      return; // the original .r3d link stays
    }
    show('keep', t('keep'));
    const b = $('start');
    b.textContent = t('start');
    b.hidden = false;
    $('s2x').textContent = t('startHint');
    b.onclick = () => {
      b.hidden = true;
      // created and woken inside the tap: browsers let audio start only from a user gesture
      const ctx = new (window.AudioContext || window.webkitAudioContext)();
      ctx.resume();
      run({ label: t('live'), inputs, make: (hdr, onTime, conv) => liveConvert(hdr, mime, onTime, ctx, conv, inputs) });
    };
  }

  main();
})();
