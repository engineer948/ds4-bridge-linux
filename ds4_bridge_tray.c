/*
 * ds4_bridge_tray.c — Sony DualShock 4 to virtual Xbox 360 pad bridge
 * with GTK3 & AppIndicator Tray Interface.
 *
 * Compilation:
 *   gcc -O2 -std=gnu11 ds4_bridge_tray.c -o ds4_bridge_tray \
 *       $(pkg-config --cflags --libs gtk+-3.0) -lpthread -lm -ldl
 *
 * The AppIndicator library (Ayatana or the older Canonical one) is loaded
 * at runtime, so no AppIndicator headers are needed and one binary works
 * with either. Without both, GTK's own status icon is used.
 *
 * Files:
 *   settings  $XDG_CONFIG_HOME/ds4-bridge/settings.ini   (~/.config/ds4-bridge/)
 *   log       $XDG_STATE_HOME/ds4-bridge/ds4_bridge.log  (~/.local/state/ds4-bridge/)
 *             rotated to ds4_bridge.log.1 when it grows above 1 MiB
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <syslog.h>
#include <dlfcn.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include <pthread.h>
#include <stdatomic.h>

#include <gtk/gtk.h>
#include <glib-unix.h>

#if !defined(UI_DEV_SETUP) || !defined(UI_ABS_SETUP)
#error "This program requires uinput v5+ (Linux 4.5+) headers: UI_DEV_SETUP / UI_ABS_SETUP not found."
#endif

/* ------------------------------------------------------------------------- */
/*  Configuration constants                                                  */
/* ------------------------------------------------------------------------- */

#define MAX_PADS           4        /* controllers bridged at the same time       */
#define MAX_PROFILES       16
#define BRIDGE_FF_SLOTS    16       /* max concurrent effects virtual device holds*/
#define RESCAN_INTERVAL_MS 1000     /* interval to scan for controller if missing */
#define EV_BATCH           64       /* max events read at once via read()         */
#define OUT_MAX            128      /* max events written to virtual device       */
#define LOG_MAX_BYTES      (1024 * 1024) /* rotate the log file above this size   */
#define LOG_VIEW_LINES     300      /* lines shown in the log viewer              */

#define TRIGGER_PRESS      40       /* 0..255: analog value that counts as pressed*/

#define DESKTOP_TICK_MS    8        /* pointer update interval in desktop mode    */
#define DESKTOP_PX_PER_S   1400.0f  /* pointer speed at full deflection, 100 %    */
#define DESKTOP_WHEEL_PER_S 12.0f   /* wheel notches per second, full deflection  */
#define DESKTOP_MIN_DZ     15       /* desktop pointer deadzone floor (%)         */

#define GYRO_RES_DEFAULT   1024     /* hid-sony / hid-playstation units per deg/s */
#define GYRO_NOISE_DPS     1.5f     /* rates below this are softly damped         */
#define GYRO_STICK_FULL_DPS 90.0f   /* deg/s for full right-stick deflection      */
#define GYRO_MOUSE_PX_PER_DEG 12.0f /* pointer pixels per degree, 100 %           */
#define GYRO_CAL_US        1000000LL

/* Xbox 360 (xpad) axis limits */
#define STICK_MIN   (-32768)
#define STICK_MAX   32767
#define STICK_FUZZ  16
#define STICK_FLAT  128
#define TRIG_MIN    0
#define TRIG_MAX    255
#define HAT_MIN     (-1)
#define HAT_MAX     1

/* ------------------------------------------------------------------------- */
/*  Bit array helpers (EVIOCGBIT results)                                    */
/* ------------------------------------------------------------------------- */

#define BITS_PER_LONG      (sizeof(unsigned long) * 8)
#define NLONGS(nbits)      (((nbits) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)
#define ARRAY_LEN(a)       (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------------- */
/*  Translations (English is the default and the fallback)                   */
/* ------------------------------------------------------------------------- */

enum lang { LANG_EN, LANG_AZ, N_LANGS };

static const char *const LANG_KEYS[N_LANGS]  = { "en", "az" };
static const char *const LANG_NAMES[N_LANGS] = { "English", "Azərbaycanca" };

static int g_lang = LANG_EN;

enum str_id {
    /* tray menu */
    S_STATUS_STOPPED, S_STATUS_RUNNING, S_STATUS_WAITING, S_STATUS_ERROR,
    S_PAD_WAITING, S_PAD_ERROR,
    S_START, S_STOP,
    S_PROFILE_MENU, S_AUTO_PROFILES, S_MANAGE_PROFILES,
    S_CONTROLLER_COUNT, S_ONE_CONTROLLER, S_N_CONTROLLERS,
    S_OUTPUT_MENU, S_DEADZONE_MENU, S_LEFT_STICK, S_RIGHT_STICK, S_RUMBLE_MENU, S_OFF,
    S_DESKTOP_MODE, S_TOUCHPAD, S_SETTINGS, S_TEST, S_LOGS_ENABLE, S_LOGS_VIEW, S_LANGUAGE,
    S_FIX_PERMS, S_AUTOSTART, S_RESET, S_QUIT,
    /* notifications */
    S_N_CONNECTED, S_N_DISCONNECTED, S_N_LOW_BATTERY_TITLE, S_N_LOW_BATTERY_BODY,
    S_N_DESKTOP_ON, S_N_DESKTOP_ON_BODY, S_N_DESKTOP_OFF, S_N_DESKTOP_OFF_BODY,
    S_N_PROFILE, S_N_PROFILE_AUTO, S_N_PROFILE_BACK,
    S_N_AUTOSTART_ON, S_N_AUTOSTART_OFF, S_N_RESET, S_N_ALREADY,
    S_N_PERMS_OK, S_N_PERMS_FAIL, S_N_PERMS_RELOGIN, S_N_NO_TRAY,
    /* dialogs */
    S_ASK_RESET_TITLE, S_ASK_RESET_TEXT, S_ASK_LOGDEL_TITLE, S_ASK_LOGDEL_TEXT,
    S_ASK_PERMS_TITLE, S_ASK_PERMS_TEXT, S_ASK_DELETE_PROFILE,
    S_OK, S_CANCEL, S_CLOSE,
    S_PROFILE_NAME, S_NEW_PROFILE_TITLE, S_RENAME_PROFILE_TITLE, S_PROFILE_NAME_INVALID,
    /* settings window */
    S_SW_TITLE, S_PROFILE, S_NEW, S_RENAME, S_DELETE, S_PROCESSES, S_DEFAULT_PROFILE_NOTE,
    S_TAB_GENERAL, S_TAB_BUTTONS, S_TAB_STICKS, S_TAB_GYRO, S_TAB_DESKTOP,
    S_LANGUAGE_LBL, S_OUTPUT_LBL, S_OUTPUT_HINT, S_COUNT_LBL, S_TOUCHPAD_LBL, S_AUTO_LBL,
    S_LOGS_LBL, S_FIX_PERMS_BTN, S_SETTINGS_FILE, S_LOG_FILE,
    S_BTN_HINT, S_DISABLED, S_RESET_BUTTONS,
    S_DZ_LEFT, S_DZ_RIGHT, S_DZ_HINT, S_INVERT_LY, S_INVERT_RY, S_SWAP, S_DIGITAL_TRIG,
    S_RUMBLE_LBL, S_RUMBLE_HINT,
    S_GYRO_HINT, S_GYRO_MODE, S_GYRO_OFF, S_GYRO_ALWAYS, S_GYRO_HOLD, S_GYRO_BUTTON,
    S_GYRO_OUTPUT, S_GYRO_TO_STICK, S_GYRO_TO_MOUSE, S_GYRO_SENS, S_GYRO_INV_X, S_GYRO_INV_Y,
    S_GYRO_CAL, S_GYRO_CALIBRATING, S_GYRO_FOUND, S_GYRO_NOT_FOUND,
    S_DESKTOP_ENABLE, S_DESKTOP_SPEED, S_DESKTOP_HELP,
    /* controller test window */
    S_TW_TITLE, S_TW_CONTROLLER, S_TW_STOPPED, S_TW_INACTIVE, S_TW_DESKTOP, S_TW_LEGEND, S_TW_GYRO,
    /* log viewer */
    S_LV_TITLE, S_LV_EMPTY, S_LV_CLEAR, S_LV_OPEN_FOLDER,
    N_STRINGS
};

/* Translations must keep the printf conversions of the English text in the
 * same order. */
static const char *const STRINGS[N_STRINGS][N_LANGS] = {
    [S_STATUS_STOPPED] = { "Status: Stopped 🔴", "Vəziyyət: Dayandırılıb 🔴" },
    [S_STATUS_RUNNING] = { "Status: Running 🟢", "Vəziyyət: İşləyir 🟢" },
    [S_STATUS_WAITING] = { "Status: Waiting for controller 🟡", "Vəziyyət: Kontroller gözlənilir 🟡" },
    [S_STATUS_ERROR]   = { "Status: Error ⚠️", "Vəziyyət: Xəta ⚠️" },
    [S_PAD_WAITING]    = { "P%d: waiting for controller…", "P%d: kontroller gözlənilir…" },
    [S_PAD_ERROR]      = { "P%d: cannot open /dev/uinput (%s)", "P%d: /dev/uinput açıla bilmir (%s)" },
    [S_START]          = { "▶ Start Bridge", "▶ Körpünü başlat" },
    [S_STOP]           = { "⏹ Stop Bridge", "⏹ Körpünü dayandır" },
    [S_PROFILE_MENU]   = { "👤 Profile: %s", "👤 Profil: %s" },
    [S_AUTO_PROFILES]  = { "🔄 Switch Automatically for Games", "🔄 Oyunlara görə avtomatik dəyiş" },
    [S_MANAGE_PROFILES]= { "✏️ Manage Profiles…", "✏️ Profilləri idarə et…" },
    [S_CONTROLLER_COUNT]={ "🎮 Controller Count (%d)", "🎮 Kontroller sayı (%d)" },
    [S_ONE_CONTROLLER] = { "1 Controller", "1 kontroller" },
    [S_N_CONTROLLERS]  = { "%d Controllers", "%d kontroller" },
    [S_OUTPUT_MENU]    = { "🕹️ Virtual Controller: %s", "🕹️ Virtual kontroller: %s" },
    [S_DEADZONE_MENU]  = { "🎯 Stick Deadzone", "🎯 Çubuq ölü zonası" },
    [S_LEFT_STICK]     = { "Left Stick", "Sol çubuq" },
    [S_RIGHT_STICK]    = { "Right Stick", "Sağ çubuq" },
    [S_RUMBLE_MENU]    = { "📳 Rumble Strength", "📳 Vibrasiya gücü" },
    [S_OFF]            = { "Off", "Söndürülüb" },
    [S_DESKTOP_MODE]   = { "🖱️ Desktop Mode (PS + Options)", "🖱️ Masaüstü rejimi (PS + Options)" },
    [S_TOUCHPAD]       = { "🖐️ Touchpad (Mouse) Enabled", "🖐️ Toxunma paneli (siçan) aktiv" },
    [S_SETTINGS]       = { "⚙️ Settings…", "⚙️ Parametrlər…" },
    [S_TEST]           = { "🧪 Controller Test…", "🧪 Kontroller testi…" },
    [S_LOGS_ENABLE]    = { "📝 Enable Logs (Write logs)", "📝 Jurnalı aktiv et (log yaz)" },
    [S_LOGS_VIEW]      = { "📄 View Logs", "📄 Jurnala bax" },
    [S_LANGUAGE]       = { "🌐 Language", "🌐 Dil" },
    [S_FIX_PERMS]      = { "🔧 Fix /dev/uinput Permissions…", "🔧 /dev/uinput icazələrini düzəlt…" },
    [S_AUTOSTART]      = { "🚀 Start on Login", "🚀 Sistemə girişdə başlat" },
    [S_RESET]          = { "♻️ Reset Settings", "♻️ Parametrləri sıfırla" },
    [S_QUIT]           = { "❌ Quit", "❌ Çıxış" },

    [S_N_CONNECTED]    = { "Controller connected 🎮", "Kontroller qoşuldu 🎮" },
    [S_N_DISCONNECTED] = { "Controller disconnected ❌", "Kontroller ayrıldı ❌" },
    [S_N_LOW_BATTERY_TITLE] = { "🎮 DS4 Low Battery!", "🎮 DS4 batareyası azdır!" },
    [S_N_LOW_BATTERY_BODY]  = { "P%d battery is very low (%d%%). Please charge it.",
                                "P%d batareyası çox azdır (%d%%). Zəhmət olmasa, şarj edin." },
    [S_N_DESKTOP_ON]   = { "Desktop mode on 🖱️", "Masaüstü rejimi aktivdir 🖱️" },
    [S_N_DESKTOP_ON_BODY] = { "The left stick moves the pointer. Press PS + Options to go back to gaming.",
                              "Sol çubuq kursoru hərəkət etdirir. Oyuna qayıtmaq üçün PS + Options basın." },
    [S_N_DESKTOP_OFF]  = { "Desktop mode off 🎮", "Masaüstü rejimi söndürüldü 🎮" },
    [S_N_DESKTOP_OFF_BODY] = { "Controller input goes to games again.",
                               "Kontroller yenidən oyunlara ötürülür." },
    [S_N_PROFILE]      = { "Profile: %s", "Profil: %s" },
    [S_N_PROFILE_AUTO] = { "Activated because %s is running.", "%s işlədiyi üçün aktiv edildi." },
    [S_N_PROFILE_BACK] = { "The game was closed, the previous profile is active again.",
                           "Oyun bağlandı, əvvəlki profil yenidən aktivdir." },
    [S_N_AUTOSTART_ON] = { "DS4 Bridge will start automatically on login.",
                           "DS4 Bridge sistemə girişdə avtomatik başlayacaq." },
    [S_N_AUTOSTART_OFF]= { "DS4 Bridge will no longer start on login.",
                           "DS4 Bridge artıq sistemə girişdə başlamayacaq." },
    [S_N_RESET]        = { "Settings have been reset to defaults.", "Parametrlər standart vəziyyətə qaytarıldı." },
    [S_N_ALREADY]      = { "DS4 Bridge is already running.", "DS4 Bridge artıq işləyir." },
    [S_N_PERMS_OK]     = { "Permissions fixed. Restarting the bridge…", "İcazələr düzəldildi. Körpü yenidən başladılır…" },
    [S_N_PERMS_FAIL]   = { "Could not install the udev rule. See the README for the manual steps.",
                           "udev qaydası quraşdırıla bilmədi. Əl ilə addımlar üçün README-yə baxın." },
    [S_N_PERMS_RELOGIN]= { "The udev rule is installed, but you need to log out and back in for it to take effect.",
                           "udev qaydası quraşdırılıb, amma qüvvəyə minməsi üçün sistemdən çıxıb yenidən daxil olmalısınız." },

    [S_N_NO_TRAY]      = { "No system tray was found, so the DS4 Bridge icon can't be shown. Install "
                           "libayatana-appindicator (on GNOME also the \"AppIndicator\" extension).",
                           "Sistem treyi (bildiriş sahəsi) tapılmadı, ona görə DS4 Bridge ikonu göstərilə bilmir. "
                           "libayatana-appindicator quraşdırın (GNOME-da həm də \"AppIndicator\" əlavəsini)." },
    [S_ASK_RESET_TITLE]= { "Reset settings?", "Parametrlər sıfırlansın?" },
    [S_ASK_RESET_TEXT] = { "This deletes the settings file and all profiles, and restores the default button "
                           "mapping, controller count, touchpad and log options.",
                           "Bu, parametrlər faylını və bütün profilləri silir, standart düymə təyinatını, "
                           "kontroller sayını, toxunma paneli və jurnal seçimlərini bərpa edir." },
    [S_ASK_LOGDEL_TITLE]={ "Logging disabled", "Jurnal söndürüldü" },
    [S_ASK_LOGDEL_TEXT]= { "Do you want to delete the old log file?\n%s", "Köhnə jurnal faylı silinsin?\n%s" },
    [S_ASK_PERMS_TITLE]= { "No access to /dev/uinput", "/dev/uinput-a giriş yoxdur" },
    [S_ASK_PERMS_TEXT] = { "DS4 Bridge needs /dev/uinput to create the virtual controller.\n\n"
                           "Install a udev rule that gives your user access now? "
                           "You will be asked for the administrator password.",
                           "DS4 Bridge virtual kontroller yaratmaq üçün /dev/uinput-a ehtiyac duyur.\n\n"
                           "İstifadəçinizə giriş verən udev qaydası indi quraşdırılsın? "
                           "Administrator şifrəsi soruşulacaq." },
    [S_ASK_DELETE_PROFILE] = { "Delete profile \"%s\"?", "\"%s\" profili silinsin?" },
    [S_OK]             = { "OK", "OK" },
    [S_CANCEL]         = { "Cancel", "Ləğv et" },
    [S_CLOSE]          = { "Close", "Bağla" },
    [S_PROFILE_NAME]   = { "Profile name:", "Profil adı:" },
    [S_NEW_PROFILE_TITLE]    = { "New Profile", "Yeni profil" },
    [S_RENAME_PROFILE_TITLE] = { "Rename Profile", "Profilin adını dəyiş" },
    [S_PROFILE_NAME_INVALID] = { "This name is empty or already in use.", "Bu ad boşdur və ya artıq istifadə olunur." },

    [S_SW_TITLE]       = { "DS4 Bridge Settings", "DS4 Bridge parametrləri" },
    [S_PROFILE]        = { "Profile:", "Profil:" },
    [S_NEW]            = { "New…", "Yeni…" },
    [S_RENAME]         = { "Rename…", "Adını dəyiş…" },
    [S_DELETE]         = { "Delete", "Sil" },
    [S_PROCESSES]      = { "Use this profile automatically while one of these programs runs "
                           "(comma separated executable names):",
                           "Bu proqramlardan biri işləyərkən bu profil avtomatik istifadə olunsun "
                           "(vergüllə ayrılmış fayl adları):" },
    [S_DEFAULT_PROFILE_NOTE] = { "The Default profile is used whenever no game rule matches. "
                                 "Create a new profile to set up per-game settings.",
                                 "Heç bir oyun qaydası uyğun gəlmədikdə Default profili istifadə olunur. "
                                 "Oyuna görə ayarlar üçün yeni profil yaradın." },
    [S_TAB_GENERAL]    = { "General", "Ümumi" },
    [S_TAB_BUTTONS]    = { "Buttons", "Düymələr" },
    [S_TAB_STICKS]     = { "Sticks & Rumble", "Çubuqlar və vibrasiya" },
    [S_TAB_GYRO]       = { "Gyro", "Giroskop" },
    [S_TAB_DESKTOP]    = { "Desktop Mode", "Masaüstü rejimi" },
    [S_LANGUAGE_LBL]   = { "Language:", "Dil:" },
    [S_OUTPUT_LBL]     = { "Virtual controller type:", "Virtual kontroller növü:" },
    [S_OUTPUT_HINT]    = { "Changing the type restarts the bridge. DualShock 4 shows PlayStation button "
                           "icons in games that support them.",
                           "Növü dəyişmək körpünü yenidən başladır. DualShock 4 seçimi dəstəkləyən oyunlarda "
                           "PlayStation düymə ikonlarını göstərir." },
    [S_COUNT_LBL]      = { "Number of controllers:", "Kontroller sayı:" },
    [S_TOUCHPAD_LBL]   = { "Touchpad works as a mouse", "Toxunma paneli siçan kimi işləsin" },
    [S_AUTO_LBL]       = { "Switch profiles automatically when a game starts",
                           "Oyun başlayanda profili avtomatik dəyiş" },
    [S_LOGS_LBL]       = { "Write a log file", "Jurnal faylı yaz" },
    [S_FIX_PERMS_BTN]  = { "Fix /dev/uinput permissions…", "/dev/uinput icazələrini düzəlt…" },
    [S_SETTINGS_FILE]  = { "Settings file: %s", "Parametrlər faylı: %s" },
    [S_LOG_FILE]       = { "Log file: %s (limited to 1 MiB, older lines move to .log.1)",
                           "Jurnal faylı: %s (1 MiB ilə məhdudlaşır, köhnə sətirlər .log.1-ə keçir)" },
    [S_BTN_HINT]       = { "Choose what each DS4 button does on the virtual controller.",
                           "Hər DS4 düyməsinin virtual kontrollerdə nə edəcəyini seçin." },
    [S_DISABLED]       = { "Disabled", "Söndürülüb" },
    [S_RESET_BUTTONS]  = { "Reset buttons to default", "Düymələri standart vəziyyətə qaytar" },
    [S_DZ_LEFT]        = { "Left stick deadzone (%):", "Sol çubuq ölü zonası (%):" },
    [S_DZ_RIGHT]       = { "Right stick deadzone (%):", "Sağ çubuq ölü zonası (%):" },
    [S_DZ_HINT]        = { "Raise the deadzone if a stick drifts while you don't touch it. "
                           "The Controller Test window shows the right value.",
                           "Çubuğa toxunmadığınız halda kursor sürüşürsə, ölü zonanı artırın. "
                           "Düzgün dəyəri Kontroller testi pəncərəsində görə bilərsiniz." },
    [S_INVERT_LY]      = { "Invert left stick Y axis", "Sol çubuğun Y oxunu tərsinə çevir" },
    [S_INVERT_RY]      = { "Invert right stick Y axis", "Sağ çubuğun Y oxunu tərsinə çevir" },
    [S_SWAP]           = { "Swap left and right sticks", "Sol və sağ çubuqların yerini dəyiş" },
    [S_DIGITAL_TRIG]   = { "Digital triggers (fully pressed or released)", "Rəqəmsal tətiklər (tam basılı və ya buraxılmış)" },
    [S_RUMBLE_LBL]     = { "Rumble strength (%):", "Vibrasiya gücü (%):" },
    [S_RUMBLE_HINT]    = { "0% turns rumble off completely.", "0% vibrasiyanı tam söndürür." },
    [S_GYRO_HINT]      = { "Aim by tilting the controller. This needs a controller with working motion "
                           "sensors; many clones don't have them.",
                           "Kontrolleri əyərək nişan alın. Bunun üçün işləyən hərəkət sensorları lazımdır; "
                           "bir çox klon kontrollerlərdə onlar yoxdur." },
    [S_GYRO_MODE]      = { "Gyro aiming:", "Giroskopla nişan:" },
    [S_GYRO_OFF]       = { "Off", "Söndürülüb" },
    [S_GYRO_ALWAYS]    = { "Always on", "Həmişə aktiv" },
    [S_GYRO_HOLD]      = { "While holding a button", "Düymə basılı olarkən" },
    [S_GYRO_BUTTON]    = { "Button:", "Düymə:" },
    [S_GYRO_OUTPUT]    = { "Gyro moves:", "Giroskop hərəkət etdirir:" },
    [S_GYRO_TO_STICK]  = { "Right stick", "Sağ çubuğu" },
    [S_GYRO_TO_MOUSE]  = { "Mouse pointer", "Siçan kursorunu" },
    [S_GYRO_SENS]      = { "Sensitivity (%):", "Həssaslıq (%):" },
    [S_GYRO_INV_X]     = { "Invert horizontal", "Üfüqi istiqaməti tərsinə çevir" },
    [S_GYRO_INV_Y]     = { "Invert vertical", "Şaquli istiqaməti tərsinə çevir" },
    [S_GYRO_CAL]       = { "Calibrate (put the controller down and keep it still)",
                           "Kalibrləmə (kontrolleri yerə qoyun və tərpətməyin)" },
    [S_GYRO_CALIBRATING] = { "Calibrating…", "Kalibrlənir…" },
    [S_GYRO_FOUND]     = { "Motion sensors found: %s", "Hərəkət sensorları tapıldı: %s" },
    [S_GYRO_NOT_FOUND] = { "No connected controller has motion sensors.",
                           "Qoşulmuş kontrollerlərin heç birində hərəkət sensoru yoxdur." },
    [S_DESKTOP_ENABLE] = { "Desktop mode enabled", "Masaüstü rejimi aktivdir" },
    [S_DESKTOP_SPEED]  = { "Pointer speed (%):", "Kursor sürəti (%):" },
    [S_DESKTOP_HELP]   = { "In desktop mode games get no input; the controller works as a mouse and keyboard:\n\n"
                           "Left stick — move the pointer\n"
                           "Right stick — scroll\n"
                           "✕ Cross — left click     ○ Circle — right click\n"
                           "□ Square — middle click    △ Triangle — Super (menu) key\n"
                           "Options — Enter     Share — Esc\n"
                           "D-pad — arrow keys\n"
                           "L1 / R1 — back / forward\n"
                           "L2 / R2 — volume down / up\n\n"
                           "PS + Options turns desktop mode on and off.",
                           "Masaüstü rejimində oyunlar giriş almır; kontroller siçan və klaviatura kimi işləyir:\n\n"
                           "Sol çubuq — kursoru hərəkət etdirir\n"
                           "Sağ çubuq — sürüşdürmə\n"
                           "✕ Cross — sol klik     ○ Circle — sağ klik\n"
                           "□ Square — orta klik    △ Triangle — Super (menyu) düyməsi\n"
                           "Options — Enter     Share — Esc\n"
                           "D-pad — ox düymələri\n"
                           "L1 / R1 — geri / irəli\n"
                           "L2 / R2 — səsi azalt / artır\n\n"
                           "PS + Options masaüstü rejimini açıb-bağlayır." },

    [S_TW_TITLE]       = { "Controller Test", "Kontroller testi" },
    [S_TW_CONTROLLER]  = { "Controller:", "Kontroller:" },
    [S_TW_STOPPED]     = { "The bridge is stopped. Start it to test the controller.",
                           "Körpü dayandırılıb. Kontrolleri yoxlamaq üçün onu başladın." },
    [S_TW_INACTIVE]    = { "P%d is not in use. Increase the controller count to use it.",
                           "P%d istifadə olunmur. Onu istifadə etmək üçün kontroller sayını artırın." },
    [S_TW_DESKTOP]     = { "  (desktop mode: games get no input)", "  (masaüstü rejimi: oyunlar giriş almır)" },
    [S_TW_LEGEND]      = { "Hollow dot: raw stick position • Filled dot: what games receive • "
                           "Orange area: deadzone • Green: pressed",
                           "Boş nöqtə: çubuğun xam mövqeyi • Dolu nöqtə: oyunların aldığı dəyər • "
                           "Narıncı sahə: ölü zona • Yaşıl: basılıb" },
    [S_TW_GYRO]        = { "Gyro (°/s)", "Giroskop (°/s)" },

    [S_LV_TITLE]       = { "DS4 Bridge Logs", "DS4 Bridge jurnalı" },
    [S_LV_EMPTY]       = { "Logs are empty or disabled.", "Jurnal boşdur və ya söndürülüb." },
    [S_LV_CLEAR]       = { "Clear", "Təmizlə" },
    [S_LV_OPEN_FOLDER] = { "Open Folder", "Qovluğu aç" },
};

