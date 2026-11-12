# Mehrspieler direkt im Originalmenü – Build 197

`Start-Multiplayer.cmd` oder `Multiplayer/SSNE-Native-Multiplayer.exe` startet jetzt unmittelbar das Spiel. Es gibt keinen vorgeschalteten Launcher und keinen zweiten Spielprozess. Im Hauptmenü führt **Mehrspieler** zu **Spiel erstellen**, **Spiel beitreten**, **Lokal spielen** und **Zurück**.

Die Seiten verwenden die native GUI des PAL-Spiels: dieselben Menüvorlagen, Schriften, Farben, Hintergrundanimationen, Auswahlgeräusche und Zurück-Funktion. Die Sprache folgt `SystemSetup.Language`, also der im Spiel gewählten Sprache Englisch, Deutsch oder Französisch. Die originalen Seiten für Spielmodus, Regeln, Charaktere und Karten bleiben erhalten.

Als Host zuerst **Spiel erstellen** wählen. Vor dem Start lassen sich die sechsstellige PIN und der Port ändern. Anschließend erscheint die originale Auswahl für Koop, Deathmatch, Behalte die Flagge und Gib die Bombe ab. Danach folgen die originalen Regeln und die Spielerlobby. **Lokal spielen** öffnet die ursprüngliche lokale Mehrspielerauswahl. Mit Escape beziehungsweise GameCube B geht es durch deren Seiten zum Hauptmenü zurück. **Mehrspieler** öffnet anschließend wieder die Auswahl **Spiel erstellen / Spiel beitreten / Lokal spielen**.

Bei **Spiel beitreten** entweder eine Host-Adresse und dessen sechsstellige PIN eingeben oder **LAN-Spiele suchen** wählen. Eine gefundene aktuelle LAN-Sitzung auswählen, um direkt beizutreten; dabei ist keine PIN-Eingabe nötig. Dieselbe Sitzung erscheint auch bei mehreren erreichbaren Netzwerkschnittstellen nur einmal. Auf demselben PC wird die lokale Adresse bevorzugt. Direkte Adressen können auch `Adresse:Port` enthalten. Die Suche verwendet UDP 27342, die Spielsitzung standardmäßig TCP 27341. Ist ein LAN-Suchergebnis abgelaufen, erneut suchen. Die Suche ersetzt keine Internetvermittlung oder Router-Portfreigabe.

Textfelder lassen sich mit Tastatur, Einfügen aus der Zwischenablage und Enter/Escape bearbeiten. Mit einem Controller wechseln Links/Rechts die Position und Oben/Unten das Zeichen; X löscht und Y fügt ein Zeichen an. A bestätigt und B verwirft die Bearbeitung.

In der Lobby öffnet die GameCube-Taste **Y** die native Bot-Verwaltung; **X** zeigt die Verbindung. Mit der PC-Standardbelegung sind das **F** beziehungsweise **Q**. Bei geänderter Belegung gelten die im F12-Menü zugewiesenen Tasten für X/Y. Im originalen Pausenmenü stehen zusätzlich **Verbindung** und beim Host **Bots**. Dort kann man die Sitzung verlassen und im selben Spielprozess zum Hauptmenü zurückkehren. Ein Host kann anschließend eine neue Sitzung erstellen. Die Schwierigkeit gilt für neu hinzugefügte Bots. Details und Grenzen der KI stehen in GAME-BOTS.md.

Unter **Verbindung** lässt sich der AFK-Modus für den eigenen Spieler starten oder beenden. Die Verbindungsseite bleibt auch auf dem Ergebnisbildschirm mit **X** erreichbar, bei PC-Standardbelegung mit **Q**. Ein Host kann dort den AFK-Modus beenden und anschließend die originale Weiter-Funktion selbst bestätigen. Nach etwa 15 Sekunden auf einem aktiven Ergebnisbildschirm betätigt der Host automatisch diese native Weiter-Funktion. Auch eine offen gelassene Verbindungsseite wird vor dem automatischen Wechsel geschlossen. Clients folgen dem autoritativen Levelwechsel.

Während einer Client-Sitzung ist die lokale Speicherkarte gegen durchgereichte Host-Speicherungen isoliert. Beim Verlassen werden die lokalen Profile und Einstellungen wiederhergestellt. Die bisherigen F12-Werkzeuge, das Insert-Modmenü und die erweiterten Startparameter bleiben verfügbar.

