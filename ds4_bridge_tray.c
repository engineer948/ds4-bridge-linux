/*
 * ds4_bridge_tray.c — Sony DualShock 4 to virtual Xbox 360 pad bridge
 * with GTK3 & AppIndicator Tray Interface.
 *
 * Compilation:
 *   gcc -O2 -std=gnu11 ds4_bridge_tray.c -o ds4_bridge_tray \
 *       $(pkg-config --cflags --libs gtk+-3.0 ayatana-appindicator3-0.1) \
 *       -lpthread
 *
 *   If ayatana-appindicator is not available, use the legacy one:
 *   gcc -O2 -std=gnu11 -DUSE_LEGACY_APPINDICATOR ds4_bridge_tray.c -o ds4_bridge_tray \
 *       $(pkg-config --cflags --libs gtk+-3.0 appindicator3-0.1) \
 *       -lpthread
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include <pthread.h>
#include <stdatomic.h>

#include <gtk/gtk.h>

#ifdef USE_LEGACY_APPINDICATOR
#include <libappindicator/app-indicator.h>
#else
#include <libayatana-appindicator/app-indicator.h>
#endif

#if !defined(UI_DEV_SETUP) || !defined(UI_ABS_SETUP)
#error "This program requires uinput v5+ (Linux 4.5+) headers: UI_DEV_SETUP / UI_ABS_SETUP not found."
#endif

/* ------------------------------------------------------------------------- */
/*  Configuration constants                                                  */
/* ------------------------------------------------------------------------- */

#define VIRT_NAME          "Microsoft X-Box 360 pad"
#define VIRT_VENDOR        0x045e   /* Microsoft                                  */
#define VIRT_PRODUCT       0x028e   /* Xbox 360 Controller                        */
#define VIRT_VERSION       0x0110   /* xpad version (for SDL GUID compatibility)  */
#define VIRT_PHYS          "ds4-bridge/input0"

#define BRIDGE_FF_SLOTS    16       /* max concurrent effects virtual device holds*/
#define RESCAN_INTERVAL_MS 1000     /* interval to scan for controller if missing */
#define EV_BATCH           64       /* max events read at once via read()         */
#define OUT_MAX            128      /* max events written to virtual device       */

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
/*  Button and axis mappings (DS4 -> Xbox 360)                               */
/* ------------------------------------------------------------------------- */

struct key_map {
    unsigned short src;   /* DS4 code (hid-playstation / hid-sony) */
    unsigned short dst;   /* virtual Xbox 360 code                 */
};

static const struct key_map KEY_MAP[] = {
    { BTN_SOUTH,  BTN_A      },
    { BTN_EAST,   BTN_B      },
    { BTN_WEST,   BTN_X      },
    { BTN_NORTH,  BTN_Y      },
    { BTN_TL,     BTN_TL     },
    { BTN_TR,     BTN_TR     },
    { BTN_SELECT, BTN_SELECT },
    { BTN_START,  BTN_START  },
    { BTN_MODE,   BTN_MODE   },
    { BTN_THUMBL, BTN_THUMBL },
    { BTN_THUMBR, BTN_THUMBR },
};
#define N_KEYS ARRAY_LEN(KEY_MAP)

enum axis_kind { AXIS_STICK, AXIS_TRIGGER, AXIS_HAT };

struct axis_map {
    unsigned short src;
    unsigned short dst;
    enum axis_kind kind;
};

static const struct axis_map AXIS_MAP[] = {
    { ABS_X,     ABS_X,     AXIS_STICK   },
    { ABS_Y,     ABS_Y,     AXIS_STICK   },
    { ABS_RX,    ABS_RX,    AXIS_STICK   },
    { ABS_RY,    ABS_RY,    AXIS_STICK   },
    { ABS_Z,     ABS_Z,     AXIS_TRIGGER },
    { ABS_RZ,    ABS_RZ,    AXIS_TRIGGER },
    { ABS_HAT0X, ABS_HAT0X, AXIS_HAT     },
    { ABS_HAT0Y, ABS_HAT0Y, AXIS_HAT     },
};
#define N_AXES ARRAY_LEN(AXIS_MAP)