static const char *T(int id)
{
    const char *s = STRINGS[id][g_lang];
    return s ? s : STRINGS[id][LANG_EN];
}

/* ------------------------------------------------------------------------- */
/*  Control model: DS4 sources -> virtual targets                            */
/* ------------------------------------------------------------------------- */

enum src_id {
    SRC_CROSS, SRC_CIRCLE, SRC_SQUARE, SRC_TRIANGLE,
    SRC_L1, SRC_R1, SRC_L2, SRC_R2,
    SRC_SHARE, SRC_OPTIONS, SRC_PS, SRC_L3, SRC_R3,
    SRC_DUP, SRC_DDOWN, SRC_DLEFT, SRC_DRIGHT,
    N_SRC
};

static const char *const SRC_KEYS[N_SRC] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
    "share", "options", "ps", "l3", "r3",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

static const char *const SRC_LABELS[N_SRC] = {
    "✕ Cross", "○ Circle", "□ Square", "△ Triangle", "L1", "R1", "L2", "R2",
    "Share", "Options", "PS", "L3", "R3",
    "D-pad ↑", "D-pad ↓", "D-pad ←", "D-pad →",
};

/* Short names for the controller test window */
static const char *const SRC_SHORT[N_SRC] = {
    "✕", "○", "□", "△", "L1", "R1", "L2", "R2",
    "Share", "Opt", "PS", "L3", "R3", "↑", "↓", "←", "→",
};

/* Default mapping is the identity: source i -> target i + 1 */
enum tgt_id {
    TGT_NONE,
    TGT_A, TGT_B, TGT_X, TGT_Y,
    TGT_LB, TGT_RB, TGT_LT, TGT_RT,
    TGT_BACK, TGT_START, TGT_GUIDE, TGT_LS, TGT_RS,
    TGT_DUP, TGT_DDOWN, TGT_DLEFT, TGT_DRIGHT,
    N_TGT
};

static const char *const TGT_KEYS[N_TGT] = {
    "none", "A", "B", "X", "Y", "LB", "RB", "LT", "RT",
    "Back", "Start", "Guide", "LS", "RS", "DUp", "DDown", "DLeft", "DRight",
};

static const char *const TGT_LABELS[N_TGT] = {
    NULL /* "Disabled", translated */, "A", "B", "X", "Y", "LB", "RB", "LT", "RT",
    "Back", "Start", "Guide", "LS (L3)", "RS (R3)",
    "D-pad ↑", "D-pad ↓", "D-pad ←", "D-pad →",
};

/* Physical DS4 buttons (hid-sony / hid-playstation codes) */
static const struct {
    unsigned short code;
    unsigned char  src;
} PHYS_KEYS[] = {
    { BTN_SOUTH,      SRC_CROSS    }, { BTN_EAST,       SRC_CIRCLE   },
    { BTN_WEST,       SRC_SQUARE   }, { BTN_NORTH,      SRC_TRIANGLE },
    { BTN_TL,         SRC_L1       }, { BTN_TR,         SRC_R1       },
    { BTN_TL2,        SRC_L2       }, { BTN_TR2,        SRC_R2       },
    { BTN_SELECT,     SRC_SHARE    }, { BTN_START,      SRC_OPTIONS  },
    { BTN_MODE,       SRC_PS       }, { BTN_THUMBL,     SRC_L3       },
    { BTN_THUMBR,     SRC_R3       },
    { BTN_DPAD_UP,    SRC_DUP      }, { BTN_DPAD_DOWN,  SRC_DDOWN    },
    { BTN_DPAD_LEFT,  SRC_DLEFT    }, { BTN_DPAD_RIGHT, SRC_DRIGHT   },
};
#define N_PKEYS ARRAY_LEN(PHYS_KEYS)

enum phys_abs { PA_LX, PA_LY, PA_RX, PA_RY, PA_L2, PA_R2, PA_HX, PA_HY, N_PABS };
static const unsigned short PHYS_ABS_CODES[N_PABS] = {
    ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y,
};

/* Virtual pad outputs */
enum out_key {
    OK_A, OK_B, OK_X, OK_Y, OK_LB, OK_RB,
    OK_BACK, OK_START, OK_GUIDE, OK_LS, OK_RS,
    OK_TL2, OK_TR2,          /* digital trigger bits, DualShock 4 output only */
    N_OK
};

enum out_abs { OA_LX, OA_LY, OA_RX, OA_RY, OA_LT, OA_RT, OA_HX, OA_HY, N_OA };
static const unsigned short OUT_ABS_CODES[N_OA] = {
    ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y,
};

enum out_type { OUT_XBOX360, OUT_XBOXONE, OUT_DS4, N_OUT };

struct out_dev {
    const char    *key;      /* value stored in settings.ini */
    const char    *label;
    const char    *name;
    unsigned short vendor, product, version;
    unsigned short keys[N_OK];   /* 0 = not exposed */
};

/* The key sets match the real drivers (xpad / hid-playstation) exactly, so
 * SDL derives the same button indices and its mapping database applies. */
static const struct out_dev OUT_DEVS[N_OUT] = {
    { "xbox360", "Xbox 360", "Microsoft X-Box 360 pad", 0x045e, 0x028e, 0x0110,
      { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
        BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, 0, 0 } },
    { "xboxone", "Xbox One S", "Microsoft X-Box One S pad", 0x045e, 0x02ea, 0x0301,
      { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
        BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, 0, 0 } },
    { "ds4", "DualShock 4", "Sony Interactive Entertainment Wireless Controller", 0x054c, 0x09cc, 0x8111,
      { BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR,
        BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, BTN_TL2, BTN_TR2 } },
};

static int tgt_out_key(int t)
{
    switch (t) {
    case TGT_A:     return OK_A;
    case TGT_B:     return OK_B;
    case TGT_X:     return OK_X;
    case TGT_Y:     return OK_Y;
    case TGT_LB:    return OK_LB;
    case TGT_RB:    return OK_RB;
    case TGT_BACK:  return OK_BACK;
    case TGT_START: return OK_START;
    case TGT_GUIDE: return OK_GUIDE;
    case TGT_LS:    return OK_LS;
    case TGT_RS:    return OK_RS;
    default:        return -1;
    }
}

/* Desktop mode: the controller drives a virtual mouse + keyboard */
static const struct {
    unsigned char  src;
    unsigned short code;
} DESKTOP_MAP[] = {
    { SRC_CROSS,    BTN_LEFT       }, { SRC_CIRCLE,  BTN_RIGHT     },
    { SRC_SQUARE,   BTN_MIDDLE     }, { SRC_TRIANGLE, KEY_LEFTMETA },
    { SRC_OPTIONS,  KEY_ENTER      }, { SRC_SHARE,   KEY_ESC       },
    { SRC_DUP,      KEY_UP         }, { SRC_DDOWN,   KEY_DOWN      },
    { SRC_DLEFT,    KEY_LEFT       }, { SRC_DRIGHT,  KEY_RIGHT     },
    { SRC_L1,       KEY_BACK       }, { SRC_R1,      KEY_FORWARD   },
    { SRC_L2,       KEY_VOLUMEDOWN }, { SRC_R2,      KEY_VOLUMEUP  },
};
#define N_DESKTOP ARRAY_LEN(DESKTOP_MAP)

/* ------------------------------------------------------------------------- */
/*  Settings model                                                           */
/* ------------------------------------------------------------------------- */

enum gyro_mode { GYRO_OFF, GYRO_ALWAYS, GYRO_HOLD, N_GYRO_MODES };
enum gyro_out  { GYRO_TO_RSTICK, GYRO_TO_MOUSE, N_GYRO_OUTS };

static const char *const GYRO_MODE_KEYS[N_GYRO_MODES] = { "off", "always", "hold" };
static const char *const GYRO_OUT_KEYS[N_GYRO_OUTS]   = { "stick", "mouse" };

/* Everything that belongs to a profile */
struct profile_cfg {
    unsigned char remap[N_SRC];     /* enum tgt_id per source                */
    int dz_left, dz_right;          /* radial stick deadzone, 0..50 %        */
    int invert_ly, invert_ry;
    int swap_sticks;
    int digital_triggers;
    int rumble_pct;                 /* 0..100                                */
    int gyro_mode;                  /* enum gyro_mode                        */
    int gyro_button;                /* enum src_id held for GYRO_HOLD        */
    int gyro_output;                /* enum gyro_out                         */
    int gyro_sens;                  /* 10..500 %                             */
    int gyro_invert_x, gyro_invert_y;
};

struct profile {
    char name[64];
    char processes[256];            /* comma separated executable names      */
    struct profile_cfg c;
};

/* Snapshot the bridge threads work from; published by the GTK thread */
struct runtime_cfg {
    struct profile_cfg p;
    int desktop_mode;
    int desktop_speed;              /* pointer speed, 10..300 %              */
};

/* Per-controller state shared with the UI (guarded by status_mutex) */
struct slot_status {
    int   active;                   /* thread owns a virtual pad             */
    int   error;                    /* errno of a failed /dev/uinput setup   */
    int   connected;
    int   has_motion;
    int   gyro_calibrating;
    char  name[128];
    char  event[32];                /* "eventN" of the physical gamepad node */
    /* live values for the controller test window */
    int   src[N_SRC];               /* 0..255                                */
    float raw[4];                   /* LX LY RX RY as read, -1..1            */
    float out[4];                   /* what games receive, -1..1             */
    int   out_trig[2];
    float gyro[3];                  /* deg/s, bias removed                   */
};

struct thread_arg {
    int id;
    int output_type;
    int disable_touchpad;
    volatile atomic_int *stop_flag;
};

/* ------------------------------------------------------------------------- */
/*  Globals                                                                  */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t scan_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cfg_mutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;

static char log_file_path[PATH_MAX] = "";
static char settings_file_path[PATH_MAX] = "";
static int g_verbose = 0;

/* Settings, owned by the GTK thread */
static int g_logs_enabled = 1;
static int g_disable_touchpad = 0;
static int num_controllers = 1;
static int g_output_type = OUT_XBOX360;
static int g_desktop_mode = 0;
static int g_desktop_speed = 100;
static int g_auto_profiles = 1;
static struct profile g_profiles[MAX_PROFILES];
static int g_n_profiles = 0;
static int g_active_profile = 0;   /* profile in use                         */
static int g_manual_profile = 0;   /* profile picked by the user             */
static int g_auto_profile = -1;    /* profile picked by the game watcher     */

/* Set while widgets are being synced from in-memory settings, so their
 * handlers don't re-apply / re-save / prompt the user. */
static int g_applying_settings = 0;

static struct runtime_cfg g_rt;    /* guarded by cfg_mutex */
static atomic_int g_rt_gen = 1;

static struct slot_status g_slots[MAX_PADS];
static atomic_int g_gyro_cal_req[MAX_PADS];

static atomic_int g_bridge_running = 0;
static atomic_int g_bridge_stop_requested = 0;
static pthread_t bridge_threads[MAX_PADS];
static struct thread_arg thread_args[MAX_PADS];
static int g_threads_started = 0;

/* Implemented in the UI part; bridge threads hand work over via g_idle_add */
static gboolean thread_note_idle(gpointer data);
static gboolean desktop_toggle_idle(gpointer data);

/* ------------------------------------------------------------------------- */
/*  Logging                                                                  */
/* ------------------------------------------------------------------------- */

static void logmsg(int prio, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logmsg(int prio, const char *fmt, ...)
{
    if (prio == LOG_DEBUG && !g_verbose)
        return;

    va_list ap;
    char ts[32] = "";
    time_t t = time(NULL);
    struct tm tm;
    if (localtime_r(&t, &tm))
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    const char *tag;
    switch (prio) {
    case LOG_ERR:     tag = "ERROR"; break;
    case LOG_WARNING: tag = "WARN "; break;
    case LOG_DEBUG:   tag = "DEBUG"; break;
    default:          tag = "INFO "; break;
    }

    pthread_mutex_lock(&log_mutex);
    if (g_logs_enabled && log_file_path[0] != '\0') {
        FILE *f = fopen(log_file_path, "a");
        if (f) {
            va_start(ap, fmt);
            fprintf(f, "[%s] %s ", ts, tag);
            vfprintf(f, fmt, ap);
            fputc('\n', f);
            va_end(ap);
            long size = ftell(f);
            fclose(f);

            /* Keep the log bounded: current file + one rotated backup */
            if (size > LOG_MAX_BYTES) {
                char old[PATH_MAX + 4];
                snprintf(old, sizeof old, "%s.1", log_file_path);
                (void)rename(log_file_path, old);
            }
        }
    }

    // Also print to stderr for immediate feedback in terminal if launched manually
    va_start(ap, fmt);
    fprintf(stderr, "[%s] %s ", ts, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);

    pthread_mutex_unlock(&log_mutex);
}

/* ------------------------------------------------------------------------- */
/*  Helpers                                                                  */
/* ------------------------------------------------------------------------- */

/* argv-based, so device names with quotes can't break the command line */
static void send_notification(const char *title, const char *msg, const char *icon)
{
    const gchar *argv[] = { "notify-send", "-a", "DS4 Bridge", "-i", icon, title, msg, NULL };
    GError *err = NULL;
    if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL, NULL, NULL, &err)) {
        logmsg(LOG_DEBUG, "notify-send failed: %s", err->message);
        g_error_free(err);
    }
}

enum note_kind { NOTE_CONNECTED, NOTE_DISCONNECTED };

struct thread_note {
    int  kind;
    int  slot;
    char name[128];
};

static void notify_from_thread(int kind, int slot, const char *name)
{
    struct thread_note *n = g_new0(struct thread_note, 1);
    n->kind = kind;
    n->slot = slot;
    g_strlcpy(n->name, name, sizeof n->name);
    g_idle_add(thread_note_idle, n);
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static long long now_ms(void)
{
    return now_us() / 1000LL;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            if (tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

static int scale_axis(int v, const struct input_absinfo *s, int dmin, int dmax)
{
    long long range = (long long)s->maximum - s->minimum;
    long long num;

    if (range <= 0) return v;
    if (v < s->minimum) v = s->minimum;
    if (v > s->maximum) v = s->maximum;

    num = ((long long)v - s->minimum) * ((long long)dmax - dmin);
    return (int)(dmin + (num + range / 2) / range);
}

/* ------------------------------------------------------------------------- */
/*  Bridge state                                                             */
/* ------------------------------------------------------------------------- */

struct evbuf {
    struct input_event ev[OUT_MAX];
    int n;
};

struct bridge {
    int  slot;
    int  ufd;                       /* virtual pad                           */
    int  dfd;                       /* physical DS4 gamepad node             */
    int  tfd;                       /* grabbed touchpad node                 */
    int  mfd;                       /* motion sensors node                   */
    int  mousefd;                   /* virtual mouse/keyboard (lazy)         */
    int  disable_touchpad;
    int  ds4_writable;
    int  ds4_has_rumble;
    int  ds4_has_gain;
    int  grabbed;
    int  syn_dropped;
    int  motion_syn_dropped;
    char dev_path[300];
    char dev_name[256];
    char dev_event[32];
    const struct out_dev *odev;

    struct runtime_cfg cfg;
    int  cfg_gen;

    /* physical DS4 state */
    struct input_absinfo src_abs[N_PABS];
    int  has_abs[N_PABS];
    int  pabs[N_PABS];
    int  pkey[N_PKEYS];
    unsigned int combo_mask;        /* sources swallowed by PS + Options     */

    /* motion sensors */
    struct input_absinfo gyro_abs[3];
    int   gyro_raw[3];
    float gyro_bias[3];
    float gyro_dps[3];
    int   gyro_active;
    long long gyro_last_us;
    float gyro_acc[2];
    long long cal_start;
    long  cal_n;
    double cal_sum[3];

    /* desktop mode */
    float desk_raw[4];
    float desk_acc[4];              /* x, y, wheel, hwheel remainders        */
    long long last_tick_us;

    /* last values written to the virtual devices */
    int  last_key[N_OK];
    int  last_abs[N_OA];
    int  last_dkey[N_DESKTOP];

    struct ff_effect ff_cache[BRIDGE_FF_SLOTS];
    int  ff_cached[BRIDGE_FF_SLOTS];
    int  ff_phys[BRIDGE_FF_SLOTS];
    int  ff_playing[BRIDGE_FF_SLOTS];

    struct evbuf pad_out;
    struct evbuf mouse_out;
};

/* ------------------------------------------------------------------------- */
/*  Write events to virtual devices                                          */
/* ------------------------------------------------------------------------- */

static void evbuf_flush(struct evbuf *o, int fd)
{
    const char *p = (const char *)o->ev;
    size_t left = (size_t)o->n * sizeof(struct input_event);

    while (fd >= 0 && left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            logmsg(LOG_WARNING, "uinput write error: %s", strerror(errno));
            break;
        }
        p    += w;
        left -= (size_t)w;
    }
    o->n = 0;
}

static void evbuf_push(struct evbuf *o, int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event *ev;
    if (o->n >= OUT_MAX) evbuf_flush(o, fd);
    ev = &o->ev[o->n++];
    memset(ev, 0, sizeof *ev);
    ev->type  = type;
    ev->code  = code;
    ev->value = value;
}

static void evbuf_syn(struct evbuf *o, int fd)
{
    evbuf_push(o, fd, EV_SYN, SYN_REPORT, 0);
    evbuf_flush(o, fd);
}

/* ------------------------------------------------------------------------- */
/*  Virtual device creation (/dev/uinput)                                    */
/* ------------------------------------------------------------------------- */

static struct input_absinfo virt_absinfo(int oa)
{
    struct input_absinfo a;
    memset(&a, 0, sizeof a);
    switch (oa) {
    case OA_LX: case OA_LY: case OA_RX: case OA_RY:
        a.minimum = STICK_MIN; a.maximum = STICK_MAX;
        a.fuzz = STICK_FUZZ;   a.flat = STICK_FLAT;
        break;
    case OA_LT: case OA_RT:
        a.minimum = TRIG_MIN;  a.maximum = TRIG_MAX;
        break;
    default:
        a.minimum = HAT_MIN;   a.maximum = HAT_MAX;
        break;
    }
    return a;
}

static int open_uinput(int *err)
{
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT)
        fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *err = errno;
        logmsg(LOG_ERR, "Failed to open /dev/uinput: %s", strerror(errno));
        return -1;
    }
    return fd;
}

