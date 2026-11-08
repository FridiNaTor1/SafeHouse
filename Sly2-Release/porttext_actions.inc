// English comes from the shared registry. The remaining columns follow LANGUAGE
// from French through Japanese, in the same order as the main port text table.
using cane::sly2input::Action;
struct ActionText { Action action; const char* text[CLANG - 1]; };
#define ACTION(name, ...) {Action::name, {__VA_ARGS__}}
constexpr ActionText s_actionNames[] = {
    ACTION(MoveUp, "Avancer", "Avanza", "Vorwärts", "Avanzar", "Vooruit", "Avançar", "Frem", "Eteenpäin", "Frem", "Framåt", "前にすすむ"),
    ACTION(MoveDown, "Reculer", "Indietreggia", "Rückwärts", "Retroceder", "Achteruit", "Recuar", "Tilbage", "Taaksepäin", "Tilbake", "Bakåt", "うしろにさがる"),
    ACTION(MoveLeft, "Aller à gauche", "Vai a sinistra", "Nach links", "Ir a la izquierda", "Naar links", "Ir para a esquerda", "Til venstre", "Vasemmalle", "Til venstre", "Åt vänster", "左にうごく"),
    ACTION(MoveRight, "Aller à droite", "Vai a destra", "Nach rechts", "Ir a la derecha", "Naar rechts", "Ir para a direita", "Til højre", "Oikealle", "Til høyre", "Åt höger", "右にうごく"),
    ACTION(Attack, "Attaquer", "Attacca", "Angreifen", "Atacar", "Aanvallen", "Atacar", "Angrib", "Hyökkää", "Angrip", "Attackera", "こうげき"),
    ACTION(Interact, "Interagir", "Interagisci", "Interagieren", "Interactuar", "Interactie", "Interagir", "Interager", "Toimi", "Samhandle", "Interagera", "しらべる"),
    ACTION(Jump, "Sauter", "Salta", "Springen", "Saltar", "Springen", "Saltar", "Hop", "Hyppää", "Hopp", "Hoppa", "ジャンプ"),
    ACTION(Pause, "Pause", "Pausa", "Pause", "Pausa", "Pauze", "Pausa", "Pause", "Tauko", "Pause", "Paus", "ポーズ"),
    ACTION(GadgetMenu, "Ouvrir le menu des gadgets", "Apri menu gadget", "Gadget-Menü öffnen", "Abrir menú de artilugios", "Gadgetmenu openen", "Abrir menu de engenhocas", "Åbn gadgetmenu", "Avaa laitevalikko", "Åpne dingsmenyen", "Öppna prylmenyn", "ガジェットメニュー"),
    ACTION(Gadget1, "Utiliser le gadget 1", "Usa gadget 1", "Gadget 1 einsetzen", "Usar artilugio 1", "Gadget 1 gebruiken", "Usar engenhoca 1", "Brug gadget 1", "Käytä laitetta 1", "Bruk dings 1", "Använd pryl 1", "ガジェット1を使う"),
    ACTION(Sprint, "Courir", "Corri", "Sprinten", "Correr", "Rennen", "Correr", "Løb", "Juokse", "Løp", "Spring", "ダッシュ"),
    ACTION(Gadget2, "Utiliser le gadget 2", "Usa gadget 2", "Gadget 2 einsetzen", "Usar artilugio 2", "Gadget 2 gebruiken", "Usar engenhoca 2", "Brug gadget 2", "Käytä laitetta 2", "Bruk dings 2", "Använd pryl 2", "ガジェット2を使う"),
    ACTION(Gadget3, "Utiliser le gadget 3", "Usa gadget 3", "Gadget 3 einsetzen", "Usar artilugio 3", "Gadget 3 gebruiken", "Usar engenhoca 3", "Brug gadget 3", "Käytä laitetta 3", "Bruk dings 3", "Använd pryl 3", "ガジェット3を使う"),
    ACTION(WorldStats, "Statistiques du monde", "Statistiche del mondo", "Weltstatistik", "Estadísticas del mundo", "Wereldstatistieken", "Estatísticas do mundo", "Verdensstatistik", "Maailman tilastot", "Verdensstatistikk", "Världsstatistik", "ワールドのきろく"),
    ACTION(Binocucom, "Ouvrir le Binocom", "Apri il Binocom", "Späh-Kom öffnen", "Abrir el bino-com", "Contactkijker openen", "Abrir o Binocucom", "Åbn binocucom", "Avaa radiokiikari", "Åpne kikkertwalkie", "Öppna binokucom", "ゴーグルを使う"),
    ACTION(DoubleJump, "Double saut", "Doppio salto", "Doppelsprung", "Doble salto", "Dubbele sprong", "Salto duplo", "Dobbelthop", "Tuplahyppy", "Dobbelthopp", "Dubbelhopp", "2だんジャンプ"),
    ACTION(ClimbJump, "Sauter en grimpant", "Salta in arrampicata", "Beim Klettern abspringen", "Saltar al trepar", "Afzetten bij klimmen", "Saltar ao trepar", "Hop fra klatrested", "Hyppää kiivetessä", "Hopp fra klatrested", "Hoppa från klätterläge", "のぼりながらジャンプ"),
    ACTION(HangJump, "Sauter d'un rebord", "Salta dal bordo", "Von Kante abspringen", "Saltar desde un saliente", "Van rand springen", "Saltar de uma beira", "Hop fra en kant", "Hyppää reunalta", "Hopp fra en kant", "Hoppa från en kant", "ぶらさがりジャンプ"),
    ACTION(SlyPickpocket, "Faire les poches", "Borseggia", "Taschendiebstahl", "Robar bolsillos", "Zakkenrollen", "Bater carteiras", "Stjæl fra lommer", "Taskuvarkaus", "Stjel fra lommer", "Stjäl ur fickor", "スリをする"),
    ACTION(SlyUppercut, "Projeter un ennemi", "Lancia in aria", "Gegner hochschlagen", "Lanzar al enemigo", "Vijand omhoog slaan", "Lançar inimigo ao ar", "Slå fjenden op", "Lyö vihollinen ilmaan", "Slå fienden opp", "Slå upp fienden", "アッパーカット"),
    ACTION(SlyAirAttack, "Coup de canne aérien", "Colpo aereo col bastone", "Stockangriff in der Luft", "Golpe aéreo con bastón", "Stokaanval in de lucht", "Ataque aéreo com bengala", "Stokangreb i luften", "Ilmaisku kepillä", "Stokkangrep i lufta", "Käppattack i luften", "ジャンプ中にステッキ"),
    ACTION(Paraglider, "Ouvrir / tenir le parapente", "Apri / tieni il parapendio", "Gleitschirm öffnen / halten", "Abrir / mantener parapente", "Paraglider openen / houden", "Abrir / manter parapente", "Åbn / hold paraglider", "Avaa / pidä liidintä", "Åpne / hold paraglider", "Fäll ut / håll glidflygaren", "パラグライダー"),
    ACTION(SlyDive, "Plonger en l'air", "Picchiata", "Sturzangriff", "Ataque en picado", "Duikaanval", "Ataque em mergulho", "Dykangreb", "Syöksyisku", "Stupangrep", "Dykattack", "ダイブこうげき"),
    ACTION(BentleySwing, "Frapper au lance-fléchettes", "Colpisci col lancia-dardi", "Mit Blasrohr zuschlagen", "Golpear con el lanzadardos", "Slaan met het dartgeweer", "Golpear com o lança-dardos", "Slå med pilegeværet", "Lyö nuolipyssyllä", "Slå med pilgeværet", "Slå med pilgeväret", "ダーツガンでなぐる"),
    ACTION(BentleyDart, "Tirer une fléchette", "Spara un dardo", "Betäubungs- / Lärmpfeil", "Disparar dardo de sueño / ruido", "Slaap- / geluidspijl schieten", "Disparar dardo de sono / ruído", "Skyd sove- / støjpil", "Ammu uni- / ääninuoli", "Skyt sove- / lydpil", "Skjut sömn- / ljudpil", "ダーツをうつ"),
    ACTION(BentleySpecial, "Poser bombe / objet spécial", "Piazza bomba / oggetto speciale", "Bombe / Spezialobjekt einsetzen", "Poner bomba / usar objeto", "Bom plaatsen / voorwerp gebruiken", "Colocar bomba / usar objeto", "Læg bombe / brug specialgenstand", "Aseta pommi / käytä erikoisesinettä", "Legg bombe / bruk spesialgjenstand", "Placera bomb / använd specialföremål", "バクダン / アイテム"),
    ACTION(MurrayUppercut, "Uppercut / soulever", "Montante / solleva", "Aufwärtshaken / hochheben", "Gancho / levantar", "Uppercut / optillen", "Gancho / levantar", "Uppercut / løft", "Kohokoukku / nosta", "Uppercut / løft", "Uppercut / lyft", "アッパー / もちあげる"),
    ACTION(MurrayFlop, "Plaquage foudroyant", "Schianto tonante", "Donnerplumpser", "Panzazo atronador", "Donderplons", "Queda trovejante", "Tordenplask", "Ukkosmäjäys", "Tordenplask", "Åskplask", "サンダーフロップ"),
    ACTION(CarryThrow, "Lancer l'objet porté", "Lancia l'oggetto in mano", "Getragenes Objekt werfen", "Lanzar objeto cargado", "Gedragen voorwerp gooien", "Atirar objeto transportado", "Kast båret genstand", "Heitä kannettu esine", "Kast båret gjenstand", "Kasta buret föremål", "もったものをなげる"),
    ACTION(ThrowRelease, "Lancer après avoir visé", "Lancia dopo aver mirato", "Gezielt werfen", "Lanzar tras apuntar", "Gericht gooien", "Atirar após apontar", "Kast efter at have sigtet", "Heitä tähdätty esine", "Kast etter å ha siktet", "Kasta efter att ha siktat", "ねらってなげる"),
    ACTION(ThrowCancel, "Annuler le lancer / poser", "Annulla lancio / posa", "Wurf abbrechen / ablegen", "Cancelar lanzamiento / soltar", "Worp annuleren / neerzetten", "Cancelar arremesso / pousar", "Annuller kast / sæt ned", "Peru heitto / laske alas", "Avbryt kast / sett ned", "Avbryt kast / sätt ner", "なげずにおろす"),
    ACTION(ReconClose, "Fermer le Binocom", "Chiudi il Binocom", "Späh-Kom schließen", "Cerrar el bino-com", "Contactkijker sluiten", "Fechar o Binocucom", "Luk binocucom", "Sulje radiokiikari", "Lukk kikkertwalkie", "Stäng binokucom", "ゴーグルをしまう"),
    ACTION(Photo, "Prendre une photo", "Scatta una foto", "Foto aufnehmen", "Hacer una foto", "Foto maken", "Tirar uma fotografia", "Tag et billede", "Ota kuva", "Ta et bilde", "Ta ett foto", "しゃしんをとる"),
    ACTION(ZoomIn, "Zoom avant", "Ingrandisci", "Heranzoomen", "Acercar", "Inzoomen", "Aproximar", "Zoom ind", "Lähennä", "Zoom inn", "Zooma in", "ズームイン"),
    ACTION(ZoomOut, "Zoom arrière", "Riduci zoom", "Herauszoomen", "Alejar", "Uitzoomen", "Afastar", "Zoom ud", "Loitonna", "Zoom ut", "Zooma ut", "ズームアウト"),
    ACTION(TurretFire, "Tirer avec la tourelle", "Spara con la torretta", "Geschütz abfeuern", "Disparar torreta", "Geschut afvuren", "Disparar torre", "Skyd med tårnet", "Ammu tykillä", "Skyt med tårnet", "Skjut med tornet", "ほうだいでうつ"),
    ACTION(VehicleDrive, "Accélérer", "Accelera", "Beschleunigen", "Acelerar", "Gas geven", "Acelerar", "Giv gas", "Kiihdytä", "Gi gass", "Gasa", "アクセル"),
    ACTION(VehiclePrimary, "Arme principale", "Arma primaria", "Hauptwaffe", "Arma principal", "Hoofdwapen", "Arma principal", "Primært våben", "Pääase", "Primærvåpen", "Huvudvapen", "メインウェポン"),
    ACTION(VehicleSecondary, "Arme secondaire", "Arma secondaria", "Zweitwaffe", "Arma secundaria", "Secundair wapen", "Arma secundária", "Sekundært våben", "Kakkosase", "Sekundærvåpen", "Sekundärvapen", "サブウェポン"),
    ACTION(TankLeftForward, "Chenille gauche avant", "Cingolo sinistro avanti", "Linke Kette vorwärts", "Oruga izquierda adelante", "Linkerrups vooruit", "Lagarta esquerda para a frente", "Venstre bælte frem", "Vasen tela eteen", "Venstre belte frem", "Vänster band framåt", "左キャタピラ前進"),
    ACTION(TankLeftReverse, "Chenille gauche arrière", "Cingolo sinistro indietro", "Linke Kette rückwärts", "Oruga izquierda atrás", "Linkerrups achteruit", "Lagarta esquerda para trás", "Venstre bælte tilbage", "Vasen tela taakse", "Venstre belte tilbake", "Vänster band bakåt", "左キャタピラ後退"),
    ACTION(TankRightForward, "Chenille droite avant", "Cingolo destro avanti", "Rechte Kette vorwärts", "Oruga derecha adelante", "Rechterrups vooruit", "Lagarta direita para a frente", "Højre bælte frem", "Oikea tela eteen", "Høyre belte frem", "Höger band framåt", "右キャタピラ前進"),
    ACTION(TankRightReverse, "Chenille droite arrière", "Cingolo destro indietro", "Rechte Kette rückwärts", "Oruga derecha atrás", "Rechterrups achteruit", "Lagarta direita para trás", "Højre bælte tilbage", "Oikea tela taakse", "Høyre belte tilbake", "Höger band bakåt", "右キャタピラ後退"),
    ACTION(TankBothForward, "Deux chenilles avant", "Entrambi i cingoli avanti", "Beide Ketten vorwärts", "Ambas orugas adelante", "Beide rupsen vooruit", "Ambas as lagartas para a frente", "Begge bælter frem", "Molemmat telat eteen", "Begge belter frem", "Båda banden framåt", "両キャタピラ前進"),
    ACTION(TankBothReverse, "Deux chenilles arrière", "Entrambi i cingoli indietro", "Beide Ketten rückwärts", "Ambas orugas atrás", "Beide rupsen achteruit", "Ambas as lagartas para trás", "Begge bælter tilbage", "Molemmat telat taakse", "Begge belter tilbake", "Båda banden bakåt", "両キャタピラ後退"),
    ACTION(RcUp, "Monter (hélico RC)", "Sali (elicottero RC)", "Steigen (RC-Heli)", "Subir (helicóptero RC)", "Stijgen (RC-helikopter)", "Subir (helicóptero RC)", "Stig (RC-helikopter)", "Nouse (RC-kopteri)", "Stig (RC-helikopter)", "Stig (RC-helikopter)", "ラジコンヘリ: 上へ"),
    ACTION(RcDown, "Descendre (hélico RC)", "Scendi (elicottero RC)", "Sinken (RC-Heli)", "Bajar (helicóptero RC)", "Dalen (RC-helikopter)", "Descer (helicóptero RC)", "Synk (RC-helikopter)", "Laskeudu (RC-kopteri)", "Synk (RC-helikopter)", "Sänk (RC-helikopter)", "ラジコンヘリ: 下へ"),
    ACTION(RcFire, "Tirer (hélico RC)", "Spara (elicottero RC)", "Feuern (RC-Heli)", "Disparar (helicóptero RC)", "Schieten (RC-helikopter)", "Disparar (helicóptero RC)", "Skyd (RC-helikopter)", "Ammu (RC-kopteri)", "Skyt (RC-helikopter)", "Skjut (RC-helikopter)", "ラジコンヘリ: うつ"),
    ACTION(RcBomb, "Larguer une bombe (RC)", "Sgancia bomba (RC)", "Bombe abwerfen (RC)", "Soltar bomba (RC)", "Bom laten vallen (RC)", "Largar bomba (RC)", "Kast bombe (RC)", "Pudota pommi (RC)", "Slipp bombe (RC)", "Släpp bomb (RC)", "ラジコンヘリ: バクダン"),
    ACTION(RcCamera, "Caméra de l'hélico RC", "Telecamera elicottero RC", "RC-Heli-Kamera", "Cámara del helicóptero RC", "RC-helikoptercamera", "Câmara do helicóptero RC", "RC-helikopterkamera", "RC-kopterin kamera", "RC-helikopterkamera", "RC-helikopterns kamera", "ラジコンヘリ: カメラ"),
    ACTION(HackFire, "Tirer (piratage)", "Spara (hacking)", "Schießen (Hacken)", "Disparar (hackeo)", "Schieten (hacken)", "Disparar (pirataria)", "Skyd (hacking)", "Ammu (hakkerointi)", "Skyt (hacking)", "Skjut (hackning)", "ハッキングでうつ"),
    ACTION(DanceSquare, "Danse : gauche / Carré", "Danza: sinistra / Quadrato", "Tanz: links / Quadrat", "Baile: izquierda / Cuadrado", "Dans: links / Vierkant", "Dança: esquerda / Quadrado", "Dans: venstre / Firkant", "Tanssi: vasen / Neliö", "Dans: venstre / Firkant", "Dans: vänster / Fyrkant", "ダンス: 左 / 四角"),
    ACTION(DanceCircle, "Danse : droite / Rond", "Danza: destra / Cerchio", "Tanz: rechts / Kreis", "Baile: derecha / Círculo", "Dans: rechts / Cirkel", "Dança: direita / Círculo", "Dans: højre / Cirkel", "Tanssi: oikea / Ympyrä", "Dans: høyre / Sirkel", "Dans: höger / Cirkel", "ダンス: 右 / まる"),
    ACTION(DanceCross, "Danse : bas / Croix", "Danza: giù / Croce", "Tanz: unten / Kreuz", "Baile: abajo / Equis", "Dans: omlaag / Kruis", "Dança: baixo / Cruz", "Dans: ned / Kryds", "Tanssi: alas / Risti", "Dans: ned / Kryss", "Dans: ner / Kryss", "ダンス: 下 / バツ"),
    ACTION(DanceTriangle, "Danse : haut / Triangle", "Danza: su / Triangolo", "Tanz: oben / Dreieck", "Baile: arriba / Triángulo", "Dans: omhoog / Driehoek", "Dança: cima / Triângulo", "Dans: op / Trekant", "Tanssi: ylös / Kolmio", "Dans: opp / Trekant", "Dans: upp / Triangel", "ダンス: 上 / 三角"),
    ACTION(VaultConfirm, "Valider le chiffre du coffre", "Conferma cifra cassaforte", "Tresorziffer bestätigen", "Confirmar cifra de la caja fuerte", "Kluizencijfer bevestigen", "Confirmar dígito do cofre", "Bekræft ciffer i pengeskab", "Vahvista kassakaapin numero", "Bekreft siffer i safen", "Bekräfta kassaskåpssiffra", "きんこの数字を入れる"),
    ACTION(VaultCancel, "Annuler la saisie du coffre", "Annulla inserimento cassaforte", "Tresoreingabe abbrechen", "Cancelar entrada de la caja fuerte", "Kluisinvoer annuleren", "Cancelar entrada do cofre", "Annuller indtastning", "Peru kassakaapin syöttö", "Avbryt inntasting", "Avbryt inmatning", "きんこの入力をやめる"),
    ACTION(NoclipAscend, "Monter", "Sali", "Aufsteigen", "Subir", "Stijgen", "Subir", "Stig", "Nouse", "Stig", "Stig", "上へ"),
    ACTION(NoclipDescend, "Descendre", "Scendi", "Absteigen", "Bajar", "Dalen", "Descer", "Synk", "Laskeudu", "Synk", "Sänk", "下へ"),
    ACTION(NoclipFast, "Accélérer", "Accelera", "Schneller", "Acelerar", "Versnellen", "Acelerar", "Øg farten", "Kiihdytä", "Øk farten", "Öka farten", "スピードアップ"),
    ACTION(NoclipSlow, "Ralentir", "Rallenta", "Langsamer", "Frenar", "Vertragen", "Abrandar", "Sænk farten", "Hidasta", "Senk farten", "Sänk farten", "スピードダウン"),
};
#undef ACTION

