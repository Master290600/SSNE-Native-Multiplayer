# KI-Mitspieler und Gegner – Build 189 / Protokoll 62

Als Host `F12` öffnen. Unter **Players → AI bots** (auch im Sitzungsfenster) eine
Schwierigkeit wählen und **Add bot** drücken. **Remove** entfernt den jeweiligen
Bot auch während einer laufenden Runde. Koop hat insgesamt acht Plätze, die
drei Versus-Modi jeweils vier. Der Host belegt einen dieser Plätze. Ein echter
Spieler mit gültigem PIN erhält bei voller Runde den Platz eines Bots.

Bots sind eigenständige Host-Teilnehmer. Sie sind im Vorraum automatisch bereit,
benötigen keinen zweiten Prozess und stimmen nicht bei Zwischensequenzen mit.
Der Host simuliert ihre Bewegung, Angriffe und Inventare; Clients bekommen
dieselben normalen Spieler-Snapshots. AFK-Steuerung für menschliche Teilnehmer
bleibt getrennt. Eine Bot-Schwierigkeit gilt für den neu hinzugefügten Bot.

Die KI sucht benötigte Heilung, Rüstung, Munition und noch fehlende Waffen.
Sie sammelt sie durch die normalen Spielkollisionen ein, verwendet nur wirklich
besessene Waffen mit Munition und wählt ihre Waffe nach Entfernung und Gegnern.
Sichtlinien, endliche Drehgeschwindigkeit und Reaktionszeit begrenzen das Zielen.
Minigun, Uzis und Flammenwerfer halten den Abzug bei kleinen Zielabweichungen
weiter gedrückt. Die Minigun erhält eine längere ruhige Zielphase für ihre
originale Anlaufzeit von 0,8 Sekunden. Entfernungsgrenzen haben einen Puffer,
damit sie nicht dauernd zur Schrotflinte oder zum Scharfschützengewehr wechselt.
Sichtverlust, Zielwechsel, fehlende Munition und Verbündete in der Schusslinie
beenden die Feuerfreigabe. Der Flammenwerfer wird im Nahbereich eingesetzt.
Koop-Bots folgen lebenden menschlichen Mitspielern und vermeiden Schüsse durch
Teammitglieder sowie gefährliche Explosionen neben Verbündeten. Sie überlassen
nahe Heilung einem stärker verletzten menschlichen Mitspieler.

Deathmatch-Bots suchen Gegner und Ausrüstung und halten passende Kampfdistanz.
Bei **Behalte die Flagge** suchen sie die Flagge, verfolgen ihren Träger und
versuchen als Träger Abstand zu Gegnern zu halten. Bei **Gib die Bombe ab**
verfolgt der Träger andere Spieler; die übrigen Bots fliehen vor dem Träger.
Aufnahme, Übergabe, Schaden, Wertung und Wiedererscheinen bleiben Originalcode.

Die Wegsuche verwendet native Kollisions- und Bodenabfragen, begrenztes A*, eine
feinere Suche für enge Durchgänge und lokale Ausweichmanöver. Sie vermeidet
ungestützte Schritte und versucht blockierende benutzbare Türen zu öffnen.
Nicht erreichbare Pickups werden zeitweise zurückgestellt. Es gibt keine
Teleportation zu Mitspielern, erzeugte Waffen oder direkten Schadensschreibzugriffe.

Entwicklerstart: `--coop-host --bots 3 --bot-skill 2`. Anzahl 0–7,
Schwierigkeit 1–3. Die tatsächliche Höchstzahl hängt vom gewählten Modus ab.
Bot-Plätze werden für die aktuelle Sitzung verwaltet.

## Prüfung und Grenzen

`coop_bots_test` prüft Aufnahme, Schwierigkeitsgrenzen, volle Runden,
Vorrang echter Spieler, Abweisung falscher PINs, sichere Moduswechsel,
Slot-Generationen, Waffenwahl und Wegsuche um eine Wand ohne Absturz an Kanten.
`tools/test_game_bots.py` startet eigene unsichtbare PAL-Prozesse mit privaten
Speicherkarten und Eingabedateien. Der Nachweis erfasst Bewegung, Inventare,
Waffenwahl, Schaden, Flagge/Bombe, Wiedererscheinen und Client-Abgleich.
Gezielte Begegnungen verwenden ausschließlich unter expliziter
Hidden-Test-Aktivierung den ursprünglichen Positionshandler. Kämpfe und
Todesfälle laufen über normale Waffen und Bomben. Die Testplatzierung ist
keine während normalen Spiels verfügbare Bot-Fähigkeit.

Die Bots sind eine erste spielbare KI. Sie sind kein autonomer Kampagnenlöser:
Questgegenstände, komplexe Sprung-/Schalterrätsel und Fahrzeugpassagen verlangen
weiterhin menschliche Führung. An Höhenkanten und in engen Geometrien können
Bots noch hängen bleiben. Ein kompletter Kampagnenlauf mit Bots sowie
sämtliche Karten, Wasserwege und Internetbedingungen sind nicht verifiziert.
Die bisherigen Modusprüfungen liegen unter `Multiplayer/Pruefung/Build187`.
Die gezielten Dauerfeuer-Prüfungen stehen unter `Multiplayer/Pruefung/Build189`.
`tools/test_bot_held_fire.py` verwendet einen ausdrücklich markierten normalen
Waffen-Pickup am Bot und den nativen Positionshandler für die Begegnung.
Die KI wird dabei nicht zum Schießen gezwungen. Erfasst werden echte
Munitionsabnahme, Gesundheitswerte, native Waffen-Effekte auf Host und Client,
Dauer des gedrückten Abzugs sowie Loslassen ohne Ziel. Diese kurzen Prüfungen
ersetzen keinen vollständigen Kampagnendurchlauf und decken keine alternativen
Feuermodi, alle Karten oder jede Gegnerkombination ab.
