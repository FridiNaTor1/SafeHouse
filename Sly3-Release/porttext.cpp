// See porttext.h.

#include "porttext.h"
#include "langstore.h"
#include "region.h"

#include "pcinput.h"
#include "../platform/sly2_action_bindings.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace
{
// The columns are the game's language order (region.h LANGUAGE): English, French, Italian, German, Spanish, Dutch,
// Portuguese, Danish, Finnish, Norwegian, Swedish, Japanese. English is the port's original text (the USA disc).
// G: the game's own words (its text in that language: the menus' On / Off 0x48 / 0x49 / 0x51 / 0x52, the Binocucom
// 0x2AC, the "Operation:" job names, a job 0x6B / 0x138); P: ours. The French text says "tu" and puts a space before
// a colon, as the game's does; the Japanese text uses the game's ASCII ':' and "(O)ボタン:" button style.
constexpr int CLANG = 12;
struct ROW
{
    PTX ptx;
    const char* asz[CLANG];
};

// clang-format off
constexpr ROW s_arow[] = {
    // G: the game's separators
    {PTX_Sep, {": ", " : ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ":"}},
    {PTX_GlyphSep, {": ", " : ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", "ボタン:"}},
    {PTX_ListFirst, {" ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", ""}},
    {PTX_ListSep, {", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", "、"}},
    {PTX_ListEnd, {".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", "。"}},
    // G: the menus' On / Off (gender-neutral forms: Spanish and Portuguese use the Binocucom option's Sí / No;
    // P: the Finnish Päällä / Pois, the game's "Käytössä / Pois käytöstä" too long for the wheel)
    {PTX_On, {"On", "Oui", "Sì", "Ein", "Sí", "aan", "Sim", "Til", "Päällä", "På", "På", "オン"}},
    {PTX_Off, {"Off", "Non", "No", "Aus", "No", "uit", "Não", "Fra", "Pois", "Av", "Av", "オフ"}},

    // P: the display options
    {PTX_AspectRatio, {"Aspect Ratio", "Format d'image", "Formato", "Seitenverhältnis", "Proporción",
        "Beeldverhouding", "Proporção do ecrã", "Billedformat", "Kuvasuhde", "Bildeformat", "Bildformat", "アスペクト比"}},
    {PTX_FitToScreen, {"Fit to Screen", "Adapter à l'écran", "Adatta allo schermo", "Bildschirmfüllend",
        "Ajustar a la pantalla", "Schermvullend", "Ajustar ao ecrã", "Tilpas til skærmen", "Sovita näyttöön",
        "Tilpass skjermen", "Anpassa till skärmen", "画面に合わせる"}},
    {PTX_Resolution, {"Resolution", "Résolution", "Risoluzione", "Auflösung", "Resolución", "Resolutie", "Resolução",
        "Opløsning", "Resoluutio", "Oppløsning", "Upplösning", "解像度"}},
    {PTX_Native, {"Native", "Native", "Nativa", "Nativ", "Nativa", "Native", "Nativa", "Original", "Natiivi",
        "Original", "Original", "ネイティブ"}},
    {PTX_FrameRate, {"Frame Rate", "Fréquence d'images", "Frequenza fotogrammi", "Bildrate", "Velocidad de fotogramas",
        "Framerate", "Taxa de fotogramas", "Billedhastighed", "Kuvataajuus", "Bildefrekvens", "Bildfrekvens",
        "フレームレート"}},
    {PTX_WindowMode, {"Window Mode", "Mode d'affichage", "Modalità finestra", "Fenstermodus", "Modo de ventana",
        "Venstermodus", "Modo de janela", "Vinduestilstand", "Ikkunatila", "Vindusmodus", "Fönsterläge",
        "ウィンドウモード"}},
    {PTX_Windowed, {"Windowed", "Fenêtré", "In finestra", "Fenster", "Ventana", "Venster", "Janela", "Vindue",
        "Ikkuna", "Vindu", "Fönster", "ウィンドウ"}},
    {PTX_Borderless, {"Borderless", "Sans bordure", "Senza bordi", "Randlos", "Sin bordes", "Randloos", "Sem margens",
        "Uden kant", "Reunaton", "Rammeløs", "Kantlöst", "ボーダーレス"}},
    {PTX_Fullscreen, {"Fullscreen", "Plein écran", "Schermo intero", "Vollbild", "Pantalla completa",
        "Volledig scherm", "Ecrã inteiro", "Fuld skærm", "Koko näyttö", "Fullskjerm", "Helskärm", "フルスクリーン"}},
    {PTX_VSync, {"VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync",
        "VSync"}},
    {PTX_DrawDistance, {"Draw Distance", "Distance d'affichage", "Distanza visiva", "Sichtweite",
        "Distancia de dibujado", "Zichtafstand", "Distância de visualização", "Tegneafstand", "Piirtoetäisyys",
        "Tegneavstand", "Ritavstånd", "ドローディスタンス"}},

    // P: the pause menu's entry, the options' pages (the game's "Quit Game" verbs: Quitter, Esci, beenden, Salir, ...)
    {PTX_ExitToDesktop, {"Exit to Desktop", "Quitter vers le bureau", "Esci al desktop", "Zurück zum Desktop",
        "Salir al escritorio", "Afsluiten naar bureaublad", "Sair para o ambiente de trabalho",
        "Afslut til skrivebordet", "Poistu työpöydälle", "Avslutt til skrivebordet", "Avsluta till skrivbordet",
        "デスクトップに戻る"}},
    {PTX_DisplayOptions, {"Display Options...", "Options d'affichage...", "Opzioni video...", "Anzeigeoptionen...",
        "Opciones de pantalla...", "Weergaveopties...", "Opções de visualização...", "Skærmindstillinger...",
        "Näyttöasetukset...", "Skjerminnstillinger...", "Bildskärmsalternativ...", "画面設定"}},
    {PTX_ControlsAndPrompts, {"Controls and Prompts...", "Commandes et icônes...", "Controlli e icone...",
        "Steuerung und Symbole...", "Controles e iconos...", "Besturing en symbolen...", "Controlos e ícones...",
        "Styring og ikoner...", "Ohjaimet ja kuvakkeet...", "Kontroller og ikoner...", "Kontroller och ikoner...",
        "操作とボタン表示"}},

    // P: the controls options (G: the Binocucom's name in each language, text 0x2AC)
    {PTX_ButtonIcons, {"Button Icons", "Icônes", "Icone", "Symbole", "Iconos", "Symbolen", "Ícones", "Knapikoner",
        "Kuvakkeet", "Ikoner", "Knappikoner",
        "ボタンアイコン"}},
    {PTX_Auto, {"Auto", "Auto", "Automatico", "Automatisch", "Automático", "Automatisch", "Automático", "Automatisk",
        "Automaattinen", "Automatisk", "Automatiskt", "自動"}},
    {PTX_OriginalPsIcons, {"Original PlayStation Icons", "Icônes PlayStation d'origine", "Icone PlayStation originali",
        "Original-PlayStation-Symbole", "Iconos originales de PlayStation", "Originele PlayStation-symbolen",
        "Ícones originais da PlayStation", "Originale PlayStation-ikoner", "Alkuperäiset PS-kuvakkeet",
        "Originale PlayStation-ikoner", "Ursprungliga PlayStation-ikoner", "PlayStationのオリジナルアイコン"}},
    {PTX_InstantSkip, {"Instant Skip Cutscenes", "Saut direct des cinématiques", "Salto immediato dei filmati",
        "Szenen sofort überspringen", "Saltar escenas al instante", "Filmpjes direct overslaan",
        "Saltar cenas de imediato", "Spring sekvenser over straks", "Ohita välianimaatiot heti",
        "Hopp over sekvenser straks", "Hoppa över sekvenser direkt", "ムービーをすぐスキップ"}},
    {PTX_MouseCamera, {"Mouse Camera", "Caméra à la souris", "Telecamera col mouse", "Kamera mit Maus",
        "Cámara con ratón", "Camera met muis", "Câmara com o rato", "Kamera med mus", "Kamera hiirellä",
        "Kamera med mus", "Kamera med mus", "マウスでカメラ操作"}},
    {PTX_MouseSensitivity, {"Mouse Sensitivity", "Sensibilité de la souris", "Sensibilità del mouse",
        "Mausempfindlichkeit", "Sensibilidad del ratón", "Muisgevoeligheid", "Sensibilidade do rato",
        "Musens følsomhed", "Hiiren herkkyys", "Musefølsomhet", "Muskänslighet", "マウス感度"}},
    {PTX_MouseAimSensitivity, {"Mouse Aim Sensitivity", "Sensibilité de visée (souris)", "Sensibilità mira (mouse)",
        "Zielempfindlichkeit (Maus)", "Sensibilidad de mira (ratón)", "Muisgevoeligheid bij richten",
        "Sensibilidade de mira (rato)", "Musens følsomhed ved sigte", "Hiiren herkkyys tähdätessä",
        "Musefølsomhet ved sikting", "Muskänslighet vid sikte", "ねらう時のマウス感度"}},
    {PTX_MouseWheelZoom, {"Mouse Wheel Zoom", "Zoom à la molette", "Zoom con la rotellina",
        "Zoom mit Mausrad", "Zoom con la rueda", "Zoomen met scrollwiel",
        "Zoom com a roda do rato", "Zoom med rullehjul", "Zoomaus hiiren rullalla",
        "Zoom med musehjul", "Zooma med mushjulet", "ホイールでズーム"}},
    {PTX_MouseInMenus, {"Mouse in Menus", "Souris dans les menus", "Mouse nei menu", "Maus in Menüs",
        "Ratón en los menús", "Muis in menu's", "Rato nos menus", "Mus i menuer", "Hiiri valikoissa", "Mus i menyer",
        "Mus i menyer", "メニューでマウスを使う"}},
    {PTX_MouseAimInHacking, {"Mouse Aim in Hacking", "Visée à la souris (piratage)", "Mira col mouse (hacking)",
        "Mauszielen beim Hacken", "Apuntar con ratón (hackeo)", "Richten met muis bij hacken",
        "Mira com o rato (pirataria)", "Sigt med mus ved hacking", "Hiiritähtäys hakkeroinnissa",
        "Sikt med mus ved hacking", "Sikta med mus vid hackning", "ハッキング中のマウス操作"}},
    {PTX_KeyboardMapping, {"Keyboard Mapping...", "Configuration du clavier...", "Configurazione tastiera...",
        "Tastaturbelegung...", "Asignación de teclas...", "Toetsenbordindeling...", "Configuração do teclado...",
        "Tastaturtildeling...", "Näppäimistön määritys...", "Tastaturoppsett...", "Tangentbordsinställning...",
        "キーボード設定"}},
    {PTX_ResetKeyboardMapping, {"Reset Keyboard Mapping", "Réinitialiser le clavier", "Ripristina la tastiera",
        "Tastaturbelegung zurücksetzen", "Restablecer teclas", "Toetsenbordindeling herstellen", "Repor o teclado",
        "Nulstil tastaturtildeling", "Palauta näppäimistön oletukset", "Tilbakestill tastaturoppsett",
        "Återställ tangentbordet", "キーボード設定を元に戻す"}},

    // P: the keyboard mapping's rows (the PS2 buttons: the Japanese with the game's icons, as its own text names them)
    {PTX_MoveForward, {"Move Forward", "Avancer", "Avanti", "Vorwärts", "Avanzar", "Vooruit", "Avançar", "Fremad",
        "Eteenpäin", "Fremover", "Framåt", "前へ移動"}},
    {PTX_MoveBack, {"Move Back", "Reculer", "Indietro", "Rückwärts", "Retroceder", "Achteruit", "Recuar", "Bagud",
        "Taaksepäin", "Bakover", "Bakåt", "後ろへ移動"}},
    {PTX_MoveLeft, {"Move Left", "Aller à gauche", "Sinistra", "Links", "Izquierda", "Links", "Esquerda", "Venstre",
        "Vasemmalle", "Venstre", "Vänster", "左へ移動"}},
    {PTX_MoveRight, {"Move Right", "Aller à droite", "Destra", "Rechts", "Derecha", "Rechts", "Direita", "Højre",
        "Oikealle", "Høyre", "Höger", "右へ移動"}},
    {PTX_Square, {"Square", "Carré", "Quadrato", "Quadrat", "Cuadrado", "Vierkant", "Quadrado", "Firkant", "Neliö",
        "Firkant", "Fyrkant", "&2S&.ボタン"}},
    {PTX_Circle, {"Circle", "Rond", "Cerchio", "Kreis", "Círculo", "Rondje", "Círculo", "Cirkel", "Ympyrä", "Sirkel",
        "Cirkel", "&2O&.ボタン"}},
    {PTX_Cross, {"Cross", "Croix", "Croce", "Kreuz", "Cruz", "Kruisje", "Cruz", "Kryds", "Risti", "Kryss", "Kryss",
        "&2X&.ボタン"}},
    {PTX_Triangle, {"Triangle", "Triangle", "Triangolo", "Dreieck", "Triángulo", "Driehoek", "Triângulo", "Trekant",
        "Kolmio", "Trekant", "Triangel", "&2T&.ボタン"}},
    {PTX_Start, {"Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start",
        "STARTボタン"}},
    {PTX_Select, {"Select", "Select", "Select", "Select", "Select", "Select", "Select", "Select", "Select", "Select",
        "Select", "SELECTボタン"}},
    {PTX_CameraLeft, {"Camera Left", "Caméra gauche", "Visuale sinistra", "Kamera links", "Cámara izquierda",
        "Camera links", "Câmara esquerda", "Kamera venstre", "Kamera vasemmalle", "Kamera venstre",
        "Kamera vänster", "カメラ左"}},
    {PTX_CameraRight, {"Camera Right", "Caméra droite", "Visuale destra", "Kamera rechts", "Cámara derecha",
        "Camera rechts", "Câmara direita", "Kamera højre", "Kamera oikealle", "Kamera høyre", "Kamera höger",
        "カメラ右"}},
    {PTX_CameraUp, {"Camera Up", "Caméra haut", "Visuale su", "Kamera hoch", "Cámara arriba",
        "Camera omhoog", "Câmara cima", "Kamera op", "Kamera ylös", "Kamera opp", "Kamera upp", "カメラ上"}},
    {PTX_CameraDown, {"Camera Down", "Caméra bas", "Visuale giù", "Kamera runter", "Cámara abajo",
        "Camera omlaag", "Câmara baixo", "Kamera ned", "Kamera alas", "Kamera ned", "Kamera ner", "カメラ下"}},
    {PTX_TankLeftTrack, {"Tank: Left Track", "Char : chenille gauche", "Carro: cingolo sinistro", "Panzer: linke Kette",
        "Tanque: oruga izquierda", "Tank: linkerrups", "Tanque: lagarta esquerda", "Tank: venstre bælte",
        "Panssari: vasen tela", "Stridsvogn: venstre belte", "Stridsvagn: vänster band", "戦車：左キャタピラ"}},
    {PTX_TankRightTrack, {"Tank: Right Track", "Char : chenille droite", "Carro: cingolo destro", "Panzer: rechte Kette",
        "Tanque: oruga derecha", "Tank: rechterrups", "Tanque: lagarta direita", "Tank: højre bælte",
        "Panssari: oikea tela", "Stridsvogn: høyre belte", "Stridsvagn: höger band", "戦車：右キャタピラ"}},
    {PTX_PressAKey, {"press a key (Esc cancels)", "appuie sur une touche (Échap : annuler)",
        "premi un tasto (Esc annulla)", "Taste drücken (Esc bricht ab)", "pulsa una tecla (Esc cancela)",
        "druk op een toets (Esc annuleert)", "prime uma tecla (Esc cancela)", "tryk på en tast (Esc annullerer)",
        "paina näppäintä (Esc peruuttaa)", "trykk på en tast (Esc avbryter)", "tryck på en tangent (Esc avbryter)",
        "キーを押してください(Escでキャンセル)"}},

    // P: key and mouse names (the other keys keep their keycaps' names: Enter, Tab, Esc, ...)
    {PTX_KeyUnbound, {"Unbound", "Non assignée", "Non assegnato", "Nicht belegt", "Sin asignar", "Niet toegewezen",
        "Não atribuída", "Ikke tildelt", "Ei määritetty", "Ikke tildelt", "Ej tilldelad", "未設定"}},
    {PTX_KeySpace, {"Space", "Espace", "Spazio", "Leertaste", "Espacio", "Spatiebalk", "Espaço", "Mellemrum",
        "Välilyönti", "Mellomrom", "Mellanslag", "スペース"}},
    {PTX_KeyUp, {"Up Arrow", "Flèche haut", "Freccia su", "Pfeil hoch", "Flecha arriba", "Pijl omhoog",
        "Seta cima", "Pil op", "Nuoli ylös", "Pil opp", "Pil upp", "上キー"}},
    {PTX_KeyDown, {"Down Arrow", "Flèche bas", "Freccia giù", "Pfeil runter", "Flecha abajo", "Pijl omlaag",
        "Seta baixo", "Pil ned", "Nuoli alas", "Pil ned", "Pil ner", "下キー"}},
    {PTX_KeyLeft, {"Left Arrow", "Flèche gauche", "Freccia sinistra", "Pfeil links", "Flecha izquierda", "Pijl links",
        "Seta esquerda", "Pil venstre", "Vasen nuoli", "Pil venstre", "Pil vänster", "左キー"}},
    {PTX_KeyRight, {"Right Arrow", "Flèche droite", "Freccia destra", "Pfeil rechts", "Flecha derecha", "Pijl rechts",
        "Seta direita", "Pil højre", "Oikea nuoli", "Pil høyre", "Pil höger", "右キー"}},
    // (%s: Shift / Ctrl / Alt)
    {PTX_KeyLeftMod, {"Left %s", "%s gauche", "%s sinistro", "%s links", "%s izquierdo", "Linker %s", "%s esquerdo",
        "Venstre %s", "Vasen %s", "Venstre %s", "Vänster %s", "左%s"}},
    {PTX_KeyRightMod, {"Right %s", "%s droite", "%s destro", "%s rechts", "%s derecho", "Rechter %s", "%s direito",
        "Højre %s", "Oikea %s", "Høyre %s", "Höger %s", "右%s"}},
    {PTX_MouseLeft, {"Left Mouse", "Clic gauche", "Tasto sinistro", "Linke Maustaste", "Botón izquierdo",
        "Linkermuisknop", "Botão esquerdo", "Venstre museknap", "Hiiren vasen", "Venstre museknapp",
        "Vänster musknapp", "マウス左"}},
    {PTX_MouseRight, {"Right Mouse", "Clic droit", "Tasto destro", "Rechte Maustaste", "Botón derecho",
        "Rechtermuisknop", "Botão direito", "Højre museknap", "Hiiren oikea", "Høyre museknapp", "Höger musknapp",
        "マウス右"}},
    {PTX_MouseMiddle, {"Middle Mouse", "Clic central", "Tasto centrale", "Mittlere Maustaste", "Botón central",
        "Middelste muisknop", "Botão do meio", "Midterste knap", "Hiiren keski", "Midtre museknapp",
        "Mittenmusknapp", "マウス中"}},
    {PTX_MouseN, {"Mouse %d", "Souris %d", "Mouse %d", "Maustaste %d", "Ratón %d", "Muisknop %d", "Rato %d",
        "Museknap %d", "Hiiri %d", "Museknapp %d", "Musknapp %d", "マウス%d"}},
    {PTX_WheelUp, {"Wheel Up", "Molette haut", "Rotellina su", "Mausrad hoch", "Rueda arriba", "Scrollwiel omhoog",
        "Roda para cima", "Rullehjul op", "Rulla ylös", "Musehjul opp", "Mushjul upp", "ホイール上"}},
    {PTX_WheelDown, {"Wheel Down", "Molette bas", "Rotellina giù", "Mausrad runter", "Rueda abajo",
        "Scrollwiel omlaag", "Roda para baixo", "Rullehjul ned", "Rulla alas", "Musehjul ned", "Mushjul ner",
        "ホイール下"}},
    {PTX_Mouse, {"Mouse", "Souris", "Mouse", "Maus", "Ratón", "Muis", "Rato", "Mus", "Hiiri", "Mus", "Mus", "マウス"}},

    // P: the cutscene skip hint (Circle, as in every region: gameplay keeps the USA buttons)
    {PTX_HoldToSkip, {"Hold &2O&. to skip", "Maintiens &2O&. pour passer", "Tieni premuto &2O&. per saltare",
        "&2O&. halten zum Überspringen", "Mantén &2O&. para saltar", "Houd &2O&. ingedrukt om over te slaan",
        "Mantém &2O&. para saltar", "Hold &2O&. nede for at springe over", "Ohita pitämällä &2O&. pohjassa",
        "Hold &2O&. inne for å hoppe over", "Håll &2O&. för att hoppa över", "&2O&.ボタン長押しでスキップ"}},
    {PTX_HoldToSkipNoIcons, {"Hold Circle to skip", "Maintiens Rond pour passer", "Tieni premuto Cerchio per saltare",
        "Kreis halten zum Überspringen", "Mantén Círculo para saltar", "Houd Rondje ingedrukt om over te slaan",
        "Mantém Círculo para saltar", "Hold Cirkel nede for at springe over", "Ohita pitämällä Ympyrää pohjassa",
        "Hold Sirkel inne for å hoppe over", "Håll Cirkel för att hoppa över", "丸ボタン長押しでスキップ"}},

    // P: Replay Jobs (G: the word for a job: Abandon Job 0x6B, Jobs 0x138; the "Operation:" style of the game's
    // operation jobs, Operation: Wet Tiger 0x1BE; the Japanese names an operation "作戦" as its job names do not)
    {PTX_ReplayJobs, {"Replay Jobs", "Rejouer des missions", "Rigioca incarichi", "Aufträge wiederholen",
        "Repetir misiones", "Taken opnieuw spelen", "Repetir tarefas", "Spil job igen", "Pelaa keikkoja uudelleen",
        "Spill oppdrag på nytt", "Spela om uppdrag", "ミッションをリプレイ"}},
    {PTX_ReplayJob, {"Replay Job", "Rejouer la mission", "Rigioca l'incarico", "Auftrag wiederholen",
        "Repetir misión", "Taak opnieuw spelen", "Repetir a tarefa", "Spil jobbet igen", "Pelaa keikka uudelleen",
        "Spill oppdraget på nytt", "Spela om uppdraget", "リプレイする"}},
    {PTX_OpHippoDrop, {"Operation: Hippo Drop", "Opération : Hippo-largage", "Operazione Ippo-lancio",
        "Operation: Nilpferd-Abwurf", "Operación: Hipopótamo en picado", "Operatie Nijlpaarddropping",
        "Operação: Hipopótamo em Queda", "Operation: Flodhestedrop", "Operaatio: Virtahepopudotus",
        "Operasjon Flodhestslipp", "Operation Flodhästsläpp", "ヒッポ・ドロップ作戦"}},
    {PTX_ReplayOperation, {"Replay the whole operation in sequence:", "Rejoue toute l'opération dans l'ordre :",
        "Rigioca l'intera operazione in sequenza:", "Spiele die ganze Operation der Reihe nach:",
        "Juega de nuevo toda la operación en orden:", "Speel de hele operatie opnieuw, in volgorde:",
        "Repete a operação inteira por ordem:", "Spil hele operationen igen i rækkefølge:",
        "Pelaa koko operaatio uudelleen järjestyksessä:", "Spill hele operasjonen på nytt i rekkefølge:",
        "Spela om hela operationen i ordning:", "作戦をはじめから順番にリプレイ:"}},

    // P: the save / load pages' page indicator (Q4: more slots, paged with L1 / R1; menu.cpp StrSlotPage)
    {PTX_SlotPage, {"Page %d / %d", "Page %d / %d", "Pagina %d / %d", "Seite %d / %d", "Página %d / %d",
        "Pagina %d / %d", "Página %d / %d", "Side %d / %d", "Sivu %d / %d", "Side %d / %d", "Sida %d / %d",
        "ページ%d/%d"}},

    // the display options' frame-rate limit: none (R19; the launcher's "Uncapped")
    {PTX_Uncapped, {"Uncapped", "Illimitée", "Illimitata", "Unbegrenzt", "Sin límite", "Onbeperkt", "Ilimitada",
        "Ubegrænset", "Rajoittamaton", "Ubegrenset", "Obegränsad", "無制限"}},

    // P: the display options' Long Shadows
    {PTX_LongShadows, {"Long Shadows", "Ombres longues", "Ombre lunghe", "Lange Schatten", "Sombras largas",
        "Lange schaduwen", "Sombras longas", "Lange skygger", "Pitkät varjot", "Lange skygger", "Långa skuggor",
        "長い影"}},
    // P: Invert Controls... (G: the views' names: the camera of 0x2AC "Move Camera", the Binocucom 0x2AC, the turret of
    // 0x53 "Invert Turret Controls"; the Japanese 反転 of the mouse rows above, ゴーグル / 砲台 of 0x54 / 0x56 there)
    {PTX_InvertControlsPage, {"Invert Controls...", "Commandes inversées...", "Inversione controlli...",
        "Steuerung umkehren...", "Control invertido...", "Besturing omkeren...", "Inverter controlos...",
        "Spejl styring...", "Käänteinen ohjaus...", "Vend kontroller...", "Kasta om kontroller...", "操作の反転"}},
    {PTX_PadInvertCameraX, {"Invert Camera X", "Inverser l'axe X de la caméra", "Inverti asse X della visuale",
        "Kamera-X-Achse umkehren", "Invertir eje X de la cámara", "X-as camera omkeren", "Inverter eixo X da Câmara",
        "Spejl kameraets X-akse", "Käänteinen kameran X-akseli", "Vend kameraets X-akse", "Invertera kamerans X-axel",
        "カメラ左右反転"}},
    {PTX_PadInvertCameraY, {"Invert Camera Y", "Inverser l'axe Y de la caméra", "Inverti asse Y della visuale",
        "Kamera-Y-Achse umkehren", "Invertir eje Y de la cámara", "Y-as camera omkeren", "Inverter eixo Y da Câmara",
        "Spejl kameraets Y-akse", "Käänteinen kameran Y-akseli", "Vend kameraets Y-akse", "Invertera kamerans Y-axel",
        "カメラ上下反転"}},
    {PTX_MouseInvertCameraX, {"Invert Mouse X (Camera)", "Inverser X souris (caméra)", "Inverti X mouse (visuale)",
        "Maus-X umkehren (Kamera)", "Invertir X ratón (cámara)", "X-as muis omkeren (camera)",
        "Inverter X do rato (Câmara)", "Spejl musens X (kamera)", "Käänteinen hiiren X (kamera)",
        "Vend musens X (kamera)", "Invertera musens X (kamera)", "マウス左右反転(カメラ)"}},
    {PTX_MouseInvertCameraY, {"Invert Mouse Y (Camera)", "Inverser Y souris (caméra)", "Inverti Y mouse (visuale)",
        "Maus-Y umkehren (Kamera)", "Invertir Y ratón (cámara)", "Y-as muis omkeren (camera)",
        "Inverter Y do rato (Câmara)", "Spejl musens Y (kamera)", "Käänteinen hiiren Y (kamera)",
        "Vend musens Y (kamera)", "Invertera musens Y (kamera)", "マウス上下反転(カメラ)"}},
    {PTX_MouseInvertBinocX, {"Invert Mouse X (Binocucom)", "Inverser X souris (Binocom)", "Inverti X mouse (Binocom)",
        "Maus-X umkehren (S.-Kom.)", "Invertir X ratón (bino-com)", "X-as muis omkeren (Contactkijker)",
        "Inverter X do rato (Binocucom)", "Spejl musens X (binocucom)", "Käänteinen hiiren X (radiokiikari)",
        "Vend musens X (kikkertwalkie)", "Invertera musens X (binokucom)", "マウス左右反転(ゴーグル)"}},
    {PTX_MouseInvertBinocY, {"Invert Mouse Y (Binocucom)", "Inverser Y souris (Binocom)", "Inverti Y mouse (Binocom)",
        "Maus-Y umkehren (S.-Kom.)", "Invertir Y ratón (bino-com)", "Y-as muis omkeren (Contactkijker)",
        "Inverter Y do rato (Binocucom)", "Spejl musens Y (binocucom)", "Käänteinen hiiren Y (radiokiikari)",
        "Vend musens Y (kikkertwalkie)", "Invertera musens Y (binokucom)", "マウス上下反転(ゴーグル)"}},
    {PTX_MouseInvertTurretX, {"Invert Mouse X (Turret)", "Inverser X souris (tourelles)", "Inverti X mouse (torrette)",
        "Maus-X umkehren (Geschütz)", "Invertir X ratón (torretas)", "X-as muis omkeren (geschut)",
        "Inverter X do rato (Torre)", "Spejl musens X (skydetårn)", "Käänteinen hiiren X (tykki)",
        "Vend musens X (tårn)", "Invertera musens X (stridstorn)", "マウス左右反転(砲台)"}},
    {PTX_MouseInvertTurretY, {"Invert Mouse Y (Turret)", "Inverser Y souris (tourelles)", "Inverti Y mouse (torrette)",
        "Maus-Y umkehren (Geschütz)", "Invertir Y ratón (torretas)", "Y-as muis omkeren (geschut)",
        "Inverter Y do rato (Torre)", "Spejl musens Y (skydetårn)", "Käänteinen hiiren Y (tykki)",
        "Vend musens Y (tårn)", "Invertera musens Y (stridstorn)", "マウス上下反転(砲台)"}},
    {PTX_AutoCameraRotation, {"Automatic Camera Rotation", "Rotation auto caméra", "Rotazione auto telecamera",
        "Automatische Kameradrehung", "Giro automático de cámara", "Automatische camerarotatie",
        "Rotação automática da câmara", "Automatisk kamerarotation", "Kameran automaattikääntö",
        "Automatisk kamerarotasjon", "Automatisk kamerarotation", "カメラ自動回転"}},
    {PTX_AdvancedBindings, {"Advanced Input Mapping", "Attribution avancée des commandes", "Assegnazione avanzata comandi",
        "Erweiterte Tastenbelegung", "Asignación avanzada de controles", "Geavanceerde knoptoewijzing",
        "Mapeamento avançado de controlos", "Avanceret knaptildeling", "Laajennettu näppäinkartoitus",
        "Avansert knappetilordning", "Avancerad knapptilldelning", "くわしい操作のわりあて"}},
    {PTX_ActionBindings, {"Action Bindings...", "Attribuer les commandes...", "Assegna comandi...",
        "Aktionen belegen...", "Asignar acciones...", "Acties toewijzen...", "Atribuir ações...",
        "Tildel handlinger...", "Määritä toiminnot...", "Tilordne handlinger...", "Tilldela åtgärder...", "操作をわりあてる"}},
    {PTX_ActionKeyboard, {"Keyboard and Mouse...", "Clavier et souris...", "Tastiera e mouse...",
        "Tastatur und Maus...", "Teclado y ratón...", "Toetsenbord en muis...", "Teclado e rato...",
        "Tastatur og mus...", "Näppäimistö ja hiiri...", "Tastatur og mus...", "Tangentbord och mus...", "キーボードとマウス"}},
    {PTX_ActionController, {"Controller...", "Manette...", "Controller...", "Controller...", "Mando...", "Controller...",
        "Comando...", "Controller...", "Peliohjain...", "Kontroller...", "Handkontroll...", "コントローラ"}},
    {PTX_ResetAdvancedBindings, {"Reset Advanced Input Mapping", "Réinitialiser les commandes", "Ripristina comandi avanzati",
        "Belegung zurücksetzen", "Restablecer controles avanzados", "Standaardbesturing herstellen",
        "Repor controlos avançados", "Nulstil avanceret styring", "Palauta toimintojen oletukset",
        "Tilbakestill avanserte kontroller", "Återställ avancerade kontroller", "わりあてをもどす"}},
    {PTX_CaptureAction, {"Press input (Esc: cancel, Del: clear)", "Appuie (Échap : annuler, Suppr : effacer)",
        "Premi un comando (Esc: annulla, Canc: cancella)", "Eingabe (Esc: Abbruch, Entf: löschen)",
        "Pulsa (Esc: cancelar, Supr: borrar)", "Druk (Esc: annuleren, Del: wissen)",
        "Prime (Esc: cancelar, Del: limpar)", "Tryk (Esc: annuller, Del: ryd)", "Paina (Esc: peruuta, Del: poista)",
        "Trykk (Esc: avbryt, Del: fjern)", "Tryck (Esc: avbryt, Del: rensa)", "ボタンをおす (Esc:やめる、Del:けす)"}},
    // P: the Remap page (remappage.h; 2026-11-10)
    {PTX_RemapPage, {"Remap Controls...", "Réattribuer les commandes...", "Riassegna i comandi...", "Steuerung belegen...",
        "Reasignar controles...", "Besturing toewijzen...", "Reatribuir controlos...", "Tildel styring...",
        "Määritä ohjaimet...", "Tilordne kontroller...", "Tilldela kontroller...", "操作のわりあて"}},
    {PTX_RemapTitle, {"Remap Controls", "Réattribuer les commandes", "Riassegna i comandi", "Steuerung belegen",
        "Reasignar controles", "Besturing toewijzen", "Reatribuir controlos", "Tildel styring", "Määritä ohjaimet",
        "Tilordne kontroller", "Tilldela kontroller", "操作のわりあて"}},
    {PTX_RemapAction, {"Action", "Action", "Azione", "Aktion", "Acción", "Actie", "Ação", "Handling", "Toiminto",
        "Handling", "Åtgärd", "アクション"}},
    {PTX_RemapKeyboard, {"Keyboard / Mouse", "Clavier / souris", "Tastiera / mouse", "Tastatur / Maus", "Teclado / ratón",
        "Toetsenbord / muis", "Teclado / rato", "Tastatur / mus", "Näppäimistö / hiiri", "Tastatur / mus",
        "Tangentbord / mus", "キーボード/マウス"}},
    {PTX_RemapController, {"Controller", "Manette", "Controller", "Controller", "Mando", "Controller", "Comando",
        "Controller", "Peliohjain", "Kontroller", "Handkontroll", "コントローラ"}},
    {PTX_RemapReset, {"Reset to Defaults", "Valeurs par défaut", "Ripristina predefiniti", "Standard wiederherstellen",
        "Restablecer valores", "Standaard herstellen", "Repor predefinições", "Nulstil til standard", "Palauta oletukset",
        "Tilbakestill til standard", "Återställ standard", "しょきせっていにもどす"}},
    {PTX_RemapBack, {"Back", "Retour", "Indietro", "Zurück", "Atrás", "Terug", "Voltar", "Tilbage", "Takaisin", "Tilbake",
        "Tillbaka", "もどる"}},
    {PTX_RemapChange, {"Change", "Changer", "Cambia", "Ändern", "Cambiar", "Wijzigen", "Mudar", "Skift", "Vaihda", "Endre",
        "Ändra", "かえる"}},
    {PTX_RemapClear, {"Clear", "Effacer", "Cancella", "Löschen", "Borrar", "Wissen", "Limpar", "Ryd", "Poista", "Fjern",
        "Rensa", "けす"}},
    {PTX_RemapPressKey, {"Press a key or mouse button (Esc: cancel, Del: clear)",
        "Appuie sur une touche ou un bouton de souris (Échap : annuler, Suppr : effacer)",
        "Premi un tasto o un pulsante del mouse (Esc: annulla, Canc: cancella)",
        "Taste oder Maustaste drücken (Esc: Abbruch, Entf: löschen)",
        "Pulsa una tecla o un botón del ratón (Esc: cancelar, Supr: borrar)",
        "Druk op een toets of muisknop (Esc: annuleren, Del: wissen)",
        "Prime uma tecla ou um botão do rato (Esc: cancelar, Del: limpar)",
        "Tryk på en tast eller museknap (Esc: annuller, Del: ryd)",
        "Paina näppäintä tai hiiren painiketta (Esc: peruuta, Del: poista)",
        "Trykk en tast eller museknapp (Esc: avbryt, Del: fjern)",
        "Tryck på en tangent eller musknapp (Esc: avbryt, Del: rensa)", "キーかマウスボタンをおす (Esc:やめる、Del:けす)"}},
    {PTX_RemapPressButton, {"Press a controller button (Esc or wait: cancel, Del: clear)",
        "Appuie sur un bouton de la manette (Échap ou attendre : annuler, Suppr : effacer)",
        "Premi un pulsante del controller (Esc o attendi: annulla, Canc: cancella)",
        "Controllertaste drücken (Esc oder warten: Abbruch, Entf: löschen)",
        "Pulsa un botón del mando (Esc o espera: cancelar, Supr: borrar)",
        "Druk op een controllerknop (Esc of wachten: annuleren, Del: wissen)",
        "Prime um botão do comando (Esc ou esperar: cancelar, Del: limpar)",
        "Tryk på en knap på controlleren (Esc eller vent: annuller, Del: ryd)",
        "Paina ohjaimen painiketta (Esc tai odota: peruuta, Del: poista)",
        "Trykk en knapp på kontrolleren (Esc eller vent: avbryt, Del: fjern)",
        "Tryck på en knapp på handkontrollen (Esc eller vänta: avbryt, Del: rensa)",
        "コントローラのボタンをおす (Escかまつ:やめる、Del:けす)"}},
    {PTX_RemapFixed, {"fixed", "fixe", "fisso", "fest", "fijo", "vast", "fixo", "fast", "kiinteä", "fast", "fast", "こてい"}},
    // P: VR Settings (vr.h)
    {PTX_VrSettings, {"VR Settings", "Réglages VR", "Impostazioni VR", "VR-Einstellungen", "Ajustes de RV",
        "VR-instellingen", "Definições de RV", "VR-indstillinger", "VR-asetukset", "VR-innstillinger", "VR-inställningar", "VR設定"}},
    {PTX_VrMode, {"VR Mode", "Mode VR", "Modalità VR", "VR-Modus", "Modo RV", "VR-modus", "Modo RV", "VR-tilstand",
        "VR-tila", "VR-modus", "VR-läge", "VRモード"}},
    {PTX_VrImmersive, {"Immersive", "Immersif", "Immersiva", "Immersiv", "Inmersivo", "Immersief", "Imersivo", "Immersiv",
        "Immersiivinen", "Immersiv", "Immersiv", "イマーシブ"}},
    {PTX_VrFlatScreen, {"Flat Screen", "Écran plat", "Schermo piatto", "Flacher Bildschirm", "Pantalla plana",
        "Plat scherm", "Ecrã plano", "Flad skærm", "Litteä näyttö", "Flat skjerm", "Platt skärm", "フラット画面"}},
    {PTX_VrVignette, {"Comfort Vignette", "Vignette de confort", "Vignettatura comfort", "Komfort-Vignette",
        "Viñeta de confort", "Comfortvignet", "Vinheta de conforto", "Komfortvignet", "Mukavuusvinjetti",
        "Komfortvignett", "Komfortvinjett", "ビネット"}},
    {PTX_VrLow, {"Low", "Faible", "Bassa", "Schwach", "Baja", "Laag", "Baixa", "Lav", "Matala", "Lav", "Låg", "よわい"}},
    {PTX_VrMedium, {"Medium", "Moyenne", "Media", "Mittel", "Media", "Middel", "Média", "Middel", "Keskitaso", "Middels",
        "Medel", "ふつう"}},
    {PTX_VrHigh, {"High", "Forte", "Alta", "Stark", "Alta", "Hoog", "Alta", "Høj", "Korkea", "Høy", "Hög", "つよい"}},
    {PTX_VrScreenSize, {"Screen Size", "Taille de l'écran", "Dimensione schermo", "Bildschirmgröße", "Tamaño de pantalla",
        "Schermgrootte", "Tamanho do ecrã", "Skærmstørrelse", "Näytön koko", "Skjermstørrelse", "Skärmstorlek", "画面サイズ"}},
    {PTX_VrSmall, {"Small", "Petit", "Piccolo", "Klein", "Pequeña", "Klein", "Pequeno", "Lille", "Pieni", "Liten", "Liten",
        "ちいさい"}},
    {PTX_VrLarge, {"Large", "Grand", "Grande", "Groß", "Grande", "Groot", "Grande", "Stor", "Suuri", "Stor", "Stor", "おおきい"}},
    {PTX_VrRefreshRate, {"Refresh Rate", "Fréquence d'affichage", "Frequenza di aggiornamento", "Bildwiederholrate",
        "Frecuencia de actualización", "Verversingssnelheid", "Taxa de atualização", "Opdateringshastighed",
        "Virkistystaajuus", "Oppdateringsfrekvens", "Uppdateringsfrekvens", "リフレッシュレート"}},
    {PTX_VrWorldScale, {"World Scale", "Échelle du monde", "Scala del mondo", "Weltgröße", "Escala del mundo",
        "Wereldschaal", "Escala do mundo", "Verdensskala", "Maailman mittakaava", "Verdensskala", "Världsskala",
        "ワールドスケール"}},
    {PTX_VrRecenter, {"Recenter View", "Recentrer la vue", "Ricentra la visuale", "Ansicht zentrieren", "Recentrar la vista",
        "Beeld centreren", "Recentrar a vista", "Centrer visningen", "Keskitä näkymä", "Sentrer visningen",
        "Centrera vyn", "ビューをもどす"}},
    {PTX_VrCutscenes, {"Cutscenes", "Cinématiques", "Filmati", "Zwischensequenzen", "Escenas", "Tussenfilmpjes",
        "Cenas", "Mellemsekvenser", "Välivideot", "Mellomsekvenser", "Mellansekvenser", "ムービー"}},
    {PTX_VrRcTable, {"RC Car Table View (Beta)", "Voiture RC vue de la table (bêta)", "Auto RC dal tavolo (beta)",
        "RC-Auto vom Tisch (Beta)", "Coche RC desde la mesa (beta)", "RC-auto vanaf de tafel (bèta)",
        "Carro RC da mesa (beta)", "RC-bil fra bordet (beta)", "RC-auto pöydältä (beta)", "RC-bil fra bordet (beta)",
        "RC-bil från bordet (beta)", "RCカー テーブルビュー"}},
    {PTX_QuitToLauncher, {"Quit to Launcher", "Retour au lanceur", "Torna al launcher", "Zurück zum Launcher",
        "Volver al lanzador", "Terug naar de launcher", "Voltar ao launcher", "Tilbage til launcheren",
        "Palaa käynnistimeen", "Tilbake til startprogrammet", "Tillbaka till startprogrammet", "ランチャーに戻る"}},
    // P: (audio) the options' Audio Devices page (menu.cpp AentAudio)
    {PTX_AudioDevicesPage, {"Audio Devices...", "Périphériques audio...", "Dispositivi audio...", "Audiogeräte...",
        "Dispositivos de audio...", "Audioapparaten...", "Dispositivos de áudio...", "Lydenheder...", "Äänilaitteet...",
        "Lydenheter...", "Ljudenheter...", "オーディオデバイス"}},
    {PTX_OutputDevice, {"Output", "Sortie", "Uscita", "Ausgabe", "Salida", "Uitvoer", "Saída", "Udgang", "Ulostulo",
        "Utgang", "Utgång", "スピーカー"}},
    {PTX_InputDevice, {"Microphone", "Microphone", "Microfono", "Mikrofon", "Micrófono", "Microfoon", "Microfone",
        "Mikrofon", "Mikrofoni", "Mikrofon", "Mikrofon", "マイク"}},
    {PTX_HeadsetOutputDevice, {"Headset Output", "Sortie du casque", "Uscita cuffie", "Headset-Ausgabe",
        "Salida de auriculares", "Headsetuitvoer", "Saída dos auscultadores", "Headsetudgang", "Kuulokkeiden ulostulo",
        "Hodesettutgang", "Headsetutgång", "ヘッドセット"}},
    {PTX_SystemDefault, {"System Default", "Par défaut", "Predefinito", "Systemstandard", "Predeterminado",
        "Systeemstandaard", "Predefinição", "Systemstandard", "Järjestelmän oletus", "Systemstandard", "Systemstandard",
        "オート"}},
    {PTX_SameAsOutput, {"Same as Output", "Comme la sortie", "Come l'uscita", "Wie Ausgabe", "Igual que la salida",
        "Zelfde als uitvoer", "Igual à saída", "Samme som udgang", "Sama kuin ulostulo", "Samme som utgang",
        "Samma som utgång", "スピーカーとおなじ"}},
    {PTX_NotConnected, {"%s (not connected)", "%s (non connecté)", "%s (non collegato)", "%s (nicht verbunden)",
        "%s (no conectado)", "%s (niet verbonden)", "%s (não ligado)", "%s (ikke tilsluttet)", "%s (ei yhdistetty)",
        "%s (ikke tilkoblet)", "%s (inte ansluten)", "%s (なし)"}},
    // P: (splitscreen3.h) the split screen's binding screen (the Japanese: kana and ASCII only)
    {PTX_SplitTitle, {"Split Screen", "Écran partagé", "Schermo condiviso", "Geteilter Bildschirm", "Pantalla dividida",
        "Gedeeld scherm", "Ecrã dividido", "Delt skærm", "Jaettu näyttö", "Delt skjerm", "Delad skärm", "2プレイヤー"}},
    {PTX_SplitPlayerN, {"Player %d", "Joueur %d", "Giocatore %d", "Spieler %d", "Jugador %d", "Speler %d", "Jogador %d",
        "Spiller %d", "Pelaaja %d", "Spiller %d", "Spelare %d", "プレイヤー%d"}},
    {PTX_SplitPress, {"Player %d, press &2X&. / %s", "Joueur %d, appuyez sur &2X&. / %s", "Giocatore %d, premi &2X&. / %s",
        "Spieler %d, drücke &2X&. / %s", "Jugador %d, pulsa &2X&. / %s", "Speler %d, druk op &2X&. / %s",
        "Jogador %d, prime &2X&. / %s", "Spiller %d, tryk på &2X&. / %s", "Pelaaja %d, paina &2X&. / %s",
        "Spiller %d, trykk &2X&. / %s", "Spelare %d, tryck på &2X&. / %s", "プレイヤー%d: &2X&. / %s"}},
    {PTX_SplitReconnect, {"Player %d's controller was disconnected: press &2X&. / %s",
        "Manette du joueur %d déconnectée : appuyez sur &2X&. / %s", "Controller del giocatore %d scollegato: premi &2X&. / %s",
        "Controller von Spieler %d getrennt: drücke &2X&. / %s", "Mando del jugador %d desconectado: pulsa &2X&. / %s",
        "Controller van speler %d losgekoppeld: druk op &2X&. / %s", "Comando do jogador %d desligado: prime &2X&. / %s",
        "Spiller %ds controller er afbrudt: tryk på &2X&. / %s", "Pelaajan %d ohjain irrotettu: paina &2X&. / %s",
        "Spiller %ds kontroller er frakoblet: trykk &2X&. / %s", "Spelare %ds handkontroll har kopplats från: tryck på &2X&. / %s",
        "プレイヤー%dのコントローラーがない: &2X&. / %s"}},
    {PTX_SplitKeyboard, {"Keyboard and Mouse", "Clavier et souris", "Tastiera e mouse", "Tastatur und Maus",
        "Teclado y ratón", "Toetsenbord en muis", "Teclado e rato", "Tastatur og mus", "Näppäimistö ja hiiri",
        "Tastatur og mus", "Tangentbord och mus", "キーボード"}},
    {PTX_SplitWaiting, {"waiting", "en attente", "in attesa", "wartet", "esperando", "wacht", "à espera", "venter",
        "odottaa", "venter", "väntar", "まって"}},
    {PTX_SplitLayout, {"Layout", "Disposition", "Disposizione", "Aufteilung", "Disposición", "Indeling", "Disposição",
        "Opdeling", "Asettelu", "Oppsett", "Layout", "レイアウト"}},
    {PTX_SplitLayoutAuto, {"Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "オート"}},
    {PTX_SplitLayoutSide, {"Side by Side", "Côte à côte", "Affiancati", "Nebeneinander", "Lado a lado", "Naast elkaar",
        "Lado a lado", "Side om side", "Rinnakkain", "Side om side", "Sida vid sida", "よこならび"}},
    {PTX_SplitLayoutStacked, {"Stacked (Original)", "Superposés (original)", "Sovrapposti (originale)",
        "Übereinander (Original)", "Apiladas (original)", "Boven elkaar (origineel)", "Empilhados (original)",
        "Over hinanden (original)", "Päällekkäin (alkuperäinen)", "Over hverandre (original)", "Över varandra (original)",
        "たてならび (オリジナル)"}},
    {PTX_SplitFourPlayers, {"4 Players (Experimental)", "4 joueurs (expérimental)", "4 giocatori (sperimentale)",
        "4 Spieler (experimentell)", "4 jugadores (experimental)", "4 spelers (experimenteel)", "4 jogadores (experimental)",
        "4 spillere (eksperimentel)", "4 pelaajaa (kokeellinen)", "4 spillere (eksperimentell)", "4 spelare (experimentell)",
        "4プレイヤー (テスト)"}},
    // P: (copsmap3.h) Cops and Robbers on the other hubs (the Japanese: kana and ASCII only)
    {PTX_CopsMap, {"Map", "Carte", "Mappa", "Karte", "Mapa", "Kaart", "Mapa", "Kort", "Kartta", "Kart", "Karta", "マップ"}},
    {PTX_CopsMapOriginal, {"The original game", "Le jeu original", "Il gioco originale", "Das Originalspiel", "El juego original", "Het originele spel", "O jogo original", "Det originale spil", "Alkuperäinen peli", "Det originale spillet", "Originalspelet", "オリジナル"}},
    {PTX_CopsMapNative, {"New map: loot, safe points, power-ups", "Nouvelle carte : butin, points sûrs, bonus", "Nuova mappa: bottino, punti sicuri, potenziamenti", "Neue Karte: Beute, sichere Orte, Power-ups", "Mapa nuevo: botín, puntos seguros, potenciadores", "Nieuwe kaart: buit, veilige plekken, power-ups", "Novo mapa: saque, pontos seguros, power-ups", "Nyt kort: bytte, sikre steder, power-ups", "Uusi kartta: saalis, turvapaikat, tehosteet", "Nytt kart: bytte, trygge steder, power-ups", "Ny karta: byte, säkra platser, power-ups", "あたらしいマップ"}},
    {PTX_CopsFirstTo, {"First to 5 points wins!", "Le premier à 5 points gagne !", "Vince chi arriva a 5 punti!", "Wer zuerst 5 Punkte hat, gewinnt!", "¡Gana el primero en llegar a 5 puntos!", "Wie het eerst 5 punten heeft, wint!", "Ganha quem chegar primeiro a 5 pontos!", "Først til 5 point vinder!", "Ensimmäinen 5 pisteeseen voittaa!", "Først til 5 poeng vinner!", "Först till 5 poäng vinner!", "5ポイントでかち!"}},
    {PTX_CopsSlyLoot, {"Sly has the loot!", "Sly a le butin !", "Sly ha il bottino!", "Sly hat die Beute!", "¡Sly tiene el botín!", "Sly heeft de buit!", "O Sly tem o saque!", "Sly har byttet!", "Slyllä on saalis!", "Sly har byttet!", "Sly har bytet!", "スライがおたからをゲット!"}},
    {PTX_CopsDelivered, {"Loot delivered!", "Butin livré !", "Bottino consegnato!", "Beute abgeliefert!", "¡Botín entregado!", "Buit afgeleverd!", "Saque entregue!", "Byttet er afleveret!", "Saalis toimitettu!", "Byttet er levert!", "Bytet levererat!", "おたからをとどけた!"}},
    {PTX_CopsDropped, {"Sly dropped the loot!", "Sly a lâché le butin !", "Sly ha perso il bottino!", "Sly hat die Beute fallen lassen!", "¡Sly ha soltado el botín!", "Sly heeft de buit laten vallen!", "O Sly deixou cair o saque!", "Sly tabte byttet!", "Sly pudotti saaliin!", "Sly mistet byttet!", "Sly tappade bytet!", "スライがおたからをおとした!"}},
    {PTX_CopsCaught, {"Carmelita caught Sly!", "Carmelita a attrapé Sly !", "Carmelita ha preso Sly!", "Carmelita hat Sly erwischt!", "¡Carmelita ha atrapado a Sly!", "Carmelita heeft Sly gepakt!", "A Carmelita apanhou o Sly!", "Carmelita fangede Sly!", "Carmelita nappasi Slyn!", "Carmelita tok Sly!", "Carmelita fångade Sly!", "カルメリータがスライをつかまえた!"}},
    {PTX_CopsKnockedOut, {"Sly knocked Carmelita out!", "Sly a assommé Carmelita !", "Sly ha messo KO Carmelita!", "Sly hat Carmelita ausgeknockt!", "¡Sly ha noqueado a Carmelita!", "Sly heeft Carmelita uitgeschakeld!", "O Sly pôs a Carmelita KO!", "Sly slog Carmelita ud!", "Sly tyrmäsi Carmelitan!", "Sly slo ut Carmelita!", "Sly slog ut Carmelita!", "スライがカルメリータをたおした!"}},
    {PTX_CopsPowerSly, {"Smoke Bombs: &2L&.", "Bombes fumigènes : &2L&.", "Bombe fumogene: &2L&.", "Rauchbomben: &2L&.", "Bombas de humo: &2L&.", "Rookbommen: &2L&.", "Bombas de fumo: &2L&.", "Røgbomber: &2L&.", "Savupommit: &2L&.", "Røykbomber: &2L&.", "Rökbomber: &2L&.", "スモークボム: &2L&."}},
    {PTX_CopsPowerCarm, {"Power-up: &2L&.", "Bonus : &2L&.", "Potenziamento: &2L&.", "Power-up: &2L&.", "Potenciador: &2L&.", "Power-up: &2L&.", "Poder: &2L&.", "Power-up: &2L&.", "Tehoste: &2L&.", "Power-up: &2L&.", "Power-up: &2L&.", "パワーアップ: &2L&."}},
    {PTX_CopsYouCarry, {"You have the loot: get it to a safe point!", "Vous avez le butin : apportez-le à un point sûr !", "Hai il bottino: portalo a un punto sicuro!", "Du hast die Beute: bring sie zu einem sicheren Ort!", "¡Tienes el botín: llévalo a un punto seguro!", "Je hebt de buit: breng hem naar een veilige plek!", "Tens o saque: leva-o para um ponto seguro!", "Du har byttet: bring det til et sikkert sted!", "Saalis on sinulla: vie se turvapaikkaan!", "Du har byttet: få det til et trygt sted!", "Du har bytet: ta det till en säker plats!", "おたからをセーフポイントへ!"}},
    {PTX_CopsSlyWins, {"Sly Wins!", "Sly gagne !", "Vince Sly!", "Sly gewinnt!", "¡Gana Sly!", "Sly wint!", "O Sly ganha!", "Sly vinder!", "Sly voittaa!", "Sly vinner!", "Sly vinner!", "スライのかち!"}},
    {PTX_CopsCarmWins, {"Carmelita Wins!", "Carmelita gagne !", "Vince Carmelita!", "Carmelita gewinnt!", "¡Gana Carmelita!", "Carmelita wint!", "A Carmelita ganha!", "Carmelita vinder!", "Carmelita voittaa!", "Carmelita vinner!", "Carmelita vinner!", "カルメリータのかち!"}},
    // P: (hackmp3.h) the hackers' select (the names: the Japanese disc's katakana)
    {PTX_HackCharNames, {"Bentley|Sly|Penelope|Carmelita|Murray|Panda King", "Bentley|Sly|Penelope|Carmelita|Murray|Roi Panda", "Bentley|Sly|Penelope|Carmelita|Murray|Re Panda", "Bentley|Sly|Penelope|Carmelita|Murray|Panda-König", "Bentley|Sly|Penelope|Carmelita|Murray|Rey Panda", "Bentley|Sly|Penelope|Carmelita|Murray|Pandakoning", "Bentley|Sly|Penelope|Carmelita|Murray|Rei Panda", "Bentley|Sly|Penelope|Carmelita|Murray|Pandakongen", "Bentley|Sly|Penelope|Carmelita|Murray|Pandakuningas", "Bentley|Sly|Penelope|Carmelita|Murray|Pandakongen", "Bentley|Sly|Penelope|Carmelita|Murray|Pandakungen", "ベントレー|スライ|ペネロペ|カルメリータ|マーレイ|パンダキング"}},
    {PTX_HackSelectTitle, {"Choose your hacker", "Choisissez votre hacker", "Scegli il tuo hacker", "Wähle deinen Hacker", "Elige tu hacker", "Kies je hacker", "Escolhe o teu hacker", "Vælg din hacker", "Valitse hakkerisi", "Velg hackeren din", "Välj din hackare", "ハッカーをえらぼう"}},
    {PTX_HackSelectHint, {"&2+&. Choose   &2X&. Ready   &2O&. Back", "&2+&. Choisir   &2X&. Prêt   &2O&. Retour", "&2+&. Scegli   &2X&. Pronto   &2O&. Indietro", "&2+&. Wählen   &2X&. Bereit   &2O&. Zurück", "&2+&. Elegir   &2X&. Listo   &2O&. Atrás", "&2+&. Kiezen   &2X&. Klaar   &2O&. Terug", "&2+&. Escolher   &2X&. Pronto   &2O&. Voltar", "&2+&. Vælg   &2X&. Klar   &2O&. Tilbage", "&2+&. Valitse   &2X&. Valmis   &2O&. Takaisin", "&2+&. Velg   &2X&. Klar   &2O&. Tilbake", "&2+&. Välj   &2X&. Redo   &2O&. Tillbaka", "&2+&. えらぶ   &2X&. OK   &2O&. もどる"}},
    {PTX_HackReady, {"Ready!", "Prêt !", "Pronto!", "Bereit!", "¡Listo!", "Klaar!", "Pronto!", "Klar!", "Valmis!", "Klar!", "Redo!", "OK!"}},
    {PTX_HackChoose, {"Choose", "Choisir", "Scegli", "Wählen", "Elegir", "Kiezen", "Escolher", "Vælg", "Valitse", "Velg", "Välj", "えらぶ"}},
    // P: (duel3.h) the Biplane Duel's free-for-all (the pilots are "Player N" there; the original duel keeps its texts)
    {PTX_DuelPlayers, {"Players", "Joueurs", "Giocatori", "Spieler", "Jugadores", "Spelers", "Jogadores", "Spillere", "Pelaajat", "Spillere", "Spelare", "プレイヤー"}},
    {PTX_DuelOriginal, {"The original duel", "Le duel original", "Il duello originale", "Das Originalduell", "El duelo original", "Het originele duel", "O duelo original", "Den originale duel", "Alkuperäinen kaksintaistelu", "Den originale duellen", "Den ursprungliga duellen", "オリジナル"}},
    {PTX_DuelFreeForAll, {"Free-for-all: each kill counts for its shooter", "Chacun pour soi : chaque avion abattu compte pour le tireur", "Tutti contro tutti: ogni abbattimento conta per chi spara", "Jeder gegen jeden: jeder Abschuss zählt für den Schützen", "Todos contra todos: cada derribo cuenta para quien dispara", "Ieder voor zich: elke treffer telt voor de schutter", "Todos contra todos: cada abate conta para quem dispara", "Alle mod alle: hvert nedskud tæller for skytten", "Kaikki kaikkia vastaan: pudotus lasketaan ampujalle", "Alle mot alle: hver nedskyting teller for skytteren", "Alla mot alla: varje nedskjutning räknas för skytten", "バトルロイヤル: うちおとすとポイント"}},
    {PTX_DuelNPlayers, {"%d Players", "%d joueurs", "%d giocatori", "%d Spieler", "%d jugadores", "%d spelers", "%d jogadores", "%d spillere", "%d pelaajaa", "%d spillere", "%d spelare", "%dプレイヤー"}},
    {PTX_DuelDescFFA, {"Up to four pilots dogfight over Holland. Every plane shot down counts for its shooter: the first to 10 wins!", "Jusqu'à quatre pilotes s'affrontent au-dessus de la Hollande. Chaque avion abattu compte pour le tireur : le premier à 10 gagne !", "Fino a quattro piloti si sfidano sull'Olanda. Ogni aereo abbattuto conta per chi spara: vince il primo a 10!", "Bis zu vier Piloten kämpfen über Holland. Jedes abgeschossene Flugzeug zählt für den Schützen: Wer zuerst 10 hat, gewinnt!", "Hasta cuatro pilotos combaten sobre Holanda. Cada avión derribado cuenta para quien dispara: ¡gana el primero en llegar a 10!", "Tot vier piloten vechten boven Holland. Elk neergeschoten vliegtuig telt voor de schutter: wie het eerst 10 heeft, wint!", "Até quatro pilotos combatem sobre a Holanda. Cada avião abatido conta para quem dispara: ganha o primeiro a chegar a 10!", "Op til fire piloter kæmper over Holland. Hvert nedskudt fly tæller for skytten: først til 10 vinder!", "Jopa neljä lentäjää taistelee Hollannin yllä. Jokainen pudotettu kone lasketaan ampujalle: ensimmäinen 10:een voittaa!", "Opptil fire piloter kjemper over Holland. Hvert nedskutt fly teller for skytteren: først til 10 vinner!", "Upp till fyra piloter strider över Holland. Varje nedskjutet plan räknas för skytten: först till 10 vinner!", "4にんまでのパイロットがたたかう! うちおとすと1ポイント 10ポイントでかち!"}},
    {PTX_DuelFirstTo, {"First to %d kills wins!", "Le premier à %d avions abattus gagne !", "Vince chi arriva a %d abbattimenti!", "Wer zuerst %d Abschüsse hat, gewinnt!", "¡Gana el primero en llegar a %d derribos!", "Wie het eerst %d treffers heeft, wint!", "Ganha quem chegar primeiro a %d abates!", "Først til %d nedskydninger vinder!", "Ensimmäinen %d pudotukseen voittaa!", "Først til %d nedskytinger vinner!", "Först till %d nedskjutningar vinner!", "%dポイントでかち!"}},
    {PTX_DuelShotDown, {"%s shot down %s!", "%s a abattu %s !", "%s ha abbattuto %s!", "%s hat %s abgeschossen!", "¡%s derribó a %s!", "%s heeft %s neergeschoten!", "%s abateu %s!", "%s skød %s ned!", "%s pudotti: %s!", "%s skjøt ned %s!", "%s sköt ned %s!", "%sが%sをうちおとした!"}},
    {PTX_DuelLeads, {"%s leads!", "%s mène !", "%s è in testa!", "%s führt!", "¡%s va en cabeza!", "%s staat voor!", "%s está na frente!", "%s fører!", "%s johtaa!", "%s leder!", "%s leder!", "%sがリード!"}},
    {PTX_DuelWins, {"%s Wins!", "%s gagne !", "Vince %s!", "%s gewinnt!", "¡Gana %s!", "%s wint!", "%s ganha!", "%s vinder!", "%s voittaa!", "%s vinner!", "%s vinner!", "%sのかち!"}},
    {PTX_DuelCrashed, {"%s crashed!", "%s s'est écrasé !", "%s si è schiantato!", "%s ist abgestürzt!", "¡%s se estrelló!", "%s is neergestort!", "%s caiu!", "%s styrtede ned!", "%s syöksyi maahan!", "%s styrtet!", "%s störtade!", "%sがついらく!"}},
    // P: the display options' Monitor (settings v22; a monitor's own name is shown as the system gives it)
    {PTX_Monitor, {"Monitor", "Écran", "Monitor", "Monitor", "Monitor", "Monitor", "Monitor", "Skærm", "Näyttö", "Skjerm", "Skärm", "モニター"}},
    {PTX_Primary, {"Primary", "Principal", "Principale", "Primär", "Principal", "Primair", "Principal", "Primær", "Ensisijainen", "Primær", "Primär", "メイン"}},
};
// clang-format on
static_assert(std::size(s_arow) == PTX_Max, "one row per PTX");

constexpr bool FRowsInOrder()
{
    for (int i = 0; i < PTX_Max; ++i)
        if (s_arow[i].ptx != i) return false;
    return true;
}
static_assert(FRowsInOrder(), "the rows in PTX order");

int IlangTable()
{
    // (the text's language: a store language's own, or the disc's that it falls back to; region.h LangText)
    // (test: SLY2N_PORTTEXTLANG=N the PC texts' column N (0..11) whatever the game's language: their layouts' screenshots)
    static const int s_langTest = std::getenv("SLY2N_PORTTEXTLANG") ? std::atoi(std::getenv("SLY2N_PORTTEXTLANG")) : -1;
    const int lang = s_langTest >= 0 ? s_langTest : LangText();
    const int ilang = lang >= 0 && lang < CLANG ? lang : LANG_English;
    // (the Japanese column needs a font with kana: Sly 3's (USA) has none (SLY2N_FONTLIST), its text would draw blank, so
    // English; the Latin columns are folded to the font's letters: StrToGameText)
    return ilang == LANG_Japanese ? LANG_English : ilang;
}

const char* SzRow(PTX ptx) { return s_arow[ptx].asz[IlangTable()]; }

#include "porttext_actions.inc"

// the language writes decimals with a comma
bool FDecimalComma()
{
    switch (IlangTable())
    {
    case LANG_English: case LANG_Japanese: return false;
    default: return true;
    }
}
}  // namespace

std::string StrToGameText(const std::string& utf8)
{
    // the Japanese disc's text keeps characters >= 0x80 as UTF-8 already (region.h)
    if (FWideText()) return IlangTable() == LANG_Japanese ? utf8 : StrFoldLatinForWideFont(utf8, IlangTable());   // (langstore.h)
    // the other discs' text is Latin-1 (their fonts' glyphs 0x20..0xFF)
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();)
    {
        const uint8_t b = uint8_t(utf8[i]);
        uint32_t ch = b;
        size_t cb = 1;
        if (b >= 0xC0 && b < 0xE0 && i + 1 < utf8.size()) { ch = ((b & 0x1Fu) << 6) | (uint8_t(utf8[i + 1]) & 0x3Fu); cb = 2; }
        else if (b >= 0xE0 && b < 0xF0 && i + 2 < utf8.size()) { ch = 0x100; cb = 3; }
        else if (b >= 0xF0) { ch = 0x100; cb = 4; }
        out += ch < 0x100 ? char(ch) : '?';
        i += cb;
    }
    return out;
}

bool FQuitToLauncher()
{
    static const bool s_f = [] {
        const char* sz = std::getenv("SAFEHOUSE_LAUNCHER");
        return sz && std::string(sz) == "collection";
    }();
    return s_f;
}

std::string StrPort(PTX ptx)
{
    if (ptx < 0 || ptx >= PTX_Max) return "";
    return StrToGameText(SzRow(ptx));
}

std::string StrPortActionName(cane::sly2input::Action action)
{
    const size_t index = size_t(action);
    if (index >= cane::sly2input::kActionCount) return {};
    const int lang = IlangTable();
    return StrToGameText(lang == LANG_English ? cane::sly2input::kActions[index].label : s_actionNames[index].text[lang - 1]);
}

std::string StrPortActionGroup(std::string_view group)
{
    const int lang = IlangTable();
    if (lang != LANG_English)
        for (const auto& row : s_actionGroups)
            if (row.group == group) return StrToGameText(row.text[lang - 1]);
    return StrToGameText(std::string(group));
}

std::string StrPortValue(PTX ptxLabel, const std::string& value) { return StrPort(ptxLabel) + StrPort(PTX_Sep) + value; }

std::string StrPortOnOff(PTX ptxLabel, bool f) { return StrPortValue(ptxLabel, StrPort(f ? PTX_On : PTX_Off)); }

std::string StrPortDecimal(float g, int cdigits)
{
    char ach[32];
    std::snprintf(ach, sizeof(ach), "%.*f", cdigits, double(g));
    std::string str = ach;
    if (FDecimalComma())
        for (char& ch : str)
            if (ch == '.') ch = ',';
    return str;
}

bool FPortEnglish() { return IlangTable() == LANG_English; }

std::string StrPortKeyName(int key)
{
    if (FPortEnglish()) return PchzKeyName(key);
    char ach[96];
    auto fmt = [&](PTX ptx, const char* szArg) {
        std::snprintf(ach, sizeof(ach), SzRow(ptx), szArg);
        return StrToGameText(ach);
    };
    namespace st = cane::settings;
    if (st::mouseBinding(key))
    {
        const int i = key - st::kMouseBindingBase;
        switch (i)
        {
        case 0: return StrPort(PTX_MouseLeft);
        case 1: return StrPort(PTX_MouseRight);
        case 2: return StrPort(PTX_MouseMiddle);
        case 8: return StrPort(PTX_WheelUp);
        case 9: return StrPort(PTX_WheelDown);
        default:
            if (i >= 3 && i <= 7)
            {
                std::snprintf(ach, sizeof(ach), SzRow(PTX_MouseN), i + 1);
                return StrToGameText(ach);
            }
            return StrPort(PTX_Mouse);
        }
    }
    switch (key)
    {
    case GLFW_KEY_UNKNOWN: return StrPort(PTX_KeyUnbound);
    case GLFW_KEY_SPACE: return StrPort(PTX_KeySpace);
    case GLFW_KEY_UP: return StrPort(PTX_KeyUp);
    case GLFW_KEY_DOWN: return StrPort(PTX_KeyDown);
    case GLFW_KEY_LEFT: return StrPort(PTX_KeyLeft);
    case GLFW_KEY_RIGHT: return StrPort(PTX_KeyRight);
    case GLFW_KEY_LEFT_SHIFT: return fmt(PTX_KeyLeftMod, "Shift");
    case GLFW_KEY_RIGHT_SHIFT: return fmt(PTX_KeyRightMod, "Shift");
    case GLFW_KEY_LEFT_CONTROL: return fmt(PTX_KeyLeftMod, IlangTable() == LANG_German ? "Strg" : "Ctrl");
    case GLFW_KEY_RIGHT_CONTROL: return fmt(PTX_KeyRightMod, IlangTable() == LANG_German ? "Strg" : "Ctrl");
    case GLFW_KEY_LEFT_ALT: return fmt(PTX_KeyLeftMod, "Alt");
    case GLFW_KEY_RIGHT_ALT: return fmt(PTX_KeyRightMod, "Alt");
    default: break;
    }
    // (the layout's own key names come from the system as UTF-8: "Ö" on a Swedish keyboard)
    return StrToGameText(PchzKeyName(key));
}

// ------------------------------------------------------------ the PC wording of the game's own messages
// The PS2's memory card messages, said for a PC (the save data in the save folder, "do not close the game"): the USA /
// European ids (USA ids; the European discs' are the same below 0x6D) in English .. Swedish (the game's language
// order without Japanese), and the Japanese disc's own ids (its messages differ). All P (ours), in each language's
// own terms of the original messages (its save file, auto-save and "save game" option, its "you" / imperative). Null:
// that language's original says nothing of the PS2 (the European headset options).
namespace
{
struct PCROW
{
    int idUsa;
    const char* asz[11];
};
// clang-format off
[[maybe_unused]] constexpr PCROW s_apcrow[] = {
    {0x11, {
        "No save folder was found\n\nSly 2 requires 100 KB of free space\n\nPlease check the save folder or you will not be able to save your progress\n",
        "Aucun dossier de sauvegarde n'a été trouvé.\n\nSly 2 nécessite 100 KB d'espace libre.\n\nVérifie le dossier de sauvegarde, sinon tu ne pourras pas sauvegarder ta partie.\n",
        "Nessuna cartella di salvataggio trovata.\n\nSly 2 richiede 100 KB di spazio libero.\n\nControlla la cartella di salvataggio, altrimenti sarà impossibile salvare i progressi.\n",
        "Es wurde kein Speicherordner gefunden.\n\nSly 2 benötigt 100 KB freien Speicherplatz.\n\nBitte überprüfe den Speicherordner, sonst kannst du deinen Spielstand nicht speichern.\n",
        "No se ha encontrado la carpeta de partidas guardadas.\n\nSly 2 requiere 100KB de espacio libre.\n\nComprueba la carpeta de partidas guardadas o no podrás guardar tus datos.\n",
        "Er is geen map voor opgeslagen spellen gevonden\n\nSly 2 vereist 100 KB vrije ruimte\n\nControleer de map, anders kun je je voortgang niet opslaan\n",
        "Não foi encontrada nenhuma pasta de jogos guardados\n\nO Sly 2 requer 100 KB de espaço livre\n\nVerifica a pasta de jogos guardados ou não poderás guardar o progresso do jogo\n",
        "Der blev ikke fundet nogen mappe til gemte spil\n\nSly 2 kræver 100 KB ledig plads\n\nKontrollér mappen, ellers kan du ikke gemme udviklingen i spillet\n",
        "Tallennuskansiota ei löytynyt\n\nSly 2 vaatii 100 KB vapaata tilaa\n\nTarkista tallennuskansio, muuten et voi tallentaa edistymistäsi\n",
        "Fant ingen mappe for lagrede spill\n\nSly 2 trenger 100 KB ledig plass\n\nKontroller mappen, ellers vil du ikke kunne lagre\n",
        "Ingen mapp för sparade spel hittades\n\nSly 2 kräver 100 KB ledigt utrymme\n\nKontrollera mappen, annars kommer du inte att kunna spara dina framsteg\n"}},
    {0x12, {
        "The Sly 2 save data could not be read\n\nIf you do not reset it then you will not be able to save your progress\n\nDo you want to reset it?",
        "Les données de sauvegarde de Sly 2 sont illisibles.\n\nSi tu ne les réinitialises pas, tu ne pourras pas sauvegarder ta partie.\n\nRéinitialiser les données ?",
        "Impossibile leggere i dati di salvataggio di Sly 2.\n\nSe non verranno reimpostati sarà impossibile salvare i progressi.\n\nVuoi reimpostarli?",
        "Die Sly 2-Speicherdaten können nicht gelesen werden.\n\nWenn du sie nicht zurücksetzt, kannst du deinen Spielstand nicht speichern.\n\nMöchtest du sie zurücksetzen?",
        "No se pueden leer los datos guardados de Sly 2.\n\nSi no los restableces, no podrás guardar tus datos.\n\n¿Deseas restablecerlos?",
        "De opgeslagen gegevens van Sly 2 kunnen niet worden gelezen\n\nAls je ze niet opnieuw instelt, kun je je voortgang niet opslaan\n\nWil je ze opnieuw instellen?",
        "Não é possível ler os dados guardados do Sly 2\n\nSe não os repuseres, não poderás guardar o progresso do jogo\n\nQueres repor os dados?",
        "Sly 2-lagerdataene kan ikke læses\n\nHvis du ikke nulstiller dem, kan du ikke gemme udviklingen i spillet\n\nVil du nulstille dem?",
        "Sly 2 -tallennustietoja ei voi lukea\n\nJos et palauta niitä alkutilaan, et voi tallentaa edistymistäsi\n\nPalautetaanko tallennustiedot alkutilaan?",
        "Sly 2-lagringsdataene kan ikke leses\n\nHvis du ikke tilbakestiller dem, vil du ikke kunne lagre\n\nVil du tilbakestille dem?",
        "Sparinformationen för Sly 2 kan inte läsas\n\nOm du inte återställer den så kommer du inte att kunna spara dina framsteg\n\nVill du återställa den?"}},
    {0x13, {
        "\nResetting the Sly 2 save data\n\nPlease do not close the game\n",
        "\nRéinitialisation des données de sauvegarde de Sly 2.\n\nNe pas fermer le jeu.\n",
        "\nReimpostazione dei dati di salvataggio di Sly 2 in corso...\n\nNon chiudere il gioco!\n",
        "\nSetzt die Sly 2-Speicherdaten zurück.\n\nBitte das Spiel nicht beenden.\n",
        "\nRestableciendo los datos guardados de Sly 2...\n\nNo cierres el juego.\n",
        "\nBezig met het opnieuw instellen van de opgeslagen gegevens van Sly 2\n\nSluit het spel niet af\n",
        "\nA repor os dados guardados do Sly 2\n\nNão feches o jogo\n",
        "\nNulstiller Sly 2-lagerdataene\n\nDu må ikke lukke spillet\n",
        "\nPalautetaan Sly 2 -tallennustietoja alkutilaan\n\nÄlä sulje peliä\n",
        "\nTilbakestiller Sly 2-lagringsdataene\n\nIkke lukk spillet\n",
        "\nÅterställer sparinformationen för Sly 2\n\nAvsluta inte spelet\n"}},
    {0x14, {
        "An error occurred while resetting the Sly 2 save data\n\nYou will not be able to save your progress\n",
        "Erreur lors de la réinitialisation des données de sauvegarde de Sly 2.\n\nTu ne pourras pas sauvegarder ta partie.\n",
        "Errore durante la reimpostazione dei dati di salvataggio di Sly 2.\n\nSarà impossibile salvare i progressi.\n",
        "Fehler beim Zurücksetzen der Sly 2-Speicherdaten.\n\nDu kannst deinen Spielstand nicht speichern.\n",
        "Se ha producido un error al restablecer los datos guardados de Sly 2.\n\nNo podrás guardar tus datos.\n",
        "Er is een fout opgetreden bij het opnieuw instellen van de opgeslagen gegevens van Sly 2\n\nJe kunt je voortgang niet opslaan\n",
        "Ocorreu um erro ao repor os dados guardados do Sly 2\n\nNão poderás guardar o progresso do jogo\n",
        "Der opstod en fejl under nulstillingen af Sly 2-lagerdataene\n\nDu kan ikke gemme udviklingen i spillet\n",
        "Sly 2 -tallennustietojen palauttaminen epäonnistui\n\nEt voi tallentaa edistymistäsi\n",
        "Det oppstod en feil under tilbakestillingen av Sly 2-lagringsdataene\n\nDu vil ikke kunne lagre\n",
        "Ett fel uppstod när sparinformationen för Sly 2 skulle återställas\n\nDu kommer inte att kunna spara dina framsteg\n"}},
    {0x15, {
        "No Sly 2 save data was found\n\nSly 2 requires 100 KB of free disk space\n\nIf you do not create save data then you will not be able to save your progress\n\nDo you want to create Sly 2 save data?",
        "Aucun fichier de sauvegarde Sly 2.\n\nSly 2 nécessite 100 KB d'espace libre sur le disque.\n\nPour sauvegarder la partie, un fichier Sly 2 est nécessaire.\n\nCréer un fichier Sly 2 ?",
        "Nessun salvataggio di Sly 2.\n\nSly 2 richiede 100 KB di spazio libero su disco.\n\nSe non verrà creato un file di salvataggio di Sly 2 sarà impossibile salvare i progressi.\n\nVuoi creare un file di salvataggio di Sly 2?",
        "Keine Sly 2-Speicherdatei vorhanden.\n\nSly 2 benötigt 100 KB freien Speicherplatz auf dem Datenträger.\n\nWenn du keine Sly 2-Speicherdatei erstellst, kannst du deinen Spielstand nicht speichern.\n\nMöchtest du eine Sly 2-Speicherdatei erstellen?",
        "No hay archivo de datos de Sly 2.\n\nSly 2 requiere 100KB de espacio libre en el disco.\n\nSi no creas un archivo de datos de Sly 2, no podrás guardar tus datos.\n\n¿Deseas crear un archivo de datos de Sly 2?",
        "Geen opslagbestand van Sly 2\n\nSly 2 vereist 100 KB vrije schijfruimte\n\nAls je geen opslagbestand van Sly 2 maakt, kun je je voortgang niet opslaan\n\nWil je een opslagbestand van Sly 2 maken?",
        "Não existe nenhum ficheiro guardado para o Sly 2\n\nO Sly 2 requer 100 KB de espaço livre no disco\n\nSe não criares um ficheiro para guardar o Sly 2, não poderás guardar o progresso do jogo\n\nQueres criar um ficheiro para guardar o Sly 2?",
        "Der er ingen Sly 2-lagerfil\n\nSly 2 kræver 100 KB ledig diskplads\n\nDu vil ikke kunne gemme udviklingen i spillet, med mindre du opretter en Sly 2-lagerfil\n\nVil du oprette en Sly 2-lagerfil?",
        "Sly 2 -tallennustiedostoa ei ole\n\nSly 2 vaatii 100 KB vapaata levytilaa\n\nJos et luo Sly 2 -tallennustiedostoa, et voi tallentaa edistymistäsi\n\nLuodaanko Sly 2 -tallennustiedosto?",
        "Det er ingen Sly 2-Lagringsfil\n\nSly 2 trenger 100 KB ledig diskplass\n\nHvis du ikke oppretter en Sly 2-Lagringsfil, vil du ikke kunne lagre\n\nVil du opprette en Sly 2-Lagringsfil?",
        "Det finns ingen Sly 2-sparfil\n\nSly 2 kräver 100 KB ledigt diskutrymme\n\nOm du inte skapar en Sly 2-sparfil så kommer du inte att kunna spara dina framsteg\n\nVill du skapa en Sly 2-sparfil?"}},
    {0x16, {
        "\nCreating the Sly 2 save data\n\nPlease do not close the game\n",
        "\nCréation du fichier de sauvegarde Sly 2.\n\nNe pas fermer le jeu.\n",
        "\nCreazione del file di salvataggio di Sly 2 in corso...\n\nNon chiudere il gioco!\n",
        "\nErstellt eine Sly 2-Speicherdatei.\n\nBitte das Spiel nicht beenden.\n",
        "\nCreando el archivo de datos de Sly 2...\n\nNo cierres el juego.\n",
        "\nBezig met het maken van het opslagbestand van Sly 2\n\nSluit het spel niet af\n",
        "\nA criar o ficheiro para guardar o Sly 2\n\nNão feches o jogo\n",
        "\nOpretter Sly 2-lagerfilen\n\nDu må ikke lukke spillet\n",
        "\nLuodaan Sly 2 -tallennustiedostoa\n\nÄlä sulje peliä\n",
        "\nOppretter Sly 2-Lagringsfilen\n\nIkke lukk spillet\n",
        "\nSkapar Sly 2-sparfilen\n\nAvsluta inte spelet\n"}},
    {0x17, {
        "An error occurred while creating the Sly 2 save data\n\nYour progress is not being saved\n",
        "Erreur lors de la création du fichier de sauvegarde Sly 2.\n\nTa progression n'est pas sauvegardée.\n",
        "Errore durante la creazione del file di salvataggio di Sly 2.\n\nI progressi non vengono salvati.\n",
        "Fehler beim Erstellen der Sly 2-Speicherdatei.\n\nDein Spielstand wird nicht gespeichert.\n",
        "Se ha producido un error al crear el archivo de datos de Sly 2.\n\nNo se están guardando tus datos.\n",
        "Er is een fout opgetreden bij het maken van het opslagbestand van Sly 2\n\nJe voortgang wordt niet opgeslagen\n",
        "Ocorreu um erro ao criar o ficheiro para guardar o Sly 2\n\nO progresso do jogo não está a ser guardado\n",
        "Der opstod en fejl under oprettelsen af Sly 2-lagerfilen\n\nUdviklingen i spillet gemmes ikke\n",
        "Sly 2 -tallennustiedoston luominen epäonnistui\n\nEdistymistäsi ei tallenneta\n",
        "Det oppstod en feil under opprettelsen av Sly 2-Lagringsfilen\n\nSpillet blir ikke lagret\n",
        "Ett fel uppstod när Sly 2-sparfilen skulle skapas\n\nDina framsteg sparas inte\n"}},
    {0x18, {
        "There is not enough free disk space for the Sly 2 save data\n\nSly 2 requires 100 KB of free space\n\nPlease free up some disk space or you will not be able to save your progress\n",
        "Espace disque insuffisant pour la sauvegarde de Sly 2.\n\nSly 2 nécessite 100 KB d'espace libre.\n\nLibère de l'espace disque, sinon tu ne pourras pas sauvegarder ta partie.\n",
        "Spazio su disco insufficiente per il salvataggio di Sly 2.\n\nSly 2 richiede 100 KB di spazio libero.\n\nLibera spazio su disco, altrimenti sarà impossibile salvare i progressi.\n",
        "Auf dem Datenträger ist nicht genug Speicherplatz für die Sly 2-Speicherdatei.\n\nSly 2 benötigt 100 KB freien Speicherplatz.\n\nBitte schaffe Platz auf dem Datenträger, sonst kannst du deinen Spielstand nicht speichern.\n",
        "No hay suficiente espacio libre en el disco para el archivo de datos de Sly 2.\n\nSly 2 requiere 100KB de espacio libre.\n\nLibera espacio en el disco o no podrás guardar tus datos.\n",
        "Er is niet genoeg vrije schijfruimte voor het opslagbestand van Sly 2\n\nSly 2 vereist 100 KB vrije ruimte\n\nMaak schijfruimte vrij, anders kun je je voortgang niet opslaan\n",
        "Não existe espaço livre suficiente no disco para o ficheiro do Sly 2\n\nO Sly 2 requer 100 KB de espaço livre\n\nLiberta espaço no disco ou não poderás guardar o progresso do jogo\n",
        "Der er ikke nok ledig diskplads til Sly 2-lagerfilen\n\nSly 2 kræver 100 KB ledig plads\n\nFrigør diskplads, ellers kan du ikke gemme udviklingen i spillet\n",
        "Levyllä ei ole tarpeeksi vapaata tilaa Sly 2 -tallennustiedostolle\n\nSly 2 vaatii 100 KB vapaata tilaa\n\nVapauta levytilaa, muuten et voi tallentaa edistymistäsi\n",
        "Det er ikke nok ledig diskplass til Sly 2-Lagringsfilen\n\nSly 2 trenger 100 KB ledig plass\n\nFrigjør diskplass, ellers vil du ikke kunne lagre\n",
        "Det finns inte tillräckligt med ledigt diskutrymme för Sly 2-sparfilen\n\nSly 2 kräver 100 KB ledigt utrymme\n\nFrigör diskutrymme, annars kommer du inte att kunna spara dina framsteg\n"}},
    {0x1A, {
        "\nSaving...\n\nPlease do not close the game\n",
        "\nSauvegarde en cours...\n\nNe pas fermer le jeu.\n",
        "\nSalvataggio in corso...\n\nNon chiudere il gioco!\n",
        "\nSpeichert ...\n\nBitte das Spiel nicht beenden.\n",
        "\nGuardando...\n\nNo cierres el juego.\n",
        "\nBezig met opslaan...\n\nSluit het spel niet af\n",
        "\nA guardar...\n\nNão feches o jogo\n",
        "\nGemmer...\n\nDu må ikke lukke spillet\n",
        "\nTallennetaan...\n\nÄlä sulje peliä\n",
        "\nLagrer...\n\nIkke lukk spillet\n",
        "\nSparar...\n\nAvsluta inte spelet\n"}},
    {0x1D, {
        "An error occurred while writing the Sly 2 save data\n\nData may be corrupted\n\nYour progress is not being saved\n",
        "Erreur lors de l'écriture du fichier de sauvegarde Sly 2.\n\nLes données sont peut-être corrompues.\n\nTa progression n'est pas sauvegardée.\n",
        "Errore durante la scrittura del file di salvataggio di Sly 2.\n\nI dati possono essere danneggiati.\n\nI progressi non vengono salvati.\n",
        "Fehler beim Speichern der Sly 2-Speicherdatei.\n\nDie Daten können beschädigt sein.\n\nDein Spielstand wird nicht gespeichert.\n",
        "Se ha producido un error al escribir el archivo de datos de Sly 2.\n\nLos datos pueden estar dañados.\n\nNo se están guardando tus datos.\n",
        "Er is een fout opgetreden bij het schrijven van het opslagbestand van Sly 2\n\nGegevens zijn mogelijk beschadigd\n\nJe voortgang wordt niet opgeslagen\n",
        "Ocorreu um erro ao gravar o ficheiro do Sly 2\n\nOs dados poderão estar corrompidos\n\nO progresso do jogo não está a ser guardado\n",
        "Der opstod en fejl under skrivningen af Sly 2-lagerfilen\n\nData kan være ødelagt\n\nUdviklingen i spillet gemmes ikke\n",
        "Sly 2 -tallennustiedoston kirjoittaminen epäonnistui\n\nTiedot voivat olla vahingoittuneet\n\nEdistymistäsi ei tallenneta\n",
        "Det oppstod en feil under skrivingen av Sly 2-Lagringsfilen\n\nData kan være ødelagt\n\nSpillet blir ikke lagret\n",
        "Ett fel uppstod när Sly 2-sparfilen skulle skrivas\n\nInformationen kan vara förvrängd\n\nDina framsteg sparas inte\n"}},
    {0x21, {
        "\nErasing...\n\nPlease do not close the game\n",
        "\nSuppression en cours...\n\nNe pas fermer le jeu.\n",
        "\nCancellazione in corso...\n\nNon chiudere il gioco!\n",
        "\nLöscht ...\n\nBitte das Spiel nicht beenden.\n",
        "\nBorrando...\n\nNo cierres el juego.\n",
        "\nBezig met wissen...\n\nSluit het spel niet af\n",
        "\nA apagar...\n\nNão feches o jogo\n",
        "\nSletter...\n\nDu må ikke lukke spillet\n",
        "\nPoistetaan...\n\nÄlä sulje peliä\n",
        "\nSletter...\n\nIkke lukk spillet\n",
        "\nRaderar...\n\nAvsluta inte spelet\n"}},
    {0x22, {
        "When the following icon appears,\nyour progress is being saved\n\n&2a&.\n\nWhile this icon is on screen, please\ndo not close the game\n",
        "L'icône suivante apparaît\nlorsque la sauvegarde est en cours.\n\n&2a&.\n\nTant qu'elle est à l'écran,\nne pas fermer le jeu.\n",
        "Quando appare la seguente icona,\nè in corso il salvataggio dei tuoi progressi.\n\n&2a&.\n\nFinché l'icona è visibile,\nnon chiudere il gioco!\n",
        "Wird das folgende Symbol angezeigt,\nwird dein Spielstand gespeichert.\n\n&2a&.\n\nSolange das Symbol angezeigt wird,\nbitte das Spiel nicht beenden.\n",
        "Cuando aparezca el icono siguiente,\nse estarán guardando tus datos.\n\n&2a&.\n\nMientras aparezca este icono en la pantalla,\nno cierres el juego.\n",
        "Als het volgende pictogram verschijnt,\nwordt je voortgang opgeslagen\n\n&2a&.\n\nSluit het spel niet af zolang\ndit pictogram op het scherm staat\n",
        "Quando o seguinte ícone aparece,\no progresso do jogo está a ser guardado\n\n&2a&.\n\nEnquanto este ícone estiver no ecrã,\nnão feches o jogo\n",
        "Når det følgende ikon kommer frem,\ngemmes dine fremskridt\n\n&2a&.\n\nMens dette ikon er på skærmen,\nmå du ikke lukke spillet\n",
        "Kun esille tulee seuraava kuvake,\nedistymistäsi tallennetaan\n\n&2a&.\n\nKun tämä kuvake on ruudulla,\nälä sulje peliä\n",
        "Når du ser dette ikonet,\nblir spillet ditt lagret\n\n&2a&.\n\nIkke lukk spillet\nmens dette ikonet står på skjermen\n",
        "När följande ikon visas,\nså sparas dina framsteg\n\n&2a&.\n\nNär denna ikon syns på skärmen\nbör du inte avsluta spelet\n"}},
    {0x23, {
        "Auto-save has failed\n\nAuto-save has been disabled\nData may be corrupted\n\nUse the save game option to manually save your game and re-enable auto-save\n",
        "L'auto-sauvegarde a échoué.\n\nAuto-sauvegarde désactivée.\nLes données sont peut-être corrompues.\n\nUtiliser l'option Sauvegarder partie pour sauvegarder manuellement et réactiver l'Auto-sauvegarde.\n",
        "L'autosalvataggio è fallito\n\nL'autosalvataggio è stato disattivato\nI dati possono essere danneggiati\n\nUsa l'opzione di salvataggio per salvare manualmente i dati e riattivare l'autosalvataggio\n",
        "Auto-Speichern fehlgeschlagen.\n\nAuto-Speicherung wurde deaktiviert.\nDaten können beschädigt sein.\n\nSpiel mit \"Spiel speichern\"-Option manuell speichern und Auto-Speichern neu aktivieren.\n",
        "El autoguardado ha fallado.\n\nSe ha desactivado el autoguardado.\nLos datos pueden estar dañados.\n\nUsa la opción Guardar partida para guardar manualmente la partida y reactiva el autoguardado.\n",
        "Automatisch opslaan is mislukt\n\nAutomatisch opslaan is uitgeschakeld\nGegevens zijn mogelijk beschadigd\n\nGebruik de optie spel opslaan om het spel handmatig op te slaan en schakel automatisch opslaan opnieuw in\n",
        "A operação para guardar automaticamente falhou.\n\nA opção para guardar automaticamente foi desactivada.\nOs dados poderão estar corrompidos.\n\nUtiliza a opção para guardar o jogo manualmente e volta a activar a gravação automática.\n",
        "Den automatiske lagring mislykkedes\n\nAuto-save er blevet deaktiveret\nData kan være ødelagt\n\nBrug muligheden for at gemme spil til manuelt at gemme spillet og genaktivere auto-save\n",
        "Automaattinen tallennus epäonnistui.\n\nAutomaattinen tallennus on otettu pois käytöstä\nTiedot voivat olla vahingoittuneet\n\nKäytä pelintallennustoimintoa tallentaaksesi pelin käsin ja ota automaattinen tallennus uudelleen käyttöön\n",
        "Auto-lagring mislyktes\n\nAuto-lagring er deaktivert\nData kan være ødelagt\n\nBruk lagrefunksjonen til å lagre spillet manuelt og aktivér auto-lagring igjen\n",
        "Autosparning har misslyckats\n\nAutosparning har avbrutits\nInformationen kan vara förvrängd\n\nAnvänd spara spel-funktionen för att manuellt spara ditt spel och åter aktivera autosparning\n"}},
    {0x24, {
        "The Sly 2 save data is no longer available\n\nAuto-save has been disabled\n\nUse the save game option to manually save your game and re-enable auto-save\n",
        "Les données de sauvegarde de Sly 2 ne sont plus accessibles.\n\nAuto-sauvegarde désactivée.\n\nUtiliser l'option Sauvegarder pour sauvegarder manuellement et réactiver l'auto-sauvegarde.\n",
        "I dati di salvataggio di Sly 2 non sono più disponibili.\n\nL'Autosalvataggio è stato disattivato.\n\nUsa l'opzione Salva per salvare manualmente i progressi e riattivare l'Autosalvataggio.\n",
        "Die Sly 2-Speicherdaten sind nicht mehr verfügbar.\n\nAuto-Speichern wurde deaktiviert.\n\nSpiel mit der \"Speichern ...\"-Option manuell speichern und Auto-Speichern neu aktivieren.\n",
        "Los datos guardados de Sly 2 ya no están disponibles.\n\nSe ha desactivado el autoguardado.\n\nUsa la opción guardar para guardar manualmente la partida y reactivar el autoguardado.\n",
        "De opgeslagen gegevens van Sly 2 zijn niet meer beschikbaar\n\nAutomatisch opslaan is uitgeschakeld\n\nGebruik de optie Spel opslaan om het spel handmatig op te slaan en schakel automatisch opslaan weer in\n",
        "Os dados guardados do Sly 2 já não estão disponíveis\n\nA função que permite guardar o jogo automaticamente foi desactivada\n\nUtiliza a opção Guardar Jogo para guardares manualmente o jogo e voltar a activar esta função\n",
        "Sly 2-lagerdataene er ikke længere tilgængelige\n\nAutosave er blevet deaktiveret\n\nDu kan bruge muligheden i spillet til manuelt at gemme spillet og genaktivere autosave\n",
        "Sly 2 -tallennustiedot eivät ole enää käytettävissä\n\nAutomaattinen tallennus ei ole käytössä\n\nTallenna peli käsin ja ota automaattinen tallennus uudelleen käyttöön\n",
        "Sly 2-lagringsdataene er ikke lenger tilgjengelige\n\nAuto-lagring er ikke lenger mulig\n\nBruk funksjonen Lagre for å lagre spillet manuelt og gjøre auto-lagring mulig igjen\n",
        "Sparinformationen för Sly 2 är inte längre tillgänglig\n\nAutosparning har avbrutits\n\nAnvänd spara spel-funktionen för att manuellt spara ditt spel och återaktivera autosparning\n"}},
    {0x2A, {
        "Some of the Sly 2 save data was corrupt\n\nCorrupted data has been cleared\n",
        "Certaines données du fichier de sauvegarde Sly 2 étaient corrompues.\n\nLes données corrompues ont été effacées.\n",
        "Alcuni dati del file di salvataggio di Sly 2 erano danneggiati.\n\nI dati danneggiati sono stati cancellati.\n",
        "Einige Daten in der Sly 2-Speicherdatei waren beschädigt.\n\nBeschädigte Daten wurden gelöscht.\n",
        "Algunos datos del archivo de datos de Sly 2 estaban dañados.\n\nSe han borrado los datos dañados.\n",
        "Een deel van de gegevens in het opslagbestand van Sly 2 was beschadigd\n\nBeschadigde gegevens zijn gewist\n",
        "Alguns dados do ficheiro do Sly 2 estavam corrompidos\n\nOs dados corrompidos foram apagados\n",
        "Nogle data i Sly 2-lagerfilen var ødelagt\n\nDe ødelagte data er blevet slettet\n",
        "Osa Sly 2 -tallennustiedoston tiedoista oli vahingoittunut\n\nVahingoittuneet tiedot on poistettu\n",
        "Noen data i Sly 2-Lagringsfilen var ødelagt\n\nØdelagte data er slettet\n",
        "En del information i Sly 2-sparfilen var förvrängd\n\nDen förvrängda informationen har raderats\n"}},
    {0x2B, {
        "The game data could not be read.\n\nPlease check the Sly 2 installation.",
        "Impossible de lire les données du jeu.\n\nVérifie l'installation de Sly 2.",
        "Impossibile leggere i dati del gioco.\n\nControlla l'installazione di Sly 2.",
        "Die Spieldaten konnten nicht gelesen werden.\n\nBitte überprüfe die Installation von Sly 2.",
        "No se han podido leer los datos del juego.\n\nComprueba la instalación de Sly 2.",
        "De spelgegevens kunnen niet worden gelezen.\n\nControleer de installatie van Sly 2.",
        "Não foi possível ler os dados do jogo.\n\nVerifica a instalação do Sly 2.",
        "Spillets data kunne ikke læses.\n\nKontrollér installationen af Sly 2.",
        "Pelin tietoja ei voitu lukea.\n\nTarkista Sly 2:n asennus.",
        "Spilldataene kunne ikke leses.\n\nKontroller installasjonen av Sly 2.",
        "Spelets data kunde inte läsas.\n\nKontrollera installationen av Sly 2."}},
    {0x2E, {
        "\nPlease make sure the Sly 2 save data is in the save folder\n",
        "\nVérifie que les données de sauvegarde de Sly 2 se trouvent dans le dossier de sauvegarde.\n",
        "\nAssicurati che i dati di salvataggio di Sly 2 si trovino nella cartella di salvataggio.\n",
        "\nBitte stelle sicher, dass sich die Sly 2-Speicherdaten im Speicherordner befinden.\n",
        "\nAsegúrate de que los datos guardados de Sly 2 estén en la carpeta de partidas guardadas.\n",
        "\nZorg dat de opgeslagen gegevens van Sly 2 in de map voor opgeslagen spellen staan\n",
        "\nVerifica se os dados guardados do Sly 2 estão na pasta de jogos guardados\n",
        "\nSørg for, at Sly 2-lagerdataene ligger i mappen til gemte spil\n",
        "\nVarmista, että Sly 2 -tallennustiedot ovat tallennuskansiossa\n",
        "\nSørg for at Sly 2-lagringsdataene ligger i mappen for lagrede spill\n",
        "\nSe till att sparinformationen för Sly 2 finns i mappen för sparade spel\n"}},
    // (the European languages' headset options name no PS2)
    {0x4F, {"USB Headset: On", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0x50, {"USB Headset: Off", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
};

// the Japanese disc's messages (its own ids)
struct PCROWJP
{
    int id;
    const char* sz;
};
[[maybe_unused]] constexpr PCROWJP s_apcrowJp[] = {
    {0x11, "セーブフォルダが見つかりません。\n\nこのゲームのデータをセーブするには\n100KB以上の空き容量が必要です。"},
    {0x12, "セーブフォルダが見つかりません。\n\nこのゲームのデータをセーブするには\n100KB以上の空き容量が必要です。\n続けますか？"},
    {0x13, "セーブフォルダにこのゲームの\nセーブデータがありません。\nセーブデータをセーブフォルダに\n置いてください。\n"},
    {0x14, "このゲームのセーブデータを\n読み込めません。\n\n初期化しますか？"},
    {0x15, "初期化中です。\n\nゲームを終了しないで下さい。"},
    {0x16, "初期化に失敗しました。"},
    {0x17, "このゲームのセーブデータがありません。\n\nこのゲームのデータをセーブするには\n100KB以上の空き容量が必要です。\n\n新しくセーブデータを作成しますか？"},
    {0x18, "セーブデータを作成中です。\n\nゲームを終了しないで下さい。"},
    {0x1A, "ディスクに必要な空き容量がありません。\n\nこのゲームのデータをセーブするには\n100KB以上の空き容量が必要です。"},
    {0x1B, "ディスクに必要な空き容量がありません。\n\nこのゲームのデータをセーブするには\n100KB以上の空き容量が必要です。\n続けますか？"},
    {0x1D, "セーブ中です。\n\nゲームを終了しないで下さい。"},
    {0x24, "データを消しています。\n\nゲームを終了しないで下さい。"},
    {0x25, "このアイコンが\n\n　&2a&.　\n\n出ている時は\nデータがセーブされています。\n\nゲームを終了しないで下さい。"},
    {0x27, "このゲームのセーブデータが\n見つからなくなりました。\n\nオートセーブがキャンセルされました。\n\nポーズメニューから「セーブする」を選ぶと\nまたオートセーブがオンになります。\n"},
    {0x2E, "このゲームのデータを読み込めません"},
    {0x31, "\nこのゲームのセーブデータを\nセーブフォルダに置いてください。\n"},
};
// clang-format on
}  // namespace

const char* SzEpisodeMenuWording(int)
{
    // (Sly 2's DVD-menu renaming: its European translations. Sly 3's USA disc is English only: nothing to rename)
    return nullptr;
}

// Sly 3 (the USA disc: English only, one language in its text): its memory card messages said for a PC, in the spirit of
// Sly 2's rows above (the save folder, the Sly 3 Save File, "Please do not close the game"; the game's own "Sly 3 Save
// File", 115 KB, auto-save and "save game option" kept). Sly 3's text ids (card3.h / menu.cpp).
namespace
{
struct PCROW3
{
    int id;
    const char* sz;
};
// clang-format off
constexpr PCROW3 s_apcrow3[] = {
    {0x1A, "No save folder was found\n\nSly 3 requires 115 KB of free space\n\nPlease check the save folder or you will not be able to save your progress\n"},
    {0x1B, "No save folder was found\n\nSly 3 requires 115 KB of free space\n\nPlease check the save folder or you will not be able to save your progress\nContinue without saving?"},
    {0x1C, "No save folder was found\n\nPlease check that the save folder contains a Sly 3 Save File\n"},
    {0x1D, "The Sly 3 save data could not be read\n\nIf you do not reset it then you will not be able to save your progress\n\nDo you want to reset it?"},
    {0x1E, "\nResetting the Sly 3 save data\n\nPlease do not close the game\n"},
    {0x1F, "An error occurred while resetting the Sly 3 save data\n\nYou will not be able to save your progress\n"},
    {0x20, "No Sly 3 Save File was found\n\nSly 3 requires 115 KB of free disk space\n\nIf you do not create a Sly 3 Save File then you will not be able to save your progress\n\nDo you want to create a Sly 3 Save File?"},
    {0x21, "\nCreating the Sly 3 Save File\n\nPlease do not close the game\n"},
    {0x22, "An error occurred while creating the Sly 3 Save File\n\nYour progress is not being saved\n"},
    {0x23, "There is not enough free disk space for the Sly 3 Save File\n\nSly 3 requires 115 KB of free space\n\nPlease free up some disk space or you will not be able to save your progress\n"},
    {0x24, "There is not enough free disk space for the Sly 3 Save File\n\nSly 3 requires 115 KB of free space\n\nPlease free up some disk space or you will not be able to save your progress\nContinue without saving?"},
    {0x26, "\nSaving...\n\nPlease do not close the game\n"},
    {0x29, "An error occurred while writing to the Sly 3 Save File\n\nData may be corrupted\n\nYour progress is not being saved\n"},
    {0x2E, "\nErasing...\n\nPlease do not close the game\n"},
    {0x2F, "When the following icon appears,\nyour progress is being saved\n\n&2a&.\n\nWhile this icon is on screen, please\ndo not close the game\n"},
    {0x30, "Auto-save has failed\n\nAuto-save has been disabled\nData may be corrupted\n\nUse the save game option to manually save your game and re-enable auto-save\n"},
    {0x31, "The Sly 3 Save File is no longer available\n\nAuto-save has been disabled\n\nUse the save game option to manually save your game and re-enable auto-save\n"},
    {0x37, "Some data in the Sly 3 Save File was corrupt\n\nCorrupted data has been cleared\n"},
    {0x38, "The game data could not be read.\n\nPlease check the Sly 3 installation."},
    {0x3B, "\nNo Sly 3 Save File was found in the save folder\n"},
    {0x3E, "Checking the save folder\u2026"},
    {0x3F, "Checking the save folder\u2026"},
};
// clang-format on
}  // namespace

const char* SzPcWording(int idDisc)
{
    // Sly 3 (USA, English): its own ids (Sly 2's rows above are kept for reference, unused)
    for (const PCROW3& r : s_apcrow3)
        if (r.id == idDisc) return r.sz;
    return nullptr;
}

bool FPcWording() { return !g_pcinput.fAlwaysOriginalPs; }