## Prüfung

Die automatischen Menütests verwenden verborgene Fenster, private Kartenkopien und Eingaben ausschließlich in den Testprozessen. Sie prüfen die normalen nativen Menüwege. Sprachtests wählen die originale Sprach-Laderoutine über einen nur im verborgenen Testbetrieb wirksamen Parameter. Die Anleitung behauptet weder einen Internettest noch einen erneuten vollständigen Kampagnendurchlauf.

Der zusätzliche Sitzungszustand speichert die nativen Menübindungen zusammen mit den Koop-Checkpoints. Speicherstände unterschiedlicher ausführbarer Builds werden wie bisher anhand der Build-Identität getrennt.

Build 190 behebt einen nach lokalen Menüs stehen gebliebenen lokalen Modus
und eine Schleife beim Zurückkehren aus der Mehrspielerauswahl.
`tools/test_native_local_menu.py` prüft sofortiges Abbrechen, die vier lokalen
Spielmodi, erneutes Öffnen der Mehrspielerauswahl und anschließendes Erstellen
einer Online-Sitzung in Deutsch, Englisch und Französisch. Der Regressionstest
auf Build 189 reproduziert die übersprungene Mehrspielerauswahl.
Nachweise: `Multiplayer/Pruefung/Build190`.

## Host-Eingaben ab Build 191

Beim Wechsel vom Offline-Hauptmenü zum Netzwerk-Host ersetzt der Netzwerk-Code
den ursprünglichen PADClamp-Hook. Dessen Zwischenspeicher für rohe Stickwerte
wurde zuvor nicht mehr aktualisiert. Die weiterhin aktive Verarbeitung spielte
dadurch den letzten Menüwert anstelle der aktuellen Laufrichtung zurück.
Der Netzwerk-Clamp aktualisiert nun denselben Zwischenspeicher wie der
Offline-Clamp. Die ursprüngliche Skalierung und die vier Controller bleiben
unverändert; die zweite Bank für Koop-Plätze 5–8 wird weiterhin separat verarbeitet.

`tools/test_native_movement.py` startet über **Mehrspieler → Spiel erstellen**,
wählt Spielmodus, Figuren und Karte im Originalmenü und misst die tatsächliche
Positionsänderung bei Vorwärts, Rückwärts, Links und Rechts. Der Test verwendet
private Karten und prozesseigene Pad-Eingaben; er schreibt keine Spielerpositionen,
Munition oder Waffen. Erweiterte Prüfungen kontrollieren Bewegung nach Pause und
Verbindungsmenü, Stillstand ohne Eingabe und natives Schießen mit ausreichend
lang gedrücktem Abzug für eingesammelte Miniguns. Paarprüfungen steuern Host und
Client getrennt. Die früheren Menüprüfungen deckten diese Bewegung nicht ab.

Die Build-190-Gegenprobe zeigt bei allen vier Laufrichtungen null Strecke.
Prüfberichte und die genaue Fallauswahl stehen unter `Multiplayer/Pruefung/Build191`.
Diese Prüfungen betreffen den Sitzungsstart und die grundlegende Steuerung;
sie bestätigen keinen kompletten Kampagnendurchlauf.

## Host ohne erneutes Frontend-Laden – Build 192

Der Host übernimmt die bereits geladenen ursprünglichen Lobby-Ressourcen.
Die Netzwerk-Anpassung registriert deren Lebensdauer schon beim normalen Boot
und ergänzt die vorhandenen vier Charakterfelder erst bei Sitzungsbeginn.
Beim Start und beim Abbrechen einer Sitzung im Hauptmenü wird die Frontend-Szene
nicht erneut geladen. Nach Verlassen einer laufenden Welt bleibt der originale
Quit-/Rückkehrpfad erforderlich.

Escape beziehungsweise GameCube B wird in der Host-Spielerlobby als einzelner
Zurück-Schritt zur Regelseite verarbeitet. Die nächste Esc-Eingabe darf den
nächsten Schritt auslösen; eine gehaltene Taste wird nicht doppelt weitergereicht.
Die zusätzlichen vier Koop-Felder bleiben bis zum Abbau ihres Besitzers gültig.
Beim Erstellen einer weiteren Sitzung wird die Erkennung des Menü-Rückwegs
zurückgesetzt. Clients öffnen mit B in der Lobby ihre eigene Verbindungsseite.