struct ActionGroupText { std::string_view group; const char* text[CLANG - 1]; };
constexpr ActionGroupText s_actionGroups[] = {
    {"Movement", {"Déplacements", "Movimento", "Bewegung", "Movimiento", "Beweging", "Movimento", "Bevægelse", "Liikkuminen", "Bevegelse", "Rörelse", "移動"}},
    {"Common actions", {"Actions communes", "Azioni comuni", "Allgemeine Aktionen", "Acciones comunes", "Algemene acties", "Ações comuns", "Fælles handlinger", "Yleiset toiminnot", "Vanlige handlinger", "Vanliga åtgärder", "きほん操作"}},
    {"Gadgets", {"Gadgets", "Gadget", "Gadgets", "Artilugios", "Gadgets", "Engenhocas", "Gadgets", "Laitteet", "Dingser", "Prylar", "ガジェット"}},
    {"Climbing and ledges", {"Escalade et rebords", "Arrampicata e bordi", "Klettern und Kanten", "Escalada y salientes", "Klimmen en randen", "Escalada e beiras", "Klatring og kanter", "Kiipeily ja reunat", "Klatring og kanter", "Klättring och kanter", "のぼる / ぶらさがる"}},
    {"Sly", {"Sly", "Sly", "Sly", "Sly", "Sly", "Sly", "Sly", "Sly", "Sly", "Sly", "スライ"}},
    {"Bentley", {"Bentley", "Bentley", "Bentley", "Bentley", "Bentley", "Bentley", "Bentley", "Bentley", "Bentley", "Bentley", "ベントレー"}},
    {"Murray", {"Murray", "Murray", "Murray", "Murray", "Murray", "Murray", "Murray", "Murray", "Murray", "Murray", "マーレー"}},
    {"Carrying and throwing", {"Porter et lancer", "Trasporto e lancio", "Tragen und Werfen", "Cargar y lanzar", "Dragen en gooien", "Transportar e atirar", "Bære og kaste", "Kantaminen ja heittäminen", "Bære og kaste", "Bära och kasta", "はこぶ / なげる"}},
    {"Binocucom", {"Binocom", "Binocom", "Späh-Kom", "Bino-com", "Contactkijker", "Binocucom", "Binocucom", "Radiokiikari", "Kikkertwalkie", "Binokucom", "ゴーグル"}},
    {"Vehicles and turrets", {"Véhicules et tourelles", "Veicoli e torrette", "Fahrzeuge und Geschütze", "Vehículos y torretas", "Voertuigen en geschut", "Veículos e torres", "Køretøjer og tårne", "Ajoneuvot ja tykit", "Kjøretøy og tårn", "Fordon och torn", "のりもの / ほうだい"}},
    {"RC helicopter", {"Hélico RC", "Elicottero RC", "RC-Hubschrauber", "Helicóptero RC", "RC-helikopter", "Helicóptero RC", "RC-helikopter", "RC-kopteri", "RC-helikopter", "RC-helikopter", "ラジコンヘリ"}},
    {"Minigames", {"Mini-jeux", "Minigiochi", "Minispiele", "Minijuegos", "Minigames", "Minijogos", "Minispil", "Minipelit", "Minispill", "Minispel", "ミニゲーム"}},
    {"Noclip", {"Sans collision", "Senza collisioni", "Ohne Kollision", "Sin colisiones", "Zonder botsingen", "Sem colisões", "Uden kollision", "Lento seinien läpi", "Uten kollisjon", "Utan kollision", "かべぬけ"}},
};

static_assert([] {
    const auto& actions = cane::sly2input::kActions;
    if (std::size(s_actionNames) != actions.size()) return false;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (s_actionNames[i].action != Action(i)) return false;
        for (const char* text : s_actionNames[i].text)
            if (!text || !*text) return false;
        bool groupFound = false;
        for (const auto& group : s_actionGroups)
            if (group.group == actions[i].group) groupFound = true;
        if (!groupFound) return false;
    }
    for (const auto& group : s_actionGroups)
        for (const char* text : group.text)
            if (!text || !*text) return false;
    return true;
}(), "Translate every advanced action and group into every game language");
