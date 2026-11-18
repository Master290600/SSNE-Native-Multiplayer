#include "map_help.h"
#include <algorithm>

namespace mapedit {
const std::array<HelpLesson, 12> &help_lessons() {
  static const std::array<HelpLesson, 12> lessons{
      {{"map",
        HelpEvent::Loaded,
        {"1. Open a map", "1. Karte öffnen", "1. Ouvrir une carte"},
        {"Open an original or a saved project. Start practice opens a fresh "
         "map in memory; your current edits use the normal unsaved-changes "
         "dialog.",
         "Öffne eine Originalkarte oder ein Projekt. Übung starten lädt eine "
         "frische Karte im Speicher; bei ungespeicherten Änderungen erscheint "
         "die normale Rückfrage.",
         "Ouvrez une carte originale ou un projet. Démarrer l'exercice ouvre "
         "une copie en mémoire et respecte vos modifications non "
         "enregistrées."},
        {"Open original / Open project, or Start practice.",
         "Open original / Open project oder Übung starten.",
         "Open original / Open project ou Démarrer l'exercice."}},
       {"navigate",
        HelpEvent::Navigate,
        {"2. Navigate the viewport", "2. In der 3D-Ansicht bewegen",
         "2. Naviguer dans la vue"},
        {"Hold right mouse to look and fly with WASD. Q/E moves down/up; Shift "
         "is faster. Alt + left drag orbits the fixed pivot; middle drag pans. "
         "A plain click only selects.",
         "Halte die rechte Maustaste und fliege mit WASD. Q/E bewegt "
         "abwärts/aufwärts, Shift beschleunigt. Alt + Linksziehen dreht um den "
         "festen Mittelpunkt, mittleres Ziehen verschiebt die Ansicht. Ein "
         "normaler Klick wählt nur aus.",
         "Maintenez le bouton droit pour regarder et voler avec WASD. Q/E "
         "descend/monte, Maj accélère. Alt + glisser gauche orbite autour du "
         "pivot; le bouton central déplace la vue. Un simple clic "
         "sélectionne."},
        {"Move the camera using one of these gestures.",
         "Bewege die Kamera mit einer dieser Gesten.",
         "Déplacez la caméra avec l'un de ces gestes."}},
       {"selection",
        HelpEvent::Select,
        {"3. Select an object", "3. Ein Objekt auswählen",
         "3. Sélectionner un objet"},
        {"Click an object in the viewport or in the object list. Search and "
         "filters help in busy maps. Select a scene prop to practise all three "
         "transform tools.",
         "Klicke ein Objekt in der Ansicht oder Objektliste an. Suche und "
         "Filter helfen in großen Karten. Wähle ein Szenenobjekt (Scene prop), "
         "um alle drei Transform-Werkzeuge zu üben.",
         "Cliquez sur un objet dans la vue ou dans la liste. La recherche et "
         "les filtres aident dans les grandes cartes. Choisissez un décor "
         "(Scene prop) pour essayer les trois outils."},
        {"Select a different object; scene props support full transforms.",
         "Wähle ein anderes Objekt; Szenenobjekte unterstützen vollständige "
         "Transformationen.",
         "Sélectionnez un autre objet; les décors prennent en charge toutes "
         "les transformations."}},
       {"focus",
        HelpEvent::Focus,
        {"4. Focus selection", "4. Auswahl zentrieren",
         "4. Centrer la sélection"},
        {"Press F over the viewport or use Focus selection. The camera frames "
         "the selection and sets its orbit pivot. Reset view returns to the "
         "map's starting view.",
         "Drücke F über der Ansicht oder benutze Focus selection. Die Kamera "
         "rahmt die Auswahl ein und setzt den Orbit-Mittelpunkt. Reset view "
         "stellt die Startansicht der Karte wieder her.",
         "Appuyez sur F au-dessus de la vue ou sur Focus selection. La caméra "
         "cadre l'objet et définit le pivot. Reset view rétablit la vue "
         "initiale."},
        {"Use F or the Focus button.", "Benutze F oder den Focus-Button.",
         "Utilisez F ou le bouton Focus."}},
       {"move",
        HelpEvent::Move,
        {"5. Move", "5. Verschieben", "5. Déplacer"},
        {"Choose Move (W), then drag an axis or plane handle. Red is X, green "
         "is Y (height), blue is Z. Release to commit one undo step; Escape "
         "cancels. Numeric position fields are also available.",
         "Wähle Move (W) und ziehe eine Achse oder Fläche. Rot ist X, Grün ist "
         "Y (Höhe), Blau ist Z. Loslassen bestätigt einen Undo-Schritt, Escape "
         "bricht ab. Du kannst auch die Positionsfelder benutzen.",
         "Choisissez Move (W), puis faites glisser un axe ou un plan. Rouge: "
         "X, vert: Y (hauteur), bleu: Z. Relâchez pour valider; Échap annule. "
         "Les champs numériques sont aussi disponibles."},
        {"Commit a position change.", "Bestätige eine Positionsänderung.",
         "Validez un changement de position."}},
       {"rotate",
        HelpEvent::Rotate,
        {"6. Rotate", "6. Drehen", "6. Tourner"},
        {"Choose Rotate (E) and drag a coloured ring. World and Local control "
         "the axis basis. Some native point entities only support position or "
         "yaw; choose a scene prop for all rotation axes.",
         "Wähle Rotate (E) und ziehe einen farbigen Ring. World und Local "
         "bestimmen die Achsenbasis. Manche nativen Punktobjekte unterstützen "
         "nur Position oder Drehung um Y; benutze ein Szenenobjekt für alle "
         "Drehachsen.",
         "Choisissez Rotate (E) et faites glisser un anneau. World et Local "
         "définissent les axes. Certains points natifs ne gèrent que la "
         "position ou le lacet; utilisez un décor pour les trois axes."},
        {"Select a scene prop and commit a rotation.",
         "Wähle ein Szenenobjekt und bestätige eine Drehung.",
         "Sélectionnez un décor et validez une rotation."}},
       {"scale",
        HelpEvent::Scale,
        {"7. Scale", "7. Skalieren", "7. Redimensionner"},
        {"Choose Scale (R). Drag an axis box for local-axis scaling or the "
         "centre for uniform scaling. Scale is available only for entities "
         "whose native game data supports it. Trigger size is configured using "
         "its radius.",
         "Wähle Scale (R). Ziehe einen Achsenwürfel zum Skalieren entlang "
         "einer lokalen Achse oder die Mitte für gleichmäßige Skalierung. "
         "Skalieren ist nur bei Objekten verfügbar, deren Spieldaten es "
         "unterstützen. Die Triggergröße wird über den Radius eingestellt.",
         "Choisissez Scale (R). Tirez une boîte d'axe pour une échelle locale "
         "ou le centre pour une échelle uniforme. L'outil respecte les limites "
         "natives; la taille des déclencheurs utilise leur rayon."},
        {"Commit a scene prop scale change.",
         "Bestätige eine Skalierung eines Szenenobjekts.",
         "Validez un changement d'échelle d'un décor."}},
       {"create_logic",
        HelpEvent::CreateLogic,
        {"8. Create logic", "8. Logic erstellen", "8. Créer de la logique"},
        {"Use Add logic to create a button, area trigger, relay, timer or "
         "counter. Logic symbols identify each role. Place the element near "
         "the selection and edit its name in the inspector.",
         "Erstelle mit Add logic einen Button, Bereichstrigger, Relay, Timer "
         "oder Zähler. Jedes Logic-Symbol kennzeichnet die Funktion. Platziere "
         "das Element bei der Auswahl und bearbeite seinen Namen in den "
         "Eigenschaften.",
         "Utilisez Add logic pour créer un bouton, une zone, un relais, un "
         "minuteur ou un compteur. Les symboles indiquent leur rôle. Placez "
         "l'élément près de la sélection et nommez-le dans l'inspecteur."},
        {"Create an element with Add logic or Add trigger.",
         "Erstelle ein Element mit Add logic oder Add trigger.",
         "Créez un élément avec Add logic ou Add trigger."}},
       {"configure_logic",
        HelpEvent::ConfigureLogic,
        {"9. Configure a trigger", "9. Trigger konfigurieren",
         "9. Configurer un déclencheur"},
        {"Select the new logic element. Set a delay, radius, activation count "
         "or repeat behaviour in its properties and apply. A counter gates its "
         "outputs until enough input events arrive. Unsupported native actions "
         "stay unavailable.",
         "Wähle das neue Logic-Element. Ändere Verzögerung, Radius, "
         "Aktivierungsanzahl oder Wiederholung und bestätige. Ein Zähler gibt "
         "seine Ausgänge erst nach genügend Eingangssignalen frei. Nicht "
         "unterstützte native Aktionen bleiben gesperrt.",
         "Sélectionnez le nouvel élément. Modifiez le délai, le rayon, le "
         "nombre d'activations ou la répétition, puis appliquez. Un compteur "
         "attend assez d'entrées avant de transmettre ses sorties."},
        {"Change a logic property and Apply.",
         "Ändere eine Logic-Eigenschaft und bestätige mit Apply.",
         "Modifiez une propriété et cliquez sur Apply."}},
       {"connect",
        HelpEvent::Connect,
        {"10. Connect events and actions", "10. Events und Aktionen verbinden",
         "10. Relier événements et actions"},
        {"Enable Link mode or open the Logic view. Pick a source output, then "
         "a compatible target. Choose Trigger, Enable or Disable as supported. "
         "A relay can fan out to multiple targets; delays and counters form "
         "timed or conditional chains.",
         "Aktiviere Link mode oder öffne die Logic-Ansicht. Wähle einen "
         "Ausgang und ein passendes Ziel. Benutze die unterstützte Aktion "
         "Trigger, Enable oder Disable. Ein Relay kann mehrere Ziele auslösen; "
         "Verzögerungen und Zähler bilden zeitgesteuerte und bedingte Ketten.",
         "Activez Link mode ou ouvrez la vue Logic. Choisissez une sortie et "
         "une cible compatible, puis Trigger, Enable ou Disable. Un relais "
         "peut déclencher plusieurs cibles; délais et compteurs forment des "
         "chaînes temporisées ou conditionnelles."},
        {"Create a new authored connection.", "Erstelle eine neue Verknüpfung.",
         "Créez une nouvelle connexion."}},
       {"save",
        HelpEvent::Save,
        {"11. Save your project", "11. Projekt speichern",
         "11. Enregistrer le projet"},
        {"Use Save project or Ctrl+S. New projects need a name. Your exported "
         "map copy and logic are stored in MapProjects; originals remain "
         "unchanged. Undo/Redo uses Ctrl+Z/Ctrl+Y.",
         "Benutze Save project oder Strg+S. Neue Projekte benötigen einen "
         "Namen. Die Kartenkopie samt Logic wird in MapProjects gespeichert; "
         "Originalkarten bleiben erhalten. Undo/Redo: Strg+Z/Strg+Y.",
         "Utilisez Save project ou Ctrl+S. Donnez un nom au nouveau projet. La "
         "copie et sa logique sont enregistrées dans MapProjects; les "
         "originaux restent intacts. Annuler/rétablir: Ctrl+Z/Ctrl+Y."},
        {"Save the current project.", "Speichere das aktuelle Projekt.",
         "Enregistrez le projet courant."}},
       {"test",
        HelpEvent::Test,
        {"12. Test in the game", "12. Im Spiel testen",
         "12. Tester dans le jeu"},
        {"Use Test logic to run the saved map with live observations, then "
         "enter an area or activate your button in the game. The Logic/Test "
         "panel shows observed events and remaining door conditions. A drawn "
         "connection alone does not prove an event fired.",
         "Starte mit Test logic die gespeicherte Karte mit Live-Anzeige. "
         "Betritt den Bereich oder betätige den Button im Spiel. Logic/Test "
         "zeigt beobachtete Events und noch fehlende Türbedingungen. Eine "
         "gezeichnete Verbindung allein bedeutet nicht, dass sie ausgelöst "
         "wurde.",
         "Utilisez Test logic, puis entrez dans la zone ou activez le bouton "
         "dans le jeu. Logic/Test affiche les événements observés et les "
         "conditions des portes. Une ligne dessinée ne prouve pas qu'un "
         "événement a eu lieu."},
        {"Start a map test. Then check its live logic observations.",
         "Starte einen Kartentest und prüfe danach die Live-Anzeige der Logic.",
         "Lancez un test, puis consultez les observations de logique."}}}};
  return lessons;
}
void Tutorial::start(const HelpEvents &events) {
  step = 0;
  completed = skipped = 0;
  baseline = events[unsigned(help_lessons()[0].event)];
}
bool Tutorial::observe(const HelpEvents &events) {
  if (!active())
    return false;
  const auto bit = 1u << step;
  if ((completed & bit) ||
      events[unsigned(help_lessons()[step].event)] <= baseline)
    return false;
  completed |= bit;
  skipped &= ~bit;
  return true;
}
void Tutorial::next(const HelpEvents &events, bool skip) {
  if (!active())
    return;
  const auto bit = 1u << step;
  if (!(completed & bit)) {
    if (!skip)
      return;
    skipped |= bit;
  }
  if (++step < int(help_lessons().size()))
    baseline = events[unsigned(help_lessons()[step].event)];
}
void Tutorial::previous(const HelpEvents &events) {
  if (step <= 0)
    return;
  --step;
  baseline = events[unsigned(help_lessons()[step].event)];
}
std::string_view editor_tool_help(std::string_view id, unsigned language) {
  struct Tip {
    const char *id;
    std::array<const char *, 3> text;
  };
  static const Tip tips[]{
      {"open_original",
       {"Open a read-only original. Save creates your own project copy.",
        "Original schreibgeschützt öffnen. Speichern erstellt deine "
        "Projektkopie.",
        "Ouvrir un original en lecture seule; enregistrer crée votre copie."}},
      {"open_project",
       {"Reopen a saved map project with its objects and native logic.",
        "Gespeichertes Projekt mit Objekten und nativer Logic öffnen.",
        "Rouvrir un projet avec ses objets et sa logique native."}},
      {"save",
       {"Save the project and exported map. Ctrl+S.",
        "Projekt und Kartenkopie speichern. Strg+S.",
        "Enregistrer le projet et la carte. Ctrl+S."}},
      {"test",
       {"Run a separate test copy with its own memory card.",
        "Separate Testkopie mit eigener Speicherkarte starten.",
        "Lancer une copie de test avec sa propre carte mémoire."}},
      {"logic_test",
       {"Run the saved map and display actual native logic observations.",
        "Gespeicherte Karte starten und echte Logic-Beobachtungen anzeigen.",
        "Tester la carte et afficher les observations natives."}},
      {"undo",
       {"Undo one committed edit. Ctrl+Z.",
        "Einen bestätigten Bearbeitungsschritt zurücknehmen. Strg+Z.",
        "Annuler une modification. Ctrl+Z."}},
      {"redo",
       {"Redo an undone edit. Ctrl+Y.",
        "Einen zurückgenommenen Schritt wiederherstellen. Strg+Y.",
        "Rétablir une modification. Ctrl+Y."}},
      {"focus",
       {"Frame the selection and set the orbit pivot. F.",
        "Auswahl zentrieren und Orbit-Mittelpunkt setzen. F.",
        "Cadrer la sélection et définir le pivot. F."}},
      {"fit",
       {"Frame the whole map without changing its objects.",
        "Die gesamte Karte einrahmen, ohne Objekte zu ändern.",
        "Cadrer toute la carte sans modifier les objets."}},
      {"reset",
       {"Restore this object's original authored transform; undo remains "
        "available.",
        "Originale Transformation dieses Objekts wiederherstellen; Undo bleibt "
        "möglich.",
        "Rétablir la transformation d'origine; l'annulation reste possible."}},
      {"duplicate",
       {"Duplicate a supported object. Ctrl+D. Story-owned helpers remain "
        "protected.",
        "Unterstütztes Objekt duplizieren. Strg+D. Story-Helfer bleiben "
        "geschützt.",
        "Dupliquer un objet compatible. Ctrl+D. Les éléments de scénario "
        "restent protégés."}},
      {"delete",
       {"Delete a supported object and its authored connections. Delete.",
        "Unterstütztes Objekt samt Verknüpfungen löschen. Entf.",
        "Supprimer un objet compatible et ses connexions. Suppr."}},
      {"link_mode",
       {"Pick a source output and a compatible target to author a real game "
        "action.",
        "Ausgang und passendes Ziel für eine echte Spielaktion auswählen.",
        "Choisir une sortie et une cible pour une action réelle du jeu."}},
      {"add_logic",
       {"Create native game logic: buttons, triggers, relays, timers and "
        "counters.",
        "Native Spiel-Logic erstellen: Buttons, Trigger, Relays, Timer und "
        "Zähler.",
        "Créer des boutons, zones, relais, minuteurs et compteurs natifs."}},
      {"add_trigger",
       {"Create a player area linked to an enemy wave, with delay and repeat "
        "settings.",
        "Spielerbereich mit Gegnerwelle, Verzögerung und Wiederholung "
        "erstellen.",
        "Créer une zone reliée à une vague avec délai et répétition."}},
      {"custom",
       {"Export a named Custom Level with its own preview and supported game "
        "modes.",
        "Custom Level mit Namen, Vorschaubild und Spielmodi exportieren.",
        "Exporter un niveau personnalisé avec nom, aperçu et modes de jeu."}},
      {"help",
       {"Open searchable help and interactive lessons at any time. F1.",
        "Hilfe und interaktive Übungen jederzeit öffnen. F1.",
        "Ouvrir l'aide et les exercices à tout moment. F1."}}};
  for (const auto &t : tips)
    if (id == t.id)
      return t.text[std::min(language, 2u)];
  return {};
}
} // namespace mapedit
