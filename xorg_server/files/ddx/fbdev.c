/*
 * Phoenix-RTOS kdrive fbdev DDX — a minimal software X server backend that
 * paints onto the Raspberry Pi 4 HDMI framebuffer exposed at /dev/fb0.
 *
 * Why this file exists: xorg-server >= 1.17 dropped the kdrive "Xfbdev" backend
 * (hw/kdrive/fbdev). This is a fresh, small reimplementation on the surviving
 * kdrive core (hw/kdrive/src), written against the Phoenix /dev/fb0 ABI rather
 * than the Linux fbdev ABI. It is modelled structurally on the Xephyr backend
 * (hw/kdrive/ephyr/ephyr*.c) — same KdCardFuncs lifecycle — but the "host" is
 * the bare framebuffer device, not another X server.
 *
 * Framebuffer model (Phoenix-specific, see sources/.../video/rpi4-fb):
 *   - Geometry comes from the RPI4FB_GETMODE devctl (width/height/bpp/pitch).
 *   - /dev/fb0 supports read()/write() at a file offset but NOT a live
 *     mmap(fd, 0) backing (issue #149). So we always use a SHADOW framebuffer
 *     (kdrive's KdShadowFbAlloc): X renders into ordinary RAM, and on each
 *     damage update we lseek()+write() the dirty scanlines out to /dev/fb0.
 *
 * Input is wired to the Phoenix USB HID devices /dev/kbd0 (8-byte boot-keyboard
 * reports) and /dev/mouse0 (4-byte boot-mouse packets). See the input-driver
 * section below for the HID-usage -> evdev-keycode mapping and the
 * timer-drained HID input model.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS. %LICENSE%
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <phoenix/fbcon.h>   /* FBCONSETMODE / FBCON_DISABLED|ENABLED — silence the HDMI text console */

#include "kdrive.h"
#include "fb.h"
#include "shadow.h"

/*
 * /dev/fb0 client ABI (mirror of sources/phoenix-rtos-devices/video/rpi4-fb/
 * rpi4-fb.h). Kept self-contained so the X cross-build needs no path into the
 * device tree; keep in sync if that ABI changes.
 */
typedef struct {
    unsigned short width;
    unsigned short height;
    unsigned short bpp;
    unsigned short pitch;
    unsigned long long smemlen;
    unsigned long long framebuffer;
} rpi4fb_mode_t;

#define RPI4FB_GETMODE _IOR('g', 1, rpi4fb_mode_t)

#define FBDEV_DEVICE "/dev/fb0"

/* Per-card state: the open fd + the geometry reported by the device. */
typedef struct _FbdevPriv {
    int fd;
    rpi4fb_mode_t mode;
} FbdevPriv;

/* Per-screen state. */
typedef struct _FbdevScrPriv {
    Rotation randr;
    Bool shadow;
} FbdevScrPriv;

extern KdCardFuncs fbdevFuncs;  /* defined at end of file */

static Bool
fbdevInitialize(KdCardInfo *card, FbdevPriv *priv)
{
    priv->fd = open(FBDEV_DEVICE, O_RDWR);
    if (priv->fd < 0) {
        ErrorF("[fbdev] cannot open %s: %s\n", FBDEV_DEVICE, strerror(errno));
        return FALSE;
    }
    if (ioctl(priv->fd, RPI4FB_GETMODE, &priv->mode) < 0) {
        ErrorF("[fbdev] RPI4FB_GETMODE failed on %s: %s\n",
               FBDEV_DEVICE, strerror(errno));
        close(priv->fd);
        priv->fd = -1;
        return FALSE;
    }
    ErrorF("[fbdev] %s: %ux%u bpp=%u pitch=%u smemlen=%llu\n",
           FBDEV_DEVICE, priv->mode.width, priv->mode.height,
           priv->mode.bpp, priv->mode.pitch,
           (unsigned long long) priv->mode.smemlen);
    return TRUE;
}

/*
 * The HDMI text console (pl011-tty fbcon + the kernel klog mirror) keeps drawing
 * to /dev/fb0. While the X server owns the framebuffer that overdraws our output:
 * any stderr/klog line (e.g. a client's locale warning) gets painted on top of the
 * rendered screen, and the once-per-damage shadow blit loses to the console redraw
 * — exactly the problem GLQuake hit (pl_phoenix_vid.c::console_setmode). So we tell
 * the console to stop drawing (FBCON_DISABLED) while X owns the display, and restore
 * it (FBCON_ENABLED) at server teardown. Disabling fbcon's text mode is also what
 * frees /dev/kbd0 for the input driver (the console holds it in text mode).
 */
static int fbdev_ttyfd = -1;