`tools/test_native_host_flow.py` prüft mehrere Host-/Zurück-/Neustart-Zyklen,
identische native Menüobjekte und denselben Prozess, lokale Menüs dazwischen,
wiederholtes Öffnen/Schließen der Spielpause und anschließendes Laufen.
Nachweise und die genaue Fallauswahl: `Multiplayer/Pruefung/Build192`.

## Eingabepfad und gehaltenes Esc – Build 193

Die bisherigen verborgenen PAD-Tests riefen die Frontend-Eingabe stets auf.
Die normale Gerätebedienung tat das nur während einer Netzwerksitzung. Nach
Beenden des Hosts konnte dadurch der letzte B-/Esc-Wert im Frontend stehen
bleiben. Der gemeinsame Dispatcher versorgt die Frontend-Steuerung jetzt auch
offline und beim Loslassen beziehungsweise bei neutralen Eingaben. Geschlossene
lokale Spielseiten behalten die vier ursprünglichen Controller-Ports.

Zurück aus einer eigenen Menüseite führt jetzt durch deren Zustandsübergang.
Das bloße native Ausblenden sendet Ereignis 2, während die frühere Bindung auf
Ereignis 23 wartete; so konnten unsichtbare Seiten aktiv bleiben. Übergänge zurück
zur originalen Lobby oder Pause verbrauchen gehaltenes A/B/Start bis zur Freigabe.
Der Host darf erst nach Loslassen des Start-Klicks die Spielmodus-Auswahl bestätigen.
Eine bereits geladene Frontend-Szene wartet nicht mehr pauschal 30 Ticks auf den Start.

`tools/test_native_device_menu.py` benutzt private Tastatur-/Mauszustände durch
die normale Bindungsumrechnung und denselben Dispatcher wie echte Geräte.
Die Instrumentierung ist nur bei gleichzeitig gesetztem Hidden- und Device-Test-
Parameter aktiv. Sie beginnt mit intern gefangener Maus, bewegt/fokussiert kein
Desktop-Fenster und prüft nicht den ersten Klick zum Einfangen der Maus.
Geprüft werden langer Start-Klick, gehaltenes Esc über Sitzungsende, erneutes
Öffnen/Klicken, lokale Menüs, Lobby-/Verbindungsrückwege und Spielpause.
Die Gegenprobe vor der Korrektur reproduziert `returning=1` und `input=512`
nach dem Loslassen. Nachweise und genaue Fälle: `Multiplayer/Pruefung/Build193`.

## Breites Originalmenü und Beitreten – Build 194

Die folgende Beschreibung dokumentiert den damaligen Prüfstand. Der dortige
LAN-Beitritt mit PIN wurde in Build 196 durch den direkten Beitritt ersetzt;
die manuelle Adresse-/PIN-Verbindung bleibt verfügbar.

Das originale Frontend folgt jetzt dem gewählten Seitenverhältnis. Bei
**Automatisch** nutzt es das Seitenverhältnis des Fensters; ein fest gewähltes
4:3 bleibt 4:3. Die ursprünglichen Hintergrundgrafiken füllen die breitere
Menüfläche. Die Projektion der nativen 3D-Figuren und die Darstellung der
Schrift werden dabei getrennt korrigiert, damit ihre Proportionen erhalten
bleiben. Bei Menü-, Modal- und Szenenwechseln wird der Darstellungszustand
zurückgesetzt, damit vorherige Ansichten keine falsche Breite hinterlassen.
Vorhandene Originaltexte, Grafikdateien, Schriften und Menüvorlagen bleiben
erhalten. Die Sprache folgt weiterhin der Spielauswahl Englisch, Deutsch oder
Französisch; auch die neuen Fehlerhinweise sind in diesen Sprachen vorhanden.

Während ein Textfeld bearbeitet wird, lösen gehaltene Buchstaben keine
zusätzliche Menünavigation über die Laufbelegung aus. Nach Auswahl eines
LAN-Suchergebnisses öffnet sich direkt dessen PIN-Feld. Die sechsstellige
Host-PIN eingeben und mit **Enter** bestätigen; danach ist **Spiel beitreten**
ausgewählt und kann bestätigt werden. Eine ungültige Adresse, eine PIN mit
falscher Stellenzahl und ein Port außerhalb 1–65535 erhalten jeweils einen
eigenen Hinweis. Eine solche Eingabe wird vor dem Verbindungsversuch abgewiesen.

