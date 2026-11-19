# Glossary: Spanish (es), neutral international

One Spanish for Spain and Latin America. Nintendo shipped the 3DS in Spanish twice: Nintendo of Europe
(Spain, "EU") and Nintendo of America (Latin America, "NOA"). For almost every console term the two
agree word for word; where they differ, this file picks the wording both sides read as normal.

Sources used below:

- **EU-3DS**: Nintendo 3DS operations manual, Spanish (NoE):
  https://www.nintendo.com/eu/media/downloads/support_1/nintendo_3ds_14/Nintendo3DS_OperationsManual_ES.pdf
- **EU-N3DS**: New Nintendo 3DS / New Nintendo 3DS XL operations manual, ES/PT/IT (NoE):
  https://www.nintendo.com/eu/media/downloads/support_1/new_nintendo_3ds/NewNintendo3DS_NewNintendo3DSXL_OperationsManual_EUB.pdf
- **NOA-3DSXL**: Nintendo 3DS XL operations manual, Latin American Spanish (NoA):
  https://csassets.nintendo.com/noaext/image/private/t_KA_PDF/manual-nintendo-3dsxl-operations-spanish
- **NOA-support**: Nintendo of America support in Spanish, "Cómo cambiar el idioma del sistema en la familia Nintendo 3DS":
  https://es-americas-support.nintendo.com/app/answers/detail/a_id/23264/