static int create_virtual_pad(const struct out_dev *od, int slot, int *err)
{
    struct uinput_setup us;
    char phys[64];
    int fd, version = 0;
    size_t i;

    *err = 0;
    fd = open_uinput(err);
    if (fd < 0) return -1;

    if (ioctl(fd, UI_GET_VERSION, &version) == 0 && version < 5) {
        logmsg(LOG_ERR, "uinput version %d is too old (needs >= 5)", version);
        close(fd);
        *err = ENOTSUP;
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_FF)  < 0) {
        logmsg(LOG_ERR, "UI_SET_EVBIT error: %s", strerror(errno));
        goto fail;
    }

    for (i = 0; i < N_OK; i++) {
        if (od->keys[i] && ioctl(fd, UI_SET_KEYBIT, od->keys[i]) < 0) {
            logmsg(LOG_ERR, "UI_SET_KEYBIT(0x%x) error: %s", od->keys[i], strerror(errno));
            goto fail;
        }
    }

    for (i = 0; i < N_OA; i++) {
        struct uinput_abs_setup as;
        memset(&as, 0, sizeof as);
        as.code    = OUT_ABS_CODES[i];
        as.absinfo = virt_absinfo((int)i);

        if (ioctl(fd, UI_SET_ABSBIT, OUT_ABS_CODES[i]) < 0 ||
            ioctl(fd, UI_ABS_SETUP, &as) < 0) {
            logmsg(LOG_ERR, "UI_ABS_SETUP(0x%x) error: %s", OUT_ABS_CODES[i], strerror(errno));
            goto fail;
        }
    }

    if (ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) < 0 ||
        ioctl(fd, UI_SET_FFBIT, FF_GAIN)   < 0) {
        logmsg(LOG_ERR, "UI_SET_FFBIT error: %s", strerror(errno));
        goto fail;
    }

    /* The "ds4-bridge/" prefix also keeps us from bridging our own devices */
    snprintf(phys, sizeof phys, "ds4-bridge/input%d", slot);
    (void)ioctl(fd, UI_SET_PHYS, phys);

    memset(&us, 0, sizeof us);
    us.id.bustype   = BUS_USB;
    us.id.vendor    = od->vendor;
    us.id.product   = od->product;
    us.id.version   = od->version;
    us.ff_effects_max = BRIDGE_FF_SLOTS;
    snprintf(us.name, sizeof us.name, "%s", od->name);

    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        logmsg(LOG_ERR, "UI_DEV_SETUP error: %s", strerror(errno));
        goto fail;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg(LOG_ERR, "UI_DEV_CREATE error: %s", strerror(errno));
        goto fail;
    }

    return fd;

fail:
    *err = errno ? errno : EIO;
    close(fd);
    return -1;
}

static int create_virtual_mouse(int slot)
{
    struct uinput_setup us;
    char phys[64];
    int fd, err = 0;
    size_t i;

    fd = open_uinput(&err);
    if (fd < 0) return -1;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) < 0)
        goto fail;
    for (i = 0; i < N_DESKTOP; i++)
        if (ioctl(fd, UI_SET_KEYBIT, DESKTOP_MAP[i].code) < 0) goto fail;
    if (ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_Y) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL) < 0)
        goto fail;

    snprintf(phys, sizeof phys, "ds4-bridge/mouse%d", slot);
    (void)ioctl(fd, UI_SET_PHYS, phys);

    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    snprintf(us.name, sizeof us.name, "DS4 Bridge Mouse (P%d)", slot + 1);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
        goto fail;

    logmsg(LOG_INFO, "P%d: virtual mouse created", slot + 1);
    return fd;

fail:
    logmsg(LOG_ERR, "P%d: could not create virtual mouse: %s", slot + 1, strerror(errno));
    close(fd);
    return -1;
}

static int need_mouse(const struct bridge *b)
{
    return b->cfg.desktop_mode ||
           (b->cfg.p.gyro_mode != GYRO_OFF && b->cfg.p.gyro_output == GYRO_TO_MOUSE);
}

static void ensure_mouse(struct bridge *b)
{
    if (b->mousefd >= 0) return;
    b->mousefd = create_virtual_mouse(b->slot);
    memset(b->last_dkey, 0, sizeof b->last_dkey);
}

/* ------------------------------------------------------------------------- */
/*  Force feedback handling                                                  */
/* ------------------------------------------------------------------------- */

static void ff_reset_phys(struct bridge *b)
{
    int i;
    for (i = 0; i < BRIDGE_FF_SLOTS; i++)
        b->ff_phys[i] = -1;
}

static void ff_upload_to_ds4(struct bridge *b, int vid)
{
    struct ff_effect e;
    if (b->dfd < 0 || !b->ds4_has_rumble || !b->ff_cached[vid])
        return;

    e = b->ff_cache[vid];
    e.id = (short)b->ff_phys[vid];
    e.trigger.button   = 0;
    e.trigger.interval = 0;

    /* Rumble strength setting */
    if (e.type == FF_RUMBLE) {
        unsigned pct = (unsigned)clampi(b->cfg.p.rumble_pct, 0, 100);
        e.u.rumble.strong_magnitude = (unsigned short)(e.u.rumble.strong_magnitude * pct / 100U);
        e.u.rumble.weak_magnitude   = (unsigned short)(e.u.rumble.weak_magnitude * pct / 100U);
    }

    if (ioctl(b->dfd, EVIOCSFF, &e) < 0) {
        logmsg(LOG_WARNING, "EVIOCSFF (DS4) error: %s", strerror(errno));
        return;
    }
    b->ff_phys[vid] = e.id;
}

static void ds4_write_ff(struct bridge *b, unsigned short code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type  = EV_FF;
    ev.code  = code;
    ev.value = value;

    for (;;) {
        if (write(b->dfd, &ev, sizeof ev) == (ssize_t)sizeof ev)
            return;
        if (errno != EINTR) break;
    }
}

static void handle_ff_upload(struct bridge *b, unsigned int request_id)
{
    struct uinput_ff_upload up;
    int vid;

    memset(&up, 0, sizeof up);
    up.request_id = request_id;

    if (ioctl(b->ufd, UI_BEGIN_FF_UPLOAD, &up) < 0) return;

    vid = up.effect.id;
    if (vid < 0 || vid >= BRIDGE_FF_SLOTS || up.effect.type != FF_RUMBLE) {
        up.retval = -EINVAL;
    } else {
        b->ff_cache[vid]  = up.effect;
        b->ff_cached[vid] = 1;
        ff_upload_to_ds4(b, vid);
        up.retval = 0;
    }

    (void)ioctl(b->ufd, UI_END_FF_UPLOAD, &up);
}

static void handle_ff_erase(struct bridge *b, unsigned int request_id)
{
    struct uinput_ff_erase er;
    int vid;

    memset(&er, 0, sizeof er);
    er.request_id = request_id;

    if (ioctl(b->ufd, UI_BEGIN_FF_ERASE, &er) < 0) return;

    vid = (int)er.effect_id;
    if (vid >= 0 && vid < BRIDGE_FF_SLOTS) {
        if (b->dfd >= 0 && b->ff_phys[vid] >= 0) {
            (void)ioctl(b->dfd, EVIOCRMFF, b->ff_phys[vid]);
        }
        b->ff_phys[vid]    = -1;
        b->ff_cached[vid]  = 0;
        b->ff_playing[vid] = 0;
    }
    er.retval = 0;

    (void)ioctl(b->ufd, UI_END_FF_ERASE, &er);
}

static void handle_ff_event(struct bridge *b, unsigned short code, int value)
{
    if (code == FF_GAIN) {
        if (b->dfd >= 0 && b->ds4_has_gain)
            ds4_write_ff(b, FF_GAIN, value);
        return;
    }
    if (code >= BRIDGE_FF_SLOTS) return;
    b->ff_playing[code] = value;
    if (b->dfd >= 0 && b->ff_phys[code] >= 0) {
        ds4_write_ff(b, (unsigned short)b->ff_phys[code], value);
    }
}

static void ff_restore_on_connect(struct bridge *b)
{
    int i;
    ff_reset_phys(b);
    if (!b->ds4_has_rumble) return;
    for (i = 0; i < BRIDGE_FF_SLOTS; i++) {
        if (!b->ff_cached[i]) continue;
        ff_upload_to_ds4(b, i);
        if (b->ff_phys[i] >= 0 && b->ff_playing[i] > 0 && b->ff_cache[i].replay.length == 0)
            ds4_write_ff(b, (unsigned short)b->ff_phys[i], b->ff_playing[i]);
    }
}

static void ff_stop_all_on_ds4(struct bridge *b)
{
    int i;
    if (b->dfd < 0 || !b->ds4_has_rumble) return;
    for (i = 0; i < BRIDGE_FF_SLOTS; i++)
        if (b->ff_phys[i] >= 0)
            ds4_write_ff(b, (unsigned short)b->ff_phys[i], 0);
}

/* ------------------------------------------------------------------------- */
/*  Mapping engine                                                           */
/*                                                                           */
/*  The physical state is kept in b->pkey / b->pabs. On every report the    */
/*  whole output state is recomputed from it and only the differences are   */
/*  written, so remapping at runtime can never leave a button stuck.         */
/* ------------------------------------------------------------------------- */

static void read_sources(const struct bridge *b, int src[N_SRC])
{
    size_t k;

    memset(src, 0, sizeof(int) * N_SRC);
    if (b->dfd < 0) return;

    for (k = 0; k < N_PKEYS; k++)
        if (b->pkey[k]) src[PHYS_KEYS[k].src] = 255;

    /* Analog triggers win over their digital bits when present */
    if (b->has_abs[PA_L2]) src[SRC_L2] = scale_axis(b->pabs[PA_L2], &b->src_abs[PA_L2], 0, 255);
    if (b->has_abs[PA_R2]) src[SRC_R2] = scale_axis(b->pabs[PA_R2], &b->src_abs[PA_R2], 0, 255);

    if (b->has_abs[PA_HX]) {
        if (b->pabs[PA_HX] < 0) src[SRC_DLEFT]  = 255;
        if (b->pabs[PA_HX] > 0) src[SRC_DRIGHT] = 255;
    }
    if (b->has_abs[PA_HY]) {
        if (b->pabs[PA_HY] < 0) src[SRC_DUP]   = 255;
        if (b->pabs[PA_HY] > 0) src[SRC_DDOWN] = 255;
    }
}

static float stick_norm(const struct bridge *b, int pa)
{
    const struct input_absinfo *a = &b->src_abs[pa];
    if (b->dfd < 0 || !b->has_abs[pa] || a->maximum <= a->minimum) return 0.0f;
    float v = (float)(b->pabs[pa] - a->minimum) * 2.0f / (float)(a->maximum - a->minimum) - 1.0f;
    return clampf(v, -1.0f, 1.0f);
}

/* Radial deadzone, rescaled so movement starts smoothly from the edge */
static void apply_deadzone(float *x, float *y, int pct)
{
    float dz = (float)clampi(pct, 0, 90) / 100.0f;
    float mag = sqrtf(*x * *x + *y * *y);

    if (dz <= 0.0f) return;
    if (mag <= dz) { *x = *y = 0.0f; return; }

    float scaled = (fminf(mag, 1.0f) - dz) / (1.0f - dz);
    *x = clampf(*x / mag * scaled, -1.0f, 1.0f);
    *y = clampf(*y / mag * scaled, -1.0f, 1.0f);
}

static int stick_to_int(float v)
{
    v = clampf(v, -1.0f, 1.0f);
    return v >= 0.0f ? (int)(v * STICK_MAX + 0.5f) : (int)(v * -(float)STICK_MIN - 0.5f);
}

/* Horizontal / vertical aim rate in deg/s from the gyro */
static void gyro_aim(const struct bridge *b, float *h, float *v)
{
    *h = -b->gyro_dps[1];          /* yaw   */
    *v = -b->gyro_dps[0];          /* pitch */
    if (b->cfg.p.gyro_invert_x) *h = -*h;
    if (b->cfg.p.gyro_invert_y) *v = -*v;
}

static void emit_pad(struct bridge *b, const int key[N_OK], const int abs[N_OA])
{
    int any = 0;
    size_t i;

    for (i = 0; i < N_OK; i++) {
        if (!b->odev->keys[i] || key[i] == b->last_key[i]) continue;
        evbuf_push(&b->pad_out, b->ufd, EV_KEY, b->odev->keys[i], key[i]);
        b->last_key[i] = key[i];
        any = 1;
    }
    for (i = 0; i < N_OA; i++) {
        if (abs[i] == b->last_abs[i]) continue;
        evbuf_push(&b->pad_out, b->ufd, EV_ABS, OUT_ABS_CODES[i], abs[i]);
        b->last_abs[i] = abs[i];
        any = 1;
    }
    if (any) evbuf_syn(&b->pad_out, b->ufd);
}

/* Desktop mode keys; everything is released when desktop mode is off */
static void emit_desktop_keys(struct bridge *b, const int eff[N_SRC])
{
    int any = 0;
    size_t i;

    if (b->mousefd < 0) return;
    for (i = 0; i < N_DESKTOP; i++) {
        int v = b->cfg.desktop_mode && eff[DESKTOP_MAP[i].src] > TRIGGER_PRESS;
        if (v == b->last_dkey[i]) continue;
        evbuf_push(&b->mouse_out, b->mousefd, EV_KEY, DESKTOP_MAP[i].code, v);
        b->last_dkey[i] = v;
        any = 1;
    }
    if (any) evbuf_syn(&b->mouse_out, b->mousefd);
}

static void publish_live(struct bridge *b, const int src[N_SRC], const float raw[4],
                         const float out[4], const int abs[N_OA])
{
    struct slot_status *s = &g_slots[b->slot];
    pthread_mutex_lock(&status_mutex);
    memcpy(s->src, src, sizeof s->src);
    memcpy(s->raw, raw, sizeof s->raw);
    memcpy(s->out, out, sizeof s->out);
    s->out_trig[0] = abs[OA_LT];
    s->out_trig[1] = abs[OA_RT];
    pthread_mutex_unlock(&status_mutex);
}

static void process(struct bridge *b)
{
    const struct profile_cfg *p = &b->cfg.p;
    int src[N_SRC], eff[N_SRC];
    int key[N_OK] = {0}, abs[N_OA] = {0};
    int dpad[4] = {0};              /* up, down, left, right */
    float raw[4], st[4], out[4] = {0};
    int i;

    read_sources(b, src);

    /* PS + Options toggles desktop mode; both buttons are swallowed until released */
    if (src[SRC_PS] && src[SRC_OPTIONS] && !b->combo_mask) {
        b->combo_mask = (1u << SRC_PS) | (1u << SRC_OPTIONS);
        g_idle_add(desktop_toggle_idle, NULL);
    }
    if (!src[SRC_PS])      b->combo_mask &= ~(1u << SRC_PS);
    if (!src[SRC_OPTIONS]) b->combo_mask &= ~(1u << SRC_OPTIONS);

    for (i = 0; i < N_SRC; i++)
        eff[i] = (b->combo_mask & (1u << i)) ? 0 : src[i];

    raw[0] = stick_norm(b, PA_LX);
    raw[1] = stick_norm(b, PA_LY);
    raw[2] = stick_norm(b, PA_RX);
    raw[3] = stick_norm(b, PA_RY);
    memcpy(st, raw, sizeof st);
    apply_deadzone(&st[0], &st[1], p->dz_left);
    apply_deadzone(&st[2], &st[3], p->dz_right);

    b->gyro_active = b->mfd >= 0 &&
        (p->gyro_mode == GYRO_ALWAYS ||
         (p->gyro_mode == GYRO_HOLD && eff[clampi(p->gyro_button, 0, N_SRC - 1)] > TRIGGER_PRESS));

    if (b->cfg.desktop_mode) {
        /* Games see a neutral pad; desktop_tick() moves the pointer */
        memcpy(b->desk_raw, raw, sizeof raw);
    } else {
        if (p->swap_sticks) {
            float t;
            t = st[0]; st[0] = st[2]; st[2] = t;
            t = st[1]; st[1] = st[3]; st[3] = t;
        }
        if (p->invert_ly) st[1] = -st[1];
        if (p->invert_ry) st[3] = -st[3];

        if (b->gyro_active && p->gyro_output == GYRO_TO_RSTICK) {
            float h, v, k = (float)p->gyro_sens / 100.0f / GYRO_STICK_FULL_DPS;
            gyro_aim(b, &h, &v);
            st[2] = clampf(st[2] + h * k, -1.0f, 1.0f);
            st[3] = clampf(st[3] + v * k, -1.0f, 1.0f);
        }

        for (i = 0; i < N_SRC; i++) {
            int v = eff[i], t = p->remap[i], pressed = v > TRIGGER_PRESS, k;
            if (!v || t == TGT_NONE) continue;
            switch (t) {
            case TGT_LT:     if (v > abs[OA_LT]) abs[OA_LT] = v; break;
            case TGT_RT:     if (v > abs[OA_RT]) abs[OA_RT] = v; break;
            case TGT_DUP:    dpad[0] |= pressed; break;
            case TGT_DDOWN:  dpad[1] |= pressed; break;
            case TGT_DLEFT:  dpad[2] |= pressed; break;
            case TGT_DRIGHT: dpad[3] |= pressed; break;
            default:
                k = tgt_out_key(t);
                if (k >= 0 && pressed) key[k] = 1;
                break;
            }
        }

        if (p->digital_triggers) {
            abs[OA_LT] = abs[OA_LT] > TRIGGER_PRESS ? TRIG_MAX : 0;
            abs[OA_RT] = abs[OA_RT] > TRIGGER_PRESS ? TRIG_MAX : 0;
        }
        key[OK_TL2] = abs[OA_LT] > TRIGGER_PRESS;
        key[OK_TR2] = abs[OA_RT] > TRIGGER_PRESS;
        abs[OA_HX] = dpad[3] - dpad[2];
        abs[OA_HY] = dpad[1] - dpad[0];
        for (i = 0; i < 4; i++) {
            abs[OA_LX + i] = stick_to_int(st[i]);
            out[i] = st[i];
        }
    }

    emit_pad(b, key, abs);
    emit_desktop_keys(b, eff);
    publish_live(b, src, raw, out, abs);
}

static void emit_rel(struct bridge *b, float *acc, unsigned short code, int *any)
{
    int d = (int)*acc;          /* truncation keeps the remainder's sign */
    if (!d) return;
    *acc -= (float)d;
    evbuf_push(&b->mouse_out, b->mousefd, EV_REL, code, d);
    *any = 1;
}

static void desktop_tick(struct bridge *b)
{
    long long now = now_us();
    float dt = (float)(now - b->last_tick_us) / 1e6f;
    float x, y, rx, ry, mag, speed;
    int any = 0;

    b->last_tick_us = now;
    if (!b->cfg.desktop_mode || b->mousefd < 0 || b->dfd < 0 || dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;

    x  = b->desk_raw[0]; y  = b->desk_raw[1];
    rx = b->desk_raw[2]; ry = b->desk_raw[3];
    apply_deadzone(&x, &y, b->cfg.p.dz_left > DESKTOP_MIN_DZ ? b->cfg.p.dz_left : DESKTOP_MIN_DZ);
    apply_deadzone(&rx, &ry, b->cfg.p.dz_right > DESKTOP_MIN_DZ ? b->cfg.p.dz_right : DESKTOP_MIN_DZ);

    /* Quadratic curve: fine control near the center, fast at the edge */
    mag = sqrtf(x * x + y * y);
    speed = DESKTOP_PX_PER_S * (float)b->cfg.desktop_speed / 100.0f;
    b->desk_acc[0] += x * mag * speed * dt;
    b->desk_acc[1] += y * mag * speed * dt;
    b->desk_acc[2] += -ry * fabsf(ry) * DESKTOP_WHEEL_PER_S * dt;
    b->desk_acc[3] +=  rx * fabsf(rx) * DESKTOP_WHEEL_PER_S * dt;

    emit_rel(b, &b->desk_acc[0], REL_X, &any);
    emit_rel(b, &b->desk_acc[1], REL_Y, &any);
    emit_rel(b, &b->desk_acc[2], REL_WHEEL, &any);
    emit_rel(b, &b->desk_acc[3], REL_HWHEEL, &any);
    if (any) evbuf_syn(&b->mouse_out, b->mousefd);
}

static void gyro_mouse(struct bridge *b, float dt)
{
    float h, v, k = GYRO_MOUSE_PX_PER_DEG * (float)b->cfg.p.gyro_sens / 100.0f * dt;
    int any = 0;

    if (b->mousefd < 0 || dt <= 0.0f) return;
    gyro_aim(b, &h, &v);
    b->gyro_acc[0] += h * k;
    b->gyro_acc[1] += v * k;
    emit_rel(b, &b->gyro_acc[0], REL_X, &any);
    emit_rel(b, &b->gyro_acc[1], REL_Y, &any);
    if (any) evbuf_syn(&b->mouse_out, b->mousefd);
}

static void motion_update(struct bridge *b)
{
    long long now = now_us();
    float dt = b->gyro_last_us ? (float)(now - b->gyro_last_us) / 1e6f : 0.0f;
    float rate[3];
    int i;

    b->gyro_last_us = now;
    if (dt > 0.02f) dt = 0.02f;

    for (i = 0; i < 3; i++) {
        int res = b->gyro_abs[i].resolution > 0 ? b->gyro_abs[i].resolution : GYRO_RES_DEFAULT;
        rate[i] = (float)b->gyro_raw[i] / (float)res;
    }

    /* Calibration: average the resting rate for a second and use it as bias */
    if (atomic_load(&g_gyro_cal_req[b->slot])) {
        if (!b->cal_start) {
            b->cal_start = now;
            b->cal_n = 0;
            memset(b->cal_sum, 0, sizeof b->cal_sum);
        }
        for (i = 0; i < 3; i++) b->cal_sum[i] += rate[i];
        b->cal_n++;
        if (now - b->cal_start >= GYRO_CAL_US) {
            for (i = 0; i < 3; i++) b->gyro_bias[i] = (float)(b->cal_sum[i] / (double)b->cal_n);
            b->cal_start = 0;
            atomic_store(&g_gyro_cal_req[b->slot], 0);
            logmsg(LOG_INFO, "P%d: gyro calibrated, bias %.2f %.2f %.2f deg/s",
                   b->slot + 1, b->gyro_bias[0], b->gyro_bias[1], b->gyro_bias[2]);
        }
    }

    for (i = 0; i < 3; i++) {
        float r = rate[i] - b->gyro_bias[i];
        float a = fabsf(r);
        if (a < GYRO_NOISE_DPS) r *= a / GYRO_NOISE_DPS;   /* soft tightening */
        b->gyro_dps[i] = r;
    }

    pthread_mutex_lock(&status_mutex);
    memcpy(g_slots[b->slot].gyro, b->gyro_dps, sizeof b->gyro_dps);
    g_slots[b->slot].gyro_calibrating = atomic_load(&g_gyro_cal_req[b->slot]);
    pthread_mutex_unlock(&status_mutex);

    if (!b->gyro_active) return;
    if (b->cfg.p.gyro_output == GYRO_TO_MOUSE)
        gyro_mouse(b, dt);
    else if (!b->cfg.desktop_mode)
        process(b);
}

/* Pick up settings published by the GTK thread */
static void refresh_cfg(struct bridge *b)
{
    int gen = atomic_load(&g_rt_gen), i;
    int old_rumble = b->cfg.p.rumble_pct, old_desktop = b->cfg.desktop_mode;

    if (gen == b->cfg_gen) return;
    pthread_mutex_lock(&cfg_mutex);
    b->cfg = g_rt;
    pthread_mutex_unlock(&cfg_mutex);
    b->cfg_gen = gen;

    if (b->cfg.p.rumble_pct != old_rumble && b->dfd >= 0 && b->ds4_has_rumble) {
        for (i = 0; i < BRIDGE_FF_SLOTS; i++)
            if (b->ff_cached[i]) ff_upload_to_ds4(b, i);
    }
    if (b->cfg.desktop_mode != old_desktop) {
        memset(b->desk_acc, 0, sizeof b->desk_acc);
        b->last_tick_us = now_us();
    }
    if (need_mouse(b)) ensure_mouse(b);
    process(b);
}

/* ------------------------------------------------------------------------- */
/*  DS4 Discovery and connection                                             */
/* ------------------------------------------------------------------------- */

static int name_matches_ds4(const char *name)
{
    if (ci_contains(name, "Motion Sensors") || ci_contains(name, "Touchpad"))
        return 0;
    return ci_contains(name, "Sony") ||
           ci_contains(name, "DualShock 4") ||
           ci_contains(name, "Wireless Controller");
}

static int is_bridge_virtual(int fd)
{
    char phys[128] = {0};
    if (ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys) < 0) return 0;
    return strncmp(phys, "ds4-bridge/", 11) == 0;
}

