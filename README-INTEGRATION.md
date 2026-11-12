> Historical integration notes (builds 182/183). For the current player package and launch flow, use README.md and docs/RELEASE-NOTES-201.md.

# SSNE-Native 1.2.7 – Multiplayer Build 186 / Protokoll 61

Dieser Quellstand wurde aus den unveränderten SSNE-Native-1.2.7-Quellen aufgebaut
(Tag `c02c6fdfcc3b7b52870a63ee5059db745e55717c`). Die Anwendung, Hauptschleife,
Einstellungen und das F12-Menü stammen aus dieser Basis. Build 181 war dagegen
ein Mischstand mit älterer Koop-Anwendung und wird nur als Rückfallversion erhalten.

`port/src/settings.h` und `settings.cpp` sind bytegleich mit 1.2.7: drei
Tastaturbelegungen je Aktion, neun Waffen-Gruppen, native Auflösung, automatisches
Seitenverhältnis, Audio-Mischung, Controller- und lokale Spielerzuordnung.
Die Controller-Abtastung aus 1.2.7 steht für Anwendung und virtuellen Gerätetest
gemeinsam in `controller_input.h`. Die acht bestehenden F12-Seiten bleiben erhalten.
Gezielte Online-Hinweise erklären Host-Speichern und die weiterlaufende Sitzung;
unwirksame lokale Pause- und Display-Synchronisationsschalter sind online gesperrt.

Der autoritative Koop-Adapter stammt aus SSNE-RECOOP, Ausgangsrevision
`007cadd9758e46119a92c0180e3abb775fa21b72`, mit den geprüften passenden
Grafik-Anpassungen aus Build 181. Er ist über Start, Spiel-Callbacks,
Eingabeweitergabe, lokale Ansicht, gemeinsame Zustände und Speicherstände angebunden.
Die alte Hauptschleife und das alte F12-Menü wurden nicht übernommen.

## Behobene Integrationsfehler

- Korrekte 1.2.7-Einstellungsstruktur und Eingabeverarbeitung statt der alten
  Zwei-Belegungen-Struktur; native Auflösung und alle ursprünglichen F12-Seiten.
- Aktivierte lokale Controller steuern online den eigenen Teilnehmer; die
  Offline-Zuordnung für bis zu vier lokale Spieler bleibt erhalten.
- Mauszuordnung berücksichtigt den gewählten lokalen Spieler; Informations- und
  Pause-Menüs bekommen Menüeingaben statt aktiver direkter Zielsteuerung.
- Waffen-Gruppen werden als authentifizierte, einmalige Eingabe an den Host
  übertragen. Tastendrücke bleiben über alle acht logischen Plätze unabhängig;
  die vier physischen SI-Kanäle werden nicht durch höhere Plätze überschrieben.
- Die Lobby-Aktionen Enter (Level), C (Fortsetzen) und Enter bei Skip-Abstimmungen
  sind gezielt an die 1.2.7-Ereignisverarbeitung angebunden; F10 bleibt FPS-Auswahl.
- Die HUD-Randoption wirkt auch auf die lokale Koop-Ansicht.
- Koop-Speichern/Laden verwendet eigene Slots und Host-Rechte. Die Größenprüfung
  des Musikabschnitts stimmt mit dessen Version-2-Serializer (160 Byte) überein;
  der bisherige 152-Byte-Prüfwert verhinderte sogar die Rücknahme eines Ladefehlers.

Der OpenGL-3.3-Pfad mit GPU-Licht/Texturkoordinaten, Shader-Vorbereitung und
GX-Grafik-Thread bleibt erhalten. GX-Bereichsgrenzen werden synchronisiert;
lokale Kamera-/HUD-Daten liegen in aufgezeichneten Frames/Draws. Abhängige
EFB-Kopien bleiben bei ausgelassenen Bildern erhalten. Es gibt keine zusätzliche
Render-Interpolation und keine DX11-Auswahl im Produkt. Das FPS-Limit begrenzt
auch die Berechnung im Spielthread; native Menüs behalten ihre ursprüngliche Taktung.

## Nachweise und Grenzen

`Multiplayer/Pruefung/Build182` enthält die Ergebnisse und Bildnachweise:

- 18 Komponentenprüfungen einschließlich 1.2.7-Einstellungs-Rundlauf,
  virtuellem SDL-Controller, logischen Eingaben, TCP-Sitzung, Zustandscodec,
  Hook-Rückkehr, Bildratenmessung und abhängigen Frame-Kopien.
- Alle acht F12-Seiten sowie Host/Beitreten/Optionen/Charaktervorschau im Starter.
- Native Lobby mit Charakterwechsel; Levelwahl zurück zur Lobby;
  Schwierigkeit zurück zur Levelwahl; erneute Bestätigung und Arena-Start;
  native Pause/Fortsetzen, F12 öffnen/schließen und sauberes Verlassen.
- Host-Speichern/Laden im laufenden Koop mit anschließender neuer Sitzungsepoche
  und wieder verbundenen Spieleransichten; Client-Speicherrechte werden abgewiesen.
- Acht lokale Prozesse gemeinsam in der Arena, 1×/60 FPS; zwei Prozesse bei
  8×/120 FPS in der Arena. Rlevel0_1 ist die Arena; Rlevel1_1 das folgende Gebiet.
- Rlevel1_1 mit synchronisierter Einleitung/Skip-Abstimmung und anschließendem Spiel.
- Neuübersetzung aus der unveränderten EU-DOL: Code-/Datensektionen stimmen
  überein, nur die Link-Build-ID unterscheidet sich.

