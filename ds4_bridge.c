/*
 * ds4_bridge.c — Sony DualShock 4  ->  virtual "Microsoft X-Box 360 pad" bridge
 *
 * Uses only standard C library + POSIX/Linux headers
 * (no SDL / glib / libevdev).
 *
 * Compilation:
 *     gcc -O2 ds4_bridge.c -o ds4_bridge
 *
 * Usage:
 *     ./ds4_bridge            # runs in foreground, logs to stderr
 *     ./ds4_bridge -d         # background daemon (fork + setsid), logs to syslog/journal
 *     ./ds4_bridge -n         # do not grab physical DS4 exclusively (games see both devices)
 *     ./ds4_bridge -v         # verbose (debug) logs
 *
 * Architecture:
 *
 *    [DS4 /dev/input/eventN] --(EV_KEY/EV_ABS)--> bridge --(write)--> [/dev/uinput: X360]
 *    [DS4 /dev/input/eventN] <--(EVIOCSFF/EV_FF)-- bridge <--(EV_UINPUT/EV_FF)-- [game]
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
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/input.h>
#include <linux/uinput.h>

#if !defined(UI_DEV_SETUP) || !defined(UI_ABS_SETUP)
#error "This program requires uinput v5+ (Linux 4.5+) headers: UI_DEV_SETUP / UI_ABS_SETUP not found."
#endif

/* ------------------------------------------------------------------------- */
/*  Configuration constants                                                   */
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
/*  Bit array helpers (EVIOCGBIT results)                                     */
/* ------------------------------------------------------------------------- */

#define BITS_PER_LONG      (sizeof(unsigned long) * 8)
#define NLONGS(nbits)      (((nbits) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

#define ARRAY_LEN(a)       (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------------- */
/*  Button and axis mappings (DS4 -> Xbox 360)                                */
/* ------------------------------------------------------------------------- */

struct key_map {
    unsigned short src;   /* DS4 code (hid-playstation / hid-sony) */
    unsigned short dst;   /* virtual Xbox 360 code                 */
};

static const struct key_map KEY_MAP[] = {
    { BTN_SOUTH,  BTN_A      },   /* Cross            -> A           */
    { BTN_EAST,   BTN_B      },   /* Circle           -> B           */
    { BTN_WEST,   BTN_X      },   /* Square           -> X           */
    { BTN_NORTH,  BTN_Y      },   /* Triangle         -> Y           */
    { BTN_TL,     BTN_TL     },   /* L1               -> LB          */
    { BTN_TR,     BTN_TR     },   /* R1               -> RB          */
    { BTN_SELECT, BTN_SELECT },   /* Share            -> Back        */
    { BTN_START,  BTN_START  },   /* Options          -> Start       */
    { BTN_MODE,   BTN_MODE   },   /* PS               -> Guide       */
    { BTN_THUMBL, BTN_THUMBL },   /* L3               -> LS click    */
    { BTN_THUMBR, BTN_THUMBR },   /* R3               -> RS click    */
};
#define N_KEYS ARRAY_LEN(KEY_MAP)

enum axis_kind { AXIS_STICK, AXIS_TRIGGER, AXIS_HAT };

struct axis_map {
    unsigned short src;
    unsigned short dst;
    enum axis_kind kind;
};

static const struct axis_map AXIS_MAP[] = {
    { ABS_X,     ABS_X,     AXIS_STICK   },  /* left stick X                    */
    { ABS_Y,     ABS_Y,     AXIS_STICK   },  /* left stick Y (up = negative)    */
    { ABS_RX,    ABS_RX,    AXIS_STICK   },  /* right stick X                   */
    { ABS_RY,    ABS_RY,    AXIS_STICK   },  /* right stick Y                   */
    { ABS_Z,     ABS_Z,     AXIS_TRIGGER },  /* L2 analog -> LT                 */
    { ABS_RZ,    ABS_RZ,    AXIS_TRIGGER },  /* R2 analog -> RT                 */
    { ABS_HAT0X, ABS_HAT0X, AXIS_HAT     },  /* D-pad left/right                */
    { ABS_HAT0Y, ABS_HAT0Y, AXIS_HAT     },  /* D-pad up/down                   */
};
#define N_AXES ARRAY_LEN(AXIS_MAP)

/* ------------------------------------------------------------------------- */
/*  Bridge global state                                                       */
/* ------------------------------------------------------------------------- */