static void
fbdevConsoleSetMode(int mode)
{
    if (fbdev_ttyfd < 0) {
        fbdev_ttyfd = open("/dev/tty0", O_RDWR);
        if (fbdev_ttyfd < 0)
            fbdev_ttyfd = open("/dev/console", O_RDWR);
    }
    if (fbdev_ttyfd >= 0) {
        if (ioctl(fbdev_ttyfd, FBCONSETMODE, mode) == 0)
            ErrorF("[fbdev] HDMI console fbcon mode -> %d\n", mode);
        else
            ErrorF("[fbdev] FBCONSETMODE(%d) failed (console may overdraw)\n", mode);
    }
    else {
        ErrorF("[fbdev] no /dev/tty0|console to switch fbcon mode\n");
    }
}

static Bool
fbdevCardInit(KdCardInfo *card)
{
    FbdevPriv *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return FALSE;

    if (!fbdevInitialize(card, priv)) {
        free(priv);
        return FALSE;
    }
    /* Claim the framebuffer: stop the text console from overdrawing X. */
    fbdevConsoleSetMode(FBCON_DISABLED);
    card->driver = priv;
    return TRUE;
}

/*
 * Translate the device's bpp into the X depth/visual masks. The Pi 4 fb is
 * 32 bpp, 24-bit colour (BGRX in memory → standard X TrueColor masks).
 */
static Bool
fbdevScreenInitialize(KdScreenInfo *screen, FbdevScrPriv *scrpriv)
{
    FbdevPriv *priv = screen->card->driver;
    int depth, bpp;
    Pixel allbits;

    if (!screen->width || !screen->height) {
        screen->width = priv->mode.width;
        screen->height = priv->mode.height;
    }
    /* Conservative physical size if RandR/-screen gave none (96 dpi). */
    if (!screen->width_mm)
        screen->width_mm = (screen->width * 254) / 960;
    if (!screen->height_mm)
        screen->height_mm = (screen->height * 254) / 960;

    bpp = priv->mode.bpp;
    if (bpp == 32) {
        depth = 24;
        /*
         * The Pi firmware framebuffer is RGB byte order, not the x86-typical
         * BGRX: plo sets mailbox SET_PIXEL_ORDER=1 (RGB) when it allocates the
         * surface (sources/plo/hal/aarch64/generic/video.c). So in memory R is
         * at byte 0, G at byte 1, B at byte 2 — i.e. red occupies the LOW mask
         * bits and blue the high. Using the BGRX masks here swaps R<->B on every
         * write to /dev/fb0 (a chosen deep blue renders as red/orange; greys are
         * unaffected since R==G==B) — the same channel-order mismatch the GLQuake
         * scanout path had to correct (#19).
         */
        screen->fb.redMask = 0x000000ff;
        screen->fb.greenMask = 0x0000ff00;
        screen->fb.blueMask = 0x00ff0000;
    }
    else if (bpp == 16) {
        depth = 16;
        screen->fb.redMask = 0x0000f800;
        screen->fb.greenMask = 0x000007e0;
        screen->fb.blueMask = 0x0000001f;
    }
    else {
        ErrorF("[fbdev] unsupported bpp %d\n", bpp);
        return FALSE;
    }

    screen->fb.depth = depth;
    screen->fb.bitsPerPixel = bpp;
    screen->fb.visuals = (1 << TrueColor);
    allbits = screen->fb.redMask | screen->fb.greenMask | screen->fb.blueMask;
    (void) allbits;

    scrpriv->randr = screen->randr;

    /* No hardware cursor backend (initCursor is NULL) → use the software
     * cursor (mi) so the dix cursor path never dereferences a HW cursor. */
    screen->softCursor = TRUE;

    /*
     * Always shadow: /dev/fb0 has no live mmap backing (issue #149), so X must
     * draw into RAM and we flush to the device. KdShadowFbAlloc sets
     * screen->fb.frameBuffer + byteStride (= width*bpp/8, which for 1024x32
     * equals the device pitch 4096, so the blit is a straight copy).
     */
    screen->fb.shadow = FALSE;
    if (!KdShadowFbAlloc(screen, FALSE))
        return FALSE;
    scrpriv->shadow = TRUE;

    return TRUE;
}

static Bool
fbdevScreenInit(KdScreenInfo *screen)
{
    FbdevScrPriv *scrpriv = calloc(1, sizeof(*scrpriv));
    if (!scrpriv)
        return FALSE;
    screen->driver = scrpriv;
    if (!fbdevScreenInitialize(screen, scrpriv)) {
        screen->driver = NULL;
        free(scrpriv);
        return FALSE;
    }
    return TRUE;
}

/*
 * Blit a band of scanlines [y0, y1) from the shadow buffer out to /dev/fb0.
 * The shadow buffer stride equals the device pitch, so each scanline maps to a
 * contiguous device offset row*pitch. We write whole rows (full-width); this is
 * correct and simple. (A future optimisation can write only a damaged X
 * sub-extent per row.) Used by both the damage-driven flush (damaged Y span)
 * and the periodic full-screen flush (0..height).
 */