Der Client unterscheidet jetzt ausdrücklich zwischen laufendem Verbindungsaufbau,
verbundenem Zustand und beendetem Versuch. Ablehnungen durch den Host bleiben
nicht bis zum allgemeinen Zeitlimit als laufende Verbindung stehen. Beim
nichtblockierenden Winsock-Verbindungsaufbau überwacht `select` sowohl
Schreibbereitschaft als auch `exceptfds` und liest in beiden Fällen `SO_ERROR`.
So beendet auch eine aktiv verweigerte TCP-Verbindung den Versuch zeitnah.
Das Netzwerkprotokoll bleibt Version 62.

Für zwei Spielinstanzen auf **demselben PC**:

1. Im Host **Mehrspieler → Spiel erstellen** öffnen, die dort angezeigte
   **sechsstellige PIN** und den **Host-Port** merken und die Sitzung starten.
2. Im Client **Spiel beitreten** wählen. Als Adresse `127.0.0.1` verwenden;
   bei geändertem Host-Port `127.0.0.1:HOSTPORT` eintragen.
3. Genau die am Host angezeigte PIN eingeben und bestätigen. Eine selbst
   gewählte oder aus einer früheren Sitzung gemerkte PIN muss nicht passen.
4. Alternativ **LAN-Spiele suchen** wählen, die gefundene Sitzung auswählen,
   deren Host-PIN eingeben, **Enter** drücken und **Spiel beitreten** bestätigen.

`tools/test_native_join_device.py` prüft den direkten Beitritt und erneuten
Beitritt über die LAN-Suche mit zwei Instanzen auf demselben PC, Eingabefehler,
Host-Ablehnung sowie anschließend zwei bereite Spieler in derselben Welt und
die Bewegung beider Spieler. Private Gerätezustände durchlaufen die normale
Bindungsumrechnung; Text und Bearbeitungstasten gelangen über die native
Textwarteschlange ins Frontend. Das sind keine Betriebssystem-Eingabeereignisse;
die Tests fokussieren oder bedienen keine Desktop-Fenster.
`tools/test_native_widescreen.py` erfasst Ausgabegrößen und native Menüwechsel
für mehrere Seitenverhältnisse. Die CPU-Sitzungsprüfung deckt zusätzlich
Verbindungsaufbau, Zuweisung, Ablehnung, Abbruch und Verbindungsverweigerung ab.
Nachweise und die Ergebnisse der einzelnen Fälle: `Multiplayer/Pruefung/Build194`.
Dieser Umfang bestätigt weder einen vollständigen Koop-Kampagnendurchlauf noch
eine Verbindung zwischen verschiedenen Rechnern oder über das Internet.


## Levelauswahl, Bones, Laufanimation und Waffenton – Build 195

Die originale Levelauswahl behandelt ihre Filmstreifen nicht mehr als Teile
eines vergrößerten Sniper-Visiers. Vorschauen, Rahmen und Filmstreifen behalten
so dieselbe native Anordnung im breiten Menü. Die vorhandenen Grafikdateien,
Texte und Auswahlfunktionen werden weiterverwendet.

ESP-Skelett und Zielauswahl beziehen ihre Punkte aus der tatsächlichen
Modellpose und dem nativen Modell-Proxy mit dessen Wurzeltransformation. Die
Darstellung verwendet dieselbe Kamera-, Sichtfeld- und Breitbildprojektion wie
das gezeichnete Modell; die zusätzliche Faktor-2-Skalierung entfällt. Damit
werden Weltposition, Knochenposition und Bildschirmposition nicht durch
unterschiedliche Transformationswege gegeneinander verschoben.

Bei höheren Bildraten kann zwischen zwei Physikschritten ein Renderbild ohne
neue Spielerposition liegen, etwa bei 120-Hz-Physik und 240 FPS. Der originale
Körper-Animator deutete solche Zwischenbilder als Anhalten und setzte den
Abschluss der Laufanimation auf Geschwindigkeit 999. Beim nächsten
Bewegungsschritt startete der Lauf erneut. Die Korrektur rekonstruiert nur die
private Bewegungsdifferenz des Animators aus den tatsächlich ausgeführten
Physikschritten. Weltposition, Bewegungsgeschwindigkeit und Bildratenlimit
werden dabei nicht verändert. Der Host überträgt die korrigierte Körperpose
weiterhin an seine Clients; diese starten keine zweite lokale Laufanimation.