struct bridge {
    int  ufd;                              /* /dev/uinput (virtual X360)         */
    int  dfd;                              /* physical DS4 evdev, -1 = offline   */
    int  ds4_writable;                     /* is DS4 O_RDWR (for FF)             */
    int  ds4_has_rumble;                   /* does DS4 support FF_RUMBLE         */
    int  ds4_has_gain;                     /* does DS4 support FF_GAIN           */
    int  grab;                             /* should use EVIOCGRAB               */
    int  grabbed;                          /* is currently grabbed               */
    int  syn_dropped;                      /* awaiting resync after SYN_DROPPED  */
    char dev_path[300];
    char dev_name[256];

    struct input_absinfo src_abs[N_AXES];  /* DS4 axis limits (for scaling)      */
    int  has_abs[N_AXES];

    /* Force-feedback: virtual effect id -> physical DS4 effect id map */
    struct ff_effect ff_cache[BRIDGE_FF_SLOTS];
    int  ff_cached[BRIDGE_FF_SLOTS];
    int  ff_phys[BRIDGE_FF_SLOTS];
    int  ff_playing[BRIDGE_FF_SLOTS];

    struct input_event out[OUT_MAX];
    int  nout;
};

/* ------------------------------------------------------------------------- */
/*  Globals (for signal handling & logging)                                   */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static int g_sigpipe[2] = { -1, -1 };
static int g_use_syslog = 0;
static int g_verbose = 0;
static int g_deadzone_pct = 0;

/* ------------------------------------------------------------------------- */
/*  Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logmsg(int prio, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logmsg(int prio, const char *fmt, ...)
{
    va_list ap;

    if (prio == LOG_DEBUG && !g_verbose)
        return;

    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(prio, fmt, ap);
    } else {
        char ts[16] = "";
        time_t t = time(NULL);
        struct tm tm;
        const char *tag;

        if (localtime_r(&t, &tm))
            strftime(ts, sizeof ts, "%H:%M:%S", &tm);

        switch (prio) {
        case LOG_ERR:     tag = "ERROR"; break;
        case LOG_WARNING: tag = "WARN "; break;
        case LOG_DEBUG:   tag = "DEBUG"; break;
        default:          tag = "INFO "; break;
        }
        fprintf(stderr, "[%s] %s ", ts, tag);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/*  Small helpers                                                             */
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
        if (i == nl)
            return 1;
    }
    return 0;
}

static int set_nonblock_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    fl = fcntl(fd, F_GETFD);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static int scale_axis(int v, const struct input_absinfo *s, int dmin, int dmax)
{
    long long range = (long long)s->maximum - s->minimum;
    long long num;

    if (range <= 0)
        return v;
    if (v < s->minimum) v = s->minimum;
    if (v > s->maximum) v = s->maximum;

    num = ((long long)v - s->minimum) * ((long long)dmax - dmin);
    return (int)(dmin + (num + range / 2) / range);
}

/* ------------------------------------------------------------------------- */
/*  Write events to virtual device                                            */
/* ------------------------------------------------------------------------- */

