// See porttext.h.

#include "porttext.h"
#include "langstore.h"
#include "region.h"
#include "textmod.h"

#include "pcinput.h"
#include "../platform/sly2_action_bindings.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <string>

namespace
{
// The columns are the game's language order (region.h LANGUAGE): English, French, Italian, German, Spanish, Dutch,
// Portuguese, Danish, Finnish, Norwegian, Swedish, Japanese, Korean. English is the port's original text (the USA disc).
// G: the game's own words (its text in that language: the menus' On / Off 0x48 / 0x49 / 0x51 / 0x52, the Binocucom
// 0x2AC, the "Operation:" job names, a job 0x6B / 0x138); P: ours. The French text says "tu" and puts a space before
// a colon, as the game's does; the Japanese text uses the game's ASCII ':' and "(O)ボタン:" button style; the Korean
// text the Korean disc's words (켜짐 / 꺼짐, 바이노큐컴, 포탑, 장비 for a gadget, 임무 for a job, "작전명:"), its " : " after a
// button and its "..." on menu entries, and only syllables the Korean font can compose (its jamo lack the finals ㅋ ㄽ
// ㄾ ㄿ) and the ASCII it has (no # * = @ [ ] { } |): tools/porttextcheck.py --kr-font.
constexpr int CLANG = 13;
struct ROW
{
    PTX ptx;
    const char* name;   // the row's name in a language's Text.json "port" (PTX_<name>: "HoldToSkip")
    const char* asz[CLANG];
};
#define P(name) PTX_##name, #name

// clang-format off
constexpr ROW s_arow[] = {
    // G: the game's separators
    {P(Sep), {": ", " : ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ":", ": "}},
    {P(GlyphSep), {": ", " : ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", ": ", "ボタン:", " : "}},
    {P(ListFirst), {" ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", "", " "}},
    {P(ListSep), {", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", ", "、", ", "}},
    {P(ListEnd), {".", ".", ".", ".", ".", ".", ".", ".", ".", ".", ".", "。", "."}},
    // G: the menus' On / Off (gender-neutral forms: Spanish and Portuguese use the Binocucom option's Sí / No;
    // P: the Finnish Päällä / Pois, the game's "Käytössä / Pois käytöstä" too long for the wheel)
    {P(On), {"On", "Oui", "Sì", "Ein", "Sí", "aan", "Sim", "Til", "Päällä", "På", "På", "オン", "켜짐"}},
    {P(Off), {"Off", "Non", "No", "Aus", "No", "uit", "Não", "Fra", "Pois", "Av", "Av", "オフ", "꺼짐"}},

    // P: the display options
    {P(AspectRatio), {"Aspect Ratio", "Format d'image", "Formato", "Seitenverhältnis", "Proporción",
        "Beeldverhouding", "Proporção do ecrã", "Billedformat", "Kuvasuhde", "Bildeformat", "Bildformat", "アスペクト比", "화면 비율"}},
    {P(FitToScreen), {"Fit to Screen", "Adapter à l'écran", "Adatta allo schermo", "Bildschirmfüllend",
        "Ajustar a la pantalla", "Schermvullend", "Ajustar ao ecrã", "Tilpas til skærmen", "Sovita näyttöön",
        "Tilpass skjermen", "Anpassa till skärmen", "画面に合わせる", "화면에 맞춤"}},
    {P(Resolution), {"Resolution", "Résolution", "Risoluzione", "Auflösung", "Resolución", "Resolutie", "Resolução",
        "Opløsning", "Resoluutio", "Oppløsning", "Upplösning", "解像度", "해상도"}},
    {P(Native), {"Native", "Native", "Nativa", "Nativ", "Nativa", "Native", "Nativa", "Original", "Natiivi",
        "Original", "Original", "ネイティブ", "네이티브"}},
    {P(FrameRate), {"Frame Rate", "Fréquence d'images", "Frequenza fotogrammi", "Bildrate", "Velocidad de fotogramas",
        "Framerate", "Taxa de fotogramas", "Billedhastighed", "Kuvataajuus", "Bildefrekvens", "Bildfrekvens",
        "フレームレート", "프레임 속도"}},
    {P(WindowMode), {"Window Mode", "Mode d'affichage", "Modalità finestra", "Fenstermodus", "Modo de ventana",
        "Venstermodus", "Modo de janela", "Vinduestilstand", "Ikkunatila", "Vindusmodus", "Fönsterläge",
        "ウィンドウモード", "창 모드"}},
    {P(Windowed), {"Windowed", "Fenêtré", "In finestra", "Fenster", "Ventana", "Venster", "Janela", "Vindue",
        "Ikkuna", "Vindu", "Fönster", "ウィンドウ", "창"}},
    {P(Borderless), {"Borderless", "Sans bordure", "Senza bordi", "Randlos", "Sin bordes", "Randloos", "Sem margens",
        "Uden kant", "Reunaton", "Rammeløs", "Kantlöst", "ボーダーレス", "테두리 없음"}},
    {P(Fullscreen), {"Fullscreen", "Plein écran", "Schermo intero", "Vollbild", "Pantalla completa",
        "Volledig scherm", "Ecrã inteiro", "Fuld skærm", "Koko näyttö", "Fullskjerm", "Helskärm", "フルスクリーン", "전체 화면"}},
    {P(VSync), {"VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync",
        "VSync", "수직 동기화"}},
    {P(DrawDistance), {"Draw Distance", "Distance d'affichage", "Distanza visiva", "Sichtweite",
        "Distancia de dibujado", "Zichtafstand", "Distância de visualização", "Tegneafstand", "Piirtoetäisyys",
        "Tegneavstand", "Ritavstånd", "ドローディスタンス", "가시 거리"}},

    // P: the pause menu's entry, the options' pages (the game's "Quit Game" verbs: Quitter, Esci, beenden, Salir, ...)
    {P(ExitToDesktop), {"Exit to Desktop", "Quitter vers le bureau", "Esci al desktop", "Zurück zum Desktop",
        "Salir al escritorio", "Afsluiten naar bureaublad", "Sair para o ambiente de trabalho",
        "Afslut til skrivebordet", "Poistu työpöydälle", "Avslutt til skrivebordet", "Avsluta till skrivbordet",
        "デスクトップに戻る", "바탕 화면으로 나가기"}},
    {P(DisplayOptions), {"Display Options...", "Options d'affichage...", "Opzioni video...", "Anzeigeoptionen...",
        "Opciones de pantalla...", "Weergaveopties...", "Opções de visualização...", "Skærmindstillinger...",
        "Näyttöasetukset...", "Skjerminnstillinger...", "Bildskärmsalternativ...", "画面設定", "화면 설정..."}},
    {P(ControlsAndPrompts), {"Controls and Prompts...", "Commandes et icônes...", "Controlli e icone...",
        "Steuerung und Symbole...", "Controles e iconos...", "Besturing en symbolen...", "Controlos e ícones...",
        "Styring og ikoner...", "Ohjaimet ja kuvakkeet...", "Kontroller og ikoner...", "Kontroller och ikoner...",
        "操作とボタン表示", "조작과 버튼 표시..."}},

    // P: the controls options (G: the Binocucom's name in each language, text 0x2AC)
    {P(ButtonIcons), {"Button Icons", "Icônes", "Icone", "Symbole", "Iconos", "Symbolen", "Ícones", "Knapikoner",
        "Kuvakkeet", "Ikoner", "Knappikoner",
        "ボタンアイコン", "버튼 아이콘"}},
    {P(Auto), {"Auto", "Auto", "Automatico", "Automatisch", "Automático", "Automatisch", "Automático", "Automatisk",
        "Automaattinen", "Automatisk", "Automatiskt", "自動", "자동"}},
    {P(OriginalPsIcons), {"Original PlayStation Icons", "Icônes PlayStation d'origine", "Icone PlayStation originali",
        "Original-PlayStation-Symbole", "Iconos originales de PlayStation", "Originele PlayStation-symbolen",
        "Ícones originais da PlayStation", "Originale PlayStation-ikoner", "Alkuperäiset PS-kuvakkeet",
        "Originale PlayStation-ikoner", "Ursprungliga PlayStation-ikoner", "PlayStationのオリジナルアイコン", "원래 PlayStation 아이콘"}},
    {P(InstantSkip), {"Instant Skip Cutscenes", "Saut direct des cinématiques", "Salto immediato dei filmati",
        "Szenen sofort überspringen", "Saltar escenas al instante", "Filmpjes direct overslaan",
        "Saltar cenas de imediato", "Spring sekvenser over straks", "Ohita välianimaatiot heti",
        "Hopp over sekvenser straks", "Hoppa över sekvenser direkt", "ムービーをすぐスキップ", "동영상 바로 건너뛰기"}},
    {P(MouseCamera), {"Mouse Camera", "Caméra à la souris", "Telecamera col mouse", "Kamera mit Maus",
        "Cámara con ratón", "Camera met muis", "Câmara com o rato", "Kamera med mus", "Kamera hiirellä",
        "Kamera med mus", "Kamera med mus", "マウスでカメラ操作", "마우스 카메라"}},
    {P(MouseSensitivity), {"Mouse Sensitivity", "Sensibilité de la souris", "Sensibilità del mouse",
        "Mausempfindlichkeit", "Sensibilidad del ratón", "Muisgevoeligheid", "Sensibilidade do rato",
        "Musens følsomhed", "Hiiren herkkyys", "Musefølsomhet", "Muskänslighet", "マウス感度", "마우스 감도"}},
    {P(MouseAimSensitivity), {"Mouse Aim Sensitivity", "Sensibilité de visée (souris)", "Sensibilità mira (mouse)",
        "Zielempfindlichkeit (Maus)", "Sensibilidad de mira (ratón)", "Muisgevoeligheid bij richten",
        "Sensibilidade de mira (rato)", "Musens følsomhed ved sigte", "Hiiren herkkyys tähdätessä",
        "Musefølsomhet ved sikting", "Muskänslighet vid sikte", "ねらう時のマウス感度", "마우스 조준 감도"}},
    {P(MouseWheelZoom), {"Mouse Wheel Zoom", "Zoom à la molette", "Zoom con la rotellina",
        "Zoom mit Mausrad", "Zoom con la rueda", "Zoomen met scrollwiel",
        "Zoom com a roda do rato", "Zoom med rullehjul", "Zoomaus hiiren rullalla",
        "Zoom med musehjul", "Zooma med mushjulet", "ホイールでズーム", "마우스 휠 줌"}},
    {P(MouseInMenus), {"Mouse in Menus", "Souris dans les menus", "Mouse nei menu", "Maus in Menüs",
        "Ratón en los menús", "Muis in menu's", "Rato nos menus", "Mus i menuer", "Hiiri valikoissa", "Mus i menyer",
        "Mus i menyer", "メニューでマウスを使う", "메뉴에서 마우스 사용"}},
    {P(MouseAimInHacking), {"Mouse Aim in Hacking", "Visée à la souris (piratage)", "Mira col mouse (hacking)",
        "Mauszielen beim Hacken", "Apuntar con ratón (hackeo)", "Richten met muis bij hacken",
        "Mira com o rato (pirataria)", "Sigt med mus ved hacking", "Hiiritähtäys hakkeroinnissa",
        "Sikt med mus ved hacking", "Sikta med mus vid hackning", "ハッキング中のマウス操作", "해킹에서 마우스 조준"}},
    {P(KeyboardMapping), {"Keyboard Mapping...", "Configuration du clavier...", "Configurazione tastiera...",
        "Tastaturbelegung...", "Asignación de teclas...", "Toetsenbordindeling...", "Configuração do teclado...",
        "Tastaturtildeling...", "Näppäimistön määritys...", "Tastaturoppsett...", "Tangentbordsinställning...",
        "キーボード設定", "키보드 설정..."}},
    {P(ResetKeyboardMapping), {"Reset Keyboard Mapping", "Réinitialiser le clavier", "Ripristina la tastiera",
        "Tastaturbelegung zurücksetzen", "Restablecer teclas", "Toetsenbordindeling herstellen", "Repor o teclado",
        "Nulstil tastaturtildeling", "Palauta näppäimistön oletukset", "Tilbakestill tastaturoppsett",
        "Återställ tangentbordet", "キーボード設定を元に戻す", "키보드 설정 초기화"}},

    // P: the keyboard mapping's rows (the PS2 buttons: the Japanese with the game's icons, as its own text names them)
    {P(MoveForward), {"Move Forward", "Avancer", "Avanti", "Vorwärts", "Avanzar", "Vooruit", "Avançar", "Fremad",
        "Eteenpäin", "Fremover", "Framåt", "前へ移動", "앞으로 이동"}},
    {P(MoveBack), {"Move Back", "Reculer", "Indietro", "Rückwärts", "Retroceder", "Achteruit", "Recuar", "Bagud",
        "Taaksepäin", "Bakover", "Bakåt", "後ろへ移動", "뒤로 이동"}},
    {P(MoveLeft), {"Move Left", "Aller à gauche", "Sinistra", "Links", "Izquierda", "Links", "Esquerda", "Venstre",
        "Vasemmalle", "Venstre", "Vänster", "左へ移動", "왼쪽으로 이동"}},
    {P(MoveRight), {"Move Right", "Aller à droite", "Destra", "Rechts", "Derecha", "Rechts", "Direita", "Højre",
        "Oikealle", "Høyre", "Höger", "右へ移動", "오른쪽으로 이동"}},
    {P(Square), {"Square", "Carré", "Quadrato", "Quadrat", "Cuadrado", "Vierkant", "Quadrado", "Firkant", "Neliö",
        "Firkant", "Fyrkant", "&2S&.ボタン", "&2S&. 버튼"}},
    {P(Circle), {"Circle", "Rond", "Cerchio", "Kreis", "Círculo", "Rondje", "Círculo", "Cirkel", "Ympyrä", "Sirkel",
        "Cirkel", "&2O&.ボタン", "&2O&. 버튼"}},
    {P(Cross), {"Cross", "Croix", "Croce", "Kreuz", "Cruz", "Kruisje", "Cruz", "Kryds", "Risti", "Kryss", "Kryss",
        "&2X&.ボタン", "&2X&. 버튼"}},
    {P(Triangle), {"Triangle", "Triangle", "Triangolo", "Dreieck", "Triángulo", "Driehoek", "Triângulo", "Trekant",
        "Kolmio", "Trekant", "Triangel", "&2T&.ボタン", "&2T&. 버튼"}},
    {P(Start), {"Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start", "Start",
        "STARTボタン", "START 버튼"}},
    {P(Select), {"Select", "Select", "Select", "Select", "Select", "Select", "Select", "Select", "Select", "Select",
        "Select", "SELECTボタン", "SELECT 버튼"}},
    {P(CameraLeft), {"Camera Left", "Caméra gauche", "Visuale sinistra", "Kamera links", "Cámara izquierda",
        "Camera links", "Câmara esquerda", "Kamera venstre", "Kamera vasemmalle", "Kamera venstre",
        "Kamera vänster", "カメラ左", "카메라 왼쪽"}},
    {P(CameraRight), {"Camera Right", "Caméra droite", "Visuale destra", "Kamera rechts", "Cámara derecha",
        "Camera rechts", "Câmara direita", "Kamera højre", "Kamera oikealle", "Kamera høyre", "Kamera höger",
        "カメラ右", "카메라 오른쪽"}},
    {P(CameraUp), {"Camera Up", "Caméra haut", "Visuale su", "Kamera hoch", "Cámara arriba",
        "Camera omhoog", "Câmara cima", "Kamera op", "Kamera ylös", "Kamera opp", "Kamera upp", "カメラ上", "카메라 위"}},
    {P(CameraDown), {"Camera Down", "Caméra bas", "Visuale giù", "Kamera runter", "Cámara abajo",
        "Camera omlaag", "Câmara baixo", "Kamera ned", "Kamera alas", "Kamera ned", "Kamera ner", "カメラ下", "카메라 아래"}},
    {P(TankLeftTrack), {"Tank: Left Track", "Char : chenille gauche", "Carro: cingolo sinistro", "Panzer: linke Kette",
        "Tanque: oruga izquierda", "Tank: linkerrups", "Tanque: lagarta esquerda", "Tank: venstre bælte",
        "Panssari: vasen tela", "Stridsvogn: venstre belte", "Stridsvagn: vänster band", "戦車:左キャタピラ", "탱크: 왼쪽 궤도"}},
    {P(TankRightTrack), {"Tank: Right Track", "Char : chenille droite", "Carro: cingolo destro", "Panzer: rechte Kette",
        "Tanque: oruga derecha", "Tank: rechterrups", "Tanque: lagarta direita", "Tank: højre bælte",
        "Panssari: oikea tela", "Stridsvogn: høyre belte", "Stridsvagn: höger band", "戦車:右キャタピラ", "탱크: 오른쪽 궤도"}},
    {P(PressAKey), {"press a key (Esc cancels)", "appuie sur une touche (Échap : annuler)",
        "premi un tasto (Esc annulla)", "Taste drücken (Esc bricht ab)", "pulsa una tecla (Esc cancela)",
        "druk op een toets (Esc annuleert)", "prime uma tecla (Esc cancela)", "tryk på en tast (Esc annullerer)",
        "paina näppäintä (Esc peruuttaa)", "trykk på en tast (Esc avbryter)", "tryck på en tangent (Esc avbryter)",
        "キーを押してください(Escでキャンセル)", "키를 누르세요 (Esc: 취소)"}},

    // P: key and mouse names (the other keys keep their keycaps' names: Enter, Tab, Esc, ...)
    {P(KeyUnbound), {"Unbound", "Non assignée", "Non assegnato", "Nicht belegt", "Sin asignar", "Niet toegewezen",
        "Não atribuída", "Ikke tildelt", "Ei määritetty", "Ikke tildelt", "Ej tilldelad", "未設定", "지정 안 됨"}},
    {P(KeySpace), {"Space", "Espace", "Spazio", "Leertaste", "Espacio", "Spatiebalk", "Espaço", "Mellemrum",
        "Välilyönti", "Mellomrom", "Mellanslag", "スペース", "스페이스"}},
    {P(KeyUp), {"Up Arrow", "Flèche haut", "Freccia su", "Pfeil hoch", "Flecha arriba", "Pijl omhoog",
        "Seta cima", "Pil op", "Nuoli ylös", "Pil opp", "Pil upp", "上キー", "위쪽 화살표"}},
    {P(KeyDown), {"Down Arrow", "Flèche bas", "Freccia giù", "Pfeil runter", "Flecha abajo", "Pijl omlaag",
        "Seta baixo", "Pil ned", "Nuoli alas", "Pil ned", "Pil ner", "下キー", "아래쪽 화살표"}},
    {P(KeyLeft), {"Left Arrow", "Flèche gauche", "Freccia sinistra", "Pfeil links", "Flecha izquierda", "Pijl links",
        "Seta esquerda", "Pil venstre", "Vasen nuoli", "Pil venstre", "Pil vänster", "左キー", "왼쪽 화살표"}},
    {P(KeyRight), {"Right Arrow", "Flèche droite", "Freccia destra", "Pfeil rechts", "Flecha derecha", "Pijl rechts",
        "Seta direita", "Pil højre", "Oikea nuoli", "Pil høyre", "Pil höger", "右キー", "오른쪽 화살표"}},
    // (%s: Shift / Ctrl / Alt)
    {P(KeyLeftMod), {"Left %s", "%s gauche", "%s sinistro", "%s links", "%s izquierdo", "Linker %s", "%s esquerdo",
        "Venstre %s", "Vasen %s", "Venstre %s", "Vänster %s", "左%s", "왼쪽 %s"}},
    {P(KeyRightMod), {"Right %s", "%s droite", "%s destro", "%s rechts", "%s derecho", "Rechter %s", "%s direito",
        "Højre %s", "Oikea %s", "Høyre %s", "Höger %s", "右%s", "오른쪽 %s"}},
    {P(MouseLeft), {"Left Mouse", "Clic gauche", "Tasto sinistro", "Linke Maustaste", "Botón izquierdo",
        "Linkermuisknop", "Botão esquerdo", "Venstre museknap", "Hiiren vasen", "Venstre museknapp",
        "Vänster musknapp", "マウス左", "마우스 왼쪽"}},
    {P(MouseRight), {"Right Mouse", "Clic droit", "Tasto destro", "Rechte Maustaste", "Botón derecho",
        "Rechtermuisknop", "Botão direito", "Højre museknap", "Hiiren oikea", "Høyre museknapp", "Höger musknapp",
        "マウス右", "마우스 오른쪽"}},
    {P(MouseMiddle), {"Middle Mouse", "Clic central", "Tasto centrale", "Mittlere Maustaste", "Botón central",
        "Middelste muisknop", "Botão do meio", "Midterste knap", "Hiiren keski", "Midtre museknapp",
        "Mittenmusknapp", "マウス中", "마우스 가운데"}},
    {P(MouseN), {"Mouse %d", "Souris %d", "Mouse %d", "Maustaste %d", "Ratón %d", "Muisknop %d", "Rato %d",
        "Museknap %d", "Hiiri %d", "Museknapp %d", "Musknapp %d", "マウス%d", "마우스 %d"}},
    {P(WheelUp), {"Wheel Up", "Molette haut", "Rotellina su", "Mausrad hoch", "Rueda arriba", "Scrollwiel omhoog",
        "Roda para cima", "Rullehjul op", "Rulla ylös", "Musehjul opp", "Mushjul upp", "ホイール上", "휠 위"}},
    {P(WheelDown), {"Wheel Down", "Molette bas", "Rotellina giù", "Mausrad runter", "Rueda abajo",
        "Scrollwiel omlaag", "Roda para baixo", "Rullehjul ned", "Rulla alas", "Musehjul ned", "Mushjul ner",
        "ホイール下", "휠 아래"}},
    {P(Mouse), {"Mouse", "Souris", "Mouse", "Maus", "Ratón", "Muis", "Rato", "Mus", "Hiiri", "Mus", "Mus", "マウス", "마우스"}},

    // P: the cutscene skip hint (Circle, as in every region: gameplay keeps the USA buttons)
    {P(HoldToSkip), {"Hold &2O&. to skip", "Maintiens &2O&. pour passer", "Tieni premuto &2O&. per saltare",
        "&2O&. halten zum Überspringen", "Mantén &2O&. para saltar", "Houd &2O&. ingedrukt om over te slaan",
        "Mantém &2O&. para saltar", "Hold &2O&. nede for at springe over", "Ohita pitämällä &2O&. pohjassa",
        "Hold &2O&. inne for å hoppe over", "Håll &2O&. för att hoppa över", "&2O&.ボタン長押しでスキップ", "&2O&. 버튼을 길게 눌러 건너뛰기"}},
    {P(HoldToSkipNoIcons), {"Hold Circle to skip", "Maintiens Rond pour passer", "Tieni premuto Cerchio per saltare",
        "Kreis halten zum Überspringen", "Mantén Círculo para saltar", "Houd Rondje ingedrukt om over te slaan",
        "Mantém Círculo para saltar", "Hold Cirkel nede for at springe over", "Ohita pitämällä Ympyrää pohjassa",
        "Hold Sirkel inne for å hoppe over", "Håll Cirkel för att hoppa över", "丸ボタン長押しでスキップ", "동그라미 버튼을 길게 눌러 건너뛰기"}},

    // P: Replay Jobs (G: the word for a job: Abandon Job 0x6B, Jobs 0x138; the "Operation:" style of the game's
    // operation jobs, Operation: Wet Tiger 0x1BE; the Japanese names an operation "作戦" as its job names do not)
    {P(ReplayJobs), {"Replay Jobs", "Rejouer des missions", "Rigioca incarichi", "Aufträge wiederholen",
        "Repetir misiones", "Taken opnieuw spelen", "Repetir tarefas", "Spil job igen", "Pelaa keikkoja uudelleen",
        "Spill oppdrag på nytt", "Spela om uppdrag", "ミッションをリプレイ", "임무 다시 하기"}},
    {P(ReplayJob), {"Replay Job", "Rejouer la mission", "Rigioca l'incarico", "Auftrag wiederholen",
        "Repetir misión", "Taak opnieuw spelen", "Repetir a tarefa", "Spil jobbet igen", "Pelaa keikka uudelleen",
        "Spill oppdraget på nytt", "Spela om uppdraget", "リプレイする", "다시 하기"}},
    {P(OpHippoDrop), {"Operation: Hippo Drop", "Opération : Hippo-largage", "Operazione Ippo-lancio",
        "Operation: Nilpferd-Abwurf", "Operación: Hipopótamo en picado", "Operatie Nijlpaarddropping",
        "Operação: Hipopótamo em Queda", "Operation: Flodhestedrop", "Operaatio: Virtahepopudotus",
        "Operasjon Flodhestslipp", "Operation Flodhästsläpp", "ヒッポ・ドロップ作戦", "작전명: 하마 투하"}},
    {P(ReplayOperation), {"Replay the whole operation in sequence:", "Rejoue toute l'opération dans l'ordre :",
        "Rigioca l'intera operazione in sequenza:", "Spiele die ganze Operation der Reihe nach:",
        "Juega de nuevo toda la operación en orden:", "Speel de hele operatie opnieuw, in volgorde:",
        "Repete a operação inteira por ordem:", "Spil hele operationen igen i rækkefølge:",
        "Pelaa koko operaatio uudelleen järjestyksessä:", "Spill hele operasjonen på nytt i rekkefølge:",
        "Spela om hela operationen i ordning:", "作戦をはじめから順番にリプレイ:", "작전 전체를 순서대로 다시 하기:"}},

    // P: the save / load pages' page indicator (Q4: more slots, paged with L1 / R1; menu.cpp StrSlotPage)
    {P(SlotPage), {"Page %d / %d", "Page %d / %d", "Pagina %d / %d", "Seite %d / %d", "Página %d / %d",
        "Pagina %d / %d", "Página %d / %d", "Side %d / %d", "Sivu %d / %d", "Side %d / %d", "Sida %d / %d",
        "ページ%d/%d", "페이지 %d / %d"}},

    // the display options' frame-rate limit: none (R19; the launcher's "Uncapped")
    {P(Uncapped), {"Uncapped", "Illimitée", "Illimitata", "Unbegrenzt", "Sin límite", "Onbeperkt", "Ilimitada",
        "Ubegrænset", "Rajoittamaton", "Ubegrenset", "Obegränsad", "無制限", "제한 없음"}},

    // P: the display options' Long Shadows
    {P(LongShadows), {"Long Shadows", "Ombres longues", "Ombre lunghe", "Lange Schatten", "Sombras largas",
        "Lange schaduwen", "Sombras longas", "Lange skygger", "Pitkät varjot", "Lange skygger", "Långa skuggor",
        "長い影", "긴 그림자"}},
    // P: the display options' Fast Shadows (the launcher's "Fast shadows"; Japanese 高速な影: the launcher's 軽 is not a
    // glyph of the game's font)
    {P(FastShadows), {"Fast Shadows", "Ombres rapides", "Ombre veloci", "Schnelle Schatten", "Sombras rápidas",
        "Snelle schaduwen", "Sombras rápidas", "Hurtige skygger", "Nopeat varjot", "Raske skygger", "Snabba skuggor",
        "高速な影", "빠른 그림자"}},
    // P: Invert Controls... (G: the views' names: the camera of 0x2AC "Move Camera", the Binocucom 0x2AC, the turret of
    // 0x53 "Invert Turret Controls"; the Japanese 反転 of the mouse rows above, ゴーグル / 砲台 of 0x54 / 0x56 there)
    {P(InvertControlsPage), {"Invert Controls...", "Commandes inversées...", "Inversione controlli...",
        "Steuerung umkehren...", "Control invertido...", "Besturing omkeren...", "Inverter controlos...",
        "Spejl styring...", "Käänteinen ohjaus...", "Vend kontroller...", "Kasta om kontroller...", "操作の反転", "조작 반전..."}},
    {P(PadInvertCameraX), {"Invert Camera X", "Inverser l'axe X de la caméra", "Inverti asse X della visuale",
        "Kamera-X-Achse umkehren", "Invertir eje X de la cámara", "X-as camera omkeren", "Inverter eixo X da Câmara",
        "Spejl kameraets X-akse", "Käänteinen kameran X-akseli", "Vend kameraets X-akse", "Invertera kamerans X-axel",
        "カメラ左右反転", "카메라 좌우 반전"}},
    {P(PadInvertCameraY), {"Invert Camera Y", "Inverser l'axe Y de la caméra", "Inverti asse Y della visuale",
        "Kamera-Y-Achse umkehren", "Invertir eje Y de la cámara", "Y-as camera omkeren", "Inverter eixo Y da Câmara",
        "Spejl kameraets Y-akse", "Käänteinen kameran Y-akseli", "Vend kameraets Y-akse", "Invertera kamerans Y-axel",
        "カメラ上下反転", "카메라 상하 반전"}},
    {P(MouseInvertCameraX), {"Invert Mouse X (Camera)", "Inverser X souris (caméra)", "Inverti X mouse (visuale)",
        "Maus-X umkehren (Kamera)", "Invertir X ratón (cámara)", "X-as muis omkeren (camera)",
        "Inverter X do rato (Câmara)", "Spejl musens X (kamera)", "Käänteinen hiiren X (kamera)",
        "Vend musens X (kamera)", "Invertera musens X (kamera)", "マウス左右反転(カメラ)", "마우스 좌우 반전 (카메라)"}},
    {P(MouseInvertCameraY), {"Invert Mouse Y (Camera)", "Inverser Y souris (caméra)", "Inverti Y mouse (visuale)",
        "Maus-Y umkehren (Kamera)", "Invertir Y ratón (cámara)", "Y-as muis omkeren (camera)",
        "Inverter Y do rato (Câmara)", "Spejl musens Y (kamera)", "Käänteinen hiiren Y (kamera)",
        "Vend musens Y (kamera)", "Invertera musens Y (kamera)", "マウス上下反転(カメラ)", "마우스 상하 반전 (카메라)"}},
    {P(MouseInvertBinocX), {"Invert Mouse X (Binocucom)", "Inverser X souris (Binocom)", "Inverti X mouse (Binocom)",
        "Maus-X umkehren (S.-Kom.)", "Invertir X ratón (bino-com)", "X-as muis omkeren (Contactkijker)",
        "Inverter X do rato (Binocucom)", "Spejl musens X (binocucom)", "Käänteinen hiiren X (radiokiikari)",
        "Vend musens X (kikkertwalkie)", "Invertera musens X (binokucom)", "マウス左右反転(ゴーグル)", "마우스 좌우 반전 (바이노큐컴)"}},
    {P(MouseInvertBinocY), {"Invert Mouse Y (Binocucom)", "Inverser Y souris (Binocom)", "Inverti Y mouse (Binocom)",
        "Maus-Y umkehren (S.-Kom.)", "Invertir Y ratón (bino-com)", "Y-as muis omkeren (Contactkijker)",
        "Inverter Y do rato (Binocucom)", "Spejl musens Y (binocucom)", "Käänteinen hiiren Y (radiokiikari)",
        "Vend musens Y (kikkertwalkie)", "Invertera musens Y (binokucom)", "マウス上下反転(ゴーグル)", "마우스 상하 반전 (바이노큐컴)"}},
    {P(MouseInvertTurretX), {"Invert Mouse X (Turret)", "Inverser X souris (tourelles)", "Inverti X mouse (torrette)",
        "Maus-X umkehren (Geschütz)", "Invertir X ratón (torretas)", "X-as muis omkeren (geschut)",
        "Inverter X do rato (Torre)", "Spejl musens X (skydetårn)", "Käänteinen hiiren X (tykki)",
        "Vend musens X (tårn)", "Invertera musens X (stridstorn)", "マウス左右反転(砲台)", "마우스 좌우 반전 (포탑)"}},
    {P(MouseInvertTurretY), {"Invert Mouse Y (Turret)", "Inverser Y souris (tourelles)", "Inverti Y mouse (torrette)",
        "Maus-Y umkehren (Geschütz)", "Invertir Y ratón (torretas)", "Y-as muis omkeren (geschut)",
        "Inverter Y do rato (Torre)", "Spejl musens Y (skydetårn)", "Käänteinen hiiren Y (tykki)",
        "Vend musens Y (tårn)", "Invertera musens Y (stridstorn)", "マウス上下反転(砲台)", "마우스 상하 반전 (포탑)"}},
    {P(AutoCameraRotation), {"Automatic Camera Rotation", "Rotation auto caméra", "Rotazione auto telecamera",
        "Automatische Kameradrehung", "Giro automático de cámara", "Automatische camerarotatie",
        "Rotação automática da câmara", "Automatisk kamerarotation", "Kameran automaattikääntö",
        "Automatisk kamerarotasjon", "Automatisk kamerarotation", "カメラ自動回転", "카메라 자동 회전"}},
    {P(AdvancedBindings), {"Advanced Input Mapping", "Attribution avancée des commandes", "Assegnazione avanzata comandi",
        "Erweiterte Tastenbelegung", "Asignación avanzada de controles", "Geavanceerde knoptoewijzing",
        "Mapeamento avançado de controlos", "Avanceret knaptildeling", "Laajennettu näppäinkartoitus",
        "Avansert knappetilordning", "Avancerad knapptilldelning", "くわしい操作のわりあて", "고급 입력 설정"}},
    {P(ActionBindings), {"Action Bindings...", "Attribuer les commandes...", "Assegna comandi...",
        "Aktionen belegen...", "Asignar acciones...", "Acties toewijzen...", "Atribuir ações...",
        "Tildel handlinger...", "Määritä toiminnot...", "Tilordne handlinger...", "Tilldela åtgärder...", "操作をわりあてる", "동작 지정..."}},
    {P(ActionKeyboard), {"Keyboard and Mouse...", "Clavier et souris...", "Tastiera e mouse...",
        "Tastatur und Maus...", "Teclado y ratón...", "Toetsenbord en muis...", "Teclado e rato...",
        "Tastatur og mus...", "Näppäimistö ja hiiri...", "Tastatur og mus...", "Tangentbord och mus...", "キーボードとマウス", "키보드와 마우스..."}},
    {P(ActionController), {"Controller...", "Manette...", "Controller...", "Controller...", "Mando...", "Controller...",
        "Comando...", "Controller...", "Peliohjain...", "Kontroller...", "Handkontroll...", "コントローラ", "컨트롤러..."}},
    {P(ResetAdvancedBindings), {"Reset Advanced Input Mapping", "Réinitialiser les commandes", "Ripristina comandi avanzati",
        "Belegung zurücksetzen", "Restablecer controles avanzados", "Standaardbesturing herstellen",
        "Repor controlos avançados", "Nulstil avanceret styring", "Palauta toimintojen oletukset",
        "Tilbakestill avanserte kontroller", "Återställ avancerade kontroller", "わりあてをもどす", "고급 입력 설정 초기화"}},
    {P(CaptureAction), {"Press input (Esc: cancel, Del: clear)", "Appuie (Échap : annuler, Suppr : effacer)",
        "Premi un comando (Esc: annulla, Canc: cancella)", "Eingabe (Esc: Abbruch, Entf: löschen)",
        "Pulsa (Esc: cancelar, Supr: borrar)", "Druk (Esc: annuleren, Del: wissen)",
        "Prime (Esc: cancelar, Del: limpar)", "Tryk (Esc: annuller, Del: ryd)", "Paina (Esc: peruuta, Del: poista)",
        "Trykk (Esc: avbryt, Del: fjern)", "Tryck (Esc: avbryt, Del: rensa)", "ボタンをおす (Esc:やめる、Del:けす)", "입력하세요 (Esc: 취소, Del: 지우기)"}},
    // P: the Remap page (remappage.h; 2026-11-10)
    {P(RemapPage), {"Remap Controls...", "Réattribuer les commandes...", "Riassegna i comandi...", "Steuerung belegen...",
        "Reasignar controles...", "Besturing toewijzen...", "Reatribuir controlos...", "Tildel styring...",
        "Määritä ohjaimet...", "Tilordne kontroller...", "Tilldela kontroller...", "操作のわりあて", "조작 변경..."}},
    {P(RemapTitle), {"Remap Controls", "Réattribuer les commandes", "Riassegna i comandi", "Steuerung belegen",
        "Reasignar controles", "Besturing toewijzen", "Reatribuir controlos", "Tildel styring", "Määritä ohjaimet",
        "Tilordne kontroller", "Tilldela kontroller", "操作のわりあて", "조작 변경"}},
    {P(RemapAction), {"Action", "Action", "Azione", "Aktion", "Acción", "Actie", "Ação", "Handling", "Toiminto",
        "Handling", "Åtgärd", "アクション", "동작"}},
    {P(RemapKeyboard), {"Keyboard / Mouse", "Clavier / souris", "Tastiera / mouse", "Tastatur / Maus", "Teclado / ratón",
        "Toetsenbord / muis", "Teclado / rato", "Tastatur / mus", "Näppäimistö / hiiri", "Tastatur / mus",
        "Tangentbord / mus", "キーボード/マウス", "키보드 / 마우스"}},
    {P(RemapController), {"Controller", "Manette", "Controller", "Controller", "Mando", "Controller", "Comando",
        "Controller", "Peliohjain", "Kontroller", "Handkontroll", "コントローラ", "컨트롤러"}},
    {P(RemapReset), {"Reset to Defaults", "Valeurs par défaut", "Ripristina predefiniti", "Standard wiederherstellen",
        "Restablecer valores", "Standaard herstellen", "Repor predefinições", "Nulstil til standard", "Palauta oletukset",
        "Tilbakestill til standard", "Återställ standard", "しょきせっていにもどす", "기본값으로 되돌리기"}},
    {P(RemapBack), {"Back", "Retour", "Indietro", "Zurück", "Atrás", "Terug", "Voltar", "Tilbage", "Takaisin", "Tilbake",
        "Tillbaka", "もどる", "뒤로"}},
    {P(RemapChange), {"Change", "Changer", "Cambia", "Ändern", "Cambiar", "Wijzigen", "Mudar", "Skift", "Vaihda", "Endre",
        "Ändra", "かえる", "변경"}},
    {P(RemapClear), {"Clear", "Effacer", "Cancella", "Löschen", "Borrar", "Wissen", "Limpar", "Ryd", "Poista", "Fjern",
        "Rensa", "けす", "지우기"}},
    {P(RemapPressKey), {"Press a key or mouse button (Esc: cancel, Del: clear)",
        "Appuie sur une touche ou un bouton de souris (Échap : annuler, Suppr : effacer)",
        "Premi un tasto o un pulsante del mouse (Esc: annulla, Canc: cancella)",
        "Taste oder Maustaste drücken (Esc: Abbruch, Entf: löschen)",
        "Pulsa una tecla o un botón del ratón (Esc: cancelar, Supr: borrar)",
        "Druk op een toets of muisknop (Esc: annuleren, Del: wissen)",
        "Prime uma tecla ou um botão do rato (Esc: cancelar, Del: limpar)",
        "Tryk på en tast eller museknap (Esc: annuller, Del: ryd)",
        "Paina näppäintä tai hiiren painiketta (Esc: peruuta, Del: poista)",
        "Trykk en tast eller museknapp (Esc: avbryt, Del: fjern)",
        "Tryck på en tangent eller musknapp (Esc: avbryt, Del: rensa)", "キーかマウスボタンをおす (Esc:やめる、Del:けす)", "키나 마우스 버튼을 누르세요 (Esc: 취소, Del: 지우기)"}},
    {P(RemapPressButton), {"Press a controller button (Esc or wait: cancel, Del: clear)",
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
        "コントローラのボタンをおす (Escかまつ:やめる、Del:けす)", "컨트롤러 버튼을 누르세요 (Esc 또는 기다리기: 취소, Del: 지우기)"}},
    {P(RemapFixed), {"fixed", "fixe", "fisso", "fest", "fijo", "vast", "fixo", "fast", "kiinteä", "fast", "fast", "こてい", "고정"}},
    // P: VR Settings (vr.h)
    {P(VrSettings), {"VR Settings", "Réglages VR", "Impostazioni VR", "VR-Einstellungen", "Ajustes de RV",
        "VR-instellingen", "Definições de RV", "VR-indstillinger", "VR-asetukset", "VR-innstillinger", "VR-inställningar", "VR設定", "VR 설정"}},
    {P(VrMode), {"VR Mode", "Mode VR", "Modalità VR", "VR-Modus", "Modo RV", "VR-modus", "Modo RV", "VR-tilstand",
        "VR-tila", "VR-modus", "VR-läge", "VRモード", "VR 모드"}},
    {P(VrImmersive), {"Immersive", "Immersif", "Immersiva", "Immersiv", "Inmersivo", "Immersief", "Imersivo", "Immersiv",
        "Immersiivinen", "Immersiv", "Immersiv", "イマーシブ", "몰입형"}},
    {P(VrFlatScreen), {"Flat Screen", "Écran plat", "Schermo piatto", "Flacher Bildschirm", "Pantalla plana",
        "Plat scherm", "Ecrã plano", "Flad skærm", "Litteä näyttö", "Flat skjerm", "Platt skärm", "フラット画面", "평면 화면"}},
    {P(VrFirstPerson), {"First Person (Experimental)", "Première personne (expérimental)",
        "Prima persona (sperimentale)", "Ego-Perspektive (experimentell)", "Primera persona (experimental)",
        "Eerste persoon (experimenteel)", "Primeira pessoa (experimental)", "Førsteperson (eksperimentel)",
        "Ensimmäinen persoona (kokeellinen)", "Førsteperson (eksperimentell)", "Förstaperson (experimentell)", "一人称視点(実験的)", "1인칭 시점 (실험적)"}},
    {P(VrVignette), {"Comfort Vignette", "Vignette de confort", "Vignettatura comfort", "Komfort-Vignette",
        "Viñeta de confort", "Comfortvignet", "Vinheta de conforto", "Komfortvignet", "Mukavuusvinjetti",
        "Komfortvignett", "Komfortvinjett", "ビネット", "멀미 방지 비네트"}},
    {P(VrLow), {"Low", "Faible", "Bassa", "Schwach", "Baja", "Laag", "Baixa", "Lav", "Matala", "Lav", "Låg", "よわい", "낮음"}},
    {P(VrMedium), {"Medium", "Moyenne", "Media", "Mittel", "Media", "Middel", "Média", "Middel", "Keskitaso", "Middels",
        "Medel", "ふつう", "보통"}},
    {P(VrHigh), {"High", "Forte", "Alta", "Stark", "Alta", "Hoog", "Alta", "Høj", "Korkea", "Høy", "Hög", "つよい", "높음"}},
    {P(VrScreenSize), {"Screen Size", "Taille de l'écran", "Dimensione schermo", "Bildschirmgröße", "Tamaño de pantalla",
        "Schermgrootte", "Tamanho do ecrã", "Skærmstørrelse", "Näytön koko", "Skjermstørrelse", "Skärmstorlek", "画面サイズ", "화면 크기"}},
    {P(VrSmall), {"Small", "Petit", "Piccolo", "Klein", "Pequeña", "Klein", "Pequeno", "Lille", "Pieni", "Liten", "Liten",
        "ちいさい", "작게"}},
    {P(VrLarge), {"Large", "Grand", "Grande", "Groß", "Grande", "Groot", "Grande", "Stor", "Suuri", "Stor", "Stor", "おおきい", "크게"}},
    {P(VrRefreshRate), {"Refresh Rate", "Fréquence d'affichage", "Frequenza di aggiornamento", "Bildwiederholrate",
        "Frecuencia de actualización", "Verversingssnelheid", "Taxa de atualização", "Opdateringshastighed",
        "Virkistystaajuus", "Oppdateringsfrekvens", "Uppdateringsfrekvens", "リフレッシュレート", "주사율"}},
    {P(VrEyeResolution), {"Eye Resolution", "Résolution par oeil", "Risoluzione per occhio", "Auflösung pro Auge",
        "Resolución por ojo", "Resolutie per oog", "Resolução por olho", "Opløsning pr. øje", "Tarkkuus silmää kohden",
        "Oppløsning per øye", "Upplösning per öga", "かいぞうど", "한쪽 눈 해상도"}},
    {P(VrFoveation), {"Foveation", "Rendu fovéal", "Foveazione", "Foveated Rendering", "Foveación", "Foveated rendering",
        "Foveação", "Foveering", "Foveaatio", "Foveering", "Foveering", "しゅうへんかいぞうど", "포비에이션"}},
    {P(VrMultiview), {"Multiview", "Multivue", "Multivista", "Multiview", "Multivista", "Multiview", "Multivisão", "Multiview",
        "Moninäkymä", "Multiview", "Multivy", "マルチビュー", "멀티뷰"}},
    {P(VrUpscaling), {"Upscaling", "Mise à l'échelle", "Upscaling", "Hochskalierung", "Escalado", "Opschalen", "Upscaling",
        "Opskalering", "Skaalaus", "Oppskalering", "Uppskalning", "アップスケール", "업스케일링"}},
    {P(VrSharpen), {"Sharpen", "Netteté", "Nitidezza", "Schärfen", "Nitidez", "Verscherpen", "Nitidez", "Skarphed",
        "Terävöinti", "Skarphet", "Skärpa", "シャープ", "선명하게"}},
    {P(VrSuperResolution), {"Super Resolution", "Super résolution", "Super risoluzione", "Super-Auflösung", "Superresolución",
        "Superresolutie", "Super-resolução", "Superopløsning", "Superresoluutio", "Superoppløsning", "Superupplösning", "スーパーレゾリューション", "슈퍼 해상도"}},
    {P(VrDynamic), {"Dynamic", "Dynamique", "Dinamica", "Dynamisch", "Dinámica", "Dynamisch", "Dinâmica", "Dynamisk",
        "Dynaaminen", "Dynamisk", "Dynamisk", "じどう", "자동"}},
    {P(VrWorldScale), {"World Scale", "Échelle du monde", "Scala del mondo", "Weltgröße", "Escala del mundo",
        "Wereldschaal", "Escala do mundo", "Verdensskala", "Maailman mittakaava", "Verdensskala", "Världsskala",
        "ワールドスケール", "월드 크기"}},
    {P(VrRecenter), {"Recenter View", "Recentrer la vue", "Ricentra la visuale", "Ansicht zentrieren", "Recentrar la vista",
        "Beeld centreren", "Recentrar a vista", "Centrer visningen", "Keskitä näkymä", "Sentrer visningen",
        "Centrera vyn", "ビューをもどす", "시점 재설정"}},
    {P(VrCutscenes), {"Cutscenes", "Cinématiques", "Filmati", "Zwischensequenzen", "Escenas", "Tussenfilmpjes",
        "Cenas", "Mellemsekvenser", "Välivideot", "Mellomsekvenser", "Mellansekvenser", "ムービー", "동영상"}},
    {P(VrRcTable), {"RC Car Table View (Beta)", "Voiture RC vue de la table (bêta)", "Auto RC dal tavolo (beta)",
        "RC-Auto vom Tisch (Beta)", "Coche RC desde la mesa (beta)", "RC-auto vanaf de tafel (bèta)",
        "Carro RC da mesa (beta)", "RC-bil fra bordet (beta)", "RC-auto pöydältä (beta)", "RC-bil fra bordet (beta)",
        "RC-bil från bordet (beta)", "RCカー テーブルビュー", "RC 카 테이블 보기 (베타)"}},
    {P(QuitToLauncher), {"Quit to Launcher", "Retour au lanceur", "Torna al launcher", "Zurück zum Launcher",
        "Volver al lanzador", "Terug naar de launcher", "Voltar ao launcher", "Tilbage til launcheren",
        "Palaa käynnistimeen", "Tilbake til startprogrammet", "Tillbaka till startprogrammet", "ランチャーに戻る", "런처로 나가기"}},
    // P: (audio) the options' Audio Devices page (menu.cpp AentAudio)
    {P(AudioDevicesPage), {"Audio Devices...", "Périphériques audio...", "Dispositivi audio...", "Audiogeräte...",
        "Dispositivos de audio...", "Audioapparaten...", "Dispositivos de áudio...", "Lydenheder...", "Äänilaitteet...",
        "Lydenheter...", "Ljudenheter...", "オーディオデバイス", "오디오 장치..."}},
    {P(OutputDevice), {"Output", "Sortie", "Uscita", "Ausgabe", "Salida", "Uitvoer", "Saída", "Udgang", "Ulostulo",
        "Utgang", "Utgång", "スピーカー", "출력"}},
    {P(InputDevice), {"Microphone", "Microphone", "Microfono", "Mikrofon", "Micrófono", "Microfoon", "Microfone",
        "Mikrofon", "Mikrofoni", "Mikrofon", "Mikrofon", "マイク", "마이크"}},
    {P(HeadsetOutputDevice), {"Headset Output", "Sortie du casque", "Uscita cuffie", "Headset-Ausgabe",
        "Salida de auriculares", "Headsetuitvoer", "Saída dos auscultadores", "Headsetudgang", "Kuulokkeiden ulostulo",
        "Hodesettutgang", "Headsetutgång", "ヘッドセット", "헤드셋 출력"}},
    {P(SystemDefault), {"System Default", "Par défaut", "Predefinito", "Systemstandard", "Predeterminado",
        "Systeemstandaard", "Predefinição", "Systemstandard", "Järjestelmän oletus", "Systemstandard", "Systemstandard",
        "オート", "시스템 기본값"}},
    {P(SameAsOutput), {"Same as Output", "Comme la sortie", "Come l'uscita", "Wie Ausgabe", "Igual que la salida",
        "Zelfde als uitvoer", "Igual à saída", "Samme som udgang", "Sama kuin ulostulo", "Samme som utgang",
        "Samma som utgång", "スピーカーとおなじ", "출력과 같음"}},
    {P(NotConnected), {"%s (not connected)", "%s (non connecté)", "%s (non collegato)", "%s (nicht verbunden)",
        "%s (no conectado)", "%s (niet verbonden)", "%s (não ligado)", "%s (ikke tilsluttet)", "%s (ei yhdistetty)",
        "%s (ikke tilkoblet)", "%s (inte ansluten)", "%s (なし)", "%s (연결 안 됨)"}},
    // P: the language menu: a language of the languages folder that plays its voices over this disc's text
    {P(LanguageVoicesOnly), {"%s (voices only)", "%s (voix seulement)", "%s (solo voci)", "%s (nur Stimmen)",
        "%s (solo voces)", "%s (alleen stemmen)", "%s (apenas vozes)", "%s (kun stemmer)", "%s (vain äänet)",
        "%s (bare stemmer)", "%s (endast röster)", "%s (音声のみ)", "%s (음성만)"}},
    // P: the options' Language... page (the text and the voices apart; G: the Japanese 使用する言語 of the title's menu)
    {P(LanguagePage), {"Language...", "Langue...", "Lingua...", "Sprache...", "Idioma...", "Taal...", "Idioma...",
        "Sprog...", "Kieli...", "Språk...", "Språk...", "使用する言語", "언어..."}},
    {P(TextLanguage), {"Text Language", "Langue du texte", "Lingua del testo", "Textsprache", "Idioma del texto",
        "Teksttaal", "Idioma do texto", "Tekstsprog", "Tekstin kieli", "Tekstspråk", "Textspråk", "テキストの言語", "텍스트 언어"}},
    {P(VoiceLanguage), {"Voice Language", "Langue des voix", "Lingua delle voci", "Sprache der Stimmen",
        "Idioma de las voces", "Taal van de stemmen", "Idioma das vozes", "Stemmernes sprog", "Puheen kieli",
        "Stemmenes språk", "Rösternas språk", "音声の言語", "음성 언어"}},
    {P(SameAsText), {"Same as Text", "Comme le texte", "Come il testo", "Wie der Text", "Igual que el texto",
        "Zelfde als tekst", "Igual ao texto", "Samme som tekst", "Sama kuin teksti", "Samme som tekst", "Samma som text",
        "テキストとおなじ", "텍스트와 같음"}},
    {P(FromNextArea), {"%s (next area)", "%s (zone suivante)", "%s (prossima zona)", "%s (nächstes Gebiet)",
        "%s (próxima zona)", "%s (volgend gebied)", "%s (próxima zona)", "%s (næste område)", "%s (seuraava alue)",
        "%s (neste område)", "%s (nästa område)", "%s (次のエリア)", "%s (다음 지역부터)"}},
    // P: the display options' Upscaling (fsr.h; the Japanese in kana, the font's)
    {P(Upscaling), {"Upscaling", "Mise à l'échelle", "Upscaling", "Hochskalierung", "Reescalado", "Opschaling",
        "Aumento de resolução", "Opskalering", "Skaalaus", "Oppskalering", "Uppskalning", "アップスケール", "업스케일링"}},
    {P(UpscaleMethod), {"Method", "Méthode", "Metodo", "Methode", "Método", "Methode", "Método", "Metode", "Menetelmä",
        "Metode", "Metod", "ほうしき", "방식"}},
    {P(UpscalePreset), {"Preset", "Préréglage", "Preimpostazione", "Voreinstellung", "Ajuste", "Voorinstelling",
        "Predefinição", "Niveau", "Esiasetus", "Nivå", "Nivå", "プリセット", "프리셋"}},
    {P(UpscaleSharpness), {"Sharpness", "Netteté", "Nitidezza", "Schärfe", "Nitidez", "Scherpte", "Nitidez", "Skarphed",
        "Terävyys", "Skarphet", "Skärpa", "シャープネス", "선명도"}},
    {P(UltraQuality), {"Ultra Quality", "Ultra qualité", "Qualità ultra", "Ultra-Qualität", "Calidad ultra",
        "Ultrakwaliteit", "Qualidade ultra", "Ultrakvalitet", "Ultralaatu", "Ultrakvalitet", "Ultrakvalitet",
        "ウルトラクオリティ", "울트라 품질"}},
    {P(Quality), {"Quality", "Qualité", "Qualità", "Qualität", "Calidad", "Kwaliteit", "Qualidade", "Kvalitet", "Laatu",
        "Kvalitet", "Kvalitet", "クオリティ", "품질"}},
    {P(Balanced), {"Balanced", "Équilibré", "Bilanciato", "Ausgewogen", "Equilibrado", "Gebalanceerd", "Equilibrado",
        "Balanceret", "Tasapainoinen", "Balansert", "Balanserad", "バランス", "균형"}},
    {P(Performance), {"Performance", "Performance", "Prestazioni", "Leistung", "Rendimiento", "Prestaties", "Desempenho",
        "Ydelse", "Suorituskyky", "Ytelse", "Prestanda", "パフォーマンス", "성능"}},
    // P: the cel outlines' width (display.h outlineThickness; the Japanese in kana, the font's)
    {P(OutlineThickness), {"Outline Thickness", "Épaisseur des contours", "Spessore dei contorni", "Konturstärke",
        "Grosor del contorno", "Dikte van de omlijning", "Espessura do contorno", "Konturtykkelse", "Ääriviivojen paksuus",
        "Konturtykkelse", "Konturtjocklek", "りんかくせんのふとさ", "외곽선 두께"}},
    {P(VrSnapTurn), {"Snap Turn", "Rotation par crans", "Rotazione a scatti", "Drehen in Schritten", "Giro por pasos",
        "Draaien in stappen", "Rotação por passos", "Drej i trin", "Käänny askelin", "Snu i trinn", "Vrid i steg",
        "スナップターン", "스냅 회전"}},
    {P(VrMenuLeft), {"Menu (Left)", "Menu (gauche)", "Menu (sinistro)", "Menü (links)", "Menú (izquierdo)", "Menu (links)",
        "Menu (esquerdo)", "Menu (venstre)", "Valikko (vasen)", "Meny (venstre)", "Meny (vänster)", "メニュー(ひだり)",
        "메뉴 (왼쪽)"}},
    {P(VrMenuRight), {"Menu (Right)", "Menu (droite)", "Menu (destro)", "Menü (rechts)", "Menú (derecho)", "Menu (rechts)",
        "Menu (direito)", "Menu (højre)", "Valikko (oikea)", "Meny (høyre)", "Meny (höger)", "メニュー(みぎ)",
        "메뉴 (오른쪽)"}},
    // P: the display options' Monitor (settings v22; a monitor's own name is shown as the system gives it)
    {P(Monitor), {"Monitor", "Écran", "Monitor", "Monitor", "Monitor", "Monitor", "Monitor", "Skærm", "Näyttö", "Skjerm",
        "Skärm", "モニター", "모니터"}},
    {P(Primary), {"Primary", "Principal", "Principale", "Primär", "Principal", "Primair", "Principal", "Primær",
        "Ensisijainen", "Primær", "Primär", "メイン", "기본"}},
    // P: the Language... page left with a new language waiting (menu.cpp): the question and its two answers (the Japanese in
    // kana, the font's)
    {P(LangReloadQuestion), {"Restart from the last checkpoint now to use the new language? Otherwise it changes when the next area loads.",
        "Recommencer au dernier point de contrôle maintenant pour utiliser la nouvelle langue ? Sinon, elle changera au chargement de la prochaine zone.",
        "Ricominciare ora dall'ultimo checkpoint per usare la nuova lingua? Altrimenti cambierà al caricamento della prossima zona.",
        "Jetzt vom letzten Kontrollpunkt neu starten, um die neue Sprache zu verwenden? Sonst wechselt sie, wenn das nächste Gebiet lädt.",
        "¿Reiniciar ahora desde el último punto de control para usar el nuevo idioma? Si no, cambiará al cargar la próxima zona.",
        "Nu opnieuw beginnen vanaf het laatste checkpoint om de nieuwe taal te gebruiken? Anders verandert die wanneer het volgende gebied laadt.",
        "Recomeçar agora a partir do último ponto de controlo para usar o novo idioma? Caso contrário, muda quando a próxima zona carregar.",
        "Start forfra fra det sidste checkpoint nu for at bruge det nye sprog? Ellers skifter det, når det næste område indlæses.",
        "Aloitetaanko nyt viimeisestä tarkistuspisteestä uudella kielellä? Muuten kieli vaihtuu, kun seuraava alue latautuu.",
        "Start på nytt fra siste sjekkpunkt nå for å bruke det nye språket? Ellers endres det når neste område lastes inn.",
        "Börja om från den senaste kontrollpunkten nu för att använda det nya språket? Annars byts det när nästa område laddas.",
        "あたらしいげんごにするため、さいごのチェックポイントからやりなおしますか?やりなおさないと、つぎのエリアからかわります。",
        "새 언어를 쓰려면 지금 마지막 체크포인트에서 다시 시작할까요? 아니면 다음 지역을 불러올 때 바뀝니다."}},
    {P(LangReloadNow), {"Restart Now", "Recommencer", "Ricomincia", "Neu starten", "Reiniciar", "Opnieuw beginnen",
        "Recomeçar", "Start forfra", "Aloita alusta", "Start på nytt", "Börja om", "やりなおす", "지금 다시 시작"}},
    {P(LangReloadLater), {"Next Area", "Zone suivante", "Prossima zona", "Nächstes Gebiet", "Próxima zona",
        "Volgend gebied", "Próxima zona", "Næste område", "Seuraava alue", "Neste område", "Nästa område", "つぎのエリア",
        "다음 지역부터"}},
    // P: the display options' HUD Placement (display.h hudPlacement) and its Match Screen (the picture's own shape; the
    // Japanese / Korean as Fit to Screen's)
    {P(HudPlacement), {"HUD Placement", "Position du HUD", "Posizione HUD", "HUD-Position", "Posición del HUD",
        "HUD-positie", "Posição do HUD", "HUD-placering", "HUD:n sijoittelu", "HUD-plassering", "HUD-placering",
        "HUDのいち", "HUD 위치"}},
    {P(MatchScreen), {"Match Screen", "Selon l'écran", "Come lo schermo", "Wie Bildschirm", "Igual que la pantalla",
        "Zoals scherm", "Igual ao ecrã", "Som skærmen", "Näytön mukaan", "Som skjermen", "Som skärmen", "画面に合わせる",
        "화면에 맞춤"}},
};
// clang-format on
#undef P
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
    // (the text's language: a store language's own, or the disc's that it falls back to; region.h LangText; a custom
    // language: the column of the language it is based on, langstore.h LangPortBase)
    const int lang = LangText();
    const int ilang = lang >= 0 && lang < CLANG ? lang : LangPortBase(lang);
    // (the Japanese / Korean column needs that script's font: the Japanese / Korean disc's, or the languages folder's
    // on the other discs; Latin columns drawn with it are folded: StrToGameText)
    if (ilang == LANG_Japanese && !(FWideChars() && ScriptOfTextShown() == SCRIPT_Japanese)) return LANG_English;
    if (ilang == LANG_Korean && !(FWideChars() && ScriptOfTextShown() == SCRIPT_Korean)) return LANG_English;
    return ilang;
}

// A string's printf conversions in order ("%s%d"; "%%" is none): a "port" string must have the built-in English one's
std::string StrFormats(const char* sz)
{
    std::string out;
    for (const char* pch = sz; *pch; ++pch)
    {
        if (*pch != '%') continue;
        if (pch[1] == '%') { ++pch; continue; }
        ++pch;
        while (*pch && std::strchr("-+ #0123456789.*hlzjtL", *pch)) ++pch;
        if (!*pch) { out += '!'; break; }
        out += '%';
        out += *pch;
    }
    return out;
}

// The text's language's own port string (its Text.json "port", langstore.h) by row name, UTF-8; nullptr: none. A string
// whose printf conversions differ from the English's is not used (it would read past its arguments); said once.
const char* SzPortOverride(const char* name, const char* szEnglish)
{
    const auto* pmp = PmpPortTextShown();
    if (!pmp) return nullptr;
    const auto it = pmp->find(name);
    if (it == pmp->end()) return nullptr;
    if (StrFormats(it->second.c_str()) != StrFormats(szEnglish))
    {
        static std::mutex s_mtxWarned;
        static std::set<std::string> s_asetWarned;
        std::lock_guard<std::mutex> lock(s_mtxWarned);
        if (s_asetWarned.insert(name).second)
            std::fprintf(stderr, "porttext: the language's \"%s\" is not used: its %%-codes (%s) are not the English one's (%s)\n", name,
                         StrFormats(it->second.c_str()).c_str(), StrFormats(szEnglish).c_str());
        return nullptr;
    }
    return it->second.c_str();
}

const char* SzRow(PTX ptx)
{
    if (const char* sz = SzPortOverride(s_arow[ptx].name, s_arow[ptx].asz[LANG_English])) return sz;
    return s_arow[ptx].asz[IlangTable()];
}

// An action group's row name: "ActionGroup.ClimbingAndLedges" (the group's English words joined, each capitalised)
std::string StrActionGroupKey(std::string_view group)
{
    std::string key = "ActionGroup.";
    bool fUpper = true;
    for (char ch : group)
    {
        if (!std::isalnum(static_cast<unsigned char>(ch))) { fUpper = true; continue; }
        key += fUpper ? char(std::toupper(static_cast<unsigned char>(ch))) : ch;
        fUpper = false;
    }
    return key;
}

#include "porttext_actions.inc"

// the language writes decimals with a comma (a language's "port" DecimalMark: "." or ",")
bool FDecimalComma()
{
    if (const char* sz = SzPortOverride("DecimalMark", ".")) return sz[0] == ',';
    switch (IlangTable())
    {
    case LANG_English: case LANG_Japanese: case LANG_Korean: return false;
    default: return true;
    }
}
}  // namespace

std::string StrToGameText(const std::string& utf8)
{
    // 16-bit text keeps characters >= 0x80 as UTF-8 already (region.h FWideChars)
    if (FWideChars())
    {
        const int ilang = IlangTable();
        return ilang == LANG_Japanese || ilang == LANG_Korean || FStoreTextWide() ? utf8 : StrFoldLatinForWideFont(utf8);   // (langstore.h)
    }
    // the other discs' text is Latin-1 (their fonts' glyphs 0x20..0xFF, typographic marks Windows-1252's, others '?': textmod.h)
    return GameTextOfUtf8(utf8);
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
    const std::string key = std::string("Action.") + cane::sly2input::kActions[index].id;
    if (const char* sz = SzPortOverride(key.c_str(), cane::sly2input::kActions[index].label)) return StrToGameText(sz);
    const int lang = IlangTable();
    return StrToGameText(lang == LANG_English ? cane::sly2input::kActions[index].label : s_actionNames[index].text[lang - 1]);
}

std::string StrPortActionGroup(std::string_view group)
{
    if (const char* sz = SzPortOverride(StrActionGroupKey(group).c_str(), std::string(group).c_str())) return StrToGameText(sz);
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

std::vector<std::pair<std::string, std::string>> PortTextColumn(int lang)
{
    const int ilang = lang >= 0 && lang < CLANG ? lang : LANG_English;
    std::vector<std::pair<std::string, std::string>> out;
    for (const ROW& row : s_arow) out.emplace_back(row.name, row.asz[ilang]);
    out.emplace_back("DecimalMark", ilang == LANG_English || ilang == LANG_Japanese || ilang == LANG_Korean ? "." : ",");
    const auto& actions = cane::sly2input::kActions;
    for (std::size_t i = 0; i < actions.size(); ++i)
        out.emplace_back(std::string("Action.") + actions[i].id, ilang == LANG_English ? actions[i].label : s_actionNames[i].text[ilang - 1]);
    for (const auto& row : s_actionGroups)
        out.emplace_back(StrActionGroupKey(row.group), ilang == LANG_English ? std::string(row.group) : std::string(row.text[ilang - 1]));
    return out;
}

std::string StrPortKeyName(int key)
{
    if (FPortEnglish() && !PmpPortTextShown()) return PchzKeyName(key);
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
constexpr PCROW s_apcrow[] = {
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
constexpr PCROWJP s_apcrowJp[] = {
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
// the Korean disc's messages (its own ids: the USA's one or two on, with a second "no memory card" 0x12 and "card full"
// 0x1A that ask to continue), in its words (저장, 진행사항, 에러) with the save data in place of the memory card
constexpr PCROWJP s_apcrowKr[] = {
    {0x11, "저장 폴더를 찾을 수 없습니다\n\n슬라이 쿠퍼 2는 100KB의 빈 공간을 필요로 합니다\n\n저장 폴더를 확인하지 않으면 게임을 저장할 수 없습니다\n"},
    {0x12, "저장 폴더를 찾을 수 없습니다\n\n슬라이 쿠퍼 2는 100KB의 빈 공간을 필요로 합니다\n\n저장 폴더를 확인하지 않으면 게임을 저장할 수 없습니다\n계속 하시겠습니까?"},
    {0x13, "슬라이 쿠퍼 2 저장 데이터를 읽을 수 없습니다\n\n초기화하지 않으면 진행사항을 저장할 수 없습니다\n\n초기화하시겠습니까?"},
    {0x14, "\n슬라이 쿠퍼 2 저장 데이터를 초기화하고 있습니다\n\n게임을 끄지 마세요\n"},
    {0x15, "슬라이 쿠퍼 2 저장 데이터를 초기화하던 중 에러가 발생했습니다\n\n진행사항을 저장할 수 없습니다\n"},
    {0x16, "슬라이 쿠퍼 2 저장 데이터가 없습니다\n\n슬라이 쿠퍼 2는 100 KB의 디스크 여유 공간을 필요로 합니다\n\n슬라이 쿠퍼 2 저장 데이터를 만들어야만 앞으로의 진행사항을 저장하실 수 있습니다\n\n슬라이 쿠퍼 2 저장 데이터를 생성할까요?"},
    {0x17, "\n슬라이 쿠퍼 2 저장 데이터를 생성하고 있습니다\n\n게임을 끄지 마세요\n"},
    {0x18, "슬라이 쿠퍼 2 저장 데이터를 생성하던 중 에러가 발생했습니다\n\n진행사항이 저장되지 않았습니다\n"},
    {0x19, "슬라이 쿠퍼 2 저장 데이터를 위한 디스크 여유 공간이 부족합니다\n\n슬라이 쿠퍼 2는 100 KB의 여유 공간을 필요로 합니다\n\n디스크 공간을 확보하지 않으면 게임을 저장할 수 없습니다\n"},
    {0x1A, "슬라이 쿠퍼 2 저장 데이터를 위한 디스크 여유 공간이 부족합니다\n\n슬라이 쿠퍼 2는 100 KB의 여유 공간을 필요로 합니다\n\n디스크 공간을 확보하지 않으면 게임을 저장할 수 없습니다\n계속 하시겠습니까?"},
    {0x1C, "\n저장중...\n\n게임을 끄지 마세요\n"},
    {0x1F, "슬라이 쿠퍼 2 저장 데이터를 쓰던 중 에러가 발생했습니다\n\n데이터에 에러가 있을 수 있습니다\n\n게임 진행 사항이 저장되지 않았습니다\n"},
    {0x23, "\n삭제중...\n\n게임을 끄지 마세요\n"},
    {0x24, "다음 아이콘이 나타나면,\n진행사항이 저장되는 중입니다\n\n&2a&.\n\n이 아이콘이 나타난 동안에는\n게임을 끄지 마세요\n"},
    {0x25, "자동 저장에 실패했습니다\n\n자동 저장 기능이 꺼져있습니다\n데이터에 에러가 있을 수 있습니다\n\n게임 저장 옵션을 이용하여 게임을 수동으로 저장한 다음 자동 저장 기능을 재설정 하세요\n"},
    {0x26, "슬라이 쿠퍼 2 저장 데이터를 더 이상 사용할 수 없습니다\n\n자동저장 기능이 중단되었습니다\n\n게임 저장 옵션을 사용하여 수동으로 저장하고 자동저장 기능을 재설정 하세요\n"},
    {0x2C, "슬라이 쿠퍼 2 저장 데이터의 일부에 에러가 있습니다\n\n에러가 있는 데이터를 삭제했습니다\n"},
    {0x2D, "게임 데이터를 읽을 수 없습니다.\n\n슬라이 쿠퍼 2 설치를 확인하세요."},
    {0x30, "\n슬라이 쿠퍼 2 저장 데이터가 저장 폴더에 있는지 확인하세요\n"},
    {0x51, "USB 헤드셋: 켜짐"},
    {0x52, "USB 헤드셋: 꺼짐"},
};
// clang-format on
}  // namespace

const char* SzEpisodeMenuWording(int idDisc)
{
    // (lang 2026-11-10) the pause menu's "Exit to Episode Menu" (USA 0x42): the Dutch, Portuguese, Danish, Finnish,
    // Norwegian and Swedish texts say "DVD menu" (their translators' word for the PS2 disc's menu); every other
    // language, and the USA / Japanese discs, name the episode menu. Each in its language's own word for an episode
    // (Dutch "Deel", Finnish "Jakso", Swedish "Episod": the episode menu's "Episode 1" and "Start episode" lines).
    if (IdTextUsa(idDisc) != 0x42) return nullptr;
    switch (IlangTable())
    {
    case LANG_Dutch: return "Ga naar deelmenu";
    case LANG_Portuguese: return "Ir para o menu de episódios";
    case LANG_Danish: return "Afslut til episodemenu";
    case LANG_Finnish: return "Poistu jaksovalikkoon";
    case LANG_Norwegian: return "Gå ut til episodemenyen";
    case LANG_Swedish: return "Avsluta till episodmeny";
    default: return nullptr;
    }
}

const char* SzPcWording(int idDisc)
{
    if (FWideChars() && IlangTable() == LANG_Japanese)
    {
        // (the other discs: the Japanese disc's id of the same message, region.h)
        const int idJp = g_disc.Region() == DISCREGION::Japan ? idDisc : IdTextJapanOfUsa(IdTextUsa(idDisc));
        for (const PCROWJP& r : s_apcrowJp)
            if (r.id == idJp) return r.sz;
        return nullptr;
    }
    if (FWideChars() && IlangTable() == LANG_Korean)
    {
        // (the other discs: the Korean disc's id of the same message, region.h)
        const int idKr = g_disc.Region() == DISCREGION::Korea ? idDisc : IdTextKoreaOfUsa(IdTextUsa(idDisc));
        for (const PCROWJP& r : s_apcrowKr)
            if (r.id == idKr) return r.sz;
        return nullptr;
    }
    const int ilang = IlangTable();
    if (ilang >= 11) return nullptr;
    // (a custom language's own messages stay; an English variant's take the English PC wording)
    if (const int lt = LangText(); lt >= LANG_Max)
        if (const char* key = SzLangStoreKey(lt); !key || std::strncmp(key, "English", 7) != 0) return nullptr;
    const int idUsa = IdTextUsa(idDisc);
    for (const PCROW& r : s_apcrow)
        if (r.idUsa == idUsa) return r.asz[ilang];
    return nullptr;
}

bool FPcWording() { return !g_pcinput.fAlwaysOriginalPs; }