static void
fbdevFlushRegion(ScreenPtr pScreen, int y0, int y1)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    FbdevPriv *priv = screen->card->driver;
    int pitch = priv->mode.pitch;
    int fbHeight = priv->mode.height;
    CARD8 *shadow = (CARD8 *) screen->fb.frameBuffer;
    int shadowStride = screen->fb.byteStride;
    int rows;
    off_t off;
    size_t len;
    ssize_t w;

    if (!shadow || priv->fd < 0)
        return;

    if (y0 < 0)
        y0 = 0;
    if (y1 > fbHeight)
        y1 = fbHeight;
    if (y1 <= y0)
        return;
    rows = y1 - y0;

    /*
     * Shadow stride and device pitch are equal here, so the band is a single
     * contiguous region in both buffers: write it in one syscall.
     */
    if (shadowStride == pitch) {
        off = (off_t) y0 * pitch;
        len = (size_t) rows * pitch;
        if (lseek(priv->fd, off, SEEK_SET) == (off_t) -1)
            return;
        while (len > 0) {
            w = write(priv->fd, shadow + off, len);
            if (w <= 0)
                break;
            off += w;
            len -= (size_t) w;
        }
    }
    else {
        /* Strides differ (rotation/reflection): write row by row. Copy only the
         * lesser of the two strides so a narrower shadow row is never over-read
         * (the surplus of a wider device pitch is off-screen padding). */
        int rowbytes = (shadowStride < pitch) ? shadowStride : pitch;
        int y;
        for (y = y0; y < y1; y++) {
            off = (off_t) y * pitch;
            if (lseek(priv->fd, off, SEEK_SET) == (off_t) -1)
                break;
            (void) write(priv->fd, shadow + (off_t) y * shadowStride,
                         (size_t) rowbytes);
        }
    }
}

/*
 * Damage flush: write the damaged Y span of the shadow buffer out to /dev/fb0.
 * We coalesce to the damaged Y extent and write whole rows (full-width). This is
 * the low-latency path: animated content (xeyes, the cursor) flushes promptly as
 * it draws. Static content is covered by the periodic full-screen flush below.
 */
/*
 * Set by the damage path (fbdevShadowUpdate) each time it blits a damaged band,
 * cleared by the periodic timer. It lets the periodic full-screen flush SKIP its
 * 8 MB blit whenever the damage path already pushed pixels this interval — so
 * during active use (continuous damage) the dispatch thread never eats the full
 * flush, and the periodic blit only runs as an idle safety net to catch static
 * content that produced no damage. (Single dispatch thread sets+clears it, so no
 * locking — same thread as both the damage update and this timer.)
 */
static int fbdevDirtySinceTick = 0;

static void
fbdevShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    FbdevPriv *priv = screen->card->driver;
    RegionPtr damage = DamageRegion(pBuf->pDamage);
    BoxPtr extents;

    if (RegionNotEmpty(damage)) {
        extents = RegionExtents(damage);
        fbdevFlushRegion(pScreen, extents->y1, extents->y2);
    }
    else {
        /* No damage info → flush the whole frame. */
        fbdevFlushRegion(pScreen, 0, priv->mode.height);
    }
    /* Tell the periodic timer the display is current so it can skip its full
     * blit this interval (see fbdevDirtySinceTick / fbdevFlushTimerCb). */
    fbdevDirtySinceTick = 1;
}

/*
 * Periodic full-screen flush.
 *
 * The damage-driven flush above only writes scanlines the X server *redrew*.
 * That is enough for animated content, but a static desktop (e.g. the twm root
 * + decorated windows under `startx desktop`) produces no further full-screen
 * damage after its initial paint — so the stale boot-console text sitting under
 * the X root on /dev/fb0 would never be overdrawn. (xeyes-alone happens to work
 * only because its continuous animation keeps generating damage.)
 *
 * To guarantee static content reaches the display we re-blit the ENTIRE shadow
 * buffer to /dev/fb0 on a timer, independent of damage. The frame is only ~8 MB
 * and the interval is coarse (FBDEV_FLUSH_MS), so the bandwidth is bounded and
 * modest; the damage path is kept for latency. The first tick after the X root
 * is created overdraws the console immediately. This mirrors the GL Quake port's
 * continuous full-frame blit to /dev/fb0.
 *
 * Runs on the main dispatch thread (TimerSet callbacks fire from
 * WaitForSomething/TimerCheck on poll() *timeout*, not fd-readiness — robust on
 * Phoenix, whose poll() does not reliably wake on fd-readiness), the same thread
 * as fbdevShadowUpdate, so no locking is needed against the damage path.
 */
/* Per-keyboard driver state. */
typedef struct _FbdevKbd {
    int fd;
    uint8_t prev[8];            /* previous raw HID report, for down/up diffing */
    KdKeyboardInfo *ki;         /* device handle, for the poll-timer drain (see
                                 * fbdevInputTimerCb) — NULL until Enable */
} FbdevKbd;