/* ------------------------------------------------------------------------- */
/*  Bridge state structures                                                  */
/* ------------------------------------------------------------------------- */

struct bridge {
    int  ufd;
    int  dfd;
    int  ds4_writable;
    int  ds4_has_rumble;
    int  ds4_has_gain;
    int  grab;
    int  grabbed;
    int  syn_dropped;
    char dev_path[300];
    char dev_name[256];

    struct input_absinfo src_abs[N_AXES];
    int  has_abs[N_AXES];

    struct ff_effect ff_cache[BRIDGE_FF_SLOTS];
    int  ff_cached[BRIDGE_FF_SLOTS];
    int  ff_phys[BRIDGE_FF_SLOTS];
    int  ff_playing[BRIDGE_FF_SLOTS];

    struct input_event out[OUT_MAX];
    int  nout;
};

struct thread_arg {
    int id;
    volatile atomic_int *stop_flag;
};

/* ------------------------------------------------------------------------- */
/*  Globals                                                                  */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_logs_enabled = 1;
static char log_file_path[1024] = "";
static int g_verbose = 0;
static int g_deadzone_pct = 0;

static AppIndicator *indicator;
static GtkWidget *menu;
static GtkWidget *status_item;
static GtkWidget *battery_item;
static GtkWidget *start_item;
static GtkWidget *stop_item;
static GtkWidget *controller_item;
static GtkWidget *log_toggle;

static atomic_int g_bridge_running = 0;
static atomic_int g_bridge_stop_requested = 0;
static pthread_t bridge_threads[4];
static struct thread_arg thread_args[4];
static int num_controllers = 1;
static int low_battery_notified = 0;

/* ------------------------------------------------------------------------- */
/*  Logging & Forward Declarations                                           */
/* ------------------------------------------------------------------------- */

static gboolean update_status_cb(gpointer user_data);

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
        strftime(ts, sizeof ts, "%H:%M:%S", &tm);

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
            fclose(f);
            va_end(ap);
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

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
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
/*  Write events to virtual device                                           */
/* ------------------------------------------------------------------------- */

static void out_flush(struct bridge *b)
{
    const char *p = (const char *)b->out;
    size_t left = (size_t)b->nout * sizeof(struct input_event);

    while (left > 0) {
        ssize_t w = write(b->ufd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            logmsg(LOG_WARNING, "uinput write error: %s", strerror(errno));
            break;
        }
        p    += w;
        left -= (size_t)w;
    }
    b->nout = 0;
}

static void out_push(struct bridge *b, unsigned short type, unsigned short code, int value)
{
    struct input_event *ev;
    if (b->nout >= OUT_MAX) out_flush(b);
    ev = &b->out[b->nout++];
    memset(ev, 0, sizeof *ev);
    ev->type  = type;
    ev->code  = code;
    ev->value = value;
}

static void out_syn(struct bridge *b)
{
    out_push(b, EV_SYN, SYN_REPORT, 0);
    out_flush(b);
}

/* ------------------------------------------------------------------------- */
/*  Map lookups and translations                                             */
/* ------------------------------------------------------------------------- */

static int find_key(unsigned short code)
{
    size_t i;
    for (i = 0; i < N_KEYS; i++)
        if (KEY_MAP[i].src == code) return (int)i;
    return -1;
}

static int find_axis(unsigned short code)
{
    size_t i;
    for (i = 0; i < N_AXES; i++)
        if (AXIS_MAP[i].src == code) return (int)i;
    return -1;
}

static int translate_axis(const struct bridge *b, int i, int v)
{
    int val = v;
    switch (AXIS_MAP[i].kind) {
    case AXIS_STICK:
        val = b->has_abs[i] ? scale_axis(v, &b->src_abs[i], STICK_MIN, STICK_MAX) : v;
        if (g_deadzone_pct > 0) {
            int dz = (STICK_MAX * g_deadzone_pct) / 100;
            if (val > -dz && val < dz) val = 0;
        }
        return val;
    case AXIS_TRIGGER:
        return b->has_abs[i] ? scale_axis(v, &b->src_abs[i], TRIG_MIN, TRIG_MAX) : v;
    case AXIS_HAT:
        return v < 0 ? -1 : (v > 0 ? 1 : 0);
    }
    return v;
}