Der Vergleich verwendet gehaltene Laufbewegungen bei 60 und 240 FPS und
beobachtet jeweils den bewegten Host und den bewegten Client getrennt. In der
bereits ausgewerteten hohen-FPS-Gegenprobe sank die Zahl der falschen
999-Abschlussaufrufe während gleichmäßigen Laufens von 375 auf 0; die
Laufphasenrate lag nahe der 60-FPS-Referenz. Das ist ein Ergebnis dieser
Gegenprobe, keine pauschale Bestätigung aller Kampagnenanimationen.

Beim Waffenwechsel eines AFK-Bots endet die Uzi- beziehungsweise
Minigun-Dauerfeuerschleife des zugehörigen Waffenkanals. Besitzer-Generation,
Spielerplatz und Kanal bestimmen, welcher Ton beendet wird. Die Beendigung
wird auch an die Clients übertragen, damit dort kein alter Dauerton weiterläuft
und kein anderer Spieler seinen eigenen Waffenton verliert.

Die Build-195-Prüfung trennt vier Bereiche: originale Levelauswahl, Bones und
Zielpunkte, Laufanimation bei verschiedenen Bildraten sowie Dauerfeuerton beim
AFK-Waffenwechsel. Native GPU-Prüfungen verwenden private Spielkopien,
Speicherkarten und prozessinterne Eingaben; sie bedienen keine Desktop-Fenster.
Die einzelnen Berichte und Bildkontrollen unter
`Multiplayer/Pruefung/Build195` legen fest, welche Fälle bestanden sind. Dieser
Umfang bestätigt weder einen vollständigen Koop-Kampagnendurchlauf noch eine
Verbindung zwischen verschiedenen Rechnern oder über das Internet.


## LAN-Direktbeitritt, AFK-Reichweite und Ergebniswechsel – Build 196

Die LAN-Suche bündelt Antworten derselben Host-Sitzung über mehrere Adressen.
Getrennte Sitzungen und verschiedene Spielports bleiben getrennte Einträge.
Für den direkten Beitritt gibt der Host eine zufällig erzeugte, höchstens
60 Sekunden gültige Einmalberechtigung aus. Sie gilt nur für die IP-Adresse
des suchenden Clients und wird erst nach erfolgreicher Spielerzuweisung
verbraucht. Die Host-PIN wird nicht mit der Suchantwort übertragen. Die manuelle
Verbindung prüft weiterhin genau sechs PIN-Ziffern und passende Spieldaten.
Das Netzwerkprotokoll bleibt Version 62.

Der AFK-Mitspieler berücksichtigt jetzt die Reichweite seiner tatsächlich
vorhandenen Waffen mit verfügbarer Munition. Geeignete Fernwaffen können sichtbare Gegner
bis zu 100 Spielmeter entfernt erfassen; Nahkampfwaffen, Flammenwerfer und
andere kurze Waffen behalten ihre eigenen kleineren Entfernungen. Die native
Sichtprüfung und die Prüfung auf gefährdete Mitspieler bleiben Voraussetzung
für das Schießen. Der AFK-Mitspieler folgt weiterhin dem aktiven Spieler;
die größere Erfassungsreichweite ist keine unbegrenzte Verfolgungsdistanz.

Der Ergebniswechsel verwendet den vorhandenen nativen Weiter-Rückruf mit
dessen Statistikabschluss, Levelwechsel und Host-Speicherpfad. Der Host wartet
15 reale Sekunden pro angezeigtem Ergebnis, unabhängig von der Bildrate.
Die erste Weiter-Bestätigung kann wie im Original noch laufende Statistikwerte
abschließen; danach folgt der eigentliche Wechsel. Ein geöffnetes
Verbindungsmenü wird davor geschlossen, ohne die Wartezeit zu verlängern. Ein neuer
Ergebnisbildschirm oder ein wiederhergestellter Checkpoint beginnt eine neue
Wartezeit. Der Client führt keinen eigenen Kampagnen-Levelwechsel aus.