/* Per-pointer driver state. */
typedef struct _FbdevPtr {
    int fd;
    KdPointerInfo *pi;          /* device handle, for the poll-timer drain */
} FbdevPtr;

/* The single keyboard/pointer instances (kdrive creates one core device each).
 * Declared above the input-drain timer (fbdevInputTimerCb) which reads them. */
static FbdevKbd fbdevKbd = { -1, { 0 }, NULL };
static FbdevPtr fbdevPtr = { -1, NULL };

/* The HID read/drain callbacks live in the input section below; forward-declare
 * them so the poll-timer (defined next) can call them. */
static void fbdevKbdRead(int fd, int ready, void *data);
static void fbdevMouseRead(int fd, int ready, void *data);

#define FBDEV_FLUSH_MS 300

static OsTimerPtr fbdevFlushTimer = NULL;

static CARD32
fbdevFlushTimerCb(OsTimerPtr timer, CARD32 now, void *arg)
{
    ScreenPtr pScreen = arg;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    FbdevPriv *priv = screen->card->driver;
    static int announced = 0;

    (void) timer;
    (void) now;
    /* One-time marker so a UART log can confirm the timer is actually serviced
     * (distinguishes "timer never fired" from "shadow content wrong" if the
     * desktop still doesn't appear). */
    if (!announced) {
        announced = 1;
        ErrorF("[fbdev] periodic full-screen flush armed (idle safety, every %d ms)\n",
               FBDEV_FLUSH_MS);
    }

    /* The damage path already updated the display this interval — skip the
     * expensive full-screen blit (measured ~17 ms for the 8 MB frame) so
     * interactive redraws are never stalled by it. The full blit then runs only
     * when the screen went a whole interval with no damage (idle safety net for
     * static content that produced none). */
    if (fbdevDirtySinceTick) {
        fbdevDirtySinceTick = 0;
        return FBDEV_FLUSH_MS;
    }

    fbdevFlushRegion(pScreen, 0, priv->mode.height);
    /* Return the interval (non-zero) to re-arm; returning 0 fires only once. */
    return FBDEV_FLUSH_MS;
}

/*
 * Input drain timer. /dev/kbd0 + /dev/mouse0 are drained here on a fast timer
 * instead of via InputThreadRegisterDev's poll()-readiness model: Phoenix poll()
 * does not reliably wake on HID fd-readiness (the very reason the screen flush is
 * timer-driven too, see fbdevFlushTimerCb), so a registered read callback never
 * fires and NO input reaches X — devices open fine but nothing reacts. This timer
 * fires from WaitForSomething/TimerCheck on poll() *timeout* on the main dispatch
 * thread and does the non-blocking, bounded drain itself. It is the SOLE reader
 * (the Enable funcs deliberately do NOT call InputThreadRegisterDev), so there is
 * no input-thread race and no input_lock is needed — same threading as the flush
 * timer and the ephyr SetNotifyFd main-loop path. 16 ms ≈ 60 Hz: the bounded
 * drain empties the FIFO each tick, so latency is ≤ one tick with no event loss.
 */
#define FBDEV_INPUT_POLL_MS 16

static OsTimerPtr fbdevInputTimer = NULL;

static CARD32
fbdevInputTimerCb(OsTimerPtr timer, CARD32 now, void *arg)
{
    (void) timer;
    (void) now;
    (void) arg;
    if (fbdevKbd.fd >= 0 && fbdevKbd.ki != NULL)
        fbdevKbdRead(fbdevKbd.fd, 1, fbdevKbd.ki);
    if (fbdevPtr.fd >= 0 && fbdevPtr.pi != NULL)
        fbdevMouseRead(fbdevPtr.fd, 1, fbdevPtr.pi);
    return FBDEV_INPUT_POLL_MS;
}

/*
 * shadow window proc: return the address (in the shadow buffer) of scanline
 * `row`. kdrive's shadow layer hands this to update procs that copy
 * shadow→device-memory; we don't use it for the device copy (we write() the
 * shadow ourselves), but shadowAdd requires a non-NULL window proc.
 */
static void *
fbdevWindowLinear(ScreenPtr pScreen, CARD32 row, CARD32 offset, int mode,
                  CARD32 *size, void *closure)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    int stride = screen->fb.byteStride;

    *size = stride;
    return (void *) ((CARD8 *) screen->fb.frameBuffer + row * stride + offset);
}

static Bool
fbdevInitScreen(ScreenPtr pScreen)
{
    return TRUE;
}

static Bool
fbdevFinishInitScreen(ScreenPtr pScreen)
{
    if (!shadowSetup(pScreen))
        return FALSE;
    return TRUE;
}

