# Glossary: French (fr)

Primary sources (Nintendo of Europe, France):
- [3DS-FR] Nintendo 3DS mode d'emploi: https://www.nintendo.com/eu/media/downloads/support_1/nintendo_3ds_14/Nintendo3DS_OperationsManual_FR.pdf
- [3DSXL-FR] Nintendo 3DS XL mode d'emploi: https://www.nintendo.com/eu/media/downloads/support_1/nintendo_3ds_14/Nintendo3DSXL_OperationsManual_FR.pdf
- [N2DSXL-FR] New Nintendo 2DS XL mode d'emploi (New-generation system software): https://www.nintendo.com/eu/media/downloads/support_1/new_nintendo_3ds/NewNintendo2DSXL_OperationsManual_FRA.pdf
- [CA-FR] Nintendo of America, Canadian French support (example): https://fr-americas-support.nintendo.com/app/answers/detail/a_id/24208/
- [HG-FR] 3DS hacks guide, French translation: https://3ds.hacks.guide/fr_FR/ (source: github.com/hacks-guide/Guide_3DS, `docs/fr_FR`)

France's wording is preferred; Canadian French differences are noted in the table.

## Conventions

- **Address: vous.** The console's own messages use vous imperatives, e.g. the on-screen error quoted in the manuals: « Une erreur est survenue. Maintenez le bouton POWER enfoncé pour éteindre la console, puis suivez les instructions… » [3DS-FR p. "En cas de problème"], [N2DSXL-FR]. The manual text and Nintendo support pages also use vous. The French homebrew guide uses vous as well [HG-FR].
- **Instructions:** imperative, vous form ("Touchez", "Appuyez sur", "Sélectionnez", "Maintenez"). EU manuals say "touchez" for the touch screen. Canadian French support says "appuyez sur" for on-screen buttons ("appuyez sur Ouvrir") [CA-FR].
- **Buttons and menu items:** infinitive verbs or nouns: DÉMARRER, QUITTER, REPRENDRE, CALIBRER, ÉTEINDRE (POWER menu), RÉINITIALISER LES PARAMÈTRES, plus fixed forms such as OK, OUI / NON, SUITE, PLUS TARD and the first-person J'ACCEPTE [3DS-FR], [N2DSXL-FR]. Use infinitives for actions ("Enregistrer", "Télécharger", "Désinstaller") and nouns for screens.
- **Capitalization:** sentence case on screen. The printed manuals set labels in CAPITALS only as a typographic convention. The on-screen labels are sentence case: "Paramètres de la console", "Gestion des données", "Autres paramètres" [HG-FR], "Options des codes QR/images", "Sauvegarder le Mii sous forme de code QR" [CA-FR]. "menu HOME" has a lowercase *menu* and an uppercase HOME everywhere [3DS-FR].
- **Quotes:** « guillemets » with a space inside, e.g. « Aucune carte détectée dans le port carte de jeu. » [3DS-FR], [3DSXL-FR]. In the app, use U+00A0 inside « ».
- **Punctuation spacing:** a space before `: ; ! ?`. The manuals show "apparaît : « Une erreur… »" and "…insérée dans le port carte de jeu ?" [3DS-FR]. In the app it is U+00A0 (style rule 7). Use the ellipsis `…`.
- **Numbers and units:** a space between a number and its unit ("16 kbps", "2,4 GHz"), with a decimal comma [3DS-FR specs]. In the app: `30 s`, `%u Mo`. Note: French UI text says "Mo"/"Go" for MB/GB, but style rule 4 keeps `MB`/`GB`. Keep them unless the team decides otherwise.
- **Gender:** *la console*. The homebrew community also uses the feminine: *une Old 3DS*, *une New 3DS* [HG-FR].

## Terms