- **guide**: 3ds.hacks.guide in Spanish (Crowdin translation of the standard CFW guide): https://3ds.hacks.guide/es_ES/
  (source: https://github.com/hacks-guide/Guide_3DS/tree/master/docs/es_ES)

## Conventions

- **Address: tú.** Both regions talk to the player as *tú*, never *usted* or *ustedes*. EU-3DS: "Si cierras
  la consola Nintendo 3DS durante una partida…", "Mantén pulsado el botón POWER". NOA-3DSXL: "Lee
  cuidadosamente este manual…", "Si te comunicas por medio de StreetPass…". NOA-support: "selecciona el
  ícono…", "Toca Aceptar para confirmar". (*Usted* appears in EU-3DS only in legal boilerplate.)
- **Instructions are imperatives in the tú form:** *Toca*, *Selecciona*, *Mantén…*.
  **On-screen buttons are infinitives or nouns:** *Abrir*, *Borrar*, *Reanudar*, *Aceptar* (EU-3DS,
  NOA-3DSXL, NOA-support). So a button says *Convertir* and its help sentence says *Toca Convertir…*.
- **Pressing a button:** Spain says *pulsa / mantén pulsado* (EU-3DS); NoA says *oprime / mantén oprimido*
  (NOA-3DSXL). Neither reads as neutral, because *oprimir* is rare in Spain and *pulsar* sounds Spanish
  from Spain in Latin America. **Use *presiona / mantén presionado*.** It is native in both, and the
  Spanish homebrew guide uses it (guide: "Presiona (L) + (Abajo) + (Select) a la vez para abrir el menú de
  Rosalina"; 152 uses against 5 of *pulsa*). Touch: *toca* (all sources).
- **Confirm button:** EU-3DS *OK*, NoA *Aceptar*. **Use *Aceptar*.** Everyone understands it, and it is
  what Latin American consoles show.
- **Capitalization: sentence case** for menu and button labels: *Configuración de la consola*,
  *Gestión de datos*, *Lista de amigos*, *Otras opciones* (EU-3DS, NOA-3DSXL). In running text Nintendo
  writes *el menú HOME* with a lowercase *menú* (EU-3DS 38 against 8 capitalized at sentence start;
  NOA-3DSXL 55 against 10). Keep English all-caps names as they are: *HOME*, *POWER*, *START*, *SELECT*.
- **Quotation marks: “ ”** (curly double quotes). Both manuals use them (EU-3DS: “Cómo ajustar las
  imágenes 3D”; NOA-3DSXL: “consolas de la familia Nintendo 3DS”). Neither uses « ». Inside “ ” use ‘ ’.
- **Punctuation:** opening *¿* and *¡* (NOA-3DSXL: “¡Estas son algunas de las emocionantes funciones…!”).
  No space before *: ; ! ?*. Ellipsis *…* (U+2026; EU-3DS uses it: "el indicador parpadeará en verde…").
  Put a space between a number and its unit (*30 s*, *2 GB*). Spain writes a decimal comma, Latin America
  mostly a decimal point. Avoid decimals in UI text where possible; otherwise use the comma (RAE).
- **Gender:** *la consola*, so *la 3DS*, *la New 3DS* (guide: "en la 3DS").
- **Other words that differ by region** and the neutral pick for this app:
  *vídeo* (ES) / *video* (LatAm): use **video**, which the RAE accepts and most Spanish speakers write
  (NOA-3DSXL: video ×22; EU-3DS: vídeo ×36).
  *móvil* (ES) / *celular* (LatAm): use **teléfono**.
  *ordenador* (ES) / *computadora* (LatAm): use **PC**.
  Use *archivo*, never *fichero*.

## Terms

| # | concept | term | source | note |
|---|---|---|---|---|
| 1 | the console / the system | **la consola** (*la consola Nintendo 3DS*) | EU-3DS (consola ×513), NOA-3DSXL (consola ×394) | NoA uses *sistema* only in phrases like *actualización del sistema*. The device is always *consola*. Write *la consola*, not *el sistema*. |
| 2 | Old 3DS / New 3DS | **Old 3DS / New 3DS** (in text: *la Old 3DS*, *la New 3DS*) | community: guide (es_ES, "Old 3DS" ×25, "New 3DS" ×22) | Nintendo names the models *Nintendo 3DS* and *New Nintendo 3DS* (EU-N3DS). The community keeps the English *Old/New*. |
| 3 | HOME Menu | **menú HOME** (*Menú HOME* at the start of a sentence or label) | EU-3DS, NOA-3DSXL | Same in both regions. |
| 4 | Sleep Mode | **modo de espera** | EU-3DS ("Modo de espera y apagado"), EU-N3DS ("en modo de espera (es decir, cerrada, pero no apagada)"), NOA-3DSXL (×20) | Same in both regions. Not *modo suspensión* or *reposo*. |
| 5 | close the system / the lid | **cerrar la consola** (*Si cierras la consola…*) | EU-3DS ("Si cierras la consola Nintendo 3DS durante una partida, se activará el modo de espera"), NOA-3DSXL ("cerrar la consola") | Nintendo says "close the console", not "the lid". If *tapa* is needed: *con la consola cerrada*. |
| 6 | System Settings | **Configuración de la consola** | EU-3DS (×32), NOA-3DSXL (×31), NOA-support | Same in both regions. Not *Configuración del sistema*. |
| 7 | Data Management | **Gestión de datos** | EU-3DS p. 80, NOA-3DSXL p. 216 | Same in both regions. Path: *Configuración de la consola > Gestión de datos*. |
| 8 | SD Card | **tarjeta SD** | EU-3DS (×78), NOA-3DSXL (×70) | New 3DS: *tarjeta microSD*. Plain *tarjeta SD* covers both. |
| 9 | Touch Screen | **pantalla táctil** | EU-3DS (×37), NOA-3DSXL (×15) | |
| 10 | upper / lower screen | **pantalla superior / pantalla inferior** | EU-3DS, NOA-3DSXL ("Pantalla 3D (pantalla superior)") | Both manuals also call the lower one *pantalla táctil*. |
| 11 | notification LED | **indicador de notificación** | EU-3DS ("Indicador de notificación… parpadeará en naranja"), NOA-3DSXL ("El indicador de notificación se pondrá de color verde…") | Same in both regions. Short form: *el indicador*. Not *LED* or *luz*. |
| 12 | POWER Button / turn off / restart | **botón POWER / apagar / reiniciar** | EU-3DS ("Para apagar la consola, mantén pulsado el botón POWER"), NOA-3DSXL ("toca Apagar"); *reiniciar*: community: guide | The manuals never say "restart"; *reiniciar* is the normal word (guide ×8). Menu *apagado* = power menu. |
| 13 | software / game / application | **programa** (title on HOME Menu); **juego** when it is a game | EU-3DS (programa ×247), NOA-3DSXL (×292): "Cómo cerrar o suspender un programa", "Uso de programas" | Nintendo says *programa* for any title. Use *juego* only when it is really a game. *software* appears only in compounds. |
| 14 | applet | **aplicación** (*aplicación del menú HOME*) | EU-3DS ("aplicación Cámara de Nintendo 3DS"), NOA-3DSXL | UNCONFIRMED as a name for the class: Nintendo has no word for "applet". It calls Camera etc. *aplicación*, and Friend List etc. *funciones del menú HOME*. Suggestion: *aplicación del menú HOME*. |
| 15 | Wi-Fi / internet connection | **Wi-Fi / conexión a internet** | EU-3DS (×20), NOA-3DSXL (×22); both write *internet* in lowercase | *Configuración de internet* is the console's own settings item (NOA-3DSXL p. 208). |
| 16 | download (to a phone) | **Descargar** (button) / *descargar en el teléfono* | EU-3DS, NOA-3DSXL ("descargar programas"); *teléfono*: see Conventions | Both regions use *descargar*. For the phone, write *teléfono*, not *móvil* or *celular*. |
| 17 | save (a file) / saved | **guardar / guardado** | EU-3DS ("Si cierras un programa sin haber guardado tu progreso…"), NOA-3DSXL ("Toca Aceptar para guardar los ajustes") | Same in both regions. |
| 18 | recording (n.) / record (v.) | **grabación / grabar** | EU-3DS ("comenzar a grabar un vídeo", "grabar imágenes y sonidos") | *grabación* is the standard noun. Not *registro* or *captura*. |
| 19 | clip | **clip** (pl. *clips*) | UNCONFIRMED: Nintendo has none for 3DS | Suggestion: *clip*, the usual word in video apps and on social media in both regions. Fuller form: *fragmento*. |
| 20 | frame rate / fps, frame | **fps** in UI; frame = **fotograma** | UNCONFIRMED: not in the 3DS manuals | Spain *fotograma*, Latin America *cuadro*, gamers everywhere *FPS / frames*. Prefer *fps* (kept per style rule 4). Where a noun is unavoidable, *fotograma* is understood in both (film word). |
| 21 | button combination / hotkey | **combinación de botones** | EU-3DS ("usa las siguientes combinaciones de botones"), community: guide (×19, "la combinación de botones del menú de Rosalina") | Not *atajo* or *tecla rápida*. |
| 22 | screenshot (Rosalina/Luma) | **captura de pantalla** (short: *captura*) | EU-N3DS ("harás una captura de la pantalla superior… de las dos pantallas"), community: guide ("hacer capturas de pantalla") | |
| 23 | settings / options | **Ajustes** (the app's own settings screen); *opciones* for items in it | EU-3DS, NOA-3DSXL ("guardar los ajustes"); NOA-support ("Otros ajustes") | The console's app is *Configuración de la consola*, and its page is *Otras opciones* (both manuals). *Ajustes* keeps the app's screen distinct from the console's while still matching Nintendo's own words. |
| 24 | Gallery | **Galería** | UNCONFIRMED: no 3DS equivalent | Standard Spanish UI word. |
| 25 | Help | **Ayuda** | UNCONFIRMED in 3DS manuals | Standard. |
| 26 | Uninstall | **Desinstalar** | community: guide ("Desinstalar CFW", ×9) | Nintendo's Data Management uses *Borrar* to remove a title. *Desinstalar* is right for removing the recorder's module. |
| 27 | sound / audio | **sonido**; **audio** for the recorded track | EU-3DS (sonido ×44, audio ×4), NOA-3DSXL (sonido ×25, audio ×11) | *Grabar sonido* is fine. *audio* for a stream or format. |
| 28 | firmware / custom firmware / homebrew / module / plugin | **firmware**; **custom firmware (CFW)**; **homebrew** (*aplicaciones homebrew*, *software homebrew*); **módulo del sistema** (sysmodule); **plugin** | community: guide ("instalar firmware personalizado (CFW)", "custom firmware" ×57, "aplicaciones homebrew", "software homebrew"); sysmodule: UNCONFIRMED | The community keeps *custom firmware* (occasionally *firmware personalizado*) and *homebrew*. No Spanish source found for "sysmodule". Suggestion: *módulo del sistema*, or *sysmodule* in technical text. *plugin* is unchanged (RAE also has *plugin*). |
| 29 | Rosalina menu | **menú de Rosalina** | community: guide ("Presiona (B) para salir del menú de Rosalina", ×5) | The guide leaves Rosalina's own items in English ("Miscellaneous options"), since Rosalina has no Spanish UI. Quote them in English. |
| 30 | QR code, scan | **código QR / escanear** | EU-3DS, NOA-3DSXL ("Cómo leer QR Code"); *código QR / escanear*: common usage | Nintendo writes the trademark form *QR Code* and the verb *leer*. The app shows a code for the phone's camera, so *Escanea el código QR con el teléfono* is natural in both regions. |
| 31 | lossless / quality | **sin pérdida(s) / calidad** | *calidad*: EU-N3DS ("sin perder calidad de imagen"); *sin pérdida*: UNCONFIRMED (standard codec term) | |
| 32 | converting / conversion | **convirtiendo / conversión**; verb **convertir** | UNCONFIRMED: not in Nintendo docs; standard Spanish | Status label: *Convirtiendo 45 %*. Feature: *Convertir a MP4*. |

### Feature names (style rule 5), suggested

Recorder: *Grabadora*. Replay (30 s): *Repetición (30 s)*. Launch On Startup: *Iniciar al encender*.
Convert To MP4: *Convertir a MP4*. Work With Lid Closed: *Funcionar con la consola cerrada*.
Share Statistics: *Compartir estadísticas*. Gallery: *Galería*. Download: *Descargar*.
Settings: *Ajustes*. Help: *Ayuda*. "Ready for download": *Listo para descargar*.

## App terms (as translated)

Final wording in `app/source/lang/es.c` and the phone page. Labels (buttons, rows, paths after →) are
capitalized as on screen; in running text a feature that is also a plain noun phrase goes lowercase with
its article (*la grabadora*, *la repetición*, *el inicio automático*). A switch named as a control in a
sentence is quoted: *desactiva “Grabadora” en el panel*, *activa “Repetición 30 s”*.

| English | Spanish | note |
|---|---|---|
| Recorder (the background module, its tile and settings group) | **Grabadora** (*la grabadora*) | states: *Grabadora activa / desactivada*, *en marcha* |
| Replay (30 s) | **Repetición 30 s**; feature *la repetición*; gallery tag *Repetición* | |
| Launch On Startup | **Inicio automático** (*el inicio automático*) | noun form, works in every sentence |
| Convert To MP4 · Automatically · On Download | **Convertir a MP4** · **Automático** · **Al descargar** | |
| Work With Lid Closed | **Funcionar con la consola cerrada** | the lid = *cerrar la consola* (term 5) |
| Share Statistics | **Compartir estadísticas** | |
| Gallery | **Galería** | |
| Download (tile, hint) | **Descargar** | step: *Descarga al teléfono* |
| Settings (the app's screen) | **Ajustes** | console's: *Configuración de la consola* |
| Settings groups: Recording / Recorder / App / Language | **Grabación / Grabadora / Aplicación / Idioma** | |
| Help | **Ayuda** | |
| How to use (guide) | **Cómo se usa** | |
| this app | **esta aplicación** | applets: *aplicaciones del menú HOME* |
| recording (n.) / record (v.) | **grabación / grabar** | |
| clip | **clip** | |
| hotkey(s) | **combinación** (row: **Combinaciones**) | *la combinación para grabar*, *la de la repetición* |
| LED | **indicador** (row: **Indicador**) | Pulse / Solid / Off: **Parpadeo / Fijo / Apagado**; breathes red: *parpadea lentamente en rojo*; blinks green twice: *parpadea dos veces en verde*; glows blue: *se enciende en azul*; faint blue: *azul tenue* |
| frame rate / FPS | **FPS** (label), *fps* in text | Bottom Screen FPS: **FPS de pantalla inferior**, Same: **Igual** |
| frames / skipped | **fotogramas / omitidos** | |
| convert / converting / not converted | **convertir / Convirtiendo %d % / Sin convertir** | space before % (RAE) |
| Ready for download | **Listo para descargar** | |
| Joining recordings | **Uniendo grabaciones** | join = *unir* |
| Saving clip | **Guardando clip** | |
| Cut off (power or a crash) | **Interrumpida (apagado o fallo)** | |
| Lossless / Auto | **Sin pérdidas / Auto** | |
| the panel (main screen) | **el panel** | |
| logs / Log for the developer | **registros / Registro para el desarrollador** | |
| delete / uninstall | **borrar / desinstalar** | Nintendo's *borrar* |
| computer | **tu PC** | the possessive avoids *el PC* / *la PC* |
| phone | **teléfono** | |
| Wi-Fi off | **Sin Wi-Fi**; turn on: *activa la conexión inalámbrica* | avoids *el/la Wi-Fi* |
| Luma option names | kept in English, in “ ” | |