**Geprüfter Build 196:** Alle 25 CPU-Komponentenprüfungen und die
13 gezielten nativen Host-/Client-Fälle einschließlich der Gegenprobe sind
bestanden. Alle abschließenden Fälle verwenden dieselbe ausführbare Datei
und private Karten; die ursprüngliche Karte blieb unverändert.

- LAN-Direktbeitritt in Deutsch und Französisch: ein Eintrag trotz mehrerer
  Schnittstellen, direkter Beitritt auch mit absichtlich falscher gespeicherter
  PIN, manuelle PIN-Prüfung und anschließende Bewegung beider Spieler.
- Ergebniswechsel: Automatik ohne Menü, mit offenem Host- oder Client-Menü,
  manuelles Weiter trotz AFK und AFK ausschalten mit mehrfacher Menürückkehr.
  Beide Spieler erreichen das Folgelevel; der Host speichert genau einmal.
- AFK-Fernkampf: Minigun mit bestätigten nativen Treffern bei rund 64 bis 67
  Metern. Das Scharfschützengewehr zeigt freie Fernsicht, Schussauslösung und
  Munitionsverbrauch; sein späterer nativer Schaden wird mit der tatsächlichen
  Entfernung ausgewiesen, nicht als bestätigter Fern-Treffer bezeichnet.
  Ein reales Hindernis blockiert die native Sicht und verhindert Fernfeuer.
  Die Gegenprobe mit alter 30-Meter-Grenze löst kein Fernfeuer aus.
- Uzi-Wechsel für AFK-Host und AFK-Client: der alte Dauerschusskanal endet.

Berichte und Helfer liegen unter `Multiplayer/Pruefung/Build196`:
`lan-final`, `afk-range-final`, `uzi-final`, `results-final` und die
qualifizierte Gegenprobe. Die Ergebnisprüfung verwendet ein originales
WorldLink-Ereignis als Abkürzung der Erkundung. Die Fernkampfprüfung platziert
private Figuren und fordert originale Gegner- und Waffenfabriken an; Schaden
und Munition werden nicht direkt geschrieben. Die zusätzliche CPU-Wiedergabe
des Animations-Callbacks verwendet ausdrücklich Setter-/Destruktor-Stubs;
deren Quittung ist keine native Levelwechsel-Freigabe.

Die gemeldete Höhlentür in `Rlevel1_2` ließ sich nicht als Fehler
bestätigen: Beim erneuten Spielen kam der Benutzer weiter. Die zugehörigen
Build-195-Aufnahmen sind Diagnosematerial, keine Build-196-Freigabe; eine
Türkorrektur wurde daraus nicht abgeleitet. Der Prüfumfang bestätigt weiterhin
keinen vollständigen Koop-Kampagnendurchlauf, keinen Internetbetrieb und keinen
Test zwischen verschiedenen PCs.

Nach Installation von Build 196 startet `Start-Multiplayer-Build195.cmd` die
aufbewahrte Build-195-Version mit ihrer Kartenkopie. Aktuelle persönliche
Einstellungen und Speicherkarten werden beim Versionswechsel beibehalten.

## Mausbedienung der Originalmenüs – Build 197

Die nativen Menüs unterstützen jetzt absolute Mausbedienung. Überfahren
markiert den Eintrag; ein Linksklick bestätigt beim Loslassen über demselben
Eintrag. Rechtsklick führt die originale Zurück-Aktion aus, soweit die
jeweilige Seite sie anbietet. Die angezeigten nativen Bestätigen-/Zurück-
Hinweise sind ebenfalls anklickbar. Tastatur und Controller bleiben nutzbar.
In Zwischensequenzen dient ein Linksklick zum Überspringen; im Koop gilt
weiterhin die gemeinsame Abstimmung der Spieler.

Auswahllisten und Zahlenfelder lassen sich mit dem Mausrad ändern. Die
originalen Pfeile sind anklickbar; Lautstärke und Blickempfindlichkeit lassen
sich am Regler ziehen. Die Bildschirmtastatur verwendet ihre vorhandenen
Buchstaben und Funktionstasten. Netzwerk-Textfelder erlauben nach dem Anklicken
weiterhin die normale Texteingabe und Zwischenablage.
Bei **Lokal spielen** zuerst den originalen Hinweis **Um beizutreten**
anklicken und anschließend das eigene Profil wählen beziehungsweise bestätigen.
Dabei gilt der in den PC-Einstellungen gewählte Tastatur-/Maus-Spieler.