static void out_flush(struct bridge *b)
{
    const char *p = (const char *)b->out;
    size_t left = (size_t)b->nout * sizeof(struct input_event);

    while (left > 0) {
        ssize_t w = write(b->ufd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
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

    if (b->nout >= OUT_MAX)
        out_flush(b);

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
/*  Map lookups and translations                                              */
/* ------------------------------------------------------------------------- */

static int find_key(unsigned short code)
{
    size_t i;
    for (i = 0; i < N_KEYS; i++)
        if (KEY_MAP[i].src == code)
            return (int)i;
    return -1;
}

static int find_axis(unsigned short code)
{
    size_t i;
    for (i = 0; i < N_AXES; i++)
        if (AXIS_MAP[i].src == code)
            return (int)i;
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
            if (val > -dz && val < dz)
                val = 0;
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
/*  Virtual Xbox 360 controller creation (/dev/uinput)                        */
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
        logmsg(LOG_ERR, "Failed to open /dev/uinput: %s (run as root or add udev 'uaccess' rules)", strerror(errno));
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

    {
        char sysname[64] = "";
        if (ioctl(fd, UI_GET_SYSNAME(sizeof sysname), sysname) >= 0)
            logmsg(LOG_INFO, "Virtual device created: \"%s\" [%04x:%04x] (/sys/devices/virtual/input/%s)",
                   VIRT_NAME, VIRT_VENDOR, VIRT_PRODUCT, sysname);
        else
            logmsg(LOG_INFO, "Virtual device created: \"%s\" [%04x:%04x]",
                   VIRT_NAME, VIRT_VENDOR, VIRT_PRODUCT);
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
/*  Force feedback handling                                                   */
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
        if (errno != EINTR)
            break;
    }
    logmsg(LOG_DEBUG, "Failed to write EV_FF to DS4: %s", strerror(errno));
}

static void handle_ff_upload(struct bridge *b, unsigned int request_id)
{
    struct uinput_ff_upload up;
    int vid;

    memset(&up, 0, sizeof up);
    up.request_id = request_id;

    if (ioctl(b->ufd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
        logmsg(LOG_WARNING, "UI_BEGIN_FF_UPLOAD error: %s", strerror(errno));
        return;
    }

    vid = up.effect.id;
    if (vid < 0 || vid >= BRIDGE_FF_SLOTS || up.effect.type != FF_RUMBLE) {
        up.retval = -EINVAL;
    } else {
        b->ff_cache[vid]  = up.effect;
        b->ff_cached[vid] = 1;
        ff_upload_to_ds4(b, vid);
        up.retval = 0;
        logmsg(LOG_DEBUG, "FF upload: v-id=%d strong=%u weak=%u length=%ums -> DS4 id=%d",
               vid, (unsigned)up.effect.u.rumble.strong_magnitude,
               (unsigned)up.effect.u.rumble.weak_magnitude,
               (unsigned)up.effect.replay.length, b->ff_phys[vid]);
    }

    if (ioctl(b->ufd, UI_END_FF_UPLOAD, &up) < 0)
        logmsg(LOG_WARNING, "UI_END_FF_UPLOAD error: %s", strerror(errno));
}

static void handle_ff_erase(struct bridge *b, unsigned int request_id)
{
    struct uinput_ff_erase er;
    int vid;

    memset(&er, 0, sizeof er);
    er.request_id = request_id;

    if (ioctl(b->ufd, UI_BEGIN_FF_ERASE, &er) < 0) {
        logmsg(LOG_WARNING, "UI_BEGIN_FF_ERASE error: %s", strerror(errno));
        return;
    }

    vid = (int)er.effect_id;
    if (vid >= 0 && vid < BRIDGE_FF_SLOTS) {
        if (b->dfd >= 0 && b->ff_phys[vid] >= 0) {
            if (ioctl(b->dfd, EVIOCRMFF, b->ff_phys[vid]) < 0)
                logmsg(LOG_DEBUG, "EVIOCRMFF (DS4) error: %s", strerror(errno));
        }
        b->ff_phys[vid]    = -1;
        b->ff_cached[vid]  = 0;
        b->ff_playing[vid] = 0;
        logmsg(LOG_DEBUG, "FF erase: v-id=%d", vid);
    }
    er.retval = 0;

    if (ioctl(b->ufd, UI_END_FF_ERASE, &er) < 0)
        logmsg(LOG_WARNING, "UI_END_FF_ERASE error: %s", strerror(errno));
}

static void handle_ff_event(struct bridge *b, unsigned short code, int value)
{
    if (code == FF_GAIN) {
        if (b->dfd >= 0 && b->ds4_has_gain)
            ds4_write_ff(b, FF_GAIN, value);
        return;
    }

    if (code >= BRIDGE_FF_SLOTS)
        return;

    b->ff_playing[code] = value;

    if (b->dfd >= 0 && b->ff_phys[code] >= 0) {
        ds4_write_ff(b, (unsigned short)b->ff_phys[code], value);
        logmsg(LOG_DEBUG, "FF play: v-id=%u -> DS4 id=%d value=%d", code, b->ff_phys[code], value);
    }
}

static void ff_restore_on_connect(struct bridge *b)
{
    int i;
    ff_reset_phys(b);
    if (!b->ds4_has_rumble)
        return;

    for (i = 0; i < BRIDGE_FF_SLOTS; i++) {
        if (!b->ff_cached[i])
            continue;
        ff_upload_to_ds4(b, i);
        if (b->ff_phys[i] >= 0 && b->ff_playing[i] > 0 && b->ff_cache[i].replay.length == 0)
            ds4_write_ff(b, (unsigned short)b->ff_phys[i], b->ff_playing[i]);
    }
}

static void ff_stop_all_on_ds4(struct bridge *b)
{
    int i;
    if (b->dfd < 0 || !b->ds4_has_rumble)
        return;
    for (i = 0; i < BRIDGE_FF_SLOTS; i++)
        if (b->ff_phys[i] >= 0)
            ds4_write_ff(b, (unsigned short)b->ff_phys[i], 0);
}

/* ------------------------------------------------------------------------- */
/*  DS4 Discovery and connection                                              */
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
    unsigned long evbits[NLONGS(EV_CNT)];
    unsigned long keybits[NLONGS(KEY_CNT)];
    unsigned long absbits[NLONGS(ABS_CNT)];
    unsigned long props[NLONGS(INPUT_PROP_CNT)];

    memset(evbits, 0, sizeof evbits);
    memset(keybits, 0, sizeof keybits);
    memset(absbits, 0, sizeof absbits);
    memset(props, 0, sizeof props);

    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
        return 0;
    if (!TEST_BIT(EV_KEY, evbits) || !TEST_BIT(EV_ABS, evbits))
        return 0;

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0)
        return 0;

    (void)ioctl(fd, EVIOCGPROP(sizeof props), props);

#ifdef INPUT_PROP_ACCELEROMETER
    if (TEST_BIT(INPUT_PROP_ACCELEROMETER, props))
        return 0;
#endif
    if (TEST_BIT(INPUT_PROP_POINTER, props) ||
        TEST_BIT(BTN_TOUCH, keybits) ||
        TEST_BIT(ABS_MT_POSITION_X, absbits))
        return 0;

    return TEST_BIT(BTN_SOUTH, keybits) && TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits);
}

static void resync_from_ds4(struct bridge *b)
{
    unsigned long keys[NLONGS(KEY_CNT)];
    size_t i;

    b->nout = 0;
    memset(keys, 0, sizeof keys);

    if (ioctl(b->dfd, EVIOCGKEY(sizeof keys), keys) >= 0) {
        for (i = 0; i < N_KEYS; i++)
            out_push(b, EV_KEY, KEY_MAP[i].dst, (int)TEST_BIT(KEY_MAP[i].src, keys));
    }

    for (i = 0; i < N_AXES; i++) {
        struct input_absinfo ai;
        if (!b->has_abs[i])
            continue;
        if (ioctl(b->dfd, EVIOCGABS(AXIS_MAP[i].src), &ai) < 0)
            continue;
        b->src_abs[i] = ai;
        out_push(b, EV_ABS, AXIS_MAP[i].dst, translate_axis(b, (int)i, ai.value));
    }
    out_syn(b);
}

static int open_candidate(const char *path, int *writable)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        *writable = 1;
        return fd;
    }
    if (errno == EACCES || errno == EPERM) {
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            *writable = 0;
            return fd;
        }
    }
    return -1;
}