static struct input_absinfo virt_absinfo(enum axis_kind k)
{
    struct input_absinfo a;
    memset(&a, 0, sizeof a);
    switch (k) {
    case AXIS_STICK:
        a.minimum = STICK_MIN; a.maximum = STICK_MAX;
        a.fuzz = STICK_FUZZ;   a.flat = STICK_FLAT;
        break;
    case AXIS_TRIGGER:
        a.minimum = TRIG_MIN;  a.maximum = TRIG_MAX;
        break;
    case AXIS_HAT:
        a.minimum = HAT_MIN;   a.maximum = HAT_MAX;
        break;
    }
    return a;
}

/* ------------------------------------------------------------------------- */
/*  Virtual Xbox 360 controller creation (/dev/uinput)                       */
/* ------------------------------------------------------------------------- */

static int create_virtual_pad(void)
{
    struct uinput_setup us;
    int fd, version = 0;
    size_t i;

    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT)
        fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        logmsg(LOG_ERR, "Failed to open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_GET_VERSION, &version) == 0 && version < 5) {
        logmsg(LOG_ERR, "uinput version %d is too old (needs >= 5)", version);
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_FF)  < 0) {
        logmsg(LOG_ERR, "UI_SET_EVBIT error: %s", strerror(errno));
        goto fail;
    }

    for (i = 0; i < N_KEYS; i++) {
        if (ioctl(fd, UI_SET_KEYBIT, KEY_MAP[i].dst) < 0) {
            logmsg(LOG_ERR, "UI_SET_KEYBIT(0x%x) error: %s", KEY_MAP[i].dst, strerror(errno));
            goto fail;
        }
    }

    for (i = 0; i < N_AXES; i++) {
        struct uinput_abs_setup as;
        memset(&as, 0, sizeof as);
        as.code    = AXIS_MAP[i].dst;
        as.absinfo = virt_absinfo(AXIS_MAP[i].kind);

        if (ioctl(fd, UI_SET_ABSBIT, AXIS_MAP[i].dst) < 0 ||
            ioctl(fd, UI_ABS_SETUP, &as) < 0) {
            logmsg(LOG_ERR, "UI_ABS_SETUP(0x%x) error: %s", AXIS_MAP[i].dst, strerror(errno));
            goto fail;
        }
    }

    if (ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) < 0 ||
        ioctl(fd, UI_SET_FFBIT, FF_GAIN)   < 0) {
        logmsg(LOG_ERR, "UI_SET_FFBIT error: %s", strerror(errno));
        goto fail;
    }

    (void)ioctl(fd, UI_SET_PHYS, VIRT_PHYS);

    memset(&us, 0, sizeof us);
    us.id.bustype   = BUS_USB;
    us.id.vendor    = VIRT_VENDOR;
    us.id.product   = VIRT_PRODUCT;
    us.id.version   = VIRT_VERSION;
    us.ff_effects_max = BRIDGE_FF_SLOTS;
    snprintf(us.name, sizeof us.name, "%s", VIRT_NAME);

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
    close(fd);
    return -1;
}