Die Levelauswahl verwendet ihre tatsächlich gezeichneten Vorschauen, Pfeile
und Kategorien. In Netricsa sind sichtbare Registerkarten und Seitenpfeile
anklickbar; über dem Text scrollt das Mausrad. Die Netzwerklobby bietet originale
Textaktionen für Start, Verbindung und Bots. Mit dem Mausrad über der eigenen
Figur oder einem Klick auf deren linke beziehungsweise rechte Hälfte lässt sich
die eigene Figur wechseln. Nur der
Host verändert gemeinsame Regeln und Bots; ein Client kann weder eine fremde
Figur umstellen noch den gemeinsamen Ergebniswechsel auslösen.

Im Pause- und Ergebnisbildschirm sind Verbindung und der eigene AFK-Schalter
mit der Maus erreichbar. Nach Schließen der Verbindung erhält der ursprüngliche
Ergebnisbildschirm seinen Eingabefokus zurück. Der automatische Levelwechsel
nach 15 Sekunden bleibt bestehen.

Trefferflächen folgen den aktuell gezeichneten Originalelementen einschließlich
Sprache, animierter Position, Bildschirmmodus, Seitenverhältnis und schwarzer
Fensterränder. Bei Fokusverlust oder einem Seitenwechsel wird ein begonnener
Klick oder Ziehvorgang verworfen. Das Spiel gibt den Mauszeiger für Menüs frei
und stellt die vorherige Maussteuerung im Spiel wieder her. Es wurden keine
neuen Menügrafiken oder übersetzten Beschriftungen erfunden.

Die Prüfwerkzeuge `tools/test_native_mouse.py` und
`tools/test_native_mouse_multiplayer.py` senden absolute Zeigerereignisse durch
denselben Eingabeadapter wie SDL. Sie verwenden verborgene native Fenster und
private Speicherkarten, keine Desktop-Klicks. Inventar und tatsächlich erreichte
Menüseiten werden getrennt ausgewiesen; eine aufgelistete GUI-Gruppe gilt nicht
allein deshalb als praktisch geprüft. Die separaten CPU-Prüfungen für
Darstellungsrechtecke, Menüelemente und Netricsa ersetzen keine native
Host-/Client-Prüfung. Abschließende Ergebnisse stehen unter
`Multiplayer/Pruefung/Build197`. Der separate
`tools/test_native_mouse_language.py` prüft die originale Sprachauswahl beim
Erststart mit einer neuen privaten Karte, ohne erzwungene Sprachwahl.

## Erstellen und Weitergeben

Die Multiplayer-Anpassungen liegen im mitgelieferten Runtime-Kit und in
`source-multiplayer`. Das gewöhnliche Einzelspieler-Erstellen durch
`SSNE-Native.exe` erzeugt weiterhin die ursprüngliche Einzelspieler-Version.
Für die Multiplayer-Version nach dem Einrichten der eigenen unterstützten
PAL-Ausgabe **G3BP9G** `Rebuild-Multiplayer.cmd` ausführen. Dafür werden Python 3,
`build-kit/multiplayer`, der mitgelieferte Recompiler und die Toolchain sowie
`source-multiplayer` benötigt. Der Vorgang prüft das Runtime-Kit und die
eigenen Spieldaten, übersetzt sie und bindet die Multiplayer-Runtime ein.
Anschließend führt `Start-Multiplayer.cmd` direkt ins originale Spielmenü.

Ein Paket für andere Spieler benötigt die normale Recompiler-Grundverteilung
mit Toolchain und Symbolen sowie die Multiplayer-Skripte,
`build-kit/multiplayer` und `source-multiplayer`. Jeder Empfänger richtet seine
eigene unterstützte PAL-Ausgabe ein und erstellt die Spielprogramme selbst.
Nicht in ein solches Weitergabepaket gehören ISO/Disc-Abbilder, `game_data`,
bereits aus Spieldaten erzeugte Spiel-EXE-Dateien oder generierte Spielquellen
und Objektdateien. Persönliche Karten, Speicherstände, Einstellungen,
Protokolle, Prüfaufnahmen und Rückfallkopien ebenfalls weglassen. Diese
Anleitung erstellt noch kein bereinigtes Weitergabepaket.