static int is_gamepad_node(int fd)
{
    unsigned long evbits[NLONGS(EV_CNT)] = {0};
    unsigned long keybits[NLONGS(KEY_CNT)] = {0};
    unsigned long absbits[NLONGS(ABS_CNT)] = {0};
    unsigned long props[NLONGS(INPUT_PROP_CNT)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0) return 0;
    if (!TEST_BIT(EV_KEY, evbits) || !TEST_BIT(EV_ABS, evbits)) return 0;

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0) return 0;

    (void)ioctl(fd, EVIOCGPROP(sizeof props), props);

#ifdef INPUT_PROP_ACCELEROMETER
    if (TEST_BIT(INPUT_PROP_ACCELEROMETER, props)) return 0;
#endif
    if (TEST_BIT(INPUT_PROP_POINTER, props) ||
        TEST_BIT(BTN_TOUCH, keybits) ||
        TEST_BIT(ABS_MT_POSITION_X, absbits)) return 0;

    return TEST_BIT(BTN_SOUTH, keybits) && TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits);
}

static int is_motion_node(int fd, const char *name)
{
    unsigned long absbits[NLONGS(ABS_CNT)] = {0};
    unsigned long props[NLONGS(INPUT_PROP_CNT)] = {0};
    int accel = ci_contains(name, "Motion Sensors");

#ifdef INPUT_PROP_ACCELEROMETER
    if (ioctl(fd, EVIOCGPROP(sizeof props), props) >= 0 && TEST_BIT(INPUT_PROP_ACCELEROMETER, props))
        accel = 1;
#endif
    if (!accel || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0) return 0;
    return TEST_BIT(ABS_RX, absbits) && TEST_BIT(ABS_RY, absbits) && TEST_BIT(ABS_RZ, absbits);
}

/* Resolve the HID device an event node belongs to. Gamepad, touchpad and
 * motion nodes of the same controller share it. */
static int event_hid_parent(const char *event_name, char *out)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/input/%s/device/device", event_name);
    return realpath(p, out) ? 0 : -1;
}

static void resync_from_ds4(struct bridge *b)
{
    unsigned long keys[NLONGS(KEY_CNT)] = {0};
    size_t i;

    if (ioctl(b->dfd, EVIOCGKEY(sizeof keys), keys) >= 0) {
        for (i = 0; i < N_PKEYS; i++)
            b->pkey[i] = (int)TEST_BIT(PHYS_KEYS[i].code, keys);
    }

    for (i = 0; i < N_PABS; i++) {
        struct input_absinfo ai;
        if (!b->has_abs[i]) continue;
        if (ioctl(b->dfd, EVIOCGABS(PHYS_ABS_CODES[i]), &ai) < 0) continue;
        b->src_abs[i] = ai;
        b->pabs[i] = ai.value;
    }
    process(b);
}

static int open_candidate(const char *path, int *writable)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) { *writable = 1; return fd; }
    if (errno == EACCES || errno == EPERM) {
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) { *writable = 0; return fd; }
    }
    return -1;
}

static void slot_set_connected(struct bridge *b, int connected)
{
    struct slot_status *s = &g_slots[b->slot];
    pthread_mutex_lock(&status_mutex);
    s->connected  = connected;
    s->has_motion = connected && b->mfd >= 0;
    g_strlcpy(s->name, connected ? b->dev_name : "", sizeof s->name);
    g_strlcpy(s->event, connected ? b->dev_event : "", sizeof s->event);
    if (!connected) {
        memset(s->src, 0, sizeof s->src);
        memset(s->raw, 0, sizeof s->raw);
        memset(s->out, 0, sizeof s->out);
        memset(s->gyro, 0, sizeof s->gyro);
        s->out_trig[0] = s->out_trig[1] = 0;
    }
    pthread_mutex_unlock(&status_mutex);
}

static int attach_ds4(struct bridge *b, int fd, int writable, const char *path,
                      const char *name, const char *event)
{
    unsigned long absbits[NLONGS(ABS_CNT)] = {0};
    unsigned long ffbits[NLONGS(FF_CNT)] = {0};
    size_t i;

    b->dfd = fd;
    b->ds4_writable = writable;
    b->syn_dropped = 0;
    b->grabbed = 0;
    snprintf(b->dev_path, sizeof b->dev_path, "%s", path);
    snprintf(b->dev_name, sizeof b->dev_name, "%s", name);
    g_strlcpy(b->dev_event, event, sizeof b->dev_event);
    memset(b->pkey, 0, sizeof b->pkey);
    memset(b->pabs, 0, sizeof b->pabs);

    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    for (i = 0; i < N_PABS; i++) {
        b->has_abs[i] = 0;
        if (TEST_BIT(PHYS_ABS_CODES[i], absbits) &&
            ioctl(fd, EVIOCGABS(PHYS_ABS_CODES[i]), &b->src_abs[i]) == 0)
            b->has_abs[i] = 1;
    }

    b->ds4_has_rumble = 0;
    b->ds4_has_gain   = 0;
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof ffbits), ffbits) >= 0) {
        b->ds4_has_rumble = writable && TEST_BIT(FF_RUMBLE, ffbits);
        b->ds4_has_gain   = writable && TEST_BIT(FF_GAIN, ffbits);
    }

    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
        b->grabbed = 1;
    } else {
        b->dfd = -1;
        return -1; // Another bridge likely grabbed it
    }

    logmsg(LOG_INFO, "P%d: DS4 connected: \"%s\" (%s) rumble=%s",
           b->slot + 1, name, path, b->ds4_has_rumble ? "yes" : "no");

    ff_restore_on_connect(b);
    resync_from_ds4(b);
    return 0;
}

/* Touchpad (grabbed when disabled) and motion sensors of the same controller */
static void attach_siblings(struct bridge *b)
{
    char parent[PATH_MAX], sp[PATH_MAX];
    struct dirent *de;
    DIR *dir;

    if (event_hid_parent(b->dev_event, parent) < 0) return;
    dir = opendir("/dev/input");
    if (!dir) return;

    while ((de = readdir(dir)) != NULL) {
        char path[300], name[256] = {0};
        int fd, writable;

        if (strncmp(de->d_name, "event", 5) != 0 || strcmp(de->d_name, b->dev_event) == 0) continue;
        if (event_hid_parent(de->d_name, sp) < 0 || strcmp(sp, parent) != 0) continue;

        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        fd = open_candidate(path, &writable);
        if (fd < 0) continue;
        (void)ioctl(fd, EVIOCGNAME(sizeof name - 1), name);

        if (b->mfd < 0 && is_motion_node(fd, name)) {
            static const unsigned short gyro_codes[3] = { ABS_RX, ABS_RY, ABS_RZ };
            int i;
            for (i = 0; i < 3; i++) {
                memset(&b->gyro_abs[i], 0, sizeof b->gyro_abs[i]);
                if (ioctl(fd, EVIOCGABS(gyro_codes[i]), &b->gyro_abs[i]) == 0)
                    b->gyro_raw[i] = b->gyro_abs[i].value;
            }
            b->mfd = fd;
            b->motion_syn_dropped = 0;
            b->gyro_last_us = 0;
            logmsg(LOG_INFO, "P%d: motion sensors found: %s (%s)", b->slot + 1, path, name);
            continue;
        }

        if (b->tfd < 0 && b->disable_touchpad && ci_contains(name, "Touchpad")) {
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                b->tfd = fd;
                logmsg(LOG_INFO, "P%d: Touchpad disabled (grabbed): %s (%s)", b->slot + 1, path, name);
                continue;
            }
            logmsg(LOG_WARNING, "Found touchpad %s, but EVIOCGRAB failed: %s", path, strerror(errno));
        }
        close(fd);
    }
    closedir(dir);
}

static int connect_ds4(struct bridge *b)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *de;
    int rc = -1;

    while ((de = readdir(dir)) != NULL) {
        char path[300];
        char name[256];
        int fd, writable = 0;

        if (strncmp(de->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);

        fd = open_candidate(path, &writable);
        if (fd < 0) continue;

        memset(name, 0, sizeof name);
        if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0 ||
            !name_matches_ds4(name) || is_bridge_virtual(fd) || !is_gamepad_node(fd)) {
            close(fd);
            continue;
        }

        if (attach_ds4(b, fd, writable, path, name, de->d_name) < 0) {
            close(fd);
            continue;
        }

        attach_siblings(b);
        slot_set_connected(b, 1);
        notify_from_thread(NOTE_CONNECTED, b->slot, name);
        process(b);     /* gyro may have become available */
        rc = 0;
        break;
    }
    closedir(dir);
    return rc;
}

static void close_motion(struct bridge *b)
{
    if (b->mfd < 0) return;
    close(b->mfd);
    b->mfd = -1;
    b->gyro_active = 0;
    memset(b->gyro_raw, 0, sizeof b->gyro_raw);
    memset(b->gyro_dps, 0, sizeof b->gyro_dps);
    b->cal_start = 0;
}

static void disconnect_ds4(struct bridge *b, const char *reason)
{
    if (b->dfd < 0) return;
    logmsg(LOG_WARNING, "P%d: DS4 disconnected (%s): \"%s\" - waiting for reconnect...",
           b->slot + 1, reason, b->dev_name);
    notify_from_thread(NOTE_DISCONNECTED, b->slot, b->dev_name);

    if (b->grabbed) (void)ioctl(b->dfd, EVIOCGRAB, 0);
    close(b->dfd);
    b->dfd = -1;
    if (b->tfd >= 0) {
        (void)ioctl(b->tfd, EVIOCGRAB, 0);
        close(b->tfd);
        b->tfd = -1;
    }
    close_motion(b);
    b->grabbed = 0;
    b->ds4_has_rumble = 0;
    b->ds4_has_gain = 0;
    b->combo_mask = 0;
    ff_reset_phys(b);
    process(b);         /* neutral output */
    slot_set_connected(b, 0);
}

/* ------------------------------------------------------------------------- */
/*  Event handling loop                                                      */
/* ------------------------------------------------------------------------- */

static int find_pkey(unsigned short code)
{
    size_t i;
    for (i = 0; i < N_PKEYS; i++)
        if (PHYS_KEYS[i].code == code) return (int)i;
    return -1;
}

static int find_pabs(unsigned short code)
{
    size_t i;
    for (i = 0; i < N_PABS; i++)
        if (PHYS_ABS_CODES[i] == code) return (int)i;
    return -1;
}

static int handle_ds4_input(struct bridge *b)
{
    struct input_event evs[EV_BATCH];
    for (;;) {
        ssize_t n = read(b->dfd, evs, sizeof evs);
        size_t cnt, k;

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;

        cnt = (size_t)n / sizeof evs[0];
        for (k = 0; k < cnt; k++) {
            const struct input_event *ev = &evs[k];
            int idx;

            if (b->syn_dropped) {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                    b->syn_dropped = 0;
                    resync_from_ds4(b);
                }
                continue;
            }

            switch (ev->type) {
            case EV_SYN:
                if (ev->code == SYN_DROPPED)
                    b->syn_dropped = 1;
                else if (ev->code == SYN_REPORT)
                    process(b);
                break;
            case EV_KEY:
                if (ev->value == 2) break;
                idx = find_pkey(ev->code);
                if (idx >= 0) b->pkey[idx] = ev->value ? 1 : 0;
                break;
            case EV_ABS:
                idx = find_pabs(ev->code);
                if (idx >= 0) b->pabs[idx] = ev->value;
                break;
            }
        }
    }
}

static int handle_motion_input(struct bridge *b)
{
    struct input_event evs[EV_BATCH];
    for (;;) {
        ssize_t n = read(b->mfd, evs, sizeof evs);
        size_t cnt, k;

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;

        cnt = (size_t)n / sizeof evs[0];
        for (k = 0; k < cnt; k++) {
            const struct input_event *ev = &evs[k];

            if (b->motion_syn_dropped) {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                    static const unsigned short gyro_codes[3] = { ABS_RX, ABS_RY, ABS_RZ };
                    struct input_absinfo ai;
                    int i;
                    b->motion_syn_dropped = 0;
                    for (i = 0; i < 3; i++)
                        if (ioctl(b->mfd, EVIOCGABS(gyro_codes[i]), &ai) == 0)
                            b->gyro_raw[i] = ai.value;
                }
                continue;
            }

            if (ev->type == EV_SYN) {
                if (ev->code == SYN_DROPPED) b->motion_syn_dropped = 1;
                else if (ev->code == SYN_REPORT) motion_update(b);
            } else if (ev->type == EV_ABS) {
                if (ev->code == ABS_RX)      b->gyro_raw[0] = ev->value;
                else if (ev->code == ABS_RY) b->gyro_raw[1] = ev->value;
                else if (ev->code == ABS_RZ) b->gyro_raw[2] = ev->value;
            }
        }
    }
}