static void neutralize_virtual(struct bridge *b)
{
    size_t i;
    b->nout = 0;
    for (i = 0; i < N_KEYS; i++)
        out_push(b, EV_KEY, KEY_MAP[i].dst, 0);
    for (i = 0; i < N_AXES; i++)
        out_push(b, EV_ABS, AXIS_MAP[i].dst, 0);
    out_syn(b);
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

static void resync_from_ds4(struct bridge *b)
{
    unsigned long keys[NLONGS(KEY_CNT)] = {0};
    size_t i;

    b->nout = 0;
    if (ioctl(b->dfd, EVIOCGKEY(sizeof keys), keys) >= 0) {
        for (i = 0; i < N_KEYS; i++)
            out_push(b, EV_KEY, KEY_MAP[i].dst, (int)TEST_BIT(KEY_MAP[i].src, keys));
    }

    for (i = 0; i < N_AXES; i++) {
        struct input_absinfo ai;
        if (!b->has_abs[i]) continue;
        if (ioctl(b->dfd, EVIOCGABS(AXIS_MAP[i].src), &ai) < 0) continue;
        b->src_abs[i] = ai;
        out_push(b, EV_ABS, AXIS_MAP[i].dst, translate_axis(b, (int)i, ai.value));
    }
    out_syn(b);
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

static int attach_ds4(struct bridge *b, int fd, int writable, const char *path, const char *name)
{
    unsigned long absbits[NLONGS(ABS_CNT)] = {0};
    unsigned long ffbits[NLONGS(FF_CNT)] = {0};
    struct input_id id;
    size_t i;

    b->dfd = fd;
    b->ds4_writable = writable;
    b->syn_dropped = 0;
    b->grabbed = 0;
    snprintf(b->dev_path, sizeof b->dev_path, "%s", path);
    snprintf(b->dev_name, sizeof b->dev_name, "%s", name);

    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    for (i = 0; i < N_AXES; i++) {
        b->has_abs[i] = 0;
        if (TEST_BIT(AXIS_MAP[i].src, absbits) &&
            ioctl(fd, EVIOCGABS(AXIS_MAP[i].src), &b->src_abs[i]) == 0)
            b->has_abs[i] = 1;
    }

    b->ds4_has_rumble = 0;
    b->ds4_has_gain   = 0;
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof ffbits), ffbits) >= 0) {
        b->ds4_has_rumble = writable && TEST_BIT(FF_RUMBLE, ffbits);
        b->ds4_has_gain   = writable && TEST_BIT(FF_GAIN, ffbits);
    }

    if (b->grab) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0) {
            b->grabbed = 1;
        } else {
            return -1; // Another bridge likely grabbed it
        }
    }

    memset(&id, 0, sizeof id);
    (void)ioctl(fd, EVIOCGID, &id);

    logmsg(LOG_INFO, "DS4 connected: \"%s\" (%s) rumble=%s",
           name, path, b->ds4_has_rumble ? "yes" : "no");

    ff_restore_on_connect(b);
    resync_from_ds4(b);
    return 0;
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
            !name_matches_ds4(name) || !is_gamepad_node(fd)) {
            close(fd);
            continue;
        }

        if (attach_ds4(b, fd, writable, path, name) < 0) {
            close(fd);
            continue;
        }
        
        rc = 0;
        break;
    }
    closedir(dir);
    return rc;
}

static void disconnect_ds4(struct bridge *b, const char *reason)
{
    if (b->dfd < 0) return;
    logmsg(LOG_WARNING, "DS4 disconnected (%s): \"%s\" - waiting for reconnect...", reason, b->dev_name);
    if (b->grabbed) (void)ioctl(b->dfd, EVIOCGRAB, 0);
    close(b->dfd);
    b->dfd = -1;
    b->grabbed = 0;
    b->ds4_has_rumble = 0;
    b->ds4_has_gain = 0;
    ff_reset_phys(b);
    neutralize_virtual(b);
}

/* ------------------------------------------------------------------------- */
/*  Event handling loop                                                      */
/* ------------------------------------------------------------------------- */

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
                if (ev->code == SYN_DROPPED) {
                    b->syn_dropped = 1;
                    b->nout = 0;
                } else if (ev->code == SYN_REPORT) {
                    out_syn(b);
                }
                break;
            case EV_KEY:
                if (ev->value == 2) break;
                idx = find_key(ev->code);
                if (idx >= 0) out_push(b, EV_KEY, KEY_MAP[idx].dst, ev->value ? 1 : 0);
                break;
            case EV_ABS:
                idx = find_axis(ev->code);
                if (idx >= 0) out_push(b, EV_ABS, AXIS_MAP[idx].dst, translate_axis(b, idx, ev->value));
                break;
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