static Bool
fbdevCreateResources(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    FbdevScrPriv *scrpriv = screen->driver;

    if (scrpriv->shadow) {
        if (!KdShadowSet(pScreen, scrpriv->randr,
                         fbdevShadowUpdate, fbdevWindowLinear))
            return FALSE;
        /* Arm the periodic full-screen shadow→fb0 flush (see
         * fbdevFlushTimerCb): guarantees static content (the twm desktop)
         * reaches the display even with no further damage. */
        fbdevFlushTimer = TimerSet(fbdevFlushTimer, 0, FBDEV_FLUSH_MS,
                                   fbdevFlushTimerCb, pScreen);
        /* Arm the HID input-drain timer too (see fbdevInputTimerCb). Safe to arm
         * before the input devices enable: the callback no-ops until kbd0/mouse0
         * are open and their device handles are stored. */
        fbdevInputTimer = TimerSet(fbdevInputTimer, 0, FBDEV_INPUT_POLL_MS,
                                   fbdevInputTimerCb, pScreen);
    }
    return TRUE;
}

static void
fbdevScreenFini(KdScreenInfo *screen)
{
    if (screen->fb.shadow)
        KdShadowFbFree(screen);
}

static void
fbdevCloseScreen(ScreenPtr pScreen)
{
}

static void
fbdevCardFini(KdCardInfo *card)
{
    FbdevPriv *priv = card->driver;
    /* Stop the periodic flush + input drain before we hand the framebuffer back. */
    if (fbdevFlushTimer) {
        TimerFree(fbdevFlushTimer);
        fbdevFlushTimer = NULL;
    }
    if (fbdevInputTimer) {
        TimerFree(fbdevInputTimer);
        fbdevInputTimer = NULL;
    }
    /* Hand the framebuffer back to the text console (reappears with everything
     * the console accumulated off-screen while X owned the display). */
    fbdevConsoleSetMode(FBCON_ENABLED);
    if (fbdev_ttyfd >= 0) {
        close(fbdev_ttyfd);
        fbdev_ttyfd = -1;
    }
    if (priv) {
        if (priv->fd >= 0)
            close(priv->fd);
        free(priv);
        card->driver = NULL;
    }
}

static void
fbdevGetColors(ScreenPtr pScreen, int n, xColorItem *pdefs)
{
    /* TrueColor only — no palette. */
    while (n--) {
        pdefs->red = pdefs->green = pdefs->blue = 0;
        pdefs++;
    }
}

static void
fbdevPutColors(ScreenPtr pScreen, int n, xColorItem *pdefs)
{
    /* TrueColor only — no palette. */
}

/* -------------------------------------------------------------------------
 * Input drivers — Phoenix USB HID keyboard (/dev/kbd0) + mouse (/dev/mouse0).
 *
 * Event sourcing. Phoenix poll() does not signal readiness reliably, so we do
 * NOT use InputThreadRegisterDev's poll()-based model. Instead Enable() opens
 * the device and both fds are drained on a fast main-loop timer (see
 * fbdevInputTimerCb); the read callbacks below enqueue events from there. A
 * single thread both enqueues (the timer callback) and dequeues (dix), so the
 * callback needs no extra locking, and reading never blocks the dispatch loop
 * (the fds are O_NONBLOCK and the drain is bounded).
 *
 * KEYBOARD. usbkbd is put into RAW mode (write a single 0x01 byte to /dev/kbd0;
 * see usbkbd.c) so it delivers raw 8-byte HID boot-keyboard reports rather than
 * a cooked ASCII stream. Raw reports carry the full key-state array on every
 * change, so by diffing each report against the previous one we derive real
 * key-DOWN and key-UP events (X needs both; cooked ASCII cannot express key-up).
 *   HID report: [0]=modifier bitmask, [1]=reserved, [2..7]=up to 6 pressed usages.
 *
 * HID-usage -> X keycode. The compiled-in keymap is us/pc105 under the evdev
 * rules, whose keycodes are POSITIONAL (not the alphabetical layout of HID
 * usages). We map HID usage -> evdev keycode via hidToEvdev[] (see
 * hid_evdev_map.h) then add the X keycode offset of 8 (X keycode = evdev
 * keycode + 8). With minScanCode = 8 (= KD_MIN_KEYCODE), KdEnqueueKeyboardEvent's
 * formula passes the value through unchanged. Anchors: Esc usage 0x29 -> evdev 1
 * -> X 9; 'a' 0x04 -> 30 -> 38; Space 0x2c -> 57 -> 65. We send keycodes ONLY:
 * XKB derives keysyms/chars from keycode + modifier state, so there is no
 * char/shift handling here. Modifier keys are just their own evdev keycodes,
 * pressed/released by diffing byte[0].
 *
 * MOUSE. usbmouse delivers raw 4-byte HID boot-mouse packets on change:
 *   [0]=buttons (bit0 L, bit1 R, bit2 M), [1]=dx int8, [2]=dy int8, [3]=wheel.
 * We map HID buttons -> KD button mask: KD_BUTTON_1=L(0x01), KD_BUTTON_2=MIDDLE
 * (0x02), KD_BUTTON_3=RIGHT(0x04) — note HID's right/middle bit order is the
 * reverse of KD's, so we remap rather than pass through. One
 * KdEnqueuePointerEvent(pi, buttons | KD_MOUSE_DELTA, dx, dy, 0) per packet
 * delivers relative motion + the current button mask together.
 *
 * GRACEFUL DEGRADE. Enable() opens the device; if the open fails it logs, leaves
 * fd = -1, skips registration, and STILL returns Success. The keyboard is the
 * mandatory virtual-core keyboard — returning failure from its Enable would
 * abort the server ("Failed to activate virtual core keyboard") — so a missing
 * device must never be fatal; the server simply runs without that input source.
 * ------------------------------------------------------------------------- */