static int handle_uinput_events(struct bridge *b)
{
    struct input_event evs[EV_BATCH];
    for (;;) {
        ssize_t n = read(b->ufd, evs, sizeof evs);
        size_t cnt, k;

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return 0;

        cnt = (size_t)n / sizeof evs[0];
        for (k = 0; k < cnt; k++) {
            const struct input_event *ev = &evs[k];
            if (ev->type == EV_UINPUT) {
                if (ev->code == UI_FF_UPLOAD) handle_ff_upload(b, (unsigned int)ev->value);
                else if (ev->code == UI_FF_ERASE) handle_ff_erase(b, (unsigned int)ev->value);
            } else if (ev->type == EV_FF) {
                handle_ff_event(b, ev->code, ev->value);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Worker Thread Logic                                                      */
/* ------------------------------------------------------------------------- */

static void slot_set_state(int slot, int active, int error)
{
    pthread_mutex_lock(&status_mutex);
    g_slots[slot].active = active;
    g_slots[slot].error  = error;
    if (!active) {
        g_slots[slot].connected = 0;
        g_slots[slot].has_motion = 0;
    }
    pthread_mutex_unlock(&status_mutex);
}

static void* bridge_thread_func(void *arg)
{
    struct thread_arg *targ = (struct thread_arg*)arg;
    struct bridge *b = calloc(1, sizeof *b);
    int first_scan = 1, err = 0;
    long long last_scan = 0;

    if (!b) return NULL;
    b->slot = targ->id - 1;
    b->ufd = b->dfd = b->tfd = b->mfd = b->mousefd = -1;
    b->odev = &OUT_DEVS[clampi(targ->output_type, 0, N_OUT - 1)];
    b->disable_touchpad = targ->disable_touchpad;
    ff_reset_phys(b);

    b->ufd = create_virtual_pad(b->odev, b->slot, &err);
    if (b->ufd < 0) {
        logmsg(LOG_ERR, "P%d: Failed to create virtual pad", targ->id);
        slot_set_state(b->slot, 0, err);
        free(b);
        return NULL;
    }
    slot_set_state(b->slot, 1, 0);
    refresh_cfg(b);

    logmsg(LOG_INFO, "P%d: Bridge active (%s), scanning for DS4...", targ->id, b->odev->label);

    while (!atomic_load(targ->stop_flag)) {
        struct pollfd pfd[3];
        int nfds = 0, idx_ds4 = -1, idx_mot = -1, r;
        int timeout = 500; // Check stop_flag every 500ms max

        refresh_cfg(b);
        if (b->mfd < 0 && atomic_load(&g_gyro_cal_req[b->slot]))
            atomic_store(&g_gyro_cal_req[b->slot], 0);

        if (b->dfd < 0) {
            long long now = now_ms();
            if (first_scan || now - last_scan >= RESCAN_INTERVAL_MS) {
                first_scan = 0;
                last_scan = now;
                pthread_mutex_lock(&scan_mutex);
                connect_ds4(b);
                pthread_mutex_unlock(&scan_mutex);
            }
            if (b->dfd < 0) {
                long long left = RESCAN_INTERVAL_MS - (now_ms() - last_scan);
                if (left >= 0 && left < timeout) timeout = (int)left;
            }
        }
        if (b->dfd >= 0 && b->cfg.desktop_mode && b->mousefd >= 0) {
            long long left = DESKTOP_TICK_MS - (now_us() - b->last_tick_us) / 1000LL;
            timeout = left > 0 ? (int)left : 0;
        }

        pfd[nfds].fd = b->ufd; pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        if (b->dfd >= 0) {
            idx_ds4 = nfds;
            pfd[nfds].fd = b->dfd; pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        }
        if (b->mfd >= 0) {
            idx_mot = nfds;
            pfd[nfds].fd = b->mfd; pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        }

        r = poll(pfd, (nfds_t)nfds, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (r > 0) {
            if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            if ((pfd[0].revents & POLLIN) && handle_uinput_events(b) < 0) break;

            if (idx_ds4 >= 0 && b->dfd >= 0 && pfd[idx_ds4].revents) {
                if (pfd[idx_ds4].revents & POLLIN) {
                    if (handle_ds4_input(b) < 0)
                        disconnect_ds4(b, strerror(errno));
                }
                if (b->dfd >= 0 && (pfd[idx_ds4].revents & (POLLERR | POLLHUP | POLLNVAL)))
                    disconnect_ds4(b, "POLLHUP/POLLERR");
                if (b->dfd < 0) last_scan = now_ms();
            }

            if (idx_mot >= 0 && b->mfd >= 0 && pfd[idx_mot].revents) {
                if (((pfd[idx_mot].revents & POLLIN) && handle_motion_input(b) < 0) ||
                    (pfd[idx_mot].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    logmsg(LOG_WARNING, "P%d: motion sensors lost", b->slot + 1);
                    close_motion(b);
                    process(b);
                }
            }
        }

        if (b->dfd >= 0 && b->cfg.desktop_mode &&
            now_us() - b->last_tick_us >= DESKTOP_TICK_MS * 1000LL)
            desktop_tick(b);
    }

    logmsg(LOG_INFO, "P%d: Stopping and cleaning up resources...", targ->id);

    if (b->dfd >= 0) {
        ff_stop_all_on_ds4(b);
        if (b->grabbed) (void)ioctl(b->dfd, EVIOCGRAB, 0);
        close(b->dfd);
        b->dfd = -1;
    }
    if (b->tfd >= 0) {
        (void)ioctl(b->tfd, EVIOCGRAB, 0);
        close(b->tfd);
    }
    close_motion(b);
    if (b->ufd >= 0) {
        process(b);     /* neutral output before the pad disappears */
        (void)ioctl(b->ufd, UI_DEV_DESTROY);
        close(b->ufd);
    }
    if (b->mousefd >= 0) {
        (void)ioctl(b->mousefd, UI_DEV_DESTROY);
        close(b->mousefd);
    }

    slot_set_state(b->slot, 0, 0);
    free(b);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/*  Settings persistence (~/.config/ds4-bridge/settings.ini)                 */
/* ------------------------------------------------------------------------- */

#define DEFAULT_PROFILE_NAME "Default"

static guint g_save_source = 0;

static struct profile_cfg *active_cfg(void)
{
    return &g_profiles[g_active_profile].c;
}

static void profile_cfg_defaults(struct profile_cfg *c)
{
    int i;
    memset(c, 0, sizeof *c);
    for (i = 0; i < N_SRC; i++)
        c->remap[i] = (unsigned char)(i + 1);
    c->rumble_pct  = 100;
    c->gyro_mode   = GYRO_OFF;
    c->gyro_button = SRC_L2;
    c->gyro_output = GYRO_TO_RSTICK;
    c->gyro_sens   = 100;
}

static void settings_reset_defaults(void)
{
    /* The UI language is kept on purpose */
    g_logs_enabled     = 1;
    g_disable_touchpad = 0;
    num_controllers    = 1;
    g_output_type      = OUT_XBOX360;
    g_desktop_mode     = 0;
    g_desktop_speed    = 100;
    g_auto_profiles    = 1;

    memset(g_profiles, 0, sizeof g_profiles);
    g_n_profiles = 1;
    g_strlcpy(g_profiles[0].name, DEFAULT_PROFILE_NAME, sizeof g_profiles[0].name);
    profile_cfg_defaults(&g_profiles[0].c);
    g_active_profile = g_manual_profile = 0;
    g_auto_profile = -1;
}

static int key_index(const char *const *keys, int n, const char *val, int def)
{
    int i;
    if (!val) return def;
    for (i = 0; i < n; i++)
        if (keys[i] && g_ascii_strcasecmp(keys[i], val) == 0) return i;
    return def;
}

static int kf_int(GKeyFile *kf, const char *grp, const char *key, int def, int lo, int hi)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(kf, grp, key, &err);
    if (err) { g_error_free(err); return def; }
    return clampi(v, lo, hi);
}

static int kf_bool(GKeyFile *kf, const char *grp, const char *key, int def)
{
    GError *err = NULL;
    gboolean v = g_key_file_get_boolean(kf, grp, key, &err);
    if (err) { g_error_free(err); return def; }
    return v ? 1 : 0;
}

static int kf_choice(GKeyFile *kf, const char *grp, const char *key,
                     const char *const *keys, int n, int def)
{
    gchar *s = g_key_file_get_string(kf, grp, key, NULL);
    int v = key_index(keys, n, s, def);
    g_free(s);
    return v;
}

static void profile_cfg_save(GKeyFile *kf, const char *grp, const struct profile_cfg *c)
{
    char key[64];
    int i;

    for (i = 0; i < N_SRC; i++) {
        snprintf(key, sizeof key, "remap_%s", SRC_KEYS[i]);
        g_key_file_set_string(kf, grp, key, TGT_KEYS[c->remap[i]]);
    }
    g_key_file_set_integer(kf, grp, "deadzone_left",    c->dz_left);
    g_key_file_set_integer(kf, grp, "deadzone_right",   c->dz_right);
    g_key_file_set_boolean(kf, grp, "invert_left_y",    c->invert_ly);
    g_key_file_set_boolean(kf, grp, "invert_right_y",   c->invert_ry);
    g_key_file_set_boolean(kf, grp, "swap_sticks",      c->swap_sticks);
    g_key_file_set_boolean(kf, grp, "digital_triggers", c->digital_triggers);
    g_key_file_set_integer(kf, grp, "rumble",           c->rumble_pct);
    g_key_file_set_string (kf, grp, "gyro_mode",        GYRO_MODE_KEYS[c->gyro_mode]);
    g_key_file_set_string (kf, grp, "gyro_button",      SRC_KEYS[c->gyro_button]);
    g_key_file_set_string (kf, grp, "gyro_output",      GYRO_OUT_KEYS[c->gyro_output]);
    g_key_file_set_integer(kf, grp, "gyro_sensitivity", c->gyro_sens);
    g_key_file_set_boolean(kf, grp, "gyro_invert_x",    c->gyro_invert_x);
    g_key_file_set_boolean(kf, grp, "gyro_invert_y",    c->gyro_invert_y);
}

static void profile_cfg_load(GKeyFile *kf, const char *grp, struct profile_cfg *c)
{
    char key[64];
    int i;

    profile_cfg_defaults(c);
    for (i = 0; i < N_SRC; i++) {
        snprintf(key, sizeof key, "remap_%s", SRC_KEYS[i]);
        c->remap[i] = (unsigned char)kf_choice(kf, grp, key, TGT_KEYS, N_TGT, i + 1);
    }
    c->dz_left          = kf_int(kf, grp, "deadzone_left", 0, 0, 50);
    c->dz_right         = kf_int(kf, grp, "deadzone_right", 0, 0, 50);
    c->invert_ly        = kf_bool(kf, grp, "invert_left_y", 0);
    c->invert_ry        = kf_bool(kf, grp, "invert_right_y", 0);
    c->swap_sticks      = kf_bool(kf, grp, "swap_sticks", 0);
    c->digital_triggers = kf_bool(kf, grp, "digital_triggers", 0);
    c->rumble_pct       = kf_int(kf, grp, "rumble", 100, 0, 100);
    c->gyro_mode        = kf_choice(kf, grp, "gyro_mode", GYRO_MODE_KEYS, N_GYRO_MODES, GYRO_OFF);
    c->gyro_button      = kf_choice(kf, grp, "gyro_button", SRC_KEYS, N_SRC, SRC_L2);
    c->gyro_output      = kf_choice(kf, grp, "gyro_output", GYRO_OUT_KEYS, N_GYRO_OUTS, GYRO_TO_RSTICK);
    c->gyro_sens        = kf_int(kf, grp, "gyro_sensitivity", 100, 10, 500);
    c->gyro_invert_x    = kf_bool(kf, grp, "gyro_invert_x", 0);
    c->gyro_invert_y    = kf_bool(kf, grp, "gyro_invert_y", 0);
}

static void save_settings(void) {
    if (settings_file_path[0] == '\0') return;

    GKeyFile *kf = g_key_file_new();
    char grp[32];
    int i;

    g_key_file_set_string (kf, "General", "language",         LANG_KEYS[g_lang]);
    g_key_file_set_boolean(kf, "General", "logs_enabled",     g_logs_enabled);
    g_key_file_set_boolean(kf, "General", "touchpad_enabled", !g_disable_touchpad);
    g_key_file_set_integer(kf, "General", "controller_count", num_controllers);
    g_key_file_set_string (kf, "General", "output_device",    OUT_DEVS[g_output_type].key);
    g_key_file_set_integer(kf, "General", "desktop_speed",    g_desktop_speed);
    g_key_file_set_boolean(kf, "General", "auto_profiles",    g_auto_profiles);
    g_key_file_set_string (kf, "General", "active_profile",   g_profiles[g_manual_profile].name);

    for (i = 0; i < g_n_profiles; i++) {
        snprintf(grp, sizeof grp, "Profile %d", i);
        g_key_file_set_string(kf, grp, "name", g_profiles[i].name);
        g_key_file_set_string(kf, grp, "processes", g_profiles[i].processes);
        profile_cfg_save(kf, grp, &g_profiles[i].c);
    }

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, settings_file_path, &err)) {
        logmsg(LOG_ERR, "Failed to save settings to %s: %s", settings_file_path, err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
}

static gboolean save_settings_cb(gpointer user_data) {
    g_save_source = 0;
    save_settings();
    return G_SOURCE_REMOVE;
}

/* Sliders fire many changes per second; write the file once they settle */
static void schedule_save(void) {
    if (g_save_source) g_source_remove(g_save_source);
    g_save_source = g_timeout_add(500, save_settings_cb, NULL);
}

static void flush_pending_save(void) {
    if (!g_save_source) return;
    g_source_remove(g_save_source);
    g_save_source = 0;
    save_settings();
}

/* Minimal JSON lookup for migrating the old settings.json: finds "key"
 * followed by ':' and returns a pointer to its value, or NULL. All keys in
 * that file were unique, so nesting doesn't need to be tracked. */
static const char *json_find(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (isspace((unsigned char)*p)) p++;
        if (*p == ':') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            return p;
        }
    }
    return NULL;
}

static int json_get_bool(const char *json, const char *key, int def) {
    const char *p = json_find(json, key);
    if (!p) return def;
    if (strncmp(p, "true", 4) == 0)  return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return def;
}

static int json_get_int(const char *json, const char *key, int def) {
    const char *p = json_find(json, key);
    if (!p) return def;
    /* g_ascii_strtoll: glibc 2.38 headers redirect strtol to a 2.38-only
     * symbol, which would stop the binary from running on older systems */
    gchar *end;
    gint64 v = g_ascii_strtoll(p, &end, 10);
    return (end == p) ? def : (int)v;
}

static int json_get_str(const char *json, const char *key, char *buf, size_t size) {
    const char *p = json_find(json, key);
    if (!p || *p != '"' || size == 0) return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size)
        buf[n++] = *p++;
    buf[n] = '\0';
    return *p == '"';
}

/* Older versions kept settings.json next to the executable */
static int migrate_legacy_json(void) {
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return 0;
    gchar *dir = g_path_get_dirname(exe);
    gchar *path = g_build_filename(dir, "settings.json", NULL);
    gchar *json = NULL;
    int ok = g_file_get_contents(path, &json, NULL, NULL);

    if (ok) {
        struct profile_cfg *c = &g_profiles[0].c;
        int i;

        g_logs_enabled     = json_get_bool(json, "logs_enabled", 1);
        g_disable_touchpad = !json_get_bool(json, "touchpad_enabled", 1);
        num_controllers    = clampi(json_get_int(json, "controller_count", 1), 1, MAX_PADS);
        c->dz_left = c->dz_right = clampi(json_get_int(json, "deadzone_pct", 0), 0, 50);

        /* Old remap slots: face buttons, bumpers and triggers */
        for (i = SRC_CROSS; i <= SRC_R2; i++) {
            char val[16];
            if (json_get_str(json, SRC_KEYS[i], val, sizeof val))
                c->remap[i] = (unsigned char)key_index(TGT_KEYS, N_TGT, val, i + 1);
        }
        logmsg(LOG_INFO, "Migrated old settings from %s", path);
        save_settings();
    }

    g_free(json);
    g_free(path);
    g_free(dir);
    g_free(exe);
    return ok;
}

static void load_settings(void) {
    settings_reset_defaults();

    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, settings_file_path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        if (!migrate_legacy_json())
            logmsg(LOG_INFO, "No settings file, using defaults (%s)", settings_file_path);
        return;
    }

    g_lang             = kf_choice(kf, "General", "language", LANG_KEYS, N_LANGS, LANG_EN);
    g_logs_enabled     = kf_bool(kf, "General", "logs_enabled", 1);
    g_disable_touchpad = !kf_bool(kf, "General", "touchpad_enabled", 1);
    num_controllers    = kf_int(kf, "General", "controller_count", 1, 1, MAX_PADS);
    {
        gchar *s = g_key_file_get_string(kf, "General", "output_device", NULL);
        int i;
        for (i = 0; s && i < N_OUT; i++)
            if (strcmp(s, OUT_DEVS[i].key) == 0) g_output_type = i;
        g_free(s);
    }
    g_desktop_speed    = kf_int(kf, "General", "desktop_speed", 100, 10, 300);
    g_auto_profiles    = kf_bool(kf, "General", "auto_profiles", 1);

    g_n_profiles = 0;
    for (int i = 0; i < MAX_PROFILES; i++) {
        char grp[32];
        snprintf(grp, sizeof grp, "Profile %d", i);
        if (!g_key_file_has_group(kf, grp)) break;

        struct profile *p = &g_profiles[g_n_profiles];
        gchar *name = g_key_file_get_string(kf, grp, "name", NULL);
        gchar *procs = g_key_file_get_string(kf, grp, "processes", NULL);
        g_strlcpy(p->name, (name && *name) ? name : DEFAULT_PROFILE_NAME, sizeof p->name);
        g_strlcpy(p->processes, procs ? procs : "", sizeof p->processes);
        g_free(name);
        g_free(procs);
        profile_cfg_load(kf, grp, &p->c);
        g_n_profiles++;
    }
    if (g_n_profiles == 0) {
        g_n_profiles = 1;
        g_strlcpy(g_profiles[0].name, DEFAULT_PROFILE_NAME, sizeof g_profiles[0].name);
        profile_cfg_defaults(&g_profiles[0].c);
    }

    gchar *active = g_key_file_get_string(kf, "General", "active_profile", NULL);
    for (int i = 0; active && i < g_n_profiles; i++)
        if (strcmp(active, g_profiles[i].name) == 0) g_manual_profile = i;
    g_free(active);
    g_active_profile = g_manual_profile;

    g_key_file_free(kf);
    logmsg(LOG_INFO, "Settings loaded from %s", settings_file_path);
}

/* Hand the active profile over to the bridge threads */
static void publish_config(void) {
    pthread_mutex_lock(&cfg_mutex);
    g_rt.p = g_profiles[g_active_profile].c;
    g_rt.desktop_mode = g_desktop_mode;
    g_rt.desktop_speed = g_desktop_speed;
    pthread_mutex_unlock(&cfg_mutex);
    atomic_fetch_add(&g_rt_gen, 1);
}

/* ------------------------------------------------------------------------- */
/*  Tray / UI Integration Logic                                              */
/* ------------------------------------------------------------------------- */

/* Distributions ship either the Ayatana or the older Canonical AppIndicator
 * library, rarely both, so linking against one breaks the binary on the
 * other. Both export the same API; it is resolved at runtime instead. */
typedef struct _AppIndicator AppIndicator;
enum { APP_INDICATOR_CATEGORY_APPLICATION_STATUS = 0 };
enum { APP_INDICATOR_STATUS_ACTIVE = 1 };

static struct {
    AppIndicator *(*new_)(const gchar *id, const gchar *icon_name, int category);
    void (*set_status)(AppIndicator *self, int status);
    void (*set_menu)(AppIndicator *self, GtkMenu *menu);
    void (*set_icon_full)(AppIndicator *self, const gchar *icon_name, const gchar *icon_desc);
    void (*set_icon)(AppIndicator *self, const gchar *icon_name);
    void (*set_title)(AppIndicator *self, const gchar *title);
} ai;

static AppIndicator *indicator;
static GtkStatusIcon *status_icon;   /* fallback when no AppIndicator library */
static GtkWidget *menu;
static GtkWidget *status_item;
static GtkWidget *pad_items[MAX_PADS];
static GtkWidget *start_item;
static GtkWidget *stop_item;
static GtkWidget *profile_item;
static GtkWidget *controller_item;
static GtkWidget *output_item;
static GtkWidget *log_toggle;
static GtkWidget *touchpad_toggle;
static GtkWidget *desktop_toggle;
static GtkWidget *auto_toggle;
static GtkWidget *fix_perms_item;

static int low_battery_notified[MAX_PADS];
static int g_perm_prompted = 0;       /* permission dialog shown for this start */
static int g_perm_fix_attempted = 0;  /* udev rule installed in this session    */
static int g_last_found = -1;         /* profile matched by the game watcher    */
static int g_auto_ignore = -1;        /* match the user overrode by hand       */
static int g_lock_fd = -1;

static gboolean update_status_cb(gpointer user_data);
static void sync_menu_from_settings(void);
static void settings_window_sync(void);
static void build_tray_menu(void);
static void open_settings_window(int page);
static void open_test_window(void);
static void open_log_window(void);
static gboolean rebuild_ui_idle(gpointer user_data);

/* ------------------------------------------------------------------------- */
/*  Tray icon: AppIndicator when available, GtkStatusIcon otherwise          */
/* ------------------------------------------------------------------------- */

static int load_appindicator(void) {
    static const char *const libs[] = {
        "libayatana-appindicator3.so.1", "libappindicator3.so.1",
        "libayatana-appindicator3.so",   "libappindicator3.so",
    };

    for (size_t i = 0; i < ARRAY_LEN(libs); i++) {
        void *h = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
        if (!h) continue;

#define AI_SYM(field, name) ai.field = (__typeof__(ai.field))dlsym(h, name)
        AI_SYM(new_,          "app_indicator_new");
        AI_SYM(set_status,    "app_indicator_set_status");
        AI_SYM(set_menu,      "app_indicator_set_menu");
        AI_SYM(set_icon_full, "app_indicator_set_icon_full");
        AI_SYM(set_icon,      "app_indicator_set_icon");
        AI_SYM(set_title,     "app_indicator_set_title");
#undef AI_SYM

        if (ai.new_ && ai.set_status && ai.set_menu && (ai.set_icon_full || ai.set_icon)) {
            logmsg(LOG_INFO, "Tray: using %s", libs[i]);
            return 1;
        }
        memset(&ai, 0, sizeof ai);
        dlclose(h);
    }
    logmsg(LOG_WARNING, "Tray: no AppIndicator library found, falling back to GtkStatusIcon");
    return 0;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void on_status_icon_popup(GtkStatusIcon *icon, guint button, guint activate_time, gpointer user_data) {
    if (menu)
        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, icon,
                       button, activate_time);
}

static void on_status_icon_activate(GtkStatusIcon *icon, gpointer user_data) {
    on_status_icon_popup(icon, 0, gtk_get_current_event_time(), NULL);
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void tray_init(void) {
    if (load_appindicator()) {
        indicator = ai.new_("ds4-bridge-tray", "input-gaming", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
        ai.set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
        if (ai.set_title) ai.set_title(indicator, "DS4 Bridge");
        return;
    }

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    status_icon = gtk_status_icon_new_from_icon_name("input-gaming");
    gtk_status_icon_set_title(status_icon, "DS4 Bridge");
    g_signal_connect(status_icon, "popup-menu", G_CALLBACK(on_status_icon_popup), NULL);
    g_signal_connect(status_icon, "activate", G_CALLBACK(on_status_icon_activate), NULL);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

/* Installs new_menu and destroys old_menu (may be NULL) */
static void tray_set_menu(GtkWidget *new_menu, GtkWidget *old_menu) {
    /* app_indicator_set_menu() drops its reference to the old menu */
    if (old_menu) g_object_ref(old_menu);
    if (indicator) {
        ai.set_menu(indicator, GTK_MENU(new_menu));
    } else {
        g_object_ref_sink(new_menu);
        if (old_menu) g_object_unref(old_menu);
    }
    if (old_menu) {
        gtk_widget_destroy(old_menu);
        g_object_unref(old_menu);
    }
}

static void tray_set_icon(const char *icon, const char *desc) {
    if (indicator) {
        if (ai.set_icon_full) ai.set_icon_full(indicator, icon, desc);
        else                  ai.set_icon(indicator, icon);
        return;
    }
    if (!status_icon) return;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_status_icon_set_from_icon_name(status_icon, icon);
    gtk_status_icon_set_tooltip_text(status_icon, desc);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

/* Without any tray the app would be invisible; tell the user why */
static gboolean tray_check_cb(gpointer user_data) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    int embedded = status_icon && gtk_status_icon_is_embedded(status_icon);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (status_icon && !embedded) {
        logmsg(LOG_WARNING, "Tray: no system tray found");
        send_notification("DS4 Bridge", T(S_N_NO_TRAY), "dialog-warning");
    }
    return G_SOURCE_REMOVE;
}

/* Apply an in-memory settings change everywhere */
static void settings_changed(void) {
    publish_config();
    schedule_save();
    sync_menu_from_settings();
    settings_window_sync();
}

static gboolean ask_yes_no(const char *title, const char *text) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO,
                                               "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", text);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_YES;
}

static void show_error(GtkWindow *parent, const char *text) {
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", text);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* Returns a newly allocated, stripped string, or NULL on cancel */
static gchar *prompt_text(GtkWindow *parent, const char *title, const char *initial) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, parent,
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    T(S_CANCEL), GTK_RESPONSE_CANCEL,
                                                    T(S_OK), GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new(T(S_PROFILE_NAME));
    GtkWidget *entry = gtk_entry_new();
    gchar *result = NULL;

    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_set_spacing(GTK_BOX(area), 6);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_entry_set_text(GTK_ENTRY(entry), initial);
    gtk_entry_set_max_length(GTK_ENTRY(entry), 60);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
        result = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    gtk_widget_destroy(dialog);
    return result;
}

/* ------------------------------------------------------------------------- */
/*  Bridge control                                                           */
/* ------------------------------------------------------------------------- */

static void stop_bridge(void) {
    if (!atomic_load(&g_bridge_running)) return;

    atomic_store(&g_bridge_stop_requested, 1);
    for (int i = 0; i < g_threads_started; i++) {
        pthread_join(bridge_threads[i], NULL);
    }
    g_threads_started = 0;
    atomic_store(&g_bridge_running, 0);
}

static void start_bridge(void) {
    if (atomic_load(&g_bridge_running)) return;

    pthread_mutex_lock(&status_mutex);
    memset(g_slots, 0, sizeof g_slots);
    pthread_mutex_unlock(&status_mutex);

    g_perm_prompted = 0;
    atomic_store(&g_bridge_stop_requested, 0);
    for (int i = 0; i < num_controllers; i++) {
        thread_args[i].id = i + 1;
        thread_args[i].output_type = g_output_type;
        thread_args[i].disable_touchpad = g_disable_touchpad;
        thread_args[i].stop_flag = &g_bridge_stop_requested;
        if (pthread_create(&bridge_threads[g_threads_started], NULL, bridge_thread_func, &thread_args[i]) == 0)
            g_threads_started++;
        else
            logmsg(LOG_ERR, "Could not start bridge thread %d", i + 1);
    }
    atomic_store(&g_bridge_running, 1);
}

static void restart_bridge_if_running(void) {
    if (!atomic_load(&g_bridge_running)) return;
    stop_bridge();
    start_bridge();
    update_status_cb(NULL);
}

static void switch_profile_manual(int idx) {
    if (idx < 0 || idx >= g_n_profiles) return;
    g_auto_ignore = g_last_found;   /* respect the choice while that game runs */
    g_manual_profile = idx;
    g_auto_profile = -1;
    g_active_profile = idx;
    logmsg(LOG_INFO, "Profile: %s", g_profiles[idx].name);
    settings_changed();
}

static void set_controller_count(int count) {
    if (count == num_controllers || count < 1 || count > MAX_PADS) return;
    int was_running = atomic_load(&g_bridge_running);
    if (was_running) stop_bridge();
    num_controllers = count;
    settings_changed();
    if (was_running) start_bridge();
    update_status_cb(NULL);
}

static void set_output_type(int type) {
    if (type == g_output_type || type < 0 || type >= N_OUT) return;
    g_output_type = type;
    logmsg(LOG_INFO, "Output device: %s", OUT_DEVS[type].label);
    settings_changed();
    restart_bridge_if_running();
}

static void set_touchpad_enabled(int enabled) {
    if (g_disable_touchpad == !enabled) return;
    g_disable_touchpad = !enabled;
    logmsg(LOG_INFO, "Touchpad mouse function %s", g_disable_touchpad ? "disabled" : "enabled");
    settings_changed();
    restart_bridge_if_running();
}

static void set_desktop_mode(int on) {
    if (g_desktop_mode == on) return;
    g_desktop_mode = on;
    logmsg(LOG_INFO, "Desktop mode %s", on ? "on" : "off");
    if (on) send_notification(T(S_N_DESKTOP_ON), T(S_N_DESKTOP_ON_BODY), "input-mouse");
    else    send_notification(T(S_N_DESKTOP_OFF), T(S_N_DESKTOP_OFF_BODY), "input-gaming");
    settings_changed();
}

static void set_auto_profiles(int on) {
    if (g_auto_profiles == on) return;
    g_auto_profiles = on;
    if (!on && g_auto_profile >= 0) {
        g_auto_profile = -1;
        g_active_profile = g_manual_profile;
    }
    settings_changed();
}

static void set_logs_enabled(int on) {
    if (g_logs_enabled == on) return;

    if (on) {
        g_logs_enabled = 1;
        settings_changed();
        logmsg(LOG_INFO, "Logging enabled");
        return;
    }

    logmsg(LOG_INFO, "Logging disabled");   /* last line written to the file */
    g_logs_enabled = 0;
    settings_changed();

    /* Ask right away (not on quit) whether the existing log should go. */
    if (access(log_file_path, F_OK) == 0) {
        char text[PATH_MAX + 256];
        snprintf(text, sizeof text, T(S_ASK_LOGDEL_TEXT), log_file_path);
        if (ask_yes_no(T(S_ASK_LOGDEL_TITLE), text)) {
            char old[PATH_MAX + 4];
            snprintf(old, sizeof old, "%s.1", log_file_path);
            (void)unlink(old);
            if (unlink(log_file_path) == 0)
                logmsg(LOG_INFO, "Old log file deleted");
            else
                logmsg(LOG_ERR, "Could not delete %s: %s", log_file_path, strerror(errno));
        }
    }
}

static void set_language(int lang) {
    if (lang == g_lang || lang < 0 || lang >= N_LANGS) return;
    g_lang = lang;
    schedule_save();
    /* Deferred: this may run inside a handler of a widget being rebuilt */
    g_idle_add(rebuild_ui_idle, NULL);
}

/* ------------------------------------------------------------------------- */
/*  Battery (per controller, from the HID device's power_supply)             */
/* ------------------------------------------------------------------------- */

static int read_small_file(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && isspace((unsigned char)buf[n - 1])) buf[--n] = '\0';
    return 0;
}

static int read_supply(const char *dir, int *capacity, int *charging) {
    char path[PATH_MAX + 32], buf[64];
    snprintf(path, sizeof path, "%s/capacity", dir);
    if (read_small_file(path, buf, sizeof buf) < 0) return -1;
    *capacity = (int)g_ascii_strtoll(buf, NULL, 10);
    snprintf(path, sizeof path, "%s/status", dir);
    *charging = read_small_file(path, buf, sizeof buf) == 0 &&
                (strcmp(buf, "Charging") == 0 || strcmp(buf, "Full") == 0);
    return 0;
}

static int read_supply_in(const char *parent, int *capacity, int *charging, int name_filter) {
    DIR *dir = opendir(parent);
    struct dirent *de;
    int rc = -1;
    if (!dir) return -1;
    while (rc < 0 && (de = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        if (de->d_name[0] == '.') continue;
        if (name_filter && !ci_contains(de->d_name, "controller") && !ci_contains(de->d_name, "sony") &&
            !ci_contains(de->d_name, "ds4") && !ci_contains(de->d_name, "dualshock"))
            continue;
        snprintf(path, sizeof path, "%s/%s", parent, de->d_name);
        rc = read_supply(path, capacity, charging);
    }
    closedir(dir);
    return rc;
}

/* `only_pad` allows the name based fallback when a single controller is
 * connected and its driver doesn't link the battery to the HID device. */
static int get_pad_battery(const char *event, int only_pad, int *capacity, int *charging) {
    char dir[PATH_MAX];
    if (event[0]) {
        snprintf(dir, sizeof dir, "/sys/class/input/%s/device/device/power_supply", event);
        if (read_supply_in(dir, capacity, charging, 0) == 0) return 0;
    }
    if (!only_pad) return -1;
    return read_supply_in("/sys/class/power_supply", capacity, charging, 1);
}

/* ------------------------------------------------------------------------- */
/*  /dev/uinput permission helper                                            */
/* ------------------------------------------------------------------------- */

static int is_permission_error(int err) {
    return err == EACCES || err == EPERM || err == ENOENT || err == ENODEV;
}

static void on_perm_fix_done(GPid pid, gint status, gpointer user_data) {
    g_spawn_close_pid(pid);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        logmsg(LOG_INFO, "udev rule for /dev/uinput installed");
        g_perm_fix_attempted = 1;
        send_notification("DS4 Bridge", T(S_N_PERMS_OK), "input-gaming");
        if (atomic_load(&g_bridge_running)) {
            stop_bridge();
            start_bridge();
        }
        update_status_cb(NULL);
    } else {
        logmsg(LOG_ERR, "Installing the udev rule failed (wait status %d)", status);
        send_notification("DS4 Bridge", T(S_N_PERMS_FAIL), "dialog-error");
    }
}

static void run_permission_fix(void) {
    static const char script[] =
        "set -e\n"
        "printf '%s\\n' 'KERNEL==\"uinput\", SUBSYSTEM==\"misc\", TAG+=\"uaccess\", OPTIONS+=\"static_node=uinput\"'"
        " > /etc/udev/rules.d/60-ds4aze-uinput.rules\n"
        "echo uinput > /etc/modules-load.d/ds4aze-uinput.conf\n"
        "modprobe uinput || true\n"
        "udevadm control --reload-rules\n"
        "udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput || true\n"
        "udevadm settle || true\n";
    const gchar *argv[] = { "pkexec", "/bin/sh", "-c", script, NULL };
    GError *err = NULL;
    GPid pid;

    if (!g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &pid, &err)) {
        logmsg(LOG_ERR, "Could not run pkexec: %s", err->message);
        g_error_free(err);
        send_notification("DS4 Bridge", T(S_N_PERMS_FAIL), "dialog-error");
        return;
    }
    g_child_watch_add(pid, on_perm_fix_done, NULL);
}

static gboolean offer_permission_fix_idle(gpointer user_data) {
    if (g_perm_fix_attempted) {
        send_notification("DS4 Bridge", T(S_N_PERMS_RELOGIN), "dialog-warning");
        return G_SOURCE_REMOVE;
    }
    if (ask_yes_no(T(S_ASK_PERMS_TITLE), T(S_ASK_PERMS_TEXT)))
        run_permission_fix();
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/*  Status                                                                   */
/* ------------------------------------------------------------------------- */

static gboolean update_status_cb(gpointer user_data) {
    struct slot_status snap[MAX_PADS];
    int running = atomic_load(&g_bridge_running);
    int connected = 0, errors = 0, perm_errors = 0;
    char buf[320];

    if (!menu) return G_SOURCE_CONTINUE;

    pthread_mutex_lock(&status_mutex);
    memcpy(snap, g_slots, sizeof snap);
    pthread_mutex_unlock(&status_mutex);

    for (int i = 0; running && i < g_threads_started; i++)
        if (snap[i].connected) connected++;

    for (int i = 0; i < MAX_PADS; i++) {
        if (!running || i >= g_threads_started) {
            gtk_widget_hide(pad_items[i]);
            low_battery_notified[i] = 0;
            continue;
        }

        if (snap[i].error) {
            errors++;
            if (is_permission_error(snap[i].error)) perm_errors++;
            snprintf(buf, sizeof buf, T(S_PAD_ERROR), i + 1, g_strerror(snap[i].error));
        } else if (snap[i].connected) {
            int capacity, charging;
            if (get_pad_battery(snap[i].event, connected == 1, &capacity, &charging) == 0) {
                snprintf(buf, sizeof buf, "P%d: %s — \xF0\x9F\x94\x8B %d%%%s", i + 1, snap[i].name,  // 🔋
                         capacity, charging ? " \xE2\x9A\xA1" : "");                               // ⚡

                if (capacity <= 20 && !charging && !low_battery_notified[i]) {
                    char msg[256];
                    snprintf(msg, sizeof msg, T(S_N_LOW_BATTERY_BODY), i + 1, capacity);
                    send_notification(T(S_N_LOW_BATTERY_TITLE), msg, "battery-low");
                    low_battery_notified[i] = 1;
                } else if (capacity > 20 || charging) {
                    low_battery_notified[i] = 0;
                }
            } else {
                snprintf(buf, sizeof buf, "P%d: %s", i + 1, snap[i].name);
            }
        } else {
            snprintf(buf, sizeof buf, T(S_PAD_WAITING), i + 1);
        }
        gtk_menu_item_set_label(GTK_MENU_ITEM(pad_items[i]), buf);
        gtk_widget_show(pad_items[i]);
    }

    const char *label, *icon;
    if (!running) {
        label = T(S_STATUS_STOPPED); icon = "media-playback-pause";
    } else if (errors && !connected) {
        label = T(S_STATUS_ERROR);   icon = "dialog-warning";
    } else if (connected) {
        label = T(S_STATUS_RUNNING); icon = "input-gaming";
    } else {
        label = T(S_STATUS_WAITING); icon = "input-gaming-symbolic";
    }
    gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), label);
    tray_set_icon(icon, label);
    gtk_widget_set_sensitive(start_item, !running);
    gtk_widget_set_sensitive(stop_item, running);
    gtk_widget_set_visible(fix_perms_item, perm_errors > 0);

    if (perm_errors && !g_perm_prompted) {
        g_perm_prompted = 1;
        g_idle_add(offer_permission_fix_idle, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean thread_note_idle(gpointer data) {
    struct thread_note *n = data;
    char body[256];

    snprintf(body, sizeof body, "P%d: %s", n->slot + 1, n->name);
    if (n->kind == NOTE_CONNECTED) {
        send_notification(T(S_N_CONNECTED), body, "input-gaming");
    } else {
        send_notification(T(S_N_DISCONNECTED), body, "input-gaming");
        low_battery_notified[n->slot] = 0;
    }
    g_free(n);
    update_status_cb(NULL);
    return G_SOURCE_REMOVE;
}

static gboolean desktop_toggle_idle(gpointer data) {
    set_desktop_mode(!g_desktop_mode);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/*  Per-game profiles: switch automatically while a listed program runs      */
/* ------------------------------------------------------------------------- */

static int rule_matches(const char *rules, const char *comm, const char *exe) {
    gchar **toks = g_strsplit(rules, ",", -1);
    int hit = 0;

    for (gchar **t = toks; *t && !hit; t++) {
        const char *s = g_strstrip(*t);
        size_t len = strlen(s);
        if (!len) continue;
        /* /proc/PID/comm is cut to 15 characters */
        hit = g_ascii_strcasecmp(s, comm) == 0 ||
              (exe[0] && g_ascii_strcasecmp(s, exe) == 0) ||
              (len > 15 && strlen(comm) == 15 && g_ascii_strncasecmp(s, comm, 15) == 0);
    }
    g_strfreev(toks);
    return hit;
}

static gboolean auto_profile_cb(gpointer user_data) {
    int found = -1, has_rules = 0;
    char found_proc[64] = "";

    for (int i = 1; i < g_n_profiles; i++)
        if (g_profiles[i].processes[0]) has_rules = 1;

    if (g_auto_profiles && has_rules) {
        DIR *dir = opendir("/proc");
        struct dirent *de;
        while (dir && (de = readdir(dir)) != NULL) {
            char path[300], comm[64], cmd[512] = "";
            const char *exe;

            if (!isdigit((unsigned char)de->d_name[0])) continue;
            snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
            if (read_small_file(path, comm, sizeof comm) < 0) continue;

            snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
            FILE *f = fopen(path, "r");
            if (f) {
                size_t n = fread(cmd, 1, sizeof cmd - 1, f);
                cmd[n] = '\0';      /* argv[0] ends at the first NUL */
                fclose(f);
            }
            /* Wine passes Windows paths, so split on both separators */
            const char *s1 = strrchr(cmd, '/'), *s2 = strrchr(cmd, '\\');
            exe = s1 > s2 ? s1 + 1 : (s2 ? s2 + 1 : cmd);

            for (int i = 1; i < g_n_profiles && (found < 0 || i < found); i++) {
                if (g_profiles[i].processes[0] && rule_matches(g_profiles[i].processes, comm, exe)) {
                    found = i;
                    g_strlcpy(found_proc, comm, sizeof found_proc);
                    break;
                }
            }
        }
        if (dir) closedir(dir);
    }

    g_last_found = found;
    if (found != g_auto_ignore) g_auto_ignore = -1;
    if (found >= 0 && found == g_auto_ignore) found = -1;

    int target = found >= 0 ? found : g_manual_profile;
    if (target != g_active_profile) {
        char title[128];
        g_active_profile = target;
        g_auto_profile = found;
        snprintf(title, sizeof title, T(S_N_PROFILE), g_profiles[target].name);
        if (found >= 0) {
            char body[160];
            snprintf(body, sizeof body, T(S_N_PROFILE_AUTO), found_proc);
            logmsg(LOG_INFO, "Profile %s activated for %s", g_profiles[target].name, found_proc);
            send_notification(title, body, "input-gaming");
        } else {
            logmsg(LOG_INFO, "Game closed, back to profile %s", g_profiles[target].name);
            send_notification(title, T(S_N_PROFILE_BACK), "input-gaming");
        }
        settings_changed();
    } else {
        g_auto_profile = found;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/*  Tray menu                                                                */
/* ------------------------------------------------------------------------- */

/* Radio-like choices are check items drawn as radios, so a value that
 * matches none of the entries (e.g. a deadzone set with the slider) can be
 * shown with nothing selected. */
enum choice_kind { CH_PROFILE, CH_COUNT, CH_OUTPUT, CH_DZ_LEFT, CH_DZ_RIGHT, CH_RUMBLE, CH_LANG, N_CH };

static struct {
    GtkWidget *items[MAX_PROFILES];
    int        values[MAX_PROFILES];
    int        n;
} g_choices[N_CH];

static int choice_current(int kind) {
    switch (kind) {
    case CH_PROFILE:  return g_active_profile;
    case CH_COUNT:    return num_controllers;
    case CH_OUTPUT:   return g_output_type;
    case CH_DZ_LEFT:  return active_cfg()->dz_left;
    case CH_DZ_RIGHT: return active_cfg()->dz_right;
    case CH_RUMBLE:   return active_cfg()->rumble_pct;
    case CH_LANG:     return g_lang;
    }
    return -1;
}

static void apply_choice(int kind, int value) {
    switch (kind) {
    case CH_PROFILE:  switch_profile_manual(value); break;
    case CH_COUNT:    set_controller_count(value); break;
    case CH_OUTPUT:   set_output_type(value); break;
    case CH_DZ_LEFT:  active_cfg()->dz_left = value; settings_changed(); break;
    case CH_DZ_RIGHT: active_cfg()->dz_right = value; settings_changed(); break;
    case CH_RUMBLE:   active_cfg()->rumble_pct = value; settings_changed(); break;
    case CH_LANG:     set_language(value); break;
    }
}

static void on_choice_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    if (g_applying_settings) return;
    int kind = GPOINTER_TO_INT(user_data);
    int value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "choice-value"));

    if (!gtk_check_menu_item_get_active(item)) {
        sync_menu_from_settings();      /* clicking the current choice keeps it */
        return;
    }
    apply_choice(kind, value);
    sync_menu_from_settings();
}

static GtkWidget *menu_add(GtkWidget *m, GtkWidget *item) {
    gtk_menu_shell_append(GTK_MENU_SHELL(m), item);
    return item;
}

static GtkWidget *menu_item(GtkWidget *m, const char *label, GCallback cb) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    if (cb) g_signal_connect(item, "activate", cb, NULL);
    return menu_add(m, item);
}

static GtkWidget *menu_check(GtkWidget *m, const char *label, GCallback cb) {
    GtkWidget *item = gtk_check_menu_item_new_with_label(label);
    g_signal_connect(item, "toggled", cb, NULL);
    return menu_add(m, item);
}

static GtkWidget *menu_sub(GtkWidget *m, const char *label, GtkWidget **item_out) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    menu_add(m, item);
    if (item_out) *item_out = item;
    return sub;
}

static void menu_sep(GtkWidget *m) {
    menu_add(m, gtk_separator_menu_item_new());
}

static void add_choice(GtkWidget *m, int kind, const char *label, int value) {
    GtkWidget *item = gtk_check_menu_item_new_with_label(label);
    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
    g_object_set_data(G_OBJECT(item), "choice-value", GINT_TO_POINTER(value));
    g_signal_connect(item, "toggled", G_CALLBACK(on_choice_toggled), GINT_TO_POINTER(kind));
    menu_add(m, item);
    if (g_choices[kind].n < MAX_PROFILES) {
        g_choices[kind].items[g_choices[kind].n]  = item;
        g_choices[kind].values[g_choices[kind].n] = value;
        g_choices[kind].n++;
    }
}

/* Reflect the in-memory settings in the tray menu without triggering the
 * toggle handlers' side effects. */
static void sync_menu_from_settings(void) {
    char label[160];
    if (!menu) return;

    g_applying_settings++;
    for (int k = 0; k < N_CH; k++) {
        int cur = choice_current(k);
        for (int i = 0; i < g_choices[k].n; i++)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_choices[k].items[i]),
                                           g_choices[k].values[i] == cur);
    }
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(log_toggle), g_logs_enabled);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(touchpad_toggle), !g_disable_touchpad);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(desktop_toggle), g_desktop_mode);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_toggle), g_auto_profiles);

    snprintf(label, sizeof label, T(S_PROFILE_MENU), g_profiles[g_active_profile].name);
    gtk_menu_item_set_label(GTK_MENU_ITEM(profile_item), label);
    snprintf(label, sizeof label, T(S_CONTROLLER_COUNT), num_controllers);
    gtk_menu_item_set_label(GTK_MENU_ITEM(controller_item), label);
    snprintf(label, sizeof label, T(S_OUTPUT_MENU), OUT_DEVS[g_output_type].label);
    gtk_menu_item_set_label(GTK_MENU_ITEM(output_item), label);
    g_applying_settings--;
}