| # | concept | term | source | note |
|---|---|---|---|---|
| 1 | the console / the system | la console (Nintendo 3DS) | [3DS-FR] "la console Nintendo 3DS", "éteindre la console" | Canadian French support also says "le système de la famille Nintendo 3DS" [CA-FR]. Prefer *console*. |
| 2 | Old 3DS / New 3DS | Old 3DS / New 3DS (official: Nintendo 3DS / New Nintendo 3DS) | community: [HG-FR] troubleshooting-soundhax, region-changing ("une Old 3DS / Old 3DS XL / Old 2DS", "une New 3DS") | Feminine. The product name in France stays "New Nintendo 3DS" [N2DSXL-FR "New Nintendo 2DS XL"]. |
| 3 | HOME Menu | menu HOME | [3DS-FR] "Le menu HOME s'affiche lorsque vous allumez…" | Lowercase *menu*. |
| 4 | Sleep Mode | mode veille | [3DS-FR] "fermez la console pour la mettre en mode veille" | "mettre en mode veille", "quitter le mode veille". |
| 5 | close the system / the lid | fermer la console | [3DS-FR] "Fermer la console", "si vous fermez la console…" | Nintendo does not speak of a lid ("rabat"). It closes the console. Open: "ouvrir la console". |
| 6 | System Settings | Paramètres de la console | [3DS-FR] "Modifier les paramètres de la console", [HG-FR] "Lancez les Paramètres de la console" | Capital P as an app name, lowercase in running text in the manual ("dans les paramètres de la console"). |
| 7 | Data Management | Gestion des données | [3DS-FR] "GESTION DES DONNÉES", [HG-FR] `Gestion des données` -> `Nintendo 3DS` -> `Logiciels` | |
| 8 | SD Card | carte SD | [3DS-FR] "carte SD", [N2DSXL-FR] "carte microSD" | |
| 9 | Touch Screen | écran tactile | [3DS-FR] "L'écran inférieur, généralement appelé écran tactile…" | |
| 10 | upper / lower screen | écran supérieur / écran inférieur | [3DS-FR] "Écran supérieur", "Écran inférieur" | The French homebrew guide also says "écran du bas" (informal). Use the official form. |
| 11 | notification LED | témoin de notification | [3DS-FR] "Témoin de notification : clignote et change de couleur…" | Other lights: témoin d'alimentation, témoin de charge, témoin de communication sans fil. Verb: "clignote en bleu / vert / orange". |
| 12 | POWER Button / turn off / restart | bouton POWER / éteindre / redémarrer | [3DS-FR] "bouton POWER", "menu POWER… ÉTEINDRE", "Quittez et redémarrez le logiciel"; [HG-FR] "redémarrer votre console" | "mettre la console hors tension" is the formal form. Use "éteindre". |
| 13 | software / game / application | logiciel (jeu) | [3DS-FR] "Icônes des logiciels", "démarrer un logiciel", "Quitter un logiciel"; [HG-FR] Data Management lists `Logiciels` | Nintendo FR calls a title *logiciel* (masc.). "jeu" is fine where only games are meant. |
| 14 | applet | application du menu HOME | [3DS-FR] "Icônes des applications du menu HOME" (Notes de jeu, Liste d'amis, Notifications…) | Examples: Liste d'amis, appareil photo Nintendo 3DS, Éditeur Mii. |
| 15 | Wi-Fi / internet connection | connexion Internet / Wi-Fi | [3DS-FR] "connexion Internet", "communication sans fil" | Capital I in *Internet* (Nintendo FR style). |
| 16 | download (to a phone) | télécharger / Télécharger | [3DS-FR] "logiciels téléchargés", "télécharger" | Project wording rule: "Download" not "To phone". |
| 17 | save (a file) / saved | sauvegarder / sauvegardé(e) | [3DS-FR] "données sauvegardées sur la carte SD"; [CA-FR] "Sauvegarder le Mii…" | Nintendo also uses "enregistrer" for files ("enregistrés sur la carte SD"). In this app *enregistrer* means **record** (#18), so use *sauvegarder* for saving to avoid the clash. |
| 18 | recording (n.), record (v.) | enregistrement, enregistrer | [3DS-FR] "commencer l'enregistrement d'une vidéo", "les enregistrements" | A recording is *un enregistrement*. "en cours d'enregistrement" = recording now. |
| 19 | clip | clip (masc.) | UNCONFIRMED | No Nintendo 3DS term. "extrait" is an alternative. *clip* is common in French gaming. |
| 20 | frame rate / fps, frame | fréquence d'images (fps) / image | UNCONFIRMED | No 3DS source. "images par seconde" written out; `fps` stays (rule 4). |
| 21 | button combination / hotkey | combinaison de touches | community: [HG-FR] finalizing-setup ("la combinaison de touches du menu Rosalina") | Nintendo FR calls physical buttons *boutons* ("bouton L"). The community phrase is *combinaison de touches*. "combinaison de boutons" is the Nintendo-style alternative. |
| 22 | screenshot | capture d'écran | [3DS-FR] "prendre une capture d'écran du menu HOME", "captures" | |
| 23 | settings / options | Paramètres / Options | [3DS-FR] "Paramètres de la console", "Paramètres du menu HOME", "options de l'appareil photo" | Use *Paramètres* for the app's Settings screen. |
| 24 | Gallery | Galerie | UNCONFIRMED | No 3DS equivalent. The Camera app has "Voir les photos" / album. *Galerie* is the usual French UI word. |
| 25 | Help | Aide | UNCONFIRMED | Standard French UI word. The manual's troubleshooting section is "En cas de problème". |
| 26 | Uninstall | Désinstaller | community: [HG-FR] uninstall-cfw ("désinstaller le CFW") | Nintendo's own Data Management action is "Supprimer" (delete software). |
| 27 | sound / audio | son / audio | [3DS-FR] System Settings item "SON", "sortie audio des haut-parleurs" | *son* for the switch ("Son activé"), *audio* as adjective. |
| 28 | firmware, custom firmware, homebrew, sysmodule, plugin | firmware; custom firmware (CFW, masc.); homebrew (masc., invariable); sysmodule (UNCONFIRMED); plugin (masc., UNCONFIRMED) | community: [HG-FR] get-started, faq ("un custom firmware", "logiciels homebrew", "des homebrew", "la liste de homebrew") | The community keeps the English words untranslated. No French source for *sysmodule*: keep it, or say "module système" if it must be explained. *plugin* is the common loanword (Luma "plugin loader"). |
| 29 | Rosalina menu | menu Rosalina | community: [HG-FR] finalizing-setup ("pour ouvrir le menu Rosalina") | |
| 30 | QR code, scan | code QR / scanner | [CA-FR] "code QR"; [HG-FR] "scannez ce code QR"; [3DS-FR] (EU) "Lire un QR Code" | EU manual writes "QR Code" (trademark form) and "lire". Prefer *code QR* (Canada, community, natural French), with the verb *scanner*. |
| 31 | lossless / quality | sans perte / qualité | UNCONFIRMED | Usual technical French. |
| 32 | converting / conversion | conversion / convertir ("Conversion… %u %%") | UNCONFIRMED | No Nintendo term. Project wording: "Converting N%" → "Conversion %u %%" (no-break space before %). |

## App terms (as translated)

Final wording in `app/source/lang/fr.c` and the phone page (`build/i18n/web-fr.js`). Address: *vous*
everywhere. Labels are sentence case; in running text a feature that is also a common noun goes lowercase
with its article (*l’enregistreur*, *la relecture*, *le lancement au démarrage*). A switch named as a
control in a sentence is quoted: *désactivez « Enregistreur » sur l’écran principal*, *activez « Relecture 30 s »*.
Typography: U+00A0 before `: ; ? %` and inside « », and between a number and its unit (`30 s`, `4 GB`,
`≈ %u min`); decimal comma (`1,5 s`); apostrophe ’ (U+2019).

| English | French | note |
|---|---|---|
| Recorder (the background module, its tile and settings group) | **Enregistreur** (*l’enregistreur*, masc.) | states: *Enregistreur actif / désactivé*, *en marche* |
| Replay (30 s) | **Relecture 30 s**; feature *la relecture*; gallery tag *Relecture* | as NVIDIA's French "Relecture instantanée" |
| Launch On Startup | **Lancement au démarrage** (*le lancement au démarrage*) | |
| Convert To MP4 · Automatically · On Download | **Conversion en MP4** · **Automatique** · **Au téléchargement** | |
| Work With Lid Closed | **Fonctionner console fermée** | the lid = *fermer la console* (term 5) |
| Share Statistics | **Partager les statistiques** | |
| Gallery | **Galerie** | |
| Download (tile, hint) | **Télécharger** | phone steps: *Téléchargement depuis la console / sur le téléphone* |
| Settings (the app's screen) | **Paramètres** | console's: *Paramètres de la console* |
| Settings groups: Recording / Recorder / App / Language | **Enregistrement / Enregistreur / Application / Langue** | |
| Help · If something is wrong | **Aide** · **En cas de problème** | the manual's heading |
| How to use (guide) | **Mode d’emploi** | |
| the panel (main screen) | **l’écran principal** | |
| this app | **cette application** | applets: *applications du menu HOME* (Navigateur Internet, Notes de jeu, Liste d’amis) |
| recording (n.) / record (v.) | **enregistrement / enregistrer** | |
| save (a file, a clip, a setting) | **sauvegarder / sauvegardé** | never *enregistrer* (clash with record); hotkey screen button *Valider*; a setting kept: *pris en compte* |
| clip | **clip** (masc.) | |
| hotkey(s) | **combinaison** (row: **Combinaisons**) | *la combinaison d’enregistrement*, *celle de la relecture* |
| LED | **témoin** (row: **Témoin**) | Pulse / Solid / Off: **Clignotant / Fixe / Éteint**; breathes red: *clignote doucement en rouge*; blinks green twice: *clignote deux fois en vert*; glows blue: *s’allume en bleu*; faint blue: *bleu pâle* |
| frame rate / FPS | **FPS** (label), *fps* / *images par seconde* in text | Bottom Screen FPS: **FPS écran inférieur**; As top: **Comme en haut** |
| frames / skipped | **images / sautées** | |
| convert / converting / could not convert | **convertir / Conversion %d % / Échec de la conversion** | |
| Ready for download | **Prêt à télécharger** | |
| Joining recordings | **Fusion des enregistrements** | join = *fusionner* |
| Saving clip | **Sauvegarde du clip** | |
| Cut off (power or a crash) | **Interrompu (coupure ou plantage)** | |
| Lossless / Auto | **Sans perte / Auto** | |
| Games that need extra memory | **jeux gourmands en mémoire** | |
| logs / Log for the developer | **journaux / Journal pour le développeur** | |
| delete / uninstall | **supprimer / désinstaller** | Nintendo's *supprimer* |
| computer / phone | **ordinateur / téléphone** | |
| Camera (in HOME) | **l’appareil photo** | |
| Friend Code | **code ami** | |
| Luma option names | kept in English, in « » | |
| MB / GB | kept as **MB / GB** (style rule 4) | French would say Mo / Go |


Note (2026-11-19): size units are translated — Mo / Go / Mo/s (keys S_MB, S_GB, S_MBS), also inside sentences.