#define KBD_DEVICE   "/dev/kbd0"
#define MOUSE_DEVICE "/dev/mouse0"

/* hidToEvdev[256] and hidModEvdev[8]: HID usage -> evdev keycode. */
#include "hid_evdev_map.h"

/* X keycode offset over the evdev keycode for the evdev-rules keymap. */
#define EVDEV_KEYCODE_OFFSET 8

static int
fbdevHidUsagePresent(const uint8_t *rep, uint8_t u)
{
    int i;
    for (i = 2; i < 8; ++i)
        if (rep[i] == u)
            return 1;
    return 0;
}

static void
fbdevKbdEmit(KdKeyboardInfo *ki, unsigned char evdev, int is_up)
{
    static int first = 1;
    if (evdev == 0)
        return;
    if (first) {                /* #30 diag: a real key event reached dix */
        first = 0;
        ErrorF("[fbdev] kbd FIRST event enqueued (keycode=%d up=%d)\n",
               evdev + EVDEV_KEYCODE_OFFSET, is_up);
    }
    KdEnqueueKeyboardEvent(ki, evdev + EVDEV_KEYCODE_OFFSET, is_up);
}

/* Diff one raw 8-byte HID report against the previous -> key down/up events. */
static void
fbdevKbdProcess(KdKeyboardInfo *ki, FbdevKbd *kbd, const uint8_t *rep)
{
    uint8_t mod = rep[0], pmod = kbd->prev[0];
    int i;

    /* modifier transitions */
    for (i = 0; i < 8; ++i) {
        int now = (mod >> i) & 1;
        int was = (pmod >> i) & 1;
        if (now != was)
            fbdevKbdEmit(ki, hidModEvdev[i], !now);
    }

    /* key-ups: usages in the old report no longer present */
    for (i = 2; i < 8; ++i) {
        uint8_t u = kbd->prev[i];
        if (u >= 0x04u && !fbdevHidUsagePresent(rep, u))
            fbdevKbdEmit(ki, hidToEvdev[u], 1);
    }
    /* key-downs: usages newly present */
    for (i = 2; i < 8; ++i) {
        uint8_t u = rep[i];
        if (u >= 0x04u && !fbdevHidUsagePresent(kbd->prev, u))
            fbdevKbdEmit(ki, hidToEvdev[u], 0);
    }

    memcpy(kbd->prev, rep, 8);
}

/*
 * Input-thread callback for /dev/kbd0. Bounded drain: cap reads so a held
 * auto-repeating key can never spin the loop and starve the input thread.
 * O_NONBLOCK makes read() return -1/EWOULDBLOCK once the fifo empties.
 */
static void
fbdevKbdRead(int fd, int ready, void *data)
{
    KdKeyboardInfo *ki = data;
    FbdevKbd *kbd = &fbdevKbd;
    uint8_t buf[64];
    ssize_t r;
    int off, guard;
    static unsigned long drained = 0;
    unsigned long before = drained;

    (void) ready;
    for (guard = 0; guard < 64 && (r = read(fd, buf, sizeof(buf))) > 0; ++guard) {
        drained += (unsigned long) r;
        for (off = 0; off + 8 <= (int) r; off += 8)
            fbdevKbdProcess(ki, kbd, buf + off);
    }
    /* #30 diag: prove the timer-drain reaches kbd0. Log the FIRST bytes ever read,
     * so a null result tells "0 bytes (drain not reaching the device)" apart from
     * "bytes read but no client reaction (downstream keycode/focus issue)". */
    if (before == 0 && drained > 0)
        ErrorF("[fbdev] kbd FIRST drain: %lu bytes from kbd0\n", drained);
}

/*
 * Init/Enable return Status (Success == 0), NOT Bool — even though the
 * KdKeyboardDriver struct declares the fields as Bool. KdKeyboardProc()
 * (hw/kdrive/src/kinput.c) checks both with `!= Success`, so returning Bool
 * TRUE (== 1) is read as failure: DEVICE_INIT is rejected and the keyboard is
 * never enabled (no Enable call, no /dev/kbd0 open — the symptom that the mouse,
 * whose KdPointerDriver path correctly uses Status, never exhibits). The stock
 * ephyr keyboard driver resolves the same struct-vs-caller mismatch by declaring
 * its impls `static Status` and returning Success; we mirror that exactly.
 */