static int attach_ds4(struct bridge *b, int fd, int writable, const char *path, const char *name)
{
    unsigned long absbits[NLONGS(ABS_CNT)];
    unsigned long ffbits[NLONGS(FF_CNT)];
    struct input_id id;
    size_t i;

    b->dfd = fd;
    b->ds4_writable = writable;
    b->syn_dropped = 0;
    b->grabbed = 0;
    snprintf(b->dev_path, sizeof b->dev_path, "%s", path);
    snprintf(b->dev_name, sizeof b->dev_name, "%s", name);

    memset(absbits, 0, sizeof absbits);
    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    for (i = 0; i < N_AXES; i++) {
        b->has_abs[i] = 0;
        if (TEST_BIT(AXIS_MAP[i].src, absbits) &&
            ioctl(fd, EVIOCGABS(AXIS_MAP[i].src), &b->src_abs[i]) == 0)
            b->has_abs[i] = 1;
    }

    memset(ffbits, 0, sizeof ffbits);
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
            logmsg(LOG_DEBUG, "EVIOCGRAB failed (%s) - device %s might be used by another instance, skipping.", strerror(errno), path);
            return -1;
        }
    }

    memset(&id, 0, sizeof id);
    (void)ioctl(fd, EVIOCGID, &id);

    logmsg(LOG_INFO, "DS4 connected: \"%s\" (%s) [%04x:%04x, bus=0x%02x] rumble=%s%s",
           name, path, id.vendor, id.product, id.bustype,
           b->ds4_has_rumble ? "yes" : "no",
           writable ? "" : " (read-only mode)");

    ff_restore_on_connect(b);
    resync_from_ds4(b);
    
    return 0;
}