static void* bridge_thread_func(void *arg)
{
    struct thread_arg *targ = (struct thread_arg*)arg;
    struct bridge b;
    int first_scan = 1;
    long long last_scan = 0;
    
    memset(&b, 0, sizeof b);
    b.ufd = -1;
    b.dfd = -1;
    b.grab = 1;
    ff_reset_phys(&b);

    b.ufd = create_virtual_pad();
    if (b.ufd < 0) {
        logmsg(LOG_ERR, "Thread %d: Failed to create virtual pad", targ->id);
        return NULL;
    }

    logmsg(LOG_INFO, "Thread %d: Bridge active, scanning for DS4...", targ->id);

    while (!atomic_load(targ->stop_flag)) {
        struct pollfd pfd[2];
        int nfds = 0, idx_ui, idx_ds4 = -1;
        int timeout = 500; // Check stop_flag every 500ms max

        if (b.dfd < 0) {
            long long now = now_ms();
            if (first_scan || now - last_scan >= RESCAN_INTERVAL_MS) {
                first_scan = 0;
                last_scan = now;
                pthread_mutex_lock(&scan_mutex);
                connect_ds4(&b);
                pthread_mutex_unlock(&scan_mutex);
            }
            if (b.dfd < 0) {
                long long left = RESCAN_INTERVAL_MS - (now_ms() - last_scan);
                if (left >= 0 && left < timeout) timeout = (int)left;
            }
        }

        idx_ui = nfds;
        pfd[nfds].fd = b.ufd; pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        if (b.dfd >= 0) {
            idx_ds4 = nfds;
            pfd[nfds].fd = b.dfd; pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        }

        int r = poll(pfd, (nfds_t)nfds, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; // Timeout, re-eval loop conditions

        if (pfd[idx_ui].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pfd[idx_ui].revents & POLLIN) {
            if (handle_uinput_events(&b) < 0) break;
        }

        if (idx_ds4 >= 0 && b.dfd >= 0 && pfd[idx_ds4].revents) {
            if (pfd[idx_ds4].revents & POLLIN) {
                if (handle_ds4_input(&b) < 0)
                    disconnect_ds4(&b, strerror(errno));
            }
            if (b.dfd >= 0 && (pfd[idx_ds4].revents & (POLLERR | POLLHUP | POLLNVAL)))
                disconnect_ds4(&b, "POLLHUP/POLLERR");
            if (b.dfd < 0) last_scan = now_ms();
        }
    }

    logmsg(LOG_INFO, "Thread %d: Stopping and cleaning up resources...", targ->id);

    if (b.dfd >= 0) {
        ff_stop_all_on_ds4(&b);
        if (b.grabbed) (void)ioctl(b.dfd, EVIOCGRAB, 0);
        close(b.dfd);
    }
    if (b.ufd >= 0) {
        neutralize_virtual(&b);
        (void)ioctl(b.ufd, UI_DEV_DESTROY);
        close(b.ufd);
    }
    
    return NULL;
}

/* ------------------------------------------------------------------------- */
/*  Tray / UI Integration Logic                                              */
/* ------------------------------------------------------------------------- */

static void stop_bridge(void) {
    if (!atomic_load(&g_bridge_running)) return;
    
    atomic_store(&g_bridge_stop_requested, 1);
    for (int i = 0; i < num_controllers; i++) {
        pthread_join(bridge_threads[i], NULL);
    }
    atomic_store(&g_bridge_running, 0);
}

static void start_bridge(void) {
    if (atomic_load(&g_bridge_running)) return;
    
    atomic_store(&g_bridge_stop_requested, 0);
    for (int i = 0; i < num_controllers; i++) {
        thread_args[i].id = i + 1;
        thread_args[i].stop_flag = &g_bridge_stop_requested;
        pthread_create(&bridge_threads[i], NULL, bridge_thread_func, &thread_args[i]);
    }
    atomic_store(&g_bridge_running, 1);
}

static int get_ds4_battery(void) {
    DIR *dir = opendir("/sys/class/power_supply/");
    if (!dir) return -1;
    
    struct dirent *de;
    int battery = -1;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "BAT", 3) == 0 || 
            strncmp(de->d_name, "AC", 2) == 0 || 
            strstr(de->d_name, "macsmc")) {
            continue;
        }
        
        char cap_path[512];
        snprintf(cap_path, sizeof(cap_path), "/sys/class/power_supply/%s/capacity", de->d_name);
        
        if (access(cap_path, F_OK) == 0) {
            int is_device = 0;
            char scope_path[512];
            snprintf(scope_path, sizeof(scope_path), "/sys/class/power_supply/%s/scope", de->d_name);
            FILE *fs = fopen(scope_path, "r");
            if (fs) {
                char buf[128] = {0};
                if (fgets(buf, sizeof(buf), fs) && strstr(buf, "Device"))
                    is_device = 1;
                fclose(fs);
            }
            
            char name_lower[256];
            strncpy(name_lower, de->d_name, sizeof(name_lower));
            for(int i=0; name_lower[i]; i++) name_lower[i] = tolower((unsigned char)name_lower[i]);
            
            if (is_device || strstr(name_lower, "sony") || 
                strstr(name_lower, "controller") || strstr(name_lower, "hid")) {
                FILE *fc = fopen(cap_path, "r");
                if (fc) {
                    if (fscanf(fc, "%d", &battery) == 1) {
                        fclose(fc);
                        break;
                    }
                    fclose(fc);
                }
            }
        }
    }
    closedir(dir);
    return battery;
}