static Status
fbdevKeyboardInit(KdKeyboardInfo *ki)
{
    ki->minScanCode = KD_MIN_KEYCODE;
    ki->maxScanCode = KD_MAX_KEYCODE;
    return Success;
}

static Status
fbdevKeyboardEnable(KdKeyboardInfo *ki)
{
    FbdevKbd *kbd = &fbdevKbd;
    unsigned char raw = 1u;
    int tries;

    /* Must return Success even on failure: this is the virtual core keyboard, and
     * a failed Enable (anything != Success) aborts the server. A missing device
     * just means no input.
     *
     * /dev/kbd0 is normally held single-opener by the pl011-tty console bridge
     * (the same gate Quake hits — see pl_phoenix_in.c). It is only released when
     * the HDMI console switches out of text mode; that release is asynchronous,
     * so we retry briefly to absorb the race. If the bridge never releases it
     * (e.g. the X server was launched from a psh sitting on the console), the
     * open keeps failing and we degrade to no keyboard — see the deliverable
     * note: freeing kbd0 for X is a separate display-ownership step. */
    for (tries = 0; tries < 40; ++tries) {
        kbd->fd = open(KBD_DEVICE, O_RDWR | O_NONBLOCK);
        if (kbd->fd >= 0)
            break;
        usleep(25000);
    }
    if (kbd->fd < 0) {
        ErrorF("[fbdev] %s open failed (%s) — keyboard input disabled\n",
               KBD_DEVICE, strerror(errno));
        return Success;
    }

    /* Ask usbkbd for raw 8-byte HID reports (carries key-up + held state). */
    if (write(kbd->fd, &raw, 1) == 1)
        ErrorF("[fbdev] %s opened (fd=%d), RAW HID mode — keyboard active\n",
               KBD_DEVICE, kbd->fd);
    else
        ErrorF("[fbdev] %s opened (fd=%d), raw-mode request failed — keyboard active\n",
               KBD_DEVICE, kbd->fd);

    memset(kbd->prev, 0, sizeof(kbd->prev));

    /* Drive reads from fbdevInputTimerCb, NOT InputThreadRegisterDev: the latter
     * polls for fd-readiness, which Phoenix poll() does not deliver on HID fds, so
     * the callback would never fire (devices open but no input — the observed
     * symptom). Storing the device handle here is what lets the timer enqueue. */
    kbd->ki = ki;
    return Success;
}

static void
fbdevKeyboardDisable(KdKeyboardInfo *ki)
{
    FbdevKbd *kbd = &fbdevKbd;

    kbd->ki = NULL;             /* stop the timer drain before closing the fd */
    if (kbd->fd >= 0) {
        close(kbd->fd);
        kbd->fd = -1;
    }
}

static void
fbdevKeyboardFini(KdKeyboardInfo *ki)
{
}

static void
fbdevKeyboardLeds(KdKeyboardInfo *ki, int leds)
{
}

static void
fbdevKeyboardBell(KdKeyboardInfo *ki, int volume, int frequency, int duration)
{
}

KdKeyboardDriver fbdevKeyboardDriver = {
    "fbdev",
    fbdevKeyboardInit,
    fbdevKeyboardEnable,
    fbdevKeyboardLeds,
    fbdevKeyboardBell,
    fbdevKeyboardDisable,
    fbdevKeyboardFini,
    NULL,
};

/* Decode one raw 4-byte HID mouse packet -> one KdEnqueuePointerEvent. */
static void
fbdevMouseProcess(KdPointerInfo *pi, const uint8_t *p)
{
    int hidbtn = p[0];
    int dx = (int) (signed char) p[1];
    int dy = (int) (signed char) p[2];
    unsigned long flags = KD_MOUSE_DELTA;

    /* Remap HID button bits to KD's order (HID bit1=right, bit2=middle; KD
     * KD_BUTTON_2=middle, KD_BUTTON_3=right). */
    if (hidbtn & 0x01)
        flags |= KD_BUTTON_1;       /* left */
    if (hidbtn & 0x04)
        flags |= KD_BUTTON_2;       /* middle */
    if (hidbtn & 0x02)
        flags |= KD_BUTTON_3;       /* right */

    {
        static int first = 1;
        if (first) {            /* #30 diag: a real pointer event reached dix */
            first = 0;
            ErrorF("[fbdev] mouse FIRST event enqueued (dx=%d dy=%d btn=0x%x)\n",
                   dx, dy, hidbtn);
        }
    }
    KdEnqueuePointerEvent(pi, flags, dx, dy, 0);
}