static int connect_ds4(struct bridge *b)
{
    DIR *dir;
    struct dirent *de;
    int rc = -1;

    dir = opendir("/dev/input");
    if (!dir) {
        logmsg(LOG_WARNING, "Failed to open /dev/input: %s", strerror(errno));
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        char path[300];
        char name[256];
        int fd, writable = 0;

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);

        fd = open_candidate(path, &writable);
        if (fd < 0)
            continue;

        memset(name, 0, sizeof name);
        if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0 ||
            !name_matches_ds4(name) ||
            !is_gamepad_node(fd)) {
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
    if (b->dfd < 0)
        return;

    logmsg(LOG_WARNING, "DS4 disconnected (%s): \"%s\" - waiting for reconnect...",
           reason, b->dev_name);

    if (b->grabbed)
        (void)ioctl(b->dfd, EVIOCGRAB, 0);
    close(b->dfd);
    b->dfd = -1;
    b->grabbed = 0;
    b->ds4_has_rumble = 0;
    b->ds4_has_gain = 0;
    ff_reset_phys(b);

    neutralize_virtual(b);
}

/* ------------------------------------------------------------------------- */
/*  Event handling loop                                                       */
/* ------------------------------------------------------------------------- */

static int handle_ds4_input(struct bridge *b)
{
    struct input_event evs[EV_BATCH];

    for (;;) {
        ssize_t n = read(b->dfd, evs, sizeof evs);
        size_t cnt, k;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n == 0)
            return -1;

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
                    logmsg(LOG_DEBUG, "SYN_DROPPED - resyncing");
                } else if (ev->code == SYN_REPORT) {
                    out_syn(b);
                }
                break;
            case EV_KEY:
                if (ev->value == 2)
                    break;
                idx = find_key(ev->code);
                if (idx >= 0)
                    out_push(b, EV_KEY, KEY_MAP[idx].dst, ev->value ? 1 : 0);
                break;
            case EV_ABS:
                idx = find_axis(ev->code);
                if (idx >= 0)
                    out_push(b, EV_ABS, AXIS_MAP[idx].dst, translate_axis(b, idx, ev->value));
                break;
            default:
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
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            logmsg(LOG_ERR, "uinput read error: %s", strerror(errno));
            return -1;
        }
        if (n == 0)
            return 0;

        cnt = (size_t)n / sizeof evs[0];
        for (k = 0; k < cnt; k++) {
            const struct input_event *ev = &evs[k];

            if (ev->type == EV_UINPUT) {
                if (ev->code == UI_FF_UPLOAD)
                    handle_ff_upload(b, (unsigned int)ev->value);
                else if (ev->code == UI_FF_ERASE)
                    handle_ff_erase(b, (unsigned int)ev->value);
            } else if (ev->type == EV_FF) {
                handle_ff_event(b, ev->code, ev->value);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Signals & daemonize                                                       */
/* ------------------------------------------------------------------------- */

static void on_signal(int sig)
{
    int saved = errno;
    (void)sig;

    g_stop = 1;
    if (g_sigpipe[1] >= 0) {
        ssize_t r = write(g_sigpipe[1], "x", 1);
        (void)r;
    }
    errno = saved;
}

static int setup_signals(void)
{
    struct sigaction sa;

    if (pipe(g_sigpipe) < 0)
        return -1;
    if (set_nonblock_cloexec(g_sigpipe[0]) < 0 || set_nonblock_cloexec(g_sigpipe[1]) < 0)
        return -1;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT,  &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        sigaction(SIGHUP,  &sa, NULL) < 0)
        return -1;

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    return 0;
}

static int daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;

    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    umask(0);
    if (chdir("/") < 0) return -1;

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    openlog("ds4_bridge", LOG_PID, LOG_DAEMON);
    g_use_syslog = 1;
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-d] [-n] [-v] [-z percent] [-h]\n"
            "  -d   Run as background daemon (logs to syslog/journal)\n"
            "  -n   Do not grab DS4 exclusively (no EVIOCGRAB)\n"
            "  -v   Verbose (debug) logs\n"
            "  -z N Analog stick deadzone percentage (e.g. 15 for 15%%)\n"
            "  -h   Show this help\n", prog);
}

