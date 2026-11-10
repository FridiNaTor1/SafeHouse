// See porttext.h.

#include "porttext.h"
#include "content.h"

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>

namespace
{
// The columns: English, French, German, Spanish, Italian, Japanese, Korean (CONTENT_LANGUAGE's order).
// English is the port's original text (the USA disc). G: the game's own words or style (its localization table:
// the Vibration row's On / Off, the Binoc-u-com / Turrets rows, " : " in French and Korean, the Japanese full-width
// colon, the menus' "..." / " …" / "…" and the Japanese and Korean menus without them); P: ours. The French text
// says "tu", as the game's does ("Lance la mission").
constexpr int CLANG = 7;
struct ROW
{
    PTX ptx;
    const char* asz[CLANG];
};

// clang-format off
constexpr ROW s_arow[] = {
    // G: separators and the menus' On / Off (the Vibration row, 138 / 139)
    {PTX_Sep, {": ", " : ", ": ", ": ", ": ", "：", " : "}},
    {PTX_On, {"On", "Oui", "Ein", "Sí", "on", "オン", "켜짐"}},
    {PTX_Off, {"Off", "Non", "Aus", "No", "off", "オフ", "꺼짐"}},

    // P: the menus' entries (G: the game's "Options..." style in each language)
    {PTX_ExitToDesktop, {"Exit to Desktop", "Quitter vers le bureau", "Zurück zum Desktop", "Salir al escritorio",
        "Esci al desktop", "デスクトップに戻る", "바탕 화면으로 나가기"}},
    {PTX_VideoEntry, {"Video...", "Vidéo", "Video …", "Vídeo...", "Video…", "ビデオ", "비디오"}},
    {PTX_SoundEntry, {"Sound...", "Son", "Sound …", "Sonido...", "Audio…", "サウンド", "사운드"}},
    {PTX_KeyboardMappingEntry, {"Keyboard Mapping...", "Clavier", "Tastaturbelegung …", "Teclado...",
        "Tastiera…", "キーボード", "키보드 설정"}},
    {PTX_ControllerMappingEntry, {"Controller Mapping...", "Manette", "Controllerbelegung …", "Mando...",
        "Controller…", "コントローラ", "컨트롤러 설정"}},
    {PTX_MouseEntry, {"Mouse...", "Souris", "Maus …", "Ratón...", "Mouse…", "マウス", "마우스"}},

    // P: the menus' titles
    {PTX_Video, {"Video", "Vidéo", "Video", "Vídeo", "Video", "ビデオ", "비디오"}},
    {PTX_Sound, {"Sound", "Son", "Sound", "Sonido", "Audio", "サウンド", "사운드"}},
    {PTX_KeyboardMapping, {"Keyboard Mapping", "Configuration du clavier", "Tastaturbelegung",
        "Asignación del teclado", "Configura tastiera", "キーボード", "키보드 설정"}},
    {PTX_ControllerMapping, {"Controller Mapping", "Configuration de la manette", "Controllerbelegung",
        "Asignación del mando", "Configura controller", "コントローラ", "컨트롤러 설정"}},
    {PTX_Mouse, {"Mouse", "Souris", "Maus", "Ratón", "Mouse", "マウス", "마우스"}},
    {PTX_AreYouSure, {"Are you sure?", "Tu es sûr ?", "Bist du sicher?", "¿Seguro?", "Sei sicuro?",
        "よろしいですか？", "정말 나가시겠습니까?"}},

    // P: Video (G: the Italian values in lower case, as its menus write them)
    {PTX_Msaa, {"MSAA", "MSAA", "MSAA", "MSAA", "MSAA", "MSAA", "MSAA"}},
    {PTX_FrameRate, {"Frame Rate", "Images par seconde", "Bildrate", "Fotogramas", "Fotogrammi", "フレームレート",
        "프레임 레이트"}},
    {PTX_Fps, {"%d FPS", "%d", "%d FPS", "%d FPS", "%d FPS", "%d FPS", "%d FPS"}},
    {PTX_Performance, {" (Performance)", " (performances)", " (Leistung)", " (rendimiento)", " (prestazioni)",
        "（パフォーマンス）", " (성능)"}},
    {PTX_Resolution, {"Resolution", "Résolution", "Auflösung", "Resolución", "Risoluzione", "解像度", "해상도"}},
    {PTX_NativeResolution, {"Native (%d x %d)", "Native (%d x %d)", "Nativ (%d x %d)", "Nativa (%d x %d)",
        "nativa (%d x %d)", "ネイティブ（%d x %d）", "기본 (%d x %d)"}},
    {PTX_Fog, {"Fog", "Brouillard", "Nebel", "Niebla", "Nebbia", "フォグ", "안개"}},
    {PTX_Ps2Style, {"PS2 Style", "Style PS2", "PS2-Stil", "Estilo PS2", "stile PS2", "PS2スタイル", "PS2 스타일"}},
    {PTX_Ps3Style, {"PS3 Style", "Style PS3", "PS3-Stil", "Estilo PS3", "stile PS3", "PS3スタイル", "PS3 스타일"}},
    {PTX_DrawDistance, {"Draw Distance", "Distance d'affichage", "Sichtweite", "Distancia de dibujado",
        "Distanza visiva", "ドローディスタンス", "시야 거리"}},
    {PTX_Low, {"Low", "Faible", "Niedrig", "Baja", "bassa", "低", "낮음"}},
    {PTX_Original, {"Original", "D'origine", "Original", "Original", "originale", "オリジナル", "원본"}},
    {PTX_High, {"High", "Élevée", "Hoch", "Alta", "alta", "高", "높음"}},
    {PTX_VeryHigh, {"Very High", "Très élevée", "Sehr hoch", "Muy alta", "molto alta", "最高", "매우 높음"}},
    {PTX_Extreme, {"Extreme", "Extrême", "Extrem", "Extrema", "estrema", "エクストリーム", "최고"}},
    {PTX_Custom, {"Custom (%.2fx)", "Perso. (%.2fx)", "Eigene (%.2fx)", "Personal. (%.2fx)",
        "personalizzata (%.2fx)", "カスタム（%.2fx）", "사용자 지정 (%.2fx)"}},
    {PTX_WindowMode, {"Window Mode", "Affichage", "Fenstermodus", "Modo de ventana", "Modalità finestra",
        "ウィンドウモード", "창 모드"}},
    {PTX_Windowed, {"Windowed", "Fenêtré", "Fenster", "Ventana", "in finestra", "ウィンドウ", "창"}},
    {PTX_Borderless, {"Borderless", "Sans bordure", "Randlos", "Sin bordes", "senza bordi", "ボーダーレス",
        "테두리 없음"}},
    {PTX_Fullscreen, {"Fullscreen", "Plein écran", "Vollbild", "Pantalla completa", "schermo intero",
        "フルスクリーン", "전체 화면"}},
    {PTX_VSync, {"VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "수직 동기화"}},
    {PTX_AspectRatio, {"Aspect Ratio", "Format d'image", "Seitenverhältnis", "Relación de aspecto", "Formato",
        "アスペクトレシオ", "화면 비율"}},
    {PTX_FitToScreen, {"Fit to Screen", "Adapté à l'écran", "Bildschirmfüllend", "Ajustar a la pantalla",
        "adatta allo schermo", "スクリーン全体", "화면에 맞춤"}},
    {PTX_GuiScale, {"GUI Scale", "Taille de l'interface", "UI-Größe", "Tamaño de la interfaz",
        "Dimensione interfaccia", "GUIサイズ", "GUI 크기"}},
    {PTX_GuiStyle, {"GUI Style", "Style de l'interface", "UI-Stil", "Estilo de la interfaz", "Stile interfaccia",
        "GUIスタイル", "GUI 스타일"}},
    {PTX_Modern, {"Modern", "Moderne", "Modern", "Moderno", "moderno", "モダン", "모던"}},
    {PTX_DisableControllerUnfocused, {"Disable Controller Input When Unfocused", "Manette inactive en arrière-plan",
        "Controller im Hintergrund sperren", "Mando inactivo en segundo plano", "Controller inattivo in background",
        "バックグラウンドでパッドを止める", "비활성 창에서 컨트롤러 끄기"}},

    // P: Sound
    {PTX_MusicVolume, {"Music Volume", "Volume musique", "Musiklautstärke", "Volumen de música", "Volume musica",
        "音楽ボリューム", "음악 음량"}},
    {PTX_SfxVolume, {"Sound Effects", "Effets sonores", "Soundeffekte", "Efectos de sonido", "Effetti sonori",
        "サウンドエフェクト", "효과음"}},
    {PTX_DialogueVolume, {"Dialogue", "Dialogues", "Dialoge", "Diálogos", "Dialoghi", "セリフ", "대사"}},

    // G: the camera row in the wording of the game's Binoc-u-com / Turrets / Jet Pack rows (144..149); the
    // Korean keeps their English direction word ("Up" there, "Right" here)
    {PTX_CameraTurnLeft, {"Camera: &2)&. Right = Turn Left", "Caméra : &2)&. droite = Tourner gauche",
        "Kamera: &2)&. Rechts = Links drehen", "Cámara: &2)&. derecha = girar a la izquierda",
        "Telecamera: &2)&. Destra = Ruota a sinistra", "カメラ：&2)&.右で左に回る", "카메라: &2)&. Right = 왼쪽으로 회전"}},
    {PTX_CameraTurnRight, {"Camera: &2)&. Right = Turn Right", "Caméra : &2)&. droite = Tourner droite",
        "Kamera: &2)&. Rechts = Rechts drehen", "Cámara: &2)&. derecha = girar a la derecha",
        "Telecamera: &2)&. Destra = Ruota a destra", "カメラ：&2)&.右で右に回る", "카메라: &2)&. Right = 오른쪽으로 회전"}},
    // P: Controls
    {PTX_ButtonIcons, {"Button Icons", "Icônes", "Tastensymbole", "Iconos", "Icone", "ボタンアイコン", "버튼 아이콘"}},
    {PTX_Automatic, {"Automatic", "Automatique", "Automatisch", "Automático", "automatico", "自動", "자동"}},
    {PTX_OriginalPsIcons, {"Original PlayStation Icons", "Icônes PlayStation d'origine",
        "Original-PlayStation-Symbole", "Iconos originales de PlayStation", "Icone PlayStation originali",
        "オリジナルのPlayStationアイコン", "오리지널 PlayStation 아이콘"}},
    {PTX_InstantSkip, {"Instant Skip Cutscenes", "Saut direct des cinématiques", "Szenen sofort überspringen",
        "Saltar escenas al instante", "Salto immediato dei filmati", "ムービーをすぐスキップ", "컷신 바로 건너뛰기"}},

    // P: Mouse (G: the Binoc-u-com's name in each language, 144)
    {PTX_MouseCamera, {"Mouse Camera", "Caméra à la souris", "Kamera mit Maus", "Cámara con ratón",
        "Telecamera col mouse", "マウスでカメラ", "마우스 카메라"}},
    {PTX_CameraSensitivity, {"Camera Sensitivity", "Sensibilité caméra", "Kameraempfindlichkeit",
        "Sensibilidad de cámara", "Sensibilità telecamera", "カメラ感度", "카메라 감도"}},
    {PTX_AimSensitivity, {"Aim Sensitivity", "Sensibilité de visée", "Zielempfindlichkeit",
        "Sensibilidad de apuntado", "Sensibilità mira", "狙う時の感度", "조준 감도"}},
    {PTX_InvertHorizontal, {"Invert Horizontal", "Inverser l'horizontale", "Horizontal umkehren",
        "Invertir horizontal", "Inverti orizzontale", "左右リバース", "좌우 반전"}},
    {PTX_InvertAimVertical, {"Invert Aim Vertical", "Inverser la visée verticale", "Zielen vertikal umkehren",
        "Invertir apuntado vertical", "Inverti mira verticale", "狙う時の上下リバース", "조준 상하 반전"}},
    {PTX_RawInput, {"Raw Input", "Entrée brute", "Rohe Eingabe", "Entrada directa", "Input diretto", "RAW入力",
        "로우 입력"}},
    {PTX_WheelZoomsBinocucom, {"Wheel Zooms Binocucom", "Zoom Binocom à la molette", "Mausrad zoomt Sicht-Kom.",
        "Zoom del bino-com con la rueda", "Zoom Bino-com con la rotellina", "ホイールでズーム", "휠로 비노큐컴 줌"}},
    {PTX_MouseInMenus, {"Mouse in Menus", "Souris dans les menus", "Maus in Menüs", "Ratón en los menús",
        "Mouse nei menu", "メニューでマウス", "메뉴에서 마우스"}},

    // P: the keyboard mapping's rows
    {PTX_MoveUp, {"Move Up", "Aller en haut", "Nach oben", "Mover arriba", "Muovi su", "上へ移動", "위로 이동"}},
    {PTX_MoveDown, {"Move Down", "Aller en bas", "Nach unten", "Mover abajo", "Muovi giù", "下へ移動", "아래로 이동"}},
    {PTX_MoveLeft, {"Move Left", "Aller à gauche", "Nach links", "Mover a la izquierda", "Muovi a sinistra",
        "左へ移動", "왼쪽으로 이동"}},
    {PTX_MoveRight, {"Move Right", "Aller à droite", "Nach rechts", "Mover a la derecha", "Muovi a destra",
        "右へ移動", "오른쪽으로 이동"}},
    {PTX_Square, {"Square", "Carré", "Quadrat", "Cuadrado", "Quadrato", "しかくボタン", "네모 버튼"}},
    {PTX_Circle, {"Circle", "Rond", "Kreis", "Círculo", "Cerchio", "まるボタン", "동그라미 버튼"}},
    {PTX_Cross, {"Cross", "Croix", "Kreuz", "Cruz", "Croce", "バツボタン", "엑스 버튼"}},
    {PTX_Triangle, {"Triangle", "Triangle", "Dreieck", "Triángulo", "Triangolo", "さんかくボタン", "세모 버튼"}},
    // G: the game's START / SELECT button names (0, 3)
    {PTX_Start, {"Start", "Start", "Start", "Start", "Start", "STARTボタン", "시작 버튼"}},
    {PTX_Select, {"Select", "Select", "Select", "Select", "Select", "SELECTボタン", "선택 버튼"}},
    {PTX_CameraLeft, {"Camera Left", "Caméra gauche", "Kamera links", "Cámara izquierda", "Telecamera sinistra",
        "カメラ左", "카메라 왼쪽"}},
    {PTX_CameraRight, {"Camera Right", "Caméra droite", "Kamera rechts", "Cámara derecha", "Telecamera destra",
        "カメラ右", "카메라 오른쪽"}},
    {PTX_CameraUp, {"Camera Up", "Caméra haut", "Kamera hoch", "Cámara arriba", "Telecamera su", "カメラ上",
        "카메라 위"}},
    {PTX_CameraDown, {"Camera Down", "Caméra bas", "Kamera runter", "Cámara abajo", "Telecamera giù", "カメラ下",
        "카메라 아래"}},
    {PTX_PressAKey, {"Press a key", "Appuie sur une touche", "Taste drücken", "Pulsa una tecla", "Premi un tasto",
        "キーを押してください", "키를 누르십시오"}},
    {PTX_PressAButton, {"Press a button", "Appuie sur un bouton", "Knopf drücken", "Pulsa un botón",
        "Premi un pulsante", "ボタンを押してください", "버튼을 누르십시오"}},

    // P: key and mouse button names (the other keys keep their keycaps' names: Tab, Esc, letters, ...)
    {PTX_KeyUnbound, {"Unbound", "Non assigné", "Nicht belegt", "Sin asignar", "Non assegnato", "なし", "없음"}},
    {PTX_KeySpace, {"Space", "Espace", "Leertaste", "Espacio", "Spazio", "スペース", "스페이스"}},
    {PTX_KeyEnter, {"Enter", "Entrée", "Eingabe", "Intro", "Invio", "エンター", "엔터"}},
    {PTX_KeyBackspace, {"Backspace", "Retour arrière", "Rücktaste", "Retroceso", "Backspace", "バックスペース",
        "백스페이스"}},
    {PTX_KeyLeftShift, {"Left Shift", "Maj gauche", "Umschalt links", "Mayús izquierdo", "Maiusc sinistro",
        "左Shift", "왼쪽 Shift"}},
    {PTX_KeyRightShift, {"Right Shift", "Maj droite", "Umschalt rechts", "Mayús derecho", "Maiusc destro",
        "右Shift", "오른쪽 Shift"}},
    {PTX_KeyLeftCtrl, {"Left Ctrl", "Ctrl gauche", "Strg links", "Ctrl izquierdo", "Ctrl sinistro", "左Ctrl",
        "왼쪽 Ctrl"}},
    {PTX_KeyRightCtrl, {"Right Ctrl", "Ctrl droite", "Strg rechts", "Ctrl derecho", "Ctrl destro", "右Ctrl",
        "오른쪽 Ctrl"}},
    {PTX_KeyLeftAlt, {"Left Alt", "Alt gauche", "Alt links", "Alt izquierdo", "Alt sinistro", "左Alt", "왼쪽 Alt"}},
    {PTX_KeyRightAlt, {"Right Alt", "Alt droite", "Alt rechts", "Alt derecho", "Alt destro", "右Alt",
        "오른쪽 Alt"}},
    {PTX_KeyUp, {"Up Arrow", "Flèche haut", "Pfeil oben", "Flecha arriba", "Freccia su", "上キー", "위쪽 화살표"}},
    {PTX_KeyDown, {"Down Arrow", "Flèche bas", "Pfeil unten", "Flecha abajo", "Freccia giù", "下キー",
        "아래쪽 화살표"}},
    {PTX_KeyLeft, {"Left Arrow", "Flèche gauche", "Pfeil links", "Flecha izquierda", "Freccia sinistra", "左キー",
        "왼쪽 화살표"}},
    {PTX_KeyRight, {"Right Arrow", "Flèche droite", "Pfeil rechts", "Flecha derecha", "Freccia destra", "右キー",
        "오른쪽 화살표"}},
    {PTX_Key, {"Key", "Touche", "Taste", "Tecla", "Tasto", "キー", "키"}},
    {PTX_MouseLeft, {"Left Mouse", "Clic gauche", "Linke Maustaste", "Botón izquierdo", "Tasto sinistro",
        "マウス左", "마우스 왼쪽"}},
    {PTX_MouseRight, {"Right Mouse", "Clic droit", "Rechte Maustaste", "Botón derecho", "Tasto destro", "マウス右",
        "마우스 오른쪽"}},
    {PTX_MouseMiddle, {"Middle Mouse", "Clic central", "Mittlere Maustaste", "Botón central", "Tasto centrale",
        "マウス中", "마우스 가운데"}},
    {PTX_MouseN, {"Mouse %d", "Souris %d", "Maustaste %d", "Ratón %d", "Mouse %d", "マウス%d", "마우스 %d"}},
    {PTX_WheelUp, {"Wheel Up", "Molette haut", "Mausrad hoch", "Rueda arriba", "Rotellina su", "ホイール上",
        "휠 위"}},
    {PTX_WheelDown, {"Wheel Down", "Molette bas", "Mausrad runter", "Rueda abajo", "Rotellina giù", "ホイール下",
        "휠 아래"}},
    {PTX_MouseButton, {"Mouse", "Souris", "Maus", "Ratón", "Mouse", "マウス", "마우스"}},

    // P: the options' one-line help
    {PTX_HelpPromptStyle, {"Which buttons the prompts show. Automatic follows the controller or keyboard you used last.",
        "Les boutons affichés. Automatique suit la dernière manette ou le clavier utilisés.",
        "Welche Tasten die Hinweise zeigen. Automatisch folgt dem zuletzt benutzten Gerät.",
        "Los botones que se muestran. Automático sigue al último mando o teclado usado.",
        "I tasti mostrati. Automatico segue l'ultimo controller o la tastiera usati.",
        "プロンプトのボタン。自動では、最後に使ったコントローラかキーボードのボタンになる。",
        "표시할 버튼. 자동은 마지막으로 사용한 컨트롤러나 키보드를 따릅니다."}},
    {PTX_HelpOriginalPsIcons, {"Shows the original PlayStation buttons whatever you play with.",
        "Affiche les boutons PlayStation d'origine, quelle que soit la manette.",
        "Zeigt die Original-PlayStation-Tasten, egal womit du spielst.",
        "Muestra los botones originales de PlayStation, juegues con lo que juegues.",
        "Mostra i tasti PlayStation originali, con qualsiasi controller.",
        "どのコントローラでも、オリジナルのPlayStationのボタンを出す。",
        "어떤 컨트롤러를 사용해도 오리지널 PlayStation 버튼을 표시합니다."}},
    {PTX_HelpInstantSkip, {"One press of Circle skips a cutscene, instead of holding it for 2 seconds.",
        "Une pression sur Rond passe une cinématique, sans le maintenir 2 secondes.",
        "Ein Druck auf Kreis überspringt eine Szene, statt ihn 2 Sekunden zu halten.",
        "Una pulsación de Círculo salta una escena, sin mantenerlo 2 segundos.",
        "Una pressione di Cerchio salta un filmato, senza tenerlo premuto 2 secondi.",
        "まるボタンを2秒押し続けるかわりに、1回押すだけでムービーをスキップ。",
        "동그라미 버튼을 2초 동안 누르는 대신 한 번 눌러 컷신을 건너뜁니다."}},
    {PTX_HelpMouse, {"Mouse camera, aiming and menu options.", "Caméra, visée et menus à la souris.",
        "Mauskamera, Zielen und Menüs.", "Cámara, apuntado y menús con el ratón.",
        "Telecamera, mira e menu col mouse.", "マウスのカメラ、狙い、メニューのオプション。",
        "마우스 카메라, 조준, 메뉴 옵션."}},
    {PTX_HelpMouseCamera, {"While playing, the mouse turns the camera and aims. Pause to free the cursor.",
        "En jeu, la souris tourne la caméra et vise. Mets en pause pour libérer le curseur.",
        "Im Spiel dreht die Maus die Kamera und zielt. Pausiere, um den Zeiger freizugeben.",
        "Al jugar, el ratón gira la cámara y apunta. Pausa para liberar el cursor.",
        "In gioco il mouse ruota la telecamera e mira. Metti in pausa per liberare il cursore.",
        "プレイ中はマウスでカメラを回して狙う。ポーズでカーソルが出る。",
        "플레이 중에는 마우스로 카메라를 돌리고 조준합니다. 일시 정지하면 커서가 나타납니다."}},
    {PTX_HelpMouseSensitivity, {"How far the camera turns per mouse movement.",
        "La rotation de la caméra par mouvement de la souris.", "Wie weit sich die Kamera pro Mausbewegung dreht.",
        "Cuánto gira la cámara con cada movimiento del ratón.",
        "Quanto ruota la telecamera per ogni movimento del mouse.", "マウスを動かした時のカメラの回る速さ。",
        "마우스를 움직일 때 카메라가 도는 정도."}},
    {PTX_HelpAimSensitivity, {"Binocucom aiming speed.", "Vitesse de visée du Binocom.",
        "Zielgeschwindigkeit des Sicht-Kommunikators.", "Velocidad de apuntado del bino-com.",
        "Velocità di mira del Bino-com.", "ビジョンゴーグルで狙う速さ。", "비노큐컴 조준 속도."}},
    {PTX_HelpInvertHorizontal, {"Moving the mouse right turns the camera left.",
        "La souris vers la droite tourne la caméra à gauche.", "Maus nach rechts dreht die Kamera nach links.",
        "Mover el ratón a la derecha gira la cámara a la izquierda.",
        "Il mouse a destra ruota la telecamera a sinistra.", "マウスを右に動かすとカメラが左に回る。",
        "마우스를 오른쪽으로 움직이면 카메라가 왼쪽으로 돕니다."}},
    {PTX_HelpInvertAimVertical, {"Moving the mouse up aims down.", "La souris vers le haut vise vers le bas.",
        "Maus nach oben zielt nach unten.", "Mover el ratón hacia arriba apunta hacia abajo.",
        "Il mouse in su mira in basso.", "マウスを上に動かすと下を狙う。", "마우스를 위로 움직이면 아래를 조준합니다."}},
    {PTX_HelpRawInput, {"Unaccelerated mouse motion where the system supports it.",
        "Mouvement de la souris sans accélération, si le système le permet.",
        "Mausbewegung ohne Beschleunigung, wo das System es unterstützt.",
        "Movimiento del ratón sin aceleración, si el sistema lo admite.",
        "Movimento del mouse senza accelerazione, se il sistema lo supporta.",
        "システムが対応していれば、マウスの動きをそのまま使う。",
        "시스템이 지원하면 가속 없는 마우스 움직임을 사용합니다."}},
    {PTX_HelpWheelZoom, {"The wheel zooms the binocucom, unless an action is bound to it.",
        "La molette zoome le Binocom, sauf si une action lui est assignée.",
        "Das Mausrad zoomt den Sicht-Kommunikator, außer es ist belegt.",
        "La rueda hace zoom con el bino-com, salvo que tenga una acción asignada.",
        "La rotellina fa lo zoom del Bino-com, se non le è assegnata un'azione.",
        "ホイールにアクションがなければ、ホイールでビジョンゴーグルをズーム。",
        "휠에 동작이 지정되지 않았으면 휠로 비노큐컴을 줌합니다."}},
    {PTX_HelpMouseMenus, {"Point at menu items, left click to choose, right click to go back.",
        "Pointe un choix, clic gauche pour choisir, clic droit pour revenir.",
        "Auf Einträge zeigen, Linksklick wählt, Rechtsklick geht zurück.",
        "Señala una opción, clic izquierdo para elegir, clic derecho para volver.",
        "Punta le voci, clic sinistro per scegliere, clic destro per tornare indietro.",
        "メニューでマウスを使う。左クリックで選ぶ、右クリックで戻る。",
        "메뉴 항목을 가리키고 왼쪽 클릭으로 선택, 오른쪽 클릭으로 뒤로 갑니다."}},
    {PTX_HelpCameraKeys, {"Keyboard camera, like the right stick. In menus these keys move the highlight.",
        "Caméra au clavier, comme le stick droit. Dans les menus, ces touches déplacent la sélection.",
        "Tastaturkamera wie der rechte Stick. In Menüs bewegen diese Tasten die Auswahl.",
        "Cámara con teclado, como el stick derecho. En los menús estas teclas mueven la selección.",
        "Telecamera da tastiera, come lo stick destro. Nei menu questi tasti spostano la selezione.",
        "キーボードでカメラを回す（右スティックのかわり）。メニューではカーソルを動かす。",
        "오른쪽 스틱처럼 키보드로 카메라를 돌립니다. 메뉴에서는 선택을 움직입니다."}},

    // P: the cutscene skip hint (Circle on every disc, as the skip itself)
    {PTX_HoldToSkip, {"Hold &2O&. to skip", "Maintiens &2O&. pour passer", "&2O&. halten zum Überspringen",
        "Mantén &2O&. para saltar", "Tieni premuto &2O&. per saltare", "&2O&.を押し続けるとスキップ",
        "&2O&.버튼을 누르고 있으면 건너뛰기"}},
    {PTX_HoldToSkipNoIcons, {"Hold Circle to skip", "Maintiens Rond pour passer", "Kreis halten zum Überspringen",
        "Mantén Círculo para saltar", "Tieni premuto Cerchio per saltare", "まるボタンを押し続けるとスキップ",
        "동그라미 버튼을 누르고 있으면 건너뛰기"}},
    {PTX_AutoCameraRotation, {"Automatic Camera Rotation", "Rotation auto caméra", "Automatische Kameradrehung",
        "Giro automático de cámara", "Rotazione auto telecamera", "カメラ自動回転", "카메라 자동 회전"}},
    {PTX_HelpAutoCameraRotation, {"Rotate the follow camera as you move. Off: keep your chosen angle. Scripted cameras still apply.",
        "La caméra tourne quand tu bouges. Non : garde ton angle. Les caméras imposées restent actives.",
        "Dreht die Kamera beim Bewegen. Aus: Blickwinkel beibehalten. Vorgegebene Kameras bleiben aktiv.",
        "Gira la cámara al moverte. No: mantiene tu ángulo. Las cámaras de escenas siguen activas.",
        "Ruota la telecamera mentre ti muovi. Off: mantiene la tua angolazione. Le visuali imposte restano attive.",
        "移動時にカメラを自動回転。オフ：選んだ角度を維持。固定カメラはそのまま。",
        "이동할 때 카메라를 자동으로 회전합니다. 끄면 선택한 각도를 유지합니다. 지정된 카메라는 유지됩니다."}},
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
}

int IlangPort()
{
    if (FKoreanContent())
        return 6;
    if (FJapaneseContent())
        return 5;
    if (!FLocalizedContent())
        return 0;
    switch (ContentPackage().language)
    {
    case CONTENT_LANGUAGE::French: return 1;
    case CONTENT_LANGUAGE::German: return 2;
    case CONTENT_LANGUAGE::Spanish: return 3;
    case CONTENT_LANGUAGE::Italian: return 4;
    default: return 0;
    }
}

const char* PchzPort(PTX ptx)
{
    if (ptx < 0 || ptx >= PTX_Max)
        return "";
    return s_arow[ptx].asz[IlangPort()];
}

std::string StrPortValue(PTX ptxLabel, const std::string& value)
{
    return std::string(PchzPort(ptxLabel)) + PchzPort(PTX_Sep) + value;
}

std::string StrPortOnOff(PTX ptxLabel, bool f)
{
    return StrPortValue(ptxLabel, PchzPort(f ? PTX_On : PTX_Off));
}

std::string StrPortFormat(PTX ptx, ...)
{
    char ach[256];
    va_list args;
    va_start(args, ptx);
    std::vsnprintf(ach, sizeof(ach), PchzPort(ptx), args);
    va_end(args);
    std::string str = ach;
    // The European languages write a decimal comma.
    const int ilang = IlangPort();
    if (ilang >= 1 && ilang <= 4)
    {
        for (size_t i = 1; i + 1 < str.size(); ++i)
            if (str[i] == '.' && str[i - 1] >= '0' && str[i - 1] <= '9' && str[i + 1] >= '0' && str[i + 1] <= '9')
                str[i] = ',';
    }
    return str;
}