static void send_notification(const char *title, const char *msg) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "notify-send -i battery-low \"%s\" \"%s\"", title, msg);
    g_spawn_command_line_async(cmd, NULL);
}

static gboolean update_status_cb(gpointer user_data) {
    gboolean running = atomic_load(&g_bridge_running);
    
    if (running) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), "Status: Running \xF0\x9F\x9F\xA2"); // 🟢
        app_indicator_set_icon(indicator, "input-gaming");
        gtk_widget_set_sensitive(start_item, FALSE);
        gtk_widget_set_sensitive(stop_item, TRUE);
    } else {
        gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), "Status: Stopped \xF0\x9F\x94\xB4"); // 🔴
        app_indicator_set_icon(indicator, "media-playback-pause");
        gtk_widget_set_sensitive(start_item, TRUE);
        gtk_widget_set_sensitive(stop_item, FALSE);
    }
    
    int battery = get_ds4_battery();
    if (battery >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Battery: %d%% \xF0\x9F\x94\x8B", battery); // 🔋
        gtk_menu_item_set_label(GTK_MENU_ITEM(battery_item), buf);
        
        if (battery <= 20 && !low_battery_notified) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Your controller battery is very low (%d%%). Please charge it.", battery);
            send_notification("🎮 DS4 Low Battery!", msg);
            low_battery_notified = 1;
        } else if (battery > 20) {
            low_battery_notified = 0;
        }
    } else {
        gtk_menu_item_set_label(GTK_MENU_ITEM(battery_item), "Battery: Not Found");
        low_battery_notified = 0;
    }
    
    return G_SOURCE_CONTINUE;
}

static void on_start_clicked(GtkMenuItem *item, gpointer user_data) {
    start_bridge();
    update_status_cb(NULL);
}

static void on_stop_clicked(GtkMenuItem *item, gpointer user_data) {
    stop_bridge();
    update_status_cb(NULL);
}

static void on_controller_count_changed(GtkRadioMenuItem *item, gpointer user_data) {
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item))) {
        int count = GPOINTER_TO_INT(user_data);
        if (count != num_controllers) {
            int was_running = atomic_load(&g_bridge_running);
            if (was_running) stop_bridge();
            
            num_controllers = count;
            char label[64];
            snprintf(label, sizeof(label), "\xF0\x9F\x8E\xAE Controller Count (%d)", count); // 🎮
            gtk_menu_item_set_label(GTK_MENU_ITEM(controller_item), label);
            
            if (was_running) {
                start_bridge();
                update_status_cb(NULL);
            }
        }
    }
}

static void on_logs_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    g_logs_enabled = gtk_check_menu_item_get_active(item);
}