/* ------------------------------------------------------------------------- */
/*  main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    struct bridge b;
    int opt, daemon_mode = 0, exit_code = EXIT_SUCCESS;
    long long last_scan = 0;
    int first_scan = 1;

    memset(&b, 0, sizeof b);
    b.ufd  = -1;
    b.dfd  = -1;
    b.grab = 1;
    ff_reset_phys(&b);

    while ((opt = getopt(argc, argv, "dnvz:h")) != -1) {
        switch (opt) {
        case 'd': daemon_mode = 1; break;
        case 'n': b.grab = 0;      break;
        case 'v': g_verbose = 1;   break;
        case 'z': g_deadzone_pct = atoi(optarg); break;
        case 'h': usage(argv[0]);  return EXIT_SUCCESS;
        default:  usage(argv[0]);  return EXIT_FAILURE;
        }
    }

    if (daemon_mode && daemonize() < 0) {
        fprintf(stderr, "daemonize failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (setup_signals() < 0) {
        logmsg(LOG_ERR, "Failed to setup signals: %s", strerror(errno));
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    b.ufd = create_virtual_pad();
    if (b.ufd < 0) {
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    logmsg(LOG_INFO, "Scanning for DS4... (Press Ctrl+C to stop)");

    while (!g_stop) {
        struct pollfd pfd[3];
        int nfds = 0, idx_sig, idx_ui, idx_ds4 = -1;
        int timeout = -1;
        int r;

        if (b.dfd < 0) {
            long long now = now_ms();
            if (first_scan || now - last_scan >= RESCAN_INTERVAL_MS) {
                first_scan = 0;
                last_scan  = now;
                connect_ds4(&b);
            }
            if (b.dfd < 0) {
                long long left = RESCAN_INTERVAL_MS - (now_ms() - last_scan);
                timeout = left > 0 ? (int)left : 0;
            }
        }

        idx_sig = nfds;
        pfd[nfds].fd = g_sigpipe[0]; pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        idx_ui = nfds;
        pfd[nfds].fd = b.ufd;        pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        if (b.dfd >= 0) {
            idx_ds4 = nfds;
            pfd[nfds].fd = b.dfd;    pfd[nfds].events = POLLIN; pfd[nfds].revents = 0; nfds++;
        }

        r = poll(pfd, (nfds_t)nfds, timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            logmsg(LOG_ERR, "poll error: %s", strerror(errno));
            exit_code = EXIT_FAILURE;
            break;
        }
        if (r == 0)
            continue;

        if (pfd[idx_sig].revents & POLLIN) {
            char buf[32];
            while (read(g_sigpipe[0], buf, sizeof buf) > 0)
                ;
            if (g_stop)
                break;
        }

        if (pfd[idx_ui].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            logmsg(LOG_ERR, "uinput fd error (revents=0x%x)", pfd[idx_ui].revents);
            exit_code = EXIT_FAILURE;
            break;
        }
        if (pfd[idx_ui].revents & POLLIN) {
            if (handle_uinput_events(&b) < 0) {
                exit_code = EXIT_FAILURE;
                break;
            }
        }

        if (idx_ds4 >= 0 && b.dfd >= 0 && pfd[idx_ds4].revents) {
            if (pfd[idx_ds4].revents & POLLIN) {
                if (handle_ds4_input(&b) < 0)
                    disconnect_ds4(&b, strerror(errno));
            }
            if (b.dfd >= 0 && (pfd[idx_ds4].revents & (POLLERR | POLLHUP | POLLNVAL)))
                disconnect_ds4(&b, "POLLHUP/POLLERR");
            if (b.dfd < 0)
                last_scan = now_ms();
        }
    }

    logmsg(LOG_INFO, "Stopping, cleaning up resources...");

cleanup:
    if (b.dfd >= 0) {
        ff_stop_all_on_ds4(&b);
        if (b.grabbed)
            (void)ioctl(b.dfd, EVIOCGRAB, 0);
        close(b.dfd);
        b.dfd = -1;
    }

    if (b.ufd >= 0) {
        neutralize_virtual(&b);
        if (ioctl(b.ufd, UI_DEV_DESTROY) < 0)
            logmsg(LOG_WARNING, "UI_DEV_DESTROY error: %s", strerror(errno));
        close(b.ufd);
        b.ufd = -1;
    }

    if (g_sigpipe[0] >= 0) close(g_sigpipe[0]);
    if (g_sigpipe[1] >= 0) close(g_sigpipe[1]);
    g_sigpipe[0] = g_sigpipe[1] = -1;

    logmsg(LOG_INFO, "Exiting.");
    if (g_use_syslog)
        closelog();

    return exit_code;
}
