#pragma once
#include <array>
#include <string>
namespace port::coop {
enum class FrontText {Multiplayer,Host,Join,Local,Back,Start,Address,Pin,Port,Connecting,Cancel,Retry,Connection,Failed,Version,Full,Lost,Invalid,Typing,Bots,AddBot,RemoveBot,Skill,Easy,Normal,Hard,Free,HostHelp,JoinHelp,BotHelp,HostKeys,ClientKeys,ModeFull,Search,Searching,None,Leave,More,InvalidAddress,InvalidPin,InvalidPort,RoomsHelp,LanExpired,AfkEnter,AfkLeave,Options,PlayerName,ProfileName,Enemies,AmmoCapacity,AmmoPickups,Unlimited,RulesHelp,ClientRules,NameLocked,NameTyping,SaveFailed,Count};
inline const char* frontend_text(FrontText key,unsigned language){
    // PAL language order is the original SystemSetup.Language: EN, DE, FR.
    static constexpr std::array<std::array<const char*,3>,unsigned(FrontText::Count)> rows{{
        {{"Multiplayer","Mehrspieler","Multijoueur"}},{{"Host game","Spiel erstellen","Cr\351er une partie"}},{{"Join game","Spiel beitreten","Rejoindre une partie"}},{{"Local game","Lokal spielen","Partie locale"}},{{"Back","Zur\374ck","Retour"}},
        {{"Start","Starten","D\351marrer"}},{{"Address","Adresse","Adresse"}},{{"PIN","PIN","Code"}},{{"Port","Port","Port"}},{{"Connecting...","Verbindung wird aufgebaut...","Connexion en cours..."}},{{"Cancel","Abbrechen","Annuler"}},{{"Retry","Erneut versuchen","R\351essayer"}},{{"Connection","Verbindung","Connexion"}},
        {{"Connection failed. Check address and PIN.","Keine Verbindung. Adresse und PIN pr\374fen.","Connexion impossible. V\351rifiez adresse et code."}},{{"PIN, game data or version differs.","PIN, Spieldaten oder Version stimmen nicht \374berein.","Code, donn\351es du jeu ou version incompatibles."}},{{"This game is full.","Die Sitzung ist voll.","Cette partie est compl\350te."}},{{"Connection closed.","Die Verbindung wurde beendet.","Connexion ferm\351e."}},{{"Check address, six-digit PIN and port (1-65535).","Adresse, sechsstellige PIN und Port (1-65535) pr\374fen.","V\351rifiez adresse, code \340 six chiffres et port (1-65535)."}},
        {{"Type to edit. Enter: accept. Esc: cancel. Arrows: change digit.","Tippen zum Bearbeiten. Enter: OK. Esc: Abbruch. Pfeile: Ziffer \344ndern.","Saisir le texte. Entr\351e : valider. Echap : annuler. Fl\350ches : modifier."}},{{"Bots","Bots","Bots"}},{{"Add bot","Bot hinzuf\374gen","Ajouter un bot"}},{{"Remove bot","Bot entfernen","Retirer un bot"}},{{"Difficulty","Schwierigkeit","Difficult\351"}},{{"Easy","Leicht","Facile"}},{{"Normal","Normal","Normal"}},{{"Hard","Schwer","Difficile"}},{{"Free","Frei","Libre"}},
        {{"Create a game, then choose its mode, rules and map.","Sitzung erstellen, dann Spielmodus, Regeln und Karte w\344hlen.","Cr\351ez une partie, puis choisissez mode, r\350gles et carte."}},{{"Enter a host address and PIN, or search the local network.","Host-Adresse und PIN eingeben oder im lokalen Netzwerk suchen.","Saisissez adresse et code ou recherchez sur le r\351seau local."}},{{"Bots use free player slots. Human players have priority.","Bots belegen freie Pl\344tze. Echte Spieler haben Vorrang.","Les bots occupent les places libres. Priorit\351 aux joueurs."}},
        {{"Left/Right: character | Enter: map | Y: bots | X: connection","Links/Rechts: Figur | Enter: Karte | Y: Bots | X: Verbindung","Gauche/Droite : personnage | Entr\351e : carte | Y : bots | X : connexion"}},{{"Left/Right: character | X: connection","Links/Rechts: Figur | X: Verbindung","Gauche/Droite : personnage | X : connexion"}},
        {{"Versus allows four players. Choose coop for more.","Versus erlaubt vier Spieler. F\374r mehr Spieler Koop w\344hlen.","Le mode versus accepte quatre joueurs. Choisissez le coop au-del\340."}},{{"Find LAN games","LAN-Spiele suchen","Rechercher en r\351seau"}},{{"Searching...","Suche l\344uft...","Recherche en cours..."}},{{"No games found. You can enter an address instead.","Keine Sitzung gefunden. Die Adresse kann direkt eingegeben werden.","Aucune partie trouv\351e. Vous pouvez saisir une adresse."}},{{"Leave game","Sitzung verlassen","Quitter la partie"}},{{"More games","Weitere Spiele","Autres parties"}},
        {{"Enter a valid host address.","Eine g\374ltige Host-Adresse eingeben.","Saisissez une adresse valide."}},{{"The PIN must contain exactly six digits.","Die PIN muss genau sechs Ziffern enthalten.","Le code doit contenir exactement six chiffres."}},{{"The port must be between 1 and 65535.","Der Port muss zwischen 1 und 65535 liegen.","Le port doit etre compris entre 1 et 65535."}},
        {{"Select a game to join directly.","Eine Sitzung w\344hlen, um direkt beizutreten.","Choisissez une partie pour la rejoindre directement."}},{{"This LAN game expired. Search again.","Diese LAN-Sitzung ist abgelaufen. Erneut suchen.","Cette partie r\351seau a expir\351. Relancez la recherche."}},
        {{"Enable AFK","AFK-Modus starten","Activer le mode AFK"}},{{"Disable AFK","AFK-Modus beenden","D\351sactiver le mode AFK"}},
        {{"Session settings","Sitzungsoptionen","Options de partie"}},{{"Name","Name","Nom"}},{{"Game profile","Spielprofil","Profil du jeu"}},
        {{"Enemies","Gegner","Ennemis"}},{{"Ammo limit","Munitionslimit","Limite de munitions"}},{{"Ammo pickups","Pickup-Munition","Par ramassage"}},{{"Unlimited","Unendlich","Illimite"}},
        {{"Host rules. Extra enemies exclude bosses and story actors. Unlimited carry: 32,767 per ammo type.","Host-Regeln. Bosse und Story-Figuren bleiben einzeln. Unendliches Limit: 32.767 je Munitionstyp.","Regles de l'hote. Boss et personnages de scenario uniques. Limite illimitee : 32 767 par type."}},
        {{"The host sets these rules. Name changes apply before joining.","Diese Regeln legt der Host fest. Namen vor dem Beitritt aendern.","L'hote choisit ces regles. Changez le nom avant de rejoindre."}},
        {{"Leave the session to change your name.","Sitzung verlassen, um den Namen zu aendern.","Quittez la partie pour changer votre nom."}},
        {{"Type a name (up to 24 letters). Enter: accept. Esc: cancel.","Namen eingeben (bis 24 Zeichen). Enter: OK. Esc: Abbruch.","Saisissez un nom (24 caracteres). Entree : valider. Echap : annuler."}},
        {{"Could not save session settings.","Sitzungsoptionen konnten nicht gespeichert werden.","Impossible d'enregistrer les options."}}
    }};
    const auto index=unsigned(key);return index<rows.size()?rows[index][language<3?language:0]:"";
}
struct FrontendEndpoint {std::string address="127.0.0.1",pin="123456";unsigned port=27341;};
enum class FrontendEndpointError {None,Address,Pin,Port};
inline FrontendEndpointError frontend_endpoint_error(const FrontendEndpoint& value){
    if(value.address.empty() || value.address.size()>253 || value.address.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:")!=std::string::npos)return FrontendEndpointError::Address;
    if(value.pin.size()!=6 || value.pin.find_first_not_of("0123456789")!=std::string::npos)return FrontendEndpointError::Pin;
    if(!value.port || value.port>65535)return FrontendEndpointError::Port;
    return FrontendEndpointError::None;
}
inline bool valid_frontend_endpoint(const FrontendEndpoint& value){return frontend_endpoint_error(value)==FrontendEndpointError::None;}
}