Alle Spielprüfungen verwenden versteckte Fenster, private Kartenkopien und
prozesslokale Skripte. Sichtbarkeit und Desktop-Fokus werden überwacht. Echte
Tastatur/Maus/Controller-Abtastung und Maus-Capture sind im versteckten Modus
gesperrt. Der Controller-Test verwendet ausschließlich sein eigenes virtuelles
Gerät und initialisiert kein Videosystem. Echtes Vollbild/Monitorwechsel und
physische Controller wurden entsprechend dieser Testvorgabe nicht betätigt;
deren Anwendungscode ist aus 1.2.7 erhalten und separat auditiert.

Internetverbindungen, komplette Kampagnen, langes Achtspieler-Spiel und Last in
späteren Gebieten sind damit nicht abgenommen. Die kurzen Arena-Werte sind kein
Versprechen durchgehend gleichmäßiger Bildzeiten.

## Lokaler Neuaufbau

`Rebuild-Multiplayer.cmd` im Installationsordner verwendet das aktualisierte
`build-kit/multiplayer` mit geprüften Headern, Port-Objekten und Bibliotheken.
Die EU-DOL muss SHA256
`c0b8ba71e0dd99ca632a2a6931db0a8f610d69c00a423d34c96a579eaa3710ad` haben.
`tools/patch_multiplayer.py` passt 192 Originalfunktionen des erzeugten Spielcodes
an. Der Recompiler selbst wurde nicht verändert. Protokoll 61 ist erforderlich;
ältere Protokolle dürfen dieser Sitzung nicht beitreten.

Die ursprünglichen Hauptordner-Programme, Quellen, Einstellungen und Karten bleiben
unverändert. SSNE-Native bleibt ein Offline-Recompiler mit PC-Laufzeit und erfüllt
nicht das pausierte Ziel eines vollständig rekonstruierten nativen Gesamtports.


## Build 183: original native multiplayer modes

The host enters the original game-type group before the native rule and player screens. Selected GameSetup rules, level and mode reach every authoritative replica (protocol 60). Coop retains eight slots; the original versus engine uses four. Admission rejects extra peers without discarding existing members; selecting versus with occupied upper slots returns to the roster.

Replica suppression covers modes 1–4. Coop-only combo/notice widgets are captured only for coop. Versus presentation transfers carrier slots, held-object matrices, the flag pose, match countdown and bomb fuse through typed mission properties; original PAL UI/model handlers apply them locally. All damage, pickups, score and objective rules still execute on the host. The native occupancy hooks also exclude unused versus actors.

Build183 evidence records the exact tested binary and hidden local peer results. These checks do not accept every versus map, long matches or Internet conditions. Build182 remains recoverable separately. Original 1.2.7 programs, source, settings and cards are preserved.


## Build 184: versus timer and idle friction

The original match timer and its two companion widgets are positioned at the top center on host and replicas through the original GUI positioning routine. Their countdown logic is retained.

A host log and a hidden ClevelDM_1 reproduction showed slow motion despite neutral input on a shallow static floor. The host now cancels only sub-0.1-unit/second corrections for neutral walking actors supported by nearly horizontal static geometry. Jumping/falling, input, vehicles, appreciable motion and geometry-owned floors retain the native solver. Coop is unchanged. The regression records zero displacement before and after a walk and jump; the timer is visually checked on both peers. Protocol 60 is unchanged.


## Build 185: eigenes Modmenü und PAL-Cheats

Einfg (Insert) öffnet das übernommene Modmenü mit Player cheats, Aimbot / ESP,
Item ESP, Spawn und Names / catalog. Alternativ F12 → Cheats → Open mod menu.
F9 bleibt Laden. Menüs geben neutrale Spielereingaben weiter.
Im Netzwerk steuert der Host Godmode, Munition, Waffen, Gegner-Spawns und
Feuerrate sowie Objekt-Spawns. Gemeinsame Schalter werden zum Client übertragen;
Zielhilfe und ESP werden pro PC eingestellt. Nicht geladene Gegnermodelle werden
weiterhin abgewiesen. Offline bleiben die originalen Versus-Beschränkungen aktiv.
Die eingebauten F12-Cheats verwenden auf G3BP9G verifizierte PAL-Signaturen und
PAL-Globals; sie gelten weiterhin für Einzelspieler. Speichergrenzen berücksichtigen
den erweiterten Party-Speicher. Ein echter PAL-RAM-Fixture-Test prüft Anwendung und
Rücknahme der Cheats sowie gemeinsame Inventar-/Feuerraten-Änderungen.
Nachweise: mods-pal-build185 (verdeckter Host/Client, fünf Menü-Reiter,
Host-Schalter ein/aus, unveränderte Benutzerkarte und Komponententests).
Nicht als vollständig geprüft gelten alle Spawn-Typen, alle Karten oder Internetspiele.


## Build 186: PAL campaign adapters

Campaign integration fixes cover the empty geometry catalog in Alevel12_4,
authored air/water spawn anchors, Dark Lord attached hit regions, native boss
Netricsa parameters, all three vehicle layouts, hidden vehicle mesh lifetimes and the
unindexed authored owners in Clevel6_2 and Alevel12_2. Coincident doors/pickups
keep distinct lifetime bindings, and level shutdown releases player HUD cameras
before the main menu reuses its camera pool. Original
result GUI initialization, cleanup, stat-order reset and transferred coefficients
prevent successive client result menus from reusing stale native state.

Protocol 61 is required on every peer. Validation procedures and explicit
shortcut limitations are documented in `docs/COOP-CAMPAIGN-VALIDATION.md`.
Deployment evidence belongs in `Multiplayer/Pruefung/Build186`; a normal complete
campaign playthrough and arbitrary Internet conditions remain unverified.