static void
fbdevMouseRead(int fd, int ready, void *data)
{
    KdPointerInfo *pi = data;
    uint8_t buf[64];
    ssize_t r;
    int off, guard;

    static unsigned long drained = 0;
    unsigned long before = drained;

    (void) ready;
    for (guard = 0; guard < 64 && (r = read(fd, buf, sizeof(buf))) > 0; ++guard) {
        drained += (unsigned long) r;
        for (off = 0; off + 4 <= (int) r; off += 4)
            fbdevMouseProcess(pi, buf + off);
    }
    if (before == 0 && drained > 0)
        ErrorF("[fbdev] mouse FIRST drain: %lu bytes from mouse0\n", drained);
}

static Status
fbdevMouseInit(KdPointerInfo *pi)
{
    return Success;
}

static Status
fbdevMouseEnable(KdPointerInfo *pi)
{
    FbdevPtr *ptr = &fbdevPtr;

    ptr->fd = open(MOUSE_DEVICE, O_RDONLY | O_NONBLOCK);
    if (ptr->fd < 0) {
        ErrorF("[fbdev] %s open failed (%s) — mouse input disabled\n",
               MOUSE_DEVICE, strerror(errno));
        return Success;             /* degrade gracefully, server keeps running */
    }
    ErrorF("[fbdev] %s opened (fd=%d) — mouse active\n", MOUSE_DEVICE, ptr->fd);

    /* Timer-driven drain, not InputThreadRegisterDev — see fbdevMouseEnable's
     * keyboard sibling and fbdevInputTimerCb (Phoenix poll() readiness gap). */
    ptr->pi = pi;
    return Success;
}

static void
fbdevMouseDisable(KdPointerInfo *pi)
{
    FbdevPtr *ptr = &fbdevPtr;

    ptr->pi = NULL;            /* stop the timer drain before closing the fd */
    if (ptr->fd >= 0) {
        close(ptr->fd);
        ptr->fd = -1;
    }
}

static void
fbdevMouseFini(KdPointerInfo *pi)
{
}

KdPointerDriver fbdevMouseDriver = {
    "fbdev",
    fbdevMouseInit,
    fbdevMouseEnable,
    fbdevMouseDisable,
    fbdevMouseFini,
    NULL,
};

/* -------------------------------------------------------------------------
 * DDX entry points (the symbols dix/os require a DDX backend to provide —
 * same set as hw/kdrive/ephyr/ephyrinit.c). main()/dix_main come from
 * dix/libmain.a; we deliberately do NOT define main().
 * ------------------------------------------------------------------------- */

void
InitCard(char *name)
{
    KdCardInfoAdd(&fbdevFuncs, 0);
}

void
InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv)
{
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    KdKeyboardInfo *ki;
    KdPointerInfo *pi;

    KdAddKeyboardDriver(&fbdevKeyboardDriver);
    KdAddPointerDriver(&fbdevMouseDriver);

    ki = KdNewKeyboard();
    if (ki) {
        ki->driver = &fbdevKeyboardDriver;
        KdAddKeyboard(ki);
    }
    pi = KdNewPointer();
    if (pi) {
        pi->driver = &fbdevMouseDriver;
        KdAddPointer(pi);
    }

    KdInitInput();
}

void
CloseInput(void)
{
    KdCloseInput();
}

#if INPUTTHREAD
void
ddxInputThreadInit(void)
{
}
#endif

void
ddxUseMsg(void)
{
    KdUseMsg();
    ErrorF("\nXphoenix (kdrive fbdev) options:\n");
    ErrorF("-screen WIDTHxHEIGHT  Screen size (default: /dev/fb0 native mode)\n");
    ErrorF("\n");
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
    return KdProcessArgument(argc, argv, i);
}

void
OsVendorInit(void)
{
    /*
     * Deliberately do NOT pre-create a card here. KdInitOutput() has its own
     * fallback (kdrive.c): when no card exists yet it calls InitCard(0) +
     * KdScreenInfoAdd() + KdParseScreen(screen, 0), giving a default screen
     * whose width/height our fbdevScreenInitialize() then fills from the
     * /dev/fb0 native mode. If we called InitCard(0) here, kdCardInfo would be
     * non-NULL and KdInitOutput would skip that fallback — registering a card
     * with NO screen, so cardinit/scrinit (the /dev/fb0 open + GETMODE +
     * shadow alloc) would never run. An explicit "-screen WxH" still works:
     * KdProcessArgument adds the card+screen on that path.
     */
}

KdCardFuncs fbdevFuncs = {
    fbdevCardInit,              /* cardinit */
    fbdevScreenInit,            /* scrinit */
    fbdevInitScreen,            /* initScreen */
    fbdevFinishInitScreen,      /* finishInitScreen */
    fbdevCreateResources,       /* createRes */
    fbdevScreenFini,            /* scrfini */
    fbdevCardFini,              /* cardfini */

    0,                          /* initCursor */

    0,                          /* initAccel */
    0,                          /* enableAccel */
    0,                          /* disableAccel */
    0,                          /* finiAccel */

    fbdevGetColors,             /* getColors */
    fbdevPutColors,             /* putColors */

    fbdevCloseScreen,           /* closeScreen */
};
