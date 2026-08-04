/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_PHOENIX

/*
 * Phoenix-RTOS audio driver (Raspberry Pi 4 / BCM2711).
 *
 * A PULL-model backend over the rpi4-audio device node /dev/audio0. SDL's audio
 * thread calls the app's callback to fill the mixing buffer, then invokes
 * PlayDevice, which write()s that buffer straight to /dev/audio0. The rpi4-audio
 * driver is a PIO device with a fixed native format (44100 Hz, stereo, signed
 * 16-bit little-endian); its blocking write() drains the PWM FIFO at the real
 * playback rate, so the write itself provides natural backpressure and paces the
 * audio thread. WaitDevice is therefore a no-op.
 *
 * This is the pull-model counterpart of the QuakeSpasm push feeder in
 * tools/quakespasm-port/platform/pl_phoenix_snd.c: same open()/format/write() on
 * /dev/audio0, opposite control flow (there a feeder thread drained a mixer ring;
 * here SDL's own audio thread drives us).
 *
 * The device format is fixed, so OpenDevice forces _this->spec to the native
 * format and lets SDL build a conversion stream from the app's requested spec.
 */

#include "SDL_audio.h"
#include "SDL_timer.h"
#include "../SDL_audio_c.h"
#include "SDL_phoenixaudio.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define PHOENIXAUDIO_DRIVER_NAME "phoenix"

#define PHOENIXAUDIO_DEVICE "/dev/audio0"

/* Fixed native format of the rpi4-audio PIO device. */
#define PHOENIXAUDIO_FREQ     44100
#define PHOENIXAUDIO_FORMAT   AUDIO_S16SYS
#define PHOENIXAUDIO_CHANNELS 2

/* Block until a full mixing buffer can be written. PlayDevice's write() is
 * itself blocking (the PWM FIFO drain paces it), so there is nothing to wait
 * for here. */
static void PHOENIXAUDIO_WaitDevice(_THIS)
{
    (void)_this;
}

static void PHOENIXAUDIO_PlayDevice(_THIS)
{
    struct SDL_PrivateAudioData *h = _this->hidden;
    const Uint8 *buf = h->mixbuf;
    size_t remaining = h->mixlen;

    /* write() to /dev/audio0 blocks at the playback rate. Loop over short
     * writes and retry EINTR; treat any hard error as a device disconnect so
     * SDL tears the stream down cleanly rather than spinning. */
    while (remaining > 0) {
        ssize_t written = write(h->fd, buf, remaining);
        if (written > 0) {
            buf += written;
            remaining -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            SDL_OpenedAudioDeviceDisconnected(_this);
            return;
        }
    }
}

static Uint8 *PHOENIXAUDIO_GetDeviceBuf(_THIS)
{
    return _this->hidden->mixbuf;
}

static void PHOENIXAUDIO_CloseDevice(_THIS)
{
    struct SDL_PrivateAudioData *h = _this->hidden;

    if (h) {
        if (h->fd >= 0) {
            close(h->fd);
        }
        SDL_free(h->mixbuf);
        SDL_free(h);
        _this->hidden = NULL;
    }
}

static int PHOENIXAUDIO_OpenDevice(_THIS, const char *devname)
{
    struct SDL_PrivateAudioData *h;

    (void)devname;

    h = (struct SDL_PrivateAudioData *)SDL_malloc(sizeof(*h));
    if (!h) {
        return SDL_OutOfMemory();
    }
    SDL_zerop(h);
    h->fd = -1;
    _this->hidden = h;

    h->fd = open(PHOENIXAUDIO_DEVICE, O_WRONLY);
    if (h->fd < 0) {
        return SDL_SetError("PHOENIXAUDIO: could not open " PHOENIXAUDIO_DEVICE);
    }

    /* The rpi4-audio device has a single fixed format. Force _this->spec to it
     * and recompute spec.size for the new frame size; SDL builds a conversion
     * stream from the app's requested spec toward this one. spec.samples (and
     * thus the callback cadence) is preserved. */
    _this->spec.format = PHOENIXAUDIO_FORMAT;
    _this->spec.channels = PHOENIXAUDIO_CHANNELS;
    _this->spec.freq = PHOENIXAUDIO_FREQ;
    SDL_CalculateAudioSpec(&_this->spec);

    /* Allocate the mixing buffer at the device-native frame size (spec.size),
     * which is what the audio thread fills and PlayDevice writes. */
    h->mixlen = _this->spec.size;
    h->mixbuf = (Uint8 *)SDL_malloc(h->mixlen);
    if (!h->mixbuf) {
        return SDL_OutOfMemory();
    }
    SDL_memset(h->mixbuf, _this->spec.silence, h->mixlen);

    return 0;
}

static SDL_bool PHOENIXAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = PHOENIXAUDIO_OpenDevice;
    impl->WaitDevice = PHOENIXAUDIO_WaitDevice;
    impl->PlayDevice = PHOENIXAUDIO_PlayDevice;
    impl->GetDeviceBuf = PHOENIXAUDIO_GetDeviceBuf;
    impl->CloseDevice = PHOENIXAUDIO_CloseDevice;

    /* One fixed output device (/dev/audio0), no capture. */
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;

    return SDL_TRUE; /* this audio target is available. */
}

/* demand_only = SDL_FALSE: this is a real playback driver and must be eligible
 * for automatic selection (every host backend is compiled out on Phoenix, so a
 * demand-only driver would leave SDL_AudioInit with nothing to pick). */
AudioBootStrap PHOENIXAUDIO_bootstrap = {
    PHOENIXAUDIO_DRIVER_NAME, "SDL Phoenix-RTOS /dev/audio0 (rpi4-audio PWM) driver",
    PHOENIXAUDIO_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_PHOENIX */

/* vi: set ts=4 sw=4 expandtab: */
