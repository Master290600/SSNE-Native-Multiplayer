#pragma once
#include <array>
#include <algorithm>
#include <string>
#include "lan_discovery.h"
namespace port::coop {
enum class FrontText {Multiplayer,Host,Join,Local,Back,Start,Address,Pin,Port,Connecting,Cancel,Retry,Connection,Failed,Version,Full,Lost,Invalid,Typing,Bots,AddBot,RemoveBot,Skill,Easy,Normal,Hard,Free,HostHelp,JoinHelp,BotHelp,HostKeys,ClientKeys,ModeFull,Search,Searching,None,Leave,More,InvalidAddress,InvalidPin,InvalidPort,RoomsHelp,LanExpired,AfkEnter,AfkLeave,Options,PlayerName,ProfileName,Enemies,AmmoCapacity,AmmoPickups,Unlimited,RulesHelp,ClientRules,NameLocked,NameTyping,SaveFailed,Survival,Watch,PlayAgain,Watching,WatchHelp,Countdown,RoundOver,Wave,Time,NextRound,BrowserOptions,Refresh,Sort,Filter,AllModes,AvailableOnly,AllRooms,Ping,Map,Players,Lobby,InProgress,Spectators,ModeCoop,ModeDeathmatch,ModeFlag,ModeBomb,SortPing,SortName,SortPlayers,SortMode,SortMap,Waiting,RoundWait,SurvivalLoading,SurvivalLoadFailed,StatusUnknown,Stale,RefreshHelp,PlayerOptions,PlayerOptionsHelp,SurvivalHelp,CustomLevels,OriginalLevels,CustomLevelUnavailable,LobbyYou,LobbyHost,LobbyPlayer,LobbyBot,LobbyOpenSlot,LobbyChooseMap,LobbyChooseCharacter,LobbyHostChoosing,CustomMapRequesting,CustomMapDownloading,CustomMapVerifying,CustomMapInstalling,CustomMapDownloadHelp,LobbyReady,LobbyNotReady,LobbyReadyYes,LobbyReadyNo,LobbyNoMap,LobbySelectFirst,LobbyWaitingReady,Serious,ExitGame,LobbySettings,InternetGame,InvitationCode,CreateInvitation,CopyInvitation,InvitationCopied,InvitationHelp,JoinInvitationHelp,InvitePreparing,InviteWaiting,InviteDirect,InviteRelay,InviteFailed,InviteInvalid,InviteRoute,NoCustomLevels,ChooseCharacter,Online,OnlineSetup,OnlineJoin,OnlineHost,OnlineCharacter,OnlineNickname,OnlineGameName,OnlinePassword,OnlineUsePassword,NoPassword,OnlinePinHelp,ConfigureGame,InviteCopyWaiting,InviteCopyCopied,InviteCopyFailed,EnterCode,JoinByIP,ConnectionDetails,LanPinOn,LanPinOff,DirectHelp,HostConnectionHelp,HostFailed,SynchronizingHost,ConnectionTimeout,LobbyWaitingForHost,LobbyWaitingForMap,Count};
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
        {{"Could not save session settings.","Sitzungsoptionen konnten nicht gespeichert werden.","Impossible d'enregistrer les options."}},
        {{"Survival","Survival","Survie"}},
        {{"Spectate","Zuschauen","Observer"}},
        {{"Join the action","Mitspielen","Jouer"}},
        {{"Watching","Zuschauen","Observation"}},
        {{"Left/Right: player | Esc: menu","Links/Rechts: Spieler | Esc: Men\374","Gauche/Droite : joueur | Echap : menu"}},
        {{"Starting in","Start in","D\351but dans"}},
        {{"Round over","Runde beendet","Manche termin\351e"}},
        {{"Wave","Welle","Vague"}},
        {{"Time","Zeit","Temps"}},
        {{"Next round","N\344chste Runde","Manche suivante"}},
        {{"Server browser","Serverbrowser","Serveurs"}},
        {{"Refresh","Aktualisieren","Actualiser"}},
        {{"Sort","Sortierung","Tri"}},
        {{"Filter","Filter","Filtre"}},
        {{"All modes","Alle Modi","Tous les modes"}},
        {{"Free slots only","Nur freie Pl\344tze","Places libres"}},
        {{"All games","Alle Sitzungen","Toutes les parties"}},
        {{"Ping","Ping","Ping"}},
        {{"Map","Karte","Carte"}},
        {{"Players","Spieler","Joueurs"}},
        {{"Lobby","Lobby","Salon"}},
        {{"Playing","Im Spiel","En cours"}},
        {{"Spectators","Zuschauer","Spectateurs"}},
        {{"Co-op","Koop","Coop"}},
        {{"Deathmatch","Deathmatch","Deathmatch"}},
        {{"Hold the Flag","Behalte die Flagge!","Capture du drapeau"}},
        {{"Pass the Bomb","Gib die Bombe ab","La bombe mistigri"}},
        {{"Ping","Ping","Ping"}},
        {{"Name","Name","Nom"}},
        {{"Players","Spieler","Joueurs"}},
        {{"Mode","Modus","Mode"}},
        {{"Map","Karte","Carte"}},
        {{"Waiting for players","Warte auf Spieler","En attente"}},
        {{"You join the next round.","Du spielst in der n\344chsten Runde mit.","Vous jouerez \340 la prochaine manche."}},
        {{"Loading Survival...","Survival wird geladen...","Chargement de la survie..."}},
        {{"Cannot load Survival. Esc: menu.","Survival kann nicht geladen werden. Esc: Men\374.","Survie indisponible. Echap : menu."}},
        {{"Unknown","Unbekannt","Inconnu"}},
        {{"Stale","Veraltet","Ancien"}},
        {{"Refresh in Server browser to update these games.","Sitzungen im Serverbrowser aktualisieren.","Actualisez ces parties dans le menu Serveurs."}},
        {{"Player settings","Spieleroptionen","Options du joueur"}},
        {{"Set your player name before joining.","Vor dem Beitritt den Spielernamen festlegen.","Choisissez votre nom avant de rejoindre une partie."}},
        {{"Survive increasingly deadly enemy waves, alone or together. Every second counts!","\334berlebe allein oder im Team immer st\344rkere Gegnerwellen. Jede Sekunde z\344hlt!","Survivez seul ou en \351quipe \340 des vagues d'ennemis toujours plus fortes. Chaque seconde compte !"}},
        {{"Custom Levels","Custom Level","Niveaux custom"}},
        {{"Original Levels","Original-Level","Niveaux originaux"}},
        {{"Custom level unavailable.\nTransfer or verification failed.\nPlease try joining again.","Custom Level nicht verf\374gbar.\nDownload oder Pr\374fung fehlgeschlagen.\nBitte erneut beitreten.","Niveau custom indisponible.\nTransfert ou v\351rification en \351chec.\nVeuillez rejoindre \340 nouveau."}},
        {{"You","Du","Vous"}},
        {{"Host","Host","H\364te"}},
        {{"Player","Spieler","Joueur"}},
        {{"Bot","Bot","Bot"}},
        {{"Open slot","Freier Platz","Place libre"}},
        {{"Choose map","Karte w\344hlen","Choisir la carte"}},
        {{"Choose character: mouse / arrows","Figur w\344hlen: Maus / Pfeile","Personnage : souris / fl\350ches"}},
        {{"The host chooses the map.","Der Host w\344hlt die Karte.","L\047h\364te choisit la carte."}},
        {{"Requesting map...","Karte anfordern...","Demande de la carte..."}},
        {{"Downloading map...","Karte herunterladen...","T\351l\351chargement..."}},
        {{"Verifying map...","Karte pr\374fen...","V\351rification..."}},
        {{"Preparing map...","Karte vorbereiten...","Pr\351paration..."}},
        {{"The host's custom level is\ndownloaded automatically.\nCancel: leave the game.","Das Custom Level des Hosts\nwird automatisch geladen.\nAbbrechen: Sitzung verlassen.","Le niveau de l'h\364te est\nt\351l\351charg\351 automatiquement.\nAnnuler : quitter la partie."}},
        {{"Ready","Bereit","Pr\352t"}},{{"Not ready","Nicht bereit","Pas pr\352t"}},
        {{"Ready: yes","Bereit: ja","Pr\352t : oui"}},{{"Ready: no","Bereit: nein","Pr\352t : non"}},
        {{"No map selected","Keine Karte gew\344hlt","Aucune carte choisie"}},
        {{"Choose map and difficulty first.","Zuerst Karte und Schwierigkeit w\344hlen.","Choisissez carte et difficult\351."}},
        {{"Waiting for ready players.","Warte auf bereite Spieler.","En attente des joueurs pr\352ts."}},
        {{"Serious","Serious","Serious"}},
        {{"Exit","Verlassen","Quitter"}},
        {{"Game settings","Spielregeln","R\350gles de partie"}},
        {{"Internet game","Internet-Spiel","Partie Internet"}},
        {{"Invitation code","Einladungscode","Code d'invitation"}},
        {{"Create invite","Einladen","Inviter"}},
        {{"Copy code","Code kopieren","Copier le code"}},
        {{"Code copied.","Code kopiert.","Code copi\351."}},
        {{"Create an invitation and send the code to your friends. Keep this game open.","Einladung erstellen und den Code an Freunde senden. Die Sitzung ge\366ffnet lassen.","Cr\351ez une invitation et envoyez le code. Gardez cette partie ouverte."}},
        {{"Paste the invitation code. The connection is set up automatically.","Einladungscode einf\374gen. Die Verbindung wird automatisch aufgebaut.","Collez le code d'invitation. La connexion est automatique."}},
        {{"Preparing invitation...","Einladung wird vorbereitet...","Pr\351paration de l'invitation..."}},
        {{"Waiting for friends.","Warte auf Freunde.","En attente des amis."}},
        {{"Direct connection","Direkte Verbindung","Connexion directe"}},
        {{"Relay connection","Relay-Verbindung","Connexion par relais"}},
        {{"Internet connection unavailable. Try again. Some networks require a configured relay.","Internet-Verbindung nicht erreichbar. Erneut versuchen. Manche Anschl\374sse ben\366tigen einen eingerichteten Relay.","Connexion Internet indisponible. R\351essayez. Certains r\351seaux n\351cessitent un relais configur\351."}},
        {{"Invalid invitation code. Copy the complete code and try again.","Ung\374ltiger Einladungscode. Den vollst\344ndigen Code kopieren und erneut versuchen.","Code d'invitation invalide. Copiez le code complet et r\351essayez."}},
        {{"Connection","Verbindung","Connexion"}},
        {{"No custom maps installed","Keine Custom-Karten installiert","Aucune carte custom install\351e"}},
        {{"Choose character","Figur w\344hlen","Choisir le personnage"}},
        {{"Online","Online","En ligne"}},
        {{"Online Setup","Online-Setup","Configuration en ligne"}},
        {{"Join Existing Game","Spiel beitreten","Rejoindre"}},
        {{"Host New Game","Spiel erstellen","H\351berger"}},
        {{"Player Character","Spieler-Charakter","Personnage du joueur"}},
        {{"Player Nickname","Spieler-Name","Pseudo joueur"}},
        {{"Host Game Name","Host-Spiel-Name","Nom de la partie h\351berg\351e"}},
        {{"Player Password","Spieler-Passwort","Mot de passe joueur"}},
        {{"Use Password","Passwort verwenden","Utiliser mot de passe"}},
        {{"<None>","<Keines>","<Aucun>"}},
        {{"Password: six-digit PIN.","Passwort: sechsstellige PIN.","Mot de passe : code de six chiffres."}},
        {{"Configure game","Spiel einrichten","Configurer la partie"}},
        {{"Wait...","Warte...","Patientez..."}},
        {{"Copied!","Kopiert!","Copi\351 !"}},
        {{"Error","Fehler","Erreur"}},
        {{"Enter invitation code","Einladungscode eingeben","Saisir un code"}},
        {{"Join by IP","Per IP verbinden","Connexion par IP"}},
        {{"Connection details","Verbindungsdetails","Param\350tres de connexion"}},
        {{"LAN PIN: on","LAN-PIN: Ein","Code en r\351seau : oui"}},
        {{"LAN PIN: off","LAN-PIN: Aus","Code en r\351seau : non"}},
        {{"Use the address, port and PIN from the host.","Adresse, Port und PIN des Hosts eingeben.","Saisissez l'adresse, le port et le code de l'h\364te."}},
        {{"Direct IP connections use the host PIN. Enable the LAN PIN to ask for it in the server browser as well.","Direkte IP-Verbindungen verwenden die Host-PIN. Mit LAN-PIN fragt auch der Serverbrowser danach.","Le code est requis pour une connexion par IP. Activez le code en r\351seau pour le demander aussi dans la liste des parties."}},
        {{"Cannot start the host. Check whether the port is already in use.","Spiel konnte nicht erstellt werden. Ist der Port schon belegt?","Cr\351ation impossible. V\351rifiez si le port est d\351j\340 utilis\351."}},
        {{"Waiting for game data from the host.","Warte auf Spieldaten vom Host.","En attente des donn\351es de l'h\364te."}},
        {{"The connection to the host timed out.","Zeitlimit der Verbindung zum Host erreicht.","Le d\351lai de connexion \340 l'h\364te est d\351pass\351."}},
        {{"Waiting for the host.","Warte auf den Host.","En attente de l'h\364te."}},
        {{"The host picks the map.","Host w\344hlt die Karte.","L'h\364te choisit la carte."}}
    }};
    const auto index=unsigned(key);return index<rows.size()?rows[index][language<3?language:0]:"";
}
inline std::string frontend_factor_value(unsigned factor,unsigned language){return factor<=1?frontend_text(FrontText::Normal,language):"x"+std::to_string(factor);}
inline std::string frontend_ammo_capacity_value(unsigned code,unsigned language){return code==2?frontend_text(FrontText::Unlimited,language):frontend_factor_value(code==0?1:code==1?2:code,language);}
inline FrontText frontend_room_state(const FrontendRoom& room,uint64_t now){
    switch(lan_room_state(room,now)){
    case LanRoomState::Lobby:return FrontText::Lobby;
    case LanRoomState::Running:return FrontText::InProgress;
    case LanRoomState::Stale:return FrontText::Stale;
    default:return FrontText::StatusUnknown;
    }
}
inline std::string frontend_room_ping(const FrontendRoom& room,uint64_t now,bool compact=false){
    if(!lan_room_fresh(room,now) || room.ping_ms==UINT32_MAX)return compact?"?ms":"?";
    return std::to_string(std::min(room.ping_ms,999u))+(room.ping_ms>999?"+":"")+(compact?"ms":" ms");
}
inline std::string frontend_room_row(const FrontendRoom& room,std::string name,unsigned language,uint64_t now){
    const auto suffix=" ("+std::to_string(room.players)+"/"+std::to_string(room.capacity)+") "+
        frontend_room_ping(room,now,true)+" "+frontend_text(frontend_room_state(room,now),language);
    // Preserve space for occupancy, measured RTT and status in all PAL fonts.
    // The full name remains in the selected row's native description widget.
    const auto limit=std::min<size_t>(16,suffix.size()<36?36-suffix.size():0);
    if(name.size()>limit)name=limit>3?name.substr(0,limit-3)+"...":name.substr(0,limit);
    return name+suffix;
}
struct FrontendEndpoint {std::string address="127.0.0.1",pin="123456";unsigned port=27341;bool internet=false;std::string invitation_code{};};
enum class FrontendEndpointError {None,Address,Pin,Port};
inline FrontendEndpointError frontend_endpoint_error(const FrontendEndpoint& value){
    if(value.address.empty() || value.address.size()>253 || value.address.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:")!=std::string::npos)return FrontendEndpointError::Address;
    if(value.pin.size()!=6 || value.pin.find_first_not_of("0123456789")!=std::string::npos)return FrontendEndpointError::Pin;
    if(!value.port || value.port>65535)return FrontendEndpointError::Port;
    return FrontendEndpointError::None;
}
inline bool valid_frontend_endpoint(const FrontendEndpoint& value){return frontend_endpoint_error(value)==FrontendEndpointError::None;}
}