static void show_logs_clicked(GtkMenuItem *item, gpointer user_data) {
    char *logs = malloc(32768);
    if (!logs) return;
    logs[0] = '\0';

    FILE *f = fopen(log_file_path, "r");
    if (f) {
        char *lines[50] = {0};
        int idx = 0;
        int count = 0;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (lines[idx]) free(lines[idx]);
            lines[idx] = strdup(line);
            idx = (idx + 1) % 50;
            count++;
        }
        fclose(f);
        
        if (count == 0) {
            strcpy(logs, "Logs are empty.");
        } else {
            int start = (count < 50) ? 0 : idx;
            int to_read = (count < 50) ? count : 50;
            for (int i = 0; i < to_read; i++) {
                int curr = (start + i) % 50;
                if (lines[curr]) {
                    strncat(logs, lines[curr], 32768 - strlen(logs) - 1);
                    free(lines[curr]);
                }
            }
        }
    } else {
        strcpy(logs, "Logs are empty or disabled.");
    }
    
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "DS4 Bridge Logs");
    gtk_window_set_default_size(GTK_WINDOW(win), 600, 400);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    
    // Provide a monospaced font
    PangoFontDescription *font_desc = pango_font_description_from_string("Monospace 10");
    gtk_widget_override_font(textview, font_desc);
    pango_font_description_free(font_desc);
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    gtk_text_buffer_set_text(buffer, logs, -1);
    free(logs);
    
    gtk_container_add(GTK_CONTAINER(scrolled), textview);
    gtk_container_add(GTK_CONTAINER(win), scrolled);
    gtk_widget_show_all(win);
    
    // Scroll to the bottom of the logs
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buffer, &iter);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &iter, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(textview), mark, 0.0, FALSE, 0, 0);
}

static void quit_app(GtkMenuItem *item, gpointer user_data) {
    if (access(log_file_path, F_OK) == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, 0,
                                                   GTK_MESSAGE_QUESTION,
                                                   GTK_BUTTONS_YES_NO,
                                                   "Closing DS4 Bridge");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "Do you want to delete the log file before quitting?\n(This keeps your PC clean)");
        
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        
        if (response == GTK_RESPONSE_YES) {
            unlink(log_file_path);
        }
    }
    
    stop_bridge();
    gtk_main_quit();
}

static void setup_tray(void) {
    indicator = app_indicator_new("ds4-bridge-tray", "input-gaming", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    
    menu = gtk_menu_new();
    
    status_item = gtk_menu_item_new_with_label("Status: Checking...");
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), status_item);
    
    battery_item = gtk_menu_item_new_with_label("Battery: Not Found");
    gtk_widget_set_sensitive(battery_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), battery_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    start_item = gtk_menu_item_new_with_label("\xE2\x96\xB6 Start Bridge"); // ▶
    g_signal_connect(start_item, "activate", G_CALLBACK(on_start_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), start_item);
    
    stop_item = gtk_menu_item_new_with_label("\xE2\x8F\xB9 Stop Bridge"); // ⏹
    g_signal_connect(stop_item, "activate", G_CALLBACK(on_stop_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), stop_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    controller_item = gtk_menu_item_new_with_label("\xF0\x9F\x8E\xAE Controller Count (1)"); // 🎮
    GtkWidget *controller_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(controller_item), controller_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), controller_item);
    
    GSList *group = NULL;
    for (int i = 1; i <= 4; i++) {
        char label[32];
        snprintf(label, sizeof(label), "%d Controller%s", i, i > 1 ? "s" : "");
        GtkWidget *radio = gtk_radio_menu_item_new_with_label(group, label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        if (i == 1) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
        g_signal_connect(radio, "toggled", G_CALLBACK(on_controller_count_changed), GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(controller_menu), radio);
    }
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    log_toggle = gtk_check_menu_item_new_with_label("\xF0\x9F\x93\x9D Enable Logs (Write logs)"); // 📝
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(log_toggle), TRUE);
    g_signal_connect(log_toggle, "toggled", G_CALLBACK(on_logs_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), log_toggle);
    
    GtkWidget *view_log_item = gtk_menu_item_new_with_label("\xF0\x9F\x93\x84 View Logs"); // 📄
    g_signal_connect(view_log_item, "activate", G_CALLBACK(show_logs_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), view_log_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    GtkWidget *quit_item = gtk_menu_item_new_with_label("\xE2\x9D\x8C Quit"); // ❌
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_app), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    
    gtk_widget_show_all(menu);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
}

/* ------------------------------------------------------------------------- */
/*  Main Entry                                                               */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(log_file_path, sizeof(log_file_path), "%s/ds4_bridge.log", cwd);
    } else {
        strcpy(log_file_path, "ds4_bridge.log");
    }
    
    setup_tray();
    update_status_cb(NULL);
    g_timeout_add_seconds(2, update_status_cb, NULL);
    
    gtk_main();
    
    return EXIT_SUCCESS;
}