static void on_start_clicked(GtkMenuItem *item, gpointer user_data) {
    start_bridge();
    update_status_cb(NULL);
}

static void on_stop_clicked(GtkMenuItem *item, gpointer user_data) {
    stop_bridge();
    update_status_cb(NULL);
}

static void on_touchpad_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    if (g_applying_settings) return;
    set_touchpad_enabled(gtk_check_menu_item_get_active(item));
}

static void on_desktop_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    if (g_applying_settings) return;
    set_desktop_mode(gtk_check_menu_item_get_active(item));
}

static void on_auto_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    if (g_applying_settings) return;
    set_auto_profiles(gtk_check_menu_item_get_active(item));
}

static void on_logs_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    if (g_applying_settings) return;
    set_logs_enabled(gtk_check_menu_item_get_active(item));
}

static void on_settings_clicked(GtkMenuItem *item, gpointer user_data) {
    open_settings_window(-1);
}

static void on_test_clicked(GtkMenuItem *item, gpointer user_data) {
    open_test_window();
}

static void show_logs_clicked(GtkMenuItem *item, gpointer user_data) {
    open_log_window();
}

static void on_fix_perms_clicked(GtkMenuItem *item, gpointer user_data) {
    run_permission_fix();
}

static void on_reset_settings_clicked(GtkMenuItem *item, gpointer user_data) {
    if (!ask_yes_no(T(S_ASK_RESET_TITLE), T(S_ASK_RESET_TEXT)))
        return;

    if (g_save_source) { g_source_remove(g_save_source); g_save_source = 0; }
    if (unlink(settings_file_path) != 0 && errno != ENOENT)
        logmsg(LOG_ERR, "Could not delete %s: %s", settings_file_path, strerror(errno));

    int was_running = atomic_load(&g_bridge_running);
    if (was_running) stop_bridge();

    settings_reset_defaults();
    g_last_found = g_auto_ignore = -1;
    publish_config();

    if (was_running) start_bridge();
    g_idle_add(rebuild_ui_idle, NULL);

    logmsg(LOG_INFO, "Settings reset to defaults");
    send_notification("DS4 Bridge", T(S_N_RESET), "input-gaming");
}

/* ------------------------------------------------------------------------- */
/*  Autostart (XDG: ~/.config/autostart/ds4_bridge_tray.desktop)             */
/* ------------------------------------------------------------------------- */

static gchar *autostart_file_path(void) {
    return g_build_filename(g_get_user_config_dir(), "autostart",
                            "ds4_bridge_tray.desktop", NULL);
}

static int autostart_is_enabled(void) {
    gchar *path = autostart_file_path();
    int enabled = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return enabled;
}

/* Quote a path as a single Exec= argument (Desktop Entry spec, exec level).
 * String-level escaping (backslashes) is done later by GKeyFile. */
static gchar *desktop_exec_quote(const char *s) {
    GString *out = g_string_new("\"");
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '`' || *p == '$' || *p == '\\')
            g_string_append_c(out, '\\');
        else if (*p == '%')
            g_string_append_c(out, '%');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

static gboolean autostart_enable(GError **err) {
    /* Inside an AppImage /proc/self/exe is a temporary mount that changes on
     * every run; the runtime exports the real file's path in $APPIMAGE. */
    const char *appimage = g_getenv("APPIMAGE");
    gchar *exe = (appimage && g_path_is_absolute(appimage))
        ? g_strdup(appimage)
        : g_file_read_link("/proc/self/exe", err);
    if (!exe) return FALSE;

    gchar *exe_dir = g_path_get_dirname(exe);
    gchar *qexe    = desktop_exec_quote(exe);
    gchar *exec    = g_strdup_printf("%s --start", qexe);
    gchar *path    = autostart_file_path();
    gchar *dir     = g_path_get_dirname(path);
    gboolean ok    = FALSE;

    if (g_mkdir_with_parents(dir, 0755) != 0) {
        int e = errno;
        g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(e),
                    "Cannot create %s: %s", dir, g_strerror(e));
    } else {
        const char *grp = G_KEY_FILE_DESKTOP_GROUP;
        GKeyFile *kf = g_key_file_new();
        g_key_file_set_string (kf, grp, G_KEY_FILE_DESKTOP_KEY_TYPE, "Application");
        g_key_file_set_string (kf, grp, G_KEY_FILE_DESKTOP_KEY_NAME, "DS4 Bridge");
        g_key_file_set_string (kf, grp, G_KEY_FILE_DESKTOP_KEY_COMMENT,
                               "DualShock 4 to Xbox 360 controller bridge");
        g_key_file_set_string (kf, grp, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
        g_key_file_set_string (kf, grp, G_KEY_FILE_DESKTOP_KEY_PATH, exe_dir);
        g_key_file_set_string (kf, grp, G_KEY_FILE_DESKTOP_KEY_ICON, "input-gaming");
        g_key_file_set_boolean(kf, grp, G_KEY_FILE_DESKTOP_KEY_TERMINAL, FALSE);
        g_key_file_set_boolean(kf, grp, "X-GNOME-Autostart-enabled", TRUE);
        g_key_file_set_integer(kf, grp, "X-GNOME-Autostart-Delay", 3);
        ok = g_key_file_save_to_file(kf, path, err);
        g_key_file_free(kf);
    }

    g_free(dir);
    g_free(path);
    g_free(exec);
    g_free(qexe);
    g_free(exe_dir);
    g_free(exe);
    return ok;
}

static gboolean autostart_disable(GError **err) {
    gchar *path = autostart_file_path();
    gboolean ok = TRUE;
    if (unlink(path) != 0 && errno != ENOENT) {
        int e = errno;
        g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(e),
                    "Cannot remove %s: %s", path, g_strerror(e));
        ok = FALSE;
    }
    g_free(path);
    return ok;
}

static void on_autostart_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    gboolean want = gtk_check_menu_item_get_active(item);
    GError *err = NULL;
    gboolean ok = want ? autostart_enable(&err) : autostart_disable(&err);

    if (ok) {
        logmsg(LOG_INFO, "Start on login %s", want ? "enabled" : "disabled");
        send_notification("DS4 Bridge", want ? T(S_N_AUTOSTART_ON) : T(S_N_AUTOSTART_OFF),
                          "input-gaming");
    } else {
        logmsg(LOG_ERR, "Failed to %s start on login: %s",
               want ? "enable" : "disable", err ? err->message : "unknown error");
        /* Revert the checkbox without re-triggering this handler */
        g_signal_handlers_block_by_func(item, on_autostart_toggled, user_data);
        gtk_check_menu_item_set_active(item, !want);
        g_signal_handlers_unblock_by_func(item, on_autostart_toggled, user_data);
    }
    if (err) g_error_free(err);
}

static void quit_app(GtkMenuItem *item, gpointer user_data) {
    stop_bridge();
    flush_pending_save();
    gtk_main_quit();
}

static void build_tray_menu(void) {
    static const int dz_values[] = { 0, 5, 10, 15, 20, 25 };
    static const int rumble_values[] = { 0, 25, 50, 75, 100 };
    GtkWidget *old = menu, *sub, *sub2, *item;
    char label[64];

    memset(g_choices, 0, sizeof g_choices);
    menu = gtk_menu_new();

    status_item = menu_item(menu, T(S_STATUS_STOPPED), NULL);
    gtk_widget_set_sensitive(status_item, FALSE);
    for (int i = 0; i < MAX_PADS; i++) {
        pad_items[i] = menu_item(menu, "", NULL);
        gtk_widget_set_sensitive(pad_items[i], FALSE);
    }
    menu_sep(menu);

    start_item = menu_item(menu, T(S_START), G_CALLBACK(on_start_clicked));
    stop_item  = menu_item(menu, T(S_STOP), G_CALLBACK(on_stop_clicked));
    menu_sep(menu);

    sub = menu_sub(menu, "", &profile_item);
    for (int i = 0; i < g_n_profiles; i++)
        add_choice(sub, CH_PROFILE, g_profiles[i].name, i);
    menu_sep(sub);
    auto_toggle = menu_check(sub, T(S_AUTO_PROFILES), G_CALLBACK(on_auto_toggled));
    menu_item(sub, T(S_MANAGE_PROFILES), G_CALLBACK(on_settings_clicked));

    sub = menu_sub(menu, "", &controller_item);
    for (int i = 1; i <= MAX_PADS; i++) {
        if (i == 1) g_strlcpy(label, T(S_ONE_CONTROLLER), sizeof label);
        else        snprintf(label, sizeof label, T(S_N_CONTROLLERS), i);
        add_choice(sub, CH_COUNT, label, i);
    }

    sub = menu_sub(menu, "", &output_item);
    for (int i = 0; i < N_OUT; i++)
        add_choice(sub, CH_OUTPUT, OUT_DEVS[i].label, i);

    sub = menu_sub(menu, T(S_DEADZONE_MENU), NULL);
    sub2 = menu_sub(sub, T(S_LEFT_STICK), NULL);
    for (size_t i = 0; i < ARRAY_LEN(dz_values); i++) {
        snprintf(label, sizeof label, "%d%%", dz_values[i]);
        add_choice(sub2, CH_DZ_LEFT, label, dz_values[i]);
    }
    sub2 = menu_sub(sub, T(S_RIGHT_STICK), NULL);
    for (size_t i = 0; i < ARRAY_LEN(dz_values); i++) {
        snprintf(label, sizeof label, "%d%%", dz_values[i]);
        add_choice(sub2, CH_DZ_RIGHT, label, dz_values[i]);
    }

    sub = menu_sub(menu, T(S_RUMBLE_MENU), NULL);
    for (size_t i = 0; i < ARRAY_LEN(rumble_values); i++) {
        if (rumble_values[i] == 0) g_strlcpy(label, T(S_OFF), sizeof label);
        else snprintf(label, sizeof label, "%d%%", rumble_values[i]);
        add_choice(sub, CH_RUMBLE, label, rumble_values[i]);
    }

    desktop_toggle  = menu_check(menu, T(S_DESKTOP_MODE), G_CALLBACK(on_desktop_toggled));
    touchpad_toggle = menu_check(menu, T(S_TOUCHPAD), G_CALLBACK(on_touchpad_toggled));
    menu_sep(menu);

    menu_item(menu, T(S_SETTINGS), G_CALLBACK(on_settings_clicked));
    menu_item(menu, T(S_TEST), G_CALLBACK(on_test_clicked));
    log_toggle = menu_check(menu, T(S_LOGS_ENABLE), G_CALLBACK(on_logs_toggled));
    menu_item(menu, T(S_LOGS_VIEW), G_CALLBACK(show_logs_clicked));
    sub = menu_sub(menu, T(S_LANGUAGE), NULL);
    for (int i = 0; i < N_LANGS; i++)
        add_choice(sub, CH_LANG, LANG_NAMES[i], i);
    menu_sep(menu);

    fix_perms_item = menu_item(menu, T(S_FIX_PERMS), G_CALLBACK(on_fix_perms_clicked));
    item = gtk_check_menu_item_new_with_label(T(S_AUTOSTART));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), autostart_is_enabled());
    g_signal_connect(item, "toggled", G_CALLBACK(on_autostart_toggled), NULL);
    menu_add(menu, item);
    menu_item(menu, T(S_RESET), G_CALLBACK(on_reset_settings_clicked));
    menu_sep(menu);

    menu_item(menu, T(S_QUIT), G_CALLBACK(quit_app));

    /* Put every toggle/radio into the state of the in-memory settings */
    sync_menu_from_settings();

    gtk_widget_show_all(menu);
    tray_set_menu(menu, old);
    update_status_cb(NULL);
}

/* ------------------------------------------------------------------------- */
/*  Settings window                                                          */
/* ------------------------------------------------------------------------- */

enum bind_kind { BIND_TOGGLE, BIND_SCALE, BIND_COMBO };

/* A widget bound to an int field of the active profile */
struct bind {
    GtkWidget *w;
    int        kind;
    size_t     off;
};

static struct {
    GtkWidget  *win, *notebook;
    GtkWidget  *profile_combo, *new_btn, *rename_btn, *delete_btn, *proc_label, *proc_entry;
    GtkWidget  *lang_combo, *output_combo, *count_spin, *touchpad_check, *logs_check, *auto_check;
    GtkWidget  *remap_combo[N_SRC];
    GtkWidget  *desktop_check, *desktop_speed;
    GtkWidget  *gyro_status, *gyro_cal_btn;
    struct bind binds[24];
    int         n_binds;
    char        profiles_sig[MAX_PROFILES * 64];
    guint       timer;
} sw;

static int *bind_field(const struct bind *b) {
    return (int *)((char *)active_cfg() + b->off);
}

static int bind_read(const struct bind *b) {
    switch (b->kind) {
    case BIND_TOGGLE: return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b->w));
    case BIND_SCALE:  return (int)(gtk_range_get_value(GTK_RANGE(b->w)) + 0.5);
    default:          return gtk_combo_box_get_active(GTK_COMBO_BOX(b->w));
    }
}

static void bind_write(const struct bind *b) {
    int v = *bind_field(b);
    switch (b->kind) {
    case BIND_TOGGLE: gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->w), v); break;
    case BIND_SCALE:  gtk_range_set_value(GTK_RANGE(b->w), v); break;
    default:          gtk_combo_box_set_active(GTK_COMBO_BOX(b->w), v); break;
    }
}

static void on_bind_changed(GtkWidget *w, gpointer data) {
    if (g_applying_settings) return;
    struct bind *b = data;
    int v = bind_read(b);
    if (v < 0 || *bind_field(b) == v) return;
    *bind_field(b) = v;
    settings_changed();
}

static GtkWidget *sw_bind(GtkWidget *w, int kind, size_t off) {
    if (sw.n_binds >= (int)ARRAY_LEN(sw.binds)) return w;
    struct bind *b = &sw.binds[sw.n_binds++];
    b->w = w;
    b->kind = kind;
    b->off = off;
    g_signal_connect(w, kind == BIND_TOGGLE ? "toggled" : kind == BIND_SCALE ? "value-changed" : "changed",
                     G_CALLBACK(on_bind_changed), b);
    return w;
}

static GtkWidget *sw_page(const char *title) {
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(sw.notebook), scroll, gtk_label_new(title));
    return grid;
}

/* label == NULL: the widget spans both columns */
static GtkWidget *grid_row(GtkWidget *grid, int *row, const char *label, GtkWidget *w) {
    if (label) {
        GtkWidget *l = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(grid), l, 0, *row, 1, 1);
        gtk_widget_set_hexpand(w, TRUE);
        gtk_grid_attach(GTK_GRID(grid), w, 1, *row, 1, 1);
    } else {
        gtk_grid_attach(GTK_GRID(grid), w, 0, *row, 2, 1);
    }
    (*row)++;
    return w;
}

static GtkWidget *hint_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    return l;
}

static GtkWidget *new_scale(int min, int max, int step) {
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_scale_set_digits(GTK_SCALE(s), 0);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    return s;
}

static GtkWidget *new_combo(const char *const *labels, int n) {
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), labels[i]);
    return c;
}

static GtkWidget *new_check(const char *label) {
    return gtk_check_button_new_with_label(label);
}

#define CFG_OFF(field) offsetof(struct profile_cfg, field)

static void settings_window_sync(void) {
    char sig[sizeof sw.profiles_sig] = "";
    if (!sw.win) return;

    g_applying_settings++;

    for (int i = 0; i < g_n_profiles; i++) {
        g_strlcat(sig, g_profiles[i].name, sizeof sig);
        g_strlcat(sig, "\n", sizeof sig);
    }
    if (strcmp(sig, sw.profiles_sig) != 0) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(sw.profile_combo));
        for (int i = 0; i < g_n_profiles; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sw.profile_combo), g_profiles[i].name);
        g_strlcpy(sw.profiles_sig, sig, sizeof sw.profiles_sig);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(sw.profile_combo), g_active_profile);
    gtk_widget_set_sensitive(sw.new_btn, g_n_profiles < MAX_PROFILES);
    gtk_widget_set_sensitive(sw.rename_btn, g_active_profile != 0);
    gtk_widget_set_sensitive(sw.delete_btn, g_active_profile != 0);
    gtk_label_set_text(GTK_LABEL(sw.proc_label),
                       g_active_profile == 0 ? T(S_DEFAULT_PROFILE_NOTE) : T(S_PROCESSES));
    gtk_widget_set_visible(sw.proc_entry, g_active_profile != 0);
    if (strcmp(gtk_entry_get_text(GTK_ENTRY(sw.proc_entry)), g_profiles[g_active_profile].processes) != 0)
        gtk_entry_set_text(GTK_ENTRY(sw.proc_entry), g_profiles[g_active_profile].processes);

    gtk_combo_box_set_active(GTK_COMBO_BOX(sw.lang_combo), g_lang);
    gtk_combo_box_set_active(GTK_COMBO_BOX(sw.output_combo), g_output_type);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw.count_spin), num_controllers);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw.touchpad_check), !g_disable_touchpad);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw.logs_check), g_logs_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw.auto_check), g_auto_profiles);

    for (int i = 0; i < N_SRC; i++)
        gtk_combo_box_set_active(GTK_COMBO_BOX(sw.remap_combo[i]), active_cfg()->remap[i]);
    for (int i = 0; i < sw.n_binds; i++)
        bind_write(&sw.binds[i]);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw.desktop_check), g_desktop_mode);
    gtk_range_set_value(GTK_RANGE(sw.desktop_speed), g_desktop_speed);

    g_applying_settings--;
}

static void on_sw_profile_changed(GtkComboBox *combo, gpointer user_data) {
    if (g_applying_settings) return;
    int idx = gtk_combo_box_get_active(combo);
    if (idx >= 0 && idx != g_active_profile) switch_profile_manual(idx);
}

static int profile_name_ok(const char *name, int except) {
    if (!name[0]) return 0;
    for (int i = 0; i < g_n_profiles; i++)
        if (i != except && g_utf8_collate(name, g_profiles[i].name) == 0) return 0;
    return 1;
}

static void on_profile_new(GtkButton *button, gpointer user_data) {
    if (g_n_profiles >= MAX_PROFILES) return;
    gchar *name = prompt_text(GTK_WINDOW(sw.win), T(S_NEW_PROFILE_TITLE), "");
    if (!name) return;
    if (!profile_name_ok(name, -1)) {
        show_error(GTK_WINDOW(sw.win), T(S_PROFILE_NAME_INVALID));
        g_free(name);
        return;
    }

    /* A new profile starts as a copy of the current one */
    struct profile *p = &g_profiles[g_n_profiles];
    memset(p, 0, sizeof *p);
    g_strlcpy(p->name, name, sizeof p->name);
    p->c = *active_cfg();
    g_n_profiles++;
    g_free(name);

    build_tray_menu();
    switch_profile_manual(g_n_profiles - 1);
}

static void on_profile_rename(GtkButton *button, gpointer user_data) {
    int idx = g_active_profile;
    if (idx == 0) return;
    gchar *name = prompt_text(GTK_WINDOW(sw.win), T(S_RENAME_PROFILE_TITLE), g_profiles[idx].name);
    if (!name) return;
    if (!profile_name_ok(name, idx)) {
        show_error(GTK_WINDOW(sw.win), T(S_PROFILE_NAME_INVALID));
    } else {
        g_strlcpy(g_profiles[idx].name, name, sizeof g_profiles[idx].name);
        build_tray_menu();
        settings_changed();
    }
    g_free(name);
}

static void on_profile_delete(GtkButton *button, gpointer user_data) {
    int idx = g_active_profile;
    char text[160];
    if (idx == 0) return;

    snprintf(text, sizeof text, T(S_ASK_DELETE_PROFILE), g_profiles[idx].name);
    if (!ask_yes_no(text, g_profiles[idx].processes)) return;

    memmove(&g_profiles[idx], &g_profiles[idx + 1], sizeof g_profiles[0] * (size_t)(g_n_profiles - idx - 1));
    g_n_profiles--;
    if (g_manual_profile == idx) g_manual_profile = 0;
    else if (g_manual_profile > idx) g_manual_profile--;
    g_auto_profile = -1;
    g_last_found = g_auto_ignore = -1;
    g_active_profile = g_manual_profile;

    build_tray_menu();
    settings_changed();
}

static void on_proc_changed(GtkEditable *editable, gpointer user_data) {
    if (g_applying_settings || g_active_profile == 0) return;
    g_strlcpy(g_profiles[g_active_profile].processes, gtk_entry_get_text(GTK_ENTRY(editable)),
              sizeof g_profiles[0].processes);
    schedule_save();
}

static void on_sw_lang_changed(GtkComboBox *combo, gpointer user_data) {
    if (!g_applying_settings) set_language(gtk_combo_box_get_active(combo));
}

static void on_sw_output_changed(GtkComboBox *combo, gpointer user_data) {
    if (!g_applying_settings) set_output_type(gtk_combo_box_get_active(combo));
}

static void on_sw_count_changed(GtkSpinButton *spin, gpointer user_data) {
    if (!g_applying_settings) set_controller_count(gtk_spin_button_get_value_as_int(spin));
}

static void on_sw_touchpad_toggled(GtkToggleButton *b, gpointer user_data) {
    if (!g_applying_settings) set_touchpad_enabled(gtk_toggle_button_get_active(b));
}

static void on_sw_logs_toggled(GtkToggleButton *b, gpointer user_data) {
    if (!g_applying_settings) set_logs_enabled(gtk_toggle_button_get_active(b));
}

static void on_sw_auto_toggled(GtkToggleButton *b, gpointer user_data) {
    if (!g_applying_settings) set_auto_profiles(gtk_toggle_button_get_active(b));
}

static void on_sw_desktop_toggled(GtkToggleButton *b, gpointer user_data) {
    if (!g_applying_settings) set_desktop_mode(gtk_toggle_button_get_active(b));
}

static void on_sw_speed_changed(GtkRange *range, gpointer user_data) {
    if (g_applying_settings) return;
    g_desktop_speed = (int)(gtk_range_get_value(range) + 0.5);
    settings_changed();
}

static void on_sw_fix_perms(GtkButton *button, gpointer user_data) {
    run_permission_fix();
}

static void on_remap_changed(GtkComboBox *combo, gpointer user_data) {
    if (g_applying_settings) return;
    int src = GPOINTER_TO_INT(user_data), t = gtk_combo_box_get_active(combo);
    if (t < 0 || t >= N_TGT || active_cfg()->remap[src] == t) return;
    active_cfg()->remap[src] = (unsigned char)t;
    logmsg(LOG_INFO, "Remapped %s -> %s", SRC_KEYS[src], TGT_KEYS[t]);
    settings_changed();
}

static void on_remap_reset(GtkButton *button, gpointer user_data) {
    for (int i = 0; i < N_SRC; i++)
        active_cfg()->remap[i] = (unsigned char)(i + 1);
    settings_changed();
}

static void on_gyro_calibrate(GtkButton *button, gpointer user_data) {
    pthread_mutex_lock(&status_mutex);
    for (int i = 0; i < MAX_PADS; i++)
        if (g_slots[i].connected && g_slots[i].has_motion) {
            atomic_store(&g_gyro_cal_req[i], 1);
            g_slots[i].gyro_calibrating = 1;
        }
    pthread_mutex_unlock(&status_mutex);
}

static gboolean sw_timer_cb(gpointer user_data) {
    char text[256] = "", pads[64] = "";
    int calibrating = 0;

    pthread_mutex_lock(&status_mutex);
    for (int i = 0; i < MAX_PADS; i++) {
        if (!g_slots[i].connected || !g_slots[i].has_motion) continue;
        char p[8];
        snprintf(p, sizeof p, "%sP%d", pads[0] ? ", " : "", i + 1);
        g_strlcat(pads, p, sizeof pads);
        calibrating |= g_slots[i].gyro_calibrating || atomic_load(&g_gyro_cal_req[i]);
    }
    pthread_mutex_unlock(&status_mutex);

    if (calibrating)  g_strlcpy(text, T(S_GYRO_CALIBRATING), sizeof text);
    else if (pads[0]) snprintf(text, sizeof text, T(S_GYRO_FOUND), pads);
    else              g_strlcpy(text, T(S_GYRO_NOT_FOUND), sizeof text);

    if (strcmp(gtk_label_get_text(GTK_LABEL(sw.gyro_status)), text) != 0)
        gtk_label_set_text(GTK_LABEL(sw.gyro_status), text);
    gtk_widget_set_sensitive(sw.gyro_cal_btn, pads[0] && !calibrating);
    return G_SOURCE_CONTINUE;
}

static void on_settings_destroy(GtkWidget *w, gpointer user_data) {
    if (sw.timer) g_source_remove(sw.timer);
    memset(&sw, 0, sizeof sw);
}

static void build_general_page(void) {
    GtkWidget *g = sw_page(T(S_TAB_GENERAL));
    const char *outs[N_OUT];
    char text[PATH_MAX + 64];
    int row = 0;

    for (int i = 0; i < N_OUT; i++) outs[i] = OUT_DEVS[i].label;

    sw.lang_combo = grid_row(g, &row, T(S_LANGUAGE_LBL), new_combo(LANG_NAMES, N_LANGS));
    g_signal_connect(sw.lang_combo, "changed", G_CALLBACK(on_sw_lang_changed), NULL);

    sw.output_combo = grid_row(g, &row, T(S_OUTPUT_LBL), new_combo(outs, N_OUT));
    g_signal_connect(sw.output_combo, "changed", G_CALLBACK(on_sw_output_changed), NULL);
    grid_row(g, &row, NULL, hint_label(T(S_OUTPUT_HINT)));

    sw.count_spin = gtk_spin_button_new_with_range(1, MAX_PADS, 1);
    grid_row(g, &row, T(S_COUNT_LBL), sw.count_spin);
    g_signal_connect(sw.count_spin, "value-changed", G_CALLBACK(on_sw_count_changed), NULL);

    sw.touchpad_check = grid_row(g, &row, NULL, new_check(T(S_TOUCHPAD_LBL)));
    g_signal_connect(sw.touchpad_check, "toggled", G_CALLBACK(on_sw_touchpad_toggled), NULL);
    sw.auto_check = grid_row(g, &row, NULL, new_check(T(S_AUTO_LBL)));
    g_signal_connect(sw.auto_check, "toggled", G_CALLBACK(on_sw_auto_toggled), NULL);
    sw.logs_check = grid_row(g, &row, NULL, new_check(T(S_LOGS_LBL)));
    g_signal_connect(sw.logs_check, "toggled", G_CALLBACK(on_sw_logs_toggled), NULL);

    GtkWidget *fix = gtk_button_new_with_label(T(S_FIX_PERMS_BTN));
    gtk_widget_set_halign(fix, GTK_ALIGN_START);
    g_signal_connect(fix, "clicked", G_CALLBACK(on_sw_fix_perms), NULL);
    grid_row(g, &row, NULL, fix);

    snprintf(text, sizeof text, T(S_SETTINGS_FILE), settings_file_path);
    grid_row(g, &row, NULL, hint_label(text));
    snprintf(text, sizeof text, T(S_LOG_FILE), log_file_path);
    grid_row(g, &row, NULL, hint_label(text));
}

static void build_buttons_page(void) {
    GtkWidget *g = sw_page(T(S_TAB_BUTTONS));
    const char *targets[N_TGT];
    int row = 0;

    targets[0] = T(S_DISABLED);
    for (int i = 1; i < N_TGT; i++) targets[i] = TGT_LABELS[i];

    grid_row(g, &row, NULL, hint_label(T(S_BTN_HINT)));
    for (int i = 0; i < N_SRC; i++) {
        sw.remap_combo[i] = grid_row(g, &row, SRC_LABELS[i], new_combo(targets, N_TGT));
        g_signal_connect(sw.remap_combo[i], "changed", G_CALLBACK(on_remap_changed), GINT_TO_POINTER(i));
    }
    GtkWidget *reset = gtk_button_new_with_label(T(S_RESET_BUTTONS));
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_remap_reset), NULL);
    grid_row(g, &row, NULL, reset);
}

static void build_sticks_page(void) {
    GtkWidget *g = sw_page(T(S_TAB_STICKS));
    int row = 0;

    grid_row(g, &row, T(S_DZ_LEFT),  sw_bind(new_scale(0, 50, 1), BIND_SCALE, CFG_OFF(dz_left)));
    grid_row(g, &row, T(S_DZ_RIGHT), sw_bind(new_scale(0, 50, 1), BIND_SCALE, CFG_OFF(dz_right)));
    grid_row(g, &row, NULL, hint_label(T(S_DZ_HINT)));
    grid_row(g, &row, NULL, sw_bind(new_check(T(S_INVERT_LY)), BIND_TOGGLE, CFG_OFF(invert_ly)));
    grid_row(g, &row, NULL, sw_bind(new_check(T(S_INVERT_RY)), BIND_TOGGLE, CFG_OFF(invert_ry)));
    grid_row(g, &row, NULL, sw_bind(new_check(T(S_SWAP)), BIND_TOGGLE, CFG_OFF(swap_sticks)));
    grid_row(g, &row, NULL, sw_bind(new_check(T(S_DIGITAL_TRIG)), BIND_TOGGLE, CFG_OFF(digital_triggers)));

    grid_row(g, &row, T(S_RUMBLE_LBL), sw_bind(new_scale(0, 100, 5), BIND_SCALE, CFG_OFF(rumble_pct)));
    grid_row(g, &row, NULL, hint_label(T(S_RUMBLE_HINT)));
}

static void build_gyro_page(void) {
    GtkWidget *g = sw_page(T(S_TAB_GYRO));
    const char *modes[N_GYRO_MODES] = { T(S_GYRO_OFF), T(S_GYRO_ALWAYS), T(S_GYRO_HOLD) };
    const char *outs[N_GYRO_OUTS]   = { T(S_GYRO_TO_STICK), T(S_GYRO_TO_MOUSE) };
    int row = 0;

    grid_row(g, &row, NULL, hint_label(T(S_GYRO_HINT)));
    grid_row(g, &row, T(S_GYRO_MODE),   sw_bind(new_combo(modes, N_GYRO_MODES), BIND_COMBO, CFG_OFF(gyro_mode)));
    grid_row(g, &row, T(S_GYRO_BUTTON), sw_bind(new_combo(SRC_LABELS, N_SRC), BIND_COMBO, CFG_OFF(gyro_button)));
    grid_row(g, &row, T(S_GYRO_OUTPUT), sw_bind(new_combo(outs, N_GYRO_OUTS), BIND_COMBO, CFG_OFF(gyro_output)));
    grid_row(g, &row, T(S_GYRO_SENS),   sw_bind(new_scale(10, 500, 5), BIND_SCALE, CFG_OFF(gyro_sens)));
    grid_row(g, &row, NULL, sw_bind(new_check(T(S_GYRO_INV_X)), BIND_TOGGLE, CFG_OFF(gyro_invert_x)));
    grid_row(g, &row, NULL, sw_bind(new_check(T(S_GYRO_INV_Y)), BIND_TOGGLE, CFG_OFF(gyro_invert_y)));

    sw.gyro_cal_btn = gtk_button_new_with_label(T(S_GYRO_CAL));
    gtk_widget_set_halign(sw.gyro_cal_btn, GTK_ALIGN_START);
    g_signal_connect(sw.gyro_cal_btn, "clicked", G_CALLBACK(on_gyro_calibrate), NULL);
    grid_row(g, &row, NULL, sw.gyro_cal_btn);
    sw.gyro_status = grid_row(g, &row, NULL, hint_label(""));
}

static void build_desktop_page(void) {
    GtkWidget *g = sw_page(T(S_TAB_DESKTOP));
    int row = 0;

    sw.desktop_check = grid_row(g, &row, NULL, new_check(T(S_DESKTOP_ENABLE)));
    g_signal_connect(sw.desktop_check, "toggled", G_CALLBACK(on_sw_desktop_toggled), NULL);
    sw.desktop_speed = grid_row(g, &row, T(S_DESKTOP_SPEED), new_scale(10, 300, 5));
    g_signal_connect(sw.desktop_speed, "value-changed", G_CALLBACK(on_sw_speed_changed), NULL);
    grid_row(g, &row, NULL, hint_label(T(S_DESKTOP_HELP)));
}

static void open_settings_window(int page) {
    GtkWidget *vbox, *hbox, *close;

    if (sw.win) {
        if (page >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(sw.notebook), page);
        gtk_window_present(GTK_WINDOW(sw.win));
        return;
    }
    memset(&sw, 0, sizeof sw);

    sw.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sw.win), T(S_SW_TITLE));
    gtk_window_set_icon_name(GTK_WINDOW(sw.win), "input-gaming");
    gtk_window_set_default_size(GTK_WINDOW(sw.win), 620, 600);
    gtk_window_set_position(GTK_WINDOW(sw.win), GTK_WIN_POS_CENTER);
    g_signal_connect(sw.win, "destroy", G_CALLBACK(on_settings_destroy), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    /* Profile bar: the tabs below edit the selected profile */
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(T(S_PROFILE)), FALSE, FALSE, 0);
    sw.profile_combo = gtk_combo_box_text_new();
    g_signal_connect(sw.profile_combo, "changed", G_CALLBACK(on_sw_profile_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), sw.profile_combo, TRUE, TRUE, 0);
    sw.new_btn = gtk_button_new_with_label(T(S_NEW));
    g_signal_connect(sw.new_btn, "clicked", G_CALLBACK(on_profile_new), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), sw.new_btn, FALSE, FALSE, 0);
    sw.rename_btn = gtk_button_new_with_label(T(S_RENAME));
    g_signal_connect(sw.rename_btn, "clicked", G_CALLBACK(on_profile_rename), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), sw.rename_btn, FALSE, FALSE, 0);
    sw.delete_btn = gtk_button_new_with_label(T(S_DELETE));
    g_signal_connect(sw.delete_btn, "clicked", G_CALLBACK(on_profile_delete), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), sw.delete_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    sw.proc_label = hint_label("");
    gtk_box_pack_start(GTK_BOX(vbox), sw.proc_label, FALSE, FALSE, 0);
    sw.proc_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw.proc_entry), "eldenring.exe, hl2_linux");
    gtk_entry_set_max_length(GTK_ENTRY(sw.proc_entry), sizeof g_profiles[0].processes - 1);
    g_signal_connect(sw.proc_entry, "changed", G_CALLBACK(on_proc_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), sw.proc_entry, FALSE, FALSE, 0);

    sw.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(sw.notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), sw.notebook, TRUE, TRUE, 0);
    build_general_page();
    build_buttons_page();
    build_sticks_page();
    build_gyro_page();
    build_desktop_page();

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    close = gtk_button_new_with_label(T(S_CLOSE));
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), sw.win);
    gtk_box_pack_end(GTK_BOX(hbox), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(sw.win), vbox);
    gtk_widget_show_all(sw.win);
    settings_window_sync();     /* after show_all: it also sets visibility */
    if (page >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(sw.notebook), page);
    sw_timer_cb(NULL);
    sw.timer = g_timeout_add(500, sw_timer_cb, NULL);
}

/* ------------------------------------------------------------------------- */
/*  Controller test window                                                   */
/* ------------------------------------------------------------------------- */

static struct {
    GtkWidget *win, *combo, *label, *area;
    guint      timer;
    int        slot;
} tw;

static int tw_is_open(void) { return tw.win != NULL; }

static void tw_close(void) {
    if (tw.win) gtk_widget_destroy(tw.win);
}

static void draw_text(cairo_t *cr, GtkWidget *w, double x, double y, const char *text, int center) {
    PangoLayout *layout = gtk_widget_create_pango_layout(w, text);
    int tw_, th;
    pango_layout_get_pixel_size(layout, &tw_, &th);
    cairo_move_to(cr, center ? x - tw_ / 2.0 : x, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void set_rgba(cairo_t *cr, const GdkRGBA *c, double alpha) {
    cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha * alpha);
}

static void draw_stick(cairo_t *cr, GtkWidget *w, const GdkRGBA *fg, double cx, double cy, double r,
                       float rx, float ry, float ox, float oy, int dz, const char *name) {
    char text[96];

    set_rgba(cr, fg, 0.12);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, cx - r, cy); cairo_line_to(cr, cx + r, cy);
    cairo_move_to(cr, cx, cy - r); cairo_line_to(cr, cx, cy + r);
    cairo_stroke(cr);

    if (dz > 0) {
        cairo_set_source_rgba(cr, 0.95, 0.60, 0.10, 0.25);
        cairo_arc(cr, cx, cy, r * dz / 100.0, 0, 2 * G_PI);
        cairo_fill(cr);
    }

    set_rgba(cr, fg, 0.4);
    cairo_set_line_width(cr, 2);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_stroke(cr);

    set_rgba(cr, fg, 0.85);
    cairo_arc(cr, cx + rx * r, cy + ry * r, 8, 0, 2 * G_PI);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.20, 0.52, 0.95);
    cairo_arc(cr, cx + ox * r, cy + oy * r, 5, 0, 2 * G_PI);
    cairo_fill(cr);

    set_rgba(cr, fg, 1.0);
    snprintf(text, sizeof text, "%s  %+.2f %+.2f", name, rx, ry);
    draw_text(cr, w, cx, cy + r + 16, text, 1);
}

static void draw_trigger(cairo_t *cr, GtkWidget *w, const GdkRGBA *fg, double x, double y,
                         double width, int raw, int out, const char *name) {
    char text[32];
    set_rgba(cr, fg, 0.15);
    cairo_rectangle(cr, x, y, width, 12);
    cairo_fill(cr);
    set_rgba(cr, fg, 0.45);
    cairo_rectangle(cr, x, y, width * raw / 255.0, 12);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.20, 0.52, 0.95);
    cairo_rectangle(cr, x, y + 9, width * out / 255.0, 3);
    cairo_fill(cr);
    set_rgba(cr, fg, 1.0);
    snprintf(text, sizeof text, "%s %3d", name, raw);
    draw_text(cr, w, x + width + 8, y + 6, text, 0);
}

static gboolean tw_draw(GtkWidget *w, cairo_t *cr, gpointer user_data) {
    struct slot_status s;
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    double width = gtk_widget_get_allocated_width(w);
    const double r = 80, cy = 140;
    const struct profile_cfg *p = active_cfg();
    int slot = clampi(tw.slot, 0, MAX_PADS - 1);

    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
    pthread_mutex_lock(&status_mutex);
    s = g_slots[slot];
    pthread_mutex_unlock(&status_mutex);

    draw_trigger(cr, w, &fg, 20, 14, 150, s.src[SRC_L2], s.out_trig[0], "L2");
    draw_trigger(cr, w, &fg, width - 230, 14, 150, s.src[SRC_R2], s.out_trig[1], "R2");

    /* Physical sticks with their own deadzone; the filled dot is the
     * matching output stick (they differ when sticks are swapped). */
    draw_stick(cr, w, &fg, 30 + r, cy, r, s.raw[0], s.raw[1], s.out[0], s.out[1], p->dz_left, "L");
    draw_stick(cr, w, &fg, width - 30 - r, cy, r, s.raw[2], s.raw[3], s.out[2], s.out[3], p->dz_right, "R");

    /* Button grid between the sticks */
    const int cols = 4;
    const double cw = 56, chh = 28, gap = 6;
    double x0 = width / 2 - (cols * cw + (cols - 1) * gap) / 2, y0 = 50;
    for (int i = 0; i < N_SRC; i++) {
        double x = x0 + (i % cols) * (cw + gap), y = y0 + (i / cols) * (chh + gap);
        if (s.src[i] > TRIGGER_PRESS) cairo_set_source_rgb(cr, 0.20, 0.70, 0.35);
        else                          set_rgba(cr, &fg, 0.12);
        cairo_rectangle(cr, x, y, cw, chh);
        cairo_fill(cr);
        set_rgba(cr, &fg, 1.0);
        draw_text(cr, w, x + cw / 2, y + chh / 2, SRC_SHORT[i], 1);
    }

    if (s.has_motion) {
        char text[128];
        snprintf(text, sizeof text, "%s  X %+7.1f   Y %+7.1f   Z %+7.1f",
                 T(S_TW_GYRO), s.gyro[0], s.gyro[1], s.gyro[2]);
        set_rgba(cr, &fg, 1.0);
        draw_text(cr, w, width / 2, cy + r + 44, text, 1);
    }
    return FALSE;
}

static gboolean tw_tick(gpointer user_data) {
    struct slot_status s;
    char text[320];
    int slot = clampi(tw.slot, 0, MAX_PADS - 1);

    pthread_mutex_lock(&status_mutex);
    s = g_slots[slot];
    pthread_mutex_unlock(&status_mutex);

    if (!atomic_load(&g_bridge_running))
        g_strlcpy(text, T(S_TW_STOPPED), sizeof text);
    else if (slot >= g_threads_started)
        snprintf(text, sizeof text, T(S_TW_INACTIVE), slot + 1);
    else if (s.error)
        snprintf(text, sizeof text, T(S_PAD_ERROR), slot + 1, g_strerror(s.error));
    else if (s.connected)
        snprintf(text, sizeof text, "P%d: %s%s", slot + 1, s.name, g_desktop_mode ? T(S_TW_DESKTOP) : "");
    else
        snprintf(text, sizeof text, T(S_PAD_WAITING), slot + 1);

    if (strcmp(gtk_label_get_text(GTK_LABEL(tw.label)), text) != 0)
        gtk_label_set_text(GTK_LABEL(tw.label), text);
    gtk_widget_queue_draw(tw.area);
    return G_SOURCE_CONTINUE;
}

static void on_tw_slot_changed(GtkComboBox *combo, gpointer user_data) {
    tw.slot = gtk_combo_box_get_active(combo);
}

static void on_tw_destroy(GtkWidget *w, gpointer user_data) {
    if (tw.timer) g_source_remove(tw.timer);
    memset(&tw, 0, sizeof tw);
}

static void open_test_window(void) {
    static const char *const slots[MAX_PADS] = { "P1", "P2", "P3", "P4" };
    GtkWidget *vbox, *hbox;

    if (tw.win) {
        gtk_window_present(GTK_WINDOW(tw.win));
        return;
    }
    memset(&tw, 0, sizeof tw);

    tw.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(tw.win), T(S_TW_TITLE));
    gtk_window_set_icon_name(GTK_WINDOW(tw.win), "input-gaming");
    gtk_window_set_default_size(GTK_WINDOW(tw.win), 720, 420);
    gtk_window_set_position(GTK_WINDOW(tw.win), GTK_WIN_POS_CENTER);
    g_signal_connect(tw.win, "destroy", G_CALLBACK(on_tw_destroy), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(T(S_TW_CONTROLLER)), FALSE, FALSE, 0);
    tw.combo = new_combo(slots, MAX_PADS);
    gtk_combo_box_set_active(GTK_COMBO_BOX(tw.combo), 0);
    g_signal_connect(tw.combo, "changed", G_CALLBACK(on_tw_slot_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), tw.combo, FALSE, FALSE, 0);
    tw.label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(tw.label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(tw.label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(hbox), tw.label, TRUE, TRUE, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    tw.area = gtk_drawing_area_new();
    gtk_widget_set_size_request(tw.area, 640, 300);
    g_signal_connect(tw.area, "draw", G_CALLBACK(tw_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), tw.area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hint_label(T(S_TW_LEGEND)), FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(tw.win), vbox);
    gtk_widget_show_all(tw.win);
    tw_tick(NULL);
    tw.timer = g_timeout_add(33, tw_tick, NULL);
}

/* ------------------------------------------------------------------------- */
/*  Log viewer (follows the file while open)                                 */
/* ------------------------------------------------------------------------- */

static struct {
    GtkWidget *win, *view;
    guint      timer;
    off_t      size;
    time_t     mtime;
} lv;

static int lv_is_open(void) { return lv.win != NULL; }

static void lv_close(void) {
    if (lv.win) gtk_widget_destroy(lv.win);
}

static gchar *read_log_tail(void) {
    gchar *data = NULL;
    gsize len = 0;

    if (!g_file_get_contents(log_file_path, &data, &len, NULL) || len == 0) {
        g_free(data);
        return g_strdup(T(S_LV_EMPTY));
    }

    /* Keep the last LOG_VIEW_LINES lines */
    const char *p = data + len;
    int lines = 0;
    if (p > data && p[-1] == '\n') p--;
    while (p > data) {
        if (p[-1] == '\n' && ++lines >= LOG_VIEW_LINES) break;
        p--;
    }

    gchar *tail = g_utf8_make_valid(p, -1);
    g_free(data);
    return tail;
}

static void lv_reload(int force) {
    struct stat st;
    off_t size = 0;
    time_t mtime = 0;

    if (stat(log_file_path, &st) == 0) {
        size = st.st_size;
        mtime = st.st_mtime;
    }
    if (!force && size == lv.size && mtime == lv.mtime) return;
    lv.size = size;
    lv.mtime = mtime;

    gchar *text = read_log_tail();
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(lv.view));
    gtk_text_buffer_set_text(buffer, text, -1);
    g_free(text);

    // Scroll to the bottom of the logs
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buffer, &iter);
    GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "end");
    if (!mark) mark = gtk_text_buffer_create_mark(buffer, "end", &iter, FALSE);
    else       gtk_text_buffer_move_mark(buffer, mark, &iter);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(lv.view), mark, 0.0, FALSE, 0, 0);
}

static gboolean lv_tick(gpointer user_data) {
    lv_reload(0);
    return G_SOURCE_CONTINUE;
}

static void on_lv_clear(GtkButton *button, gpointer user_data) {
    char old[PATH_MAX + 4];
    snprintf(old, sizeof old, "%s.1", log_file_path);

    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen(log_file_path, "w");
    if (f) fclose(f);
    (void)unlink(old);
    pthread_mutex_unlock(&log_mutex);
    lv_reload(1);
}

static void on_lv_open_folder(GtkButton *button, gpointer user_data) {
    gchar *dir = g_path_get_dirname(log_file_path);
    gchar *uri = g_filename_to_uri(dir, NULL, NULL);
    GError *err = NULL;
    if (uri && !g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        logmsg(LOG_WARNING, "Could not open %s: %s", dir, err->message);
        g_error_free(err);
    }
    g_free(uri);
    g_free(dir);
}

static void on_lv_destroy(GtkWidget *w, gpointer user_data) {
    if (lv.timer) g_source_remove(lv.timer);
    memset(&lv, 0, sizeof lv);
}

static void open_log_window(void) {
    GtkWidget *vbox, *hbox, *scrolled, *button;

    if (lv.win) {
        gtk_window_present(GTK_WINDOW(lv.win));
        return;
    }
    memset(&lv, 0, sizeof lv);

    lv.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lv.win), T(S_LV_TITLE));
    gtk_window_set_icon_name(GTK_WINDOW(lv.win), "input-gaming");
    gtk_window_set_default_size(GTK_WINDOW(lv.win), 760, 440);
    gtk_window_set_position(GTK_WINDOW(lv.win), GTK_WIN_POS_CENTER);
    g_signal_connect(lv.win, "destroy", G_CALLBACK(on_lv_destroy), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);

    scrolled = gtk_scrolled_window_new(NULL, NULL);
    lv.view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(lv.view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(lv.view), TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), lv.view);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    button = gtk_button_new_with_label(T(S_LV_CLEAR));
    g_signal_connect(button, "clicked", G_CALLBACK(on_lv_clear), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    button = gtk_button_new_with_label(T(S_LV_OPEN_FOLDER));
    g_signal_connect(button, "clicked", G_CALLBACK(on_lv_open_folder), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    button = gtk_button_new_with_label(T(S_CLOSE));
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_widget_destroy), lv.win);
    gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(lv.win), vbox);
    gtk_widget_show_all(lv.win);
    lv_reload(1);
    lv.timer = g_timeout_add(1000, lv_tick, NULL);
}

static gboolean rebuild_ui_idle(gpointer user_data) {
    int page = -1;
    int had_test = tw_is_open(), had_log = lv_is_open();

    if (sw.win) {
        page = gtk_notebook_get_current_page(GTK_NOTEBOOK(sw.notebook));
        gtk_widget_destroy(sw.win);
        open_settings_window(page);
    }
    if (had_test) { tw_close(); open_test_window(); }
    if (had_log)  { lv_close(); open_log_window(); }
    build_tray_menu();
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/*  Main Entry                                                               */
/* ------------------------------------------------------------------------- */

static void init_paths(void) {
    gchar *cfg_dir = g_build_filename(g_get_user_config_dir(), "ds4-bridge", NULL);
    gchar *spath = g_build_filename(cfg_dir, "settings.ini", NULL);
    const char *state = g_getenv("XDG_STATE_HOME");
    gchar *state_dir = (state && g_path_is_absolute(state))
        ? g_build_filename(state, "ds4-bridge", NULL)
        : g_build_filename(g_get_home_dir(), ".local", "state", "ds4-bridge", NULL);
    gchar *lpath = g_build_filename(state_dir, "ds4_bridge.log", NULL);

    if (g_mkdir_with_parents(cfg_dir, 0755) != 0)
        fprintf(stderr, "Cannot create %s: %s\n", cfg_dir, g_strerror(errno));
    if (g_mkdir_with_parents(state_dir, 0755) != 0)
        fprintf(stderr, "Cannot create %s: %s\n", state_dir, g_strerror(errno));
    g_strlcpy(settings_file_path, spath, sizeof settings_file_path);
    g_strlcpy(log_file_path, lpath, sizeof log_file_path);

    g_free(lpath);
    g_free(state_dir);
    g_free(spath);
    g_free(cfg_dir);
}

/* Two trays would fight over the same controllers */
static int acquire_single_instance(void) {
    gchar *path = g_build_filename(g_get_user_runtime_dir(), "ds4-bridge-tray.lock", NULL);
    g_lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    g_free(path);
    if (g_lock_fd < 0) return 1;    /* can't tell, don't block startup */
    return flock(g_lock_fd, LOCK_EX | LOCK_NB) == 0;
}

static gboolean on_unix_signal(gpointer user_data) {
    quit_app(NULL, NULL);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    int auto_start_bridge = 0;

    gtk_init(&argc, &argv);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--start") == 0)
            auto_start_bridge = 1;
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            g_verbose = 1;
    }

    init_paths();
    load_settings();

    if (!acquire_single_instance()) {
        logmsg(LOG_WARNING, "Another ds4_bridge_tray is already running, exiting");
        send_notification("DS4 Bridge", T(S_N_ALREADY), "input-gaming");
        return EXIT_SUCCESS;
    }
    publish_config();

    /* Refresh the autostart entry so it always points at the current binary
     * location (e.g. if the program was moved or rebuilt elsewhere). */
    if (autostart_is_enabled()) {
        GError *err = NULL;
        if (!autostart_enable(&err)) {
            logmsg(LOG_WARNING, "Could not refresh autostart entry: %s", err->message);
            g_error_free(err);
        }
    }

    tray_init();
    build_tray_menu();
    if (status_icon) g_timeout_add_seconds(3, tray_check_cb, NULL);

    if (auto_start_bridge)
        start_bridge();
    update_status_cb(NULL);
    g_timeout_add_seconds(2, update_status_cb, NULL);
    g_timeout_add_seconds(3, auto_profile_cb, NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);

    gtk_main();

    return EXIT_SUCCESS;
}
