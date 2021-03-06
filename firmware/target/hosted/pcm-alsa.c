/***************************************************************************
 *             __________               __   ___.                  
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___  
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /  
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <   
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \  
 *                     \/            \/     \/    \/            \/ 
 * $Id$
 *
 * Copyright (C) 2010 Thomas Martitz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/


/*
 * Based, but heavily modified, on the example given at
 * http://www.alsa-project.org/alsa-doc/alsa-lib/_2test_2pcm_8c-example.html
 *
 * This driver uses the so-called unsafe async callback method and hardcoded device
 * names. It fails when the audio device is busy by other apps.
 *
 * To make the async callback safer, an alternative stack is installed, since
 * it's run from a signal hanlder (which otherwise uses the user stack). If
 * tick tasks are run from a signal handler too, please install
 * an alternative stack for it too.
 *
 * TODO: Rewrite this to do it properly with multithreading
 *
 * Alternatively, a version using polling in a tick task is provided. While
 * supposedly safer, it appears to use more CPU (however I didn't measure it
 * accurately, only looked at htop). At least, in this mode the "default"
 * device works which doesnt break with other apps running.
 * device works which doesnt break with other apps running.
 */


#include "autoconf.h"

#include <stdlib.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>
#include "system.h"
#include "debug.h"
#include "kernel.h"
#include "panic.h"

#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "audiohw.h"
#include "pcm-alsa.h"

#include <pthread.h>
#include <signal.h>

#define USE_ASYNC_CALLBACK
/* plughw:0,0 works with both, however "default" is recommended.
 * default doesnt seem to work with async callback but doesn't break
 * with multple applications running */
static char device[] = "plughw:0,0";                    /* playback device */
static const snd_pcm_access_t access_ = SND_PCM_ACCESS_RW_INTERLEAVED; /* access mode */
#ifdef SONY_NWZ_LINUX
/* Sony NWZ must use 32-bit per sample */
static const snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;    /* sample format */
typedef long sample_t;
#else
static const snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
typedef short sample_t;
#endif
static const int channels = 2;                                /* count of channels */
static unsigned int rate = 44100;                       /* stream rate */

static snd_pcm_t *handle;
static snd_pcm_sframes_t buffer_size = MIX_FRAME_SAMPLES * 32; /* ~16k */
static snd_pcm_sframes_t period_size = MIX_FRAME_SAMPLES * 4;  /*  ~4k */
static sample_t *frames;

static const void  *pcm_data = 0;
static size_t       pcm_size = 0;

#ifdef USE_ASYNC_CALLBACK
static snd_async_handler_t *ahandler;
static pthread_mutex_t pcm_mtx;
static char signal_stack[SIGSTKSZ];
#else
static int recursion;
#endif

static int set_hwparams(snd_pcm_t *handle, unsigned sample_rate)
{
    unsigned int rrate;
    int err;
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_malloc(&params);


    /* choose all parameters */
    err = snd_pcm_hw_params_any(handle, params);
    if (err < 0)
    {
        printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
        goto error;
    }
    /* set the interleaved read/write format */
    err = snd_pcm_hw_params_set_access(handle, params, access_);
    if (err < 0)
    {
        printf("Access type not available for playback: %s\n", snd_strerror(err));
        goto error;
    }
    /* set the sample format */
    err = snd_pcm_hw_params_set_format(handle, params, format);
    if (err < 0)
    {
        printf("Sample format not available for playback: %s\n", snd_strerror(err));
        goto error;
    }
    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    if (err < 0)
    {
        printf("Channels count (%i) not available for playbacks: %s\n", channels, snd_strerror(err));
        goto error;
    }
    /* set the stream rate */
    rrate = sample_rate;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
    if (err < 0)
    {
        printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
        goto error;
    }
    if (rrate != sample_rate)
    {
        printf("Rate doesn't match (requested %iHz, get %iHz)\n", sample_rate, err);
        err = -EINVAL;
        goto error;
    }

    /* set the buffer size */
    err = snd_pcm_hw_params_set_buffer_size_near(handle, params, &buffer_size);
    if (err < 0)
    {
        printf("Unable to set buffer size %ld for playback: %s\n", buffer_size, snd_strerror(err));
        goto error;
    }

    /* set the period size */
    err = snd_pcm_hw_params_set_period_size_near (handle, params, &period_size, NULL);
    if (err < 0)
    {
        printf("Unable to set period size %ld for playback: %s\n", period_size, snd_strerror(err));
        goto error;
    }
    if (!frames)
        frames = malloc(period_size * channels * sizeof(sample_t));

    /* write the parameters to device */
    err = snd_pcm_hw_params(handle, params);
    if (err < 0)
    {
        printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
        goto error;
    }

    err = 0; /* success */
error:
    snd_pcm_hw_params_free(params);
    return err;
}

/* Set sw params: playback start threshold and low buffer watermark */
static int set_swparams(snd_pcm_t *handle)
{
    int err;

    snd_pcm_sw_params_t *swparams;
    snd_pcm_sw_params_malloc(&swparams);

    /* get the current swparams */
    err = snd_pcm_sw_params_current(handle, swparams);
    if (err < 0)
    {
        printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
        goto error;
    }
    /* start the transfer when the buffer is haalmost full */
    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, buffer_size / 2);
    if (err < 0)
    {
        printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
        goto error;
    }
    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
    if (err < 0)
    {
        printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
        goto error;
    }
    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(handle, swparams);
    if (err < 0)
    {
        printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
        goto error;
    }

    err = 0; /* success */
error:
    snd_pcm_sw_params_free(swparams);
    return err;
}

/* Digital volume explanation:
 * with very good approximation (<0.1dB) the convertion from dB to multiplicative
 * factor, for dB>=0, is 2^(dB/3). We can then notice that if we write dB=3*k+r
 * then this is 2^k*2^(r/3) so we only need to look at r=0,1,2. For r=0 this is
 * 1, for r=1 we have 2^(1/3)~=1.25 so we approximate by 1+1/4, and 2^(2/3)~=1.5
 * so we approximate by 1+1/2. To go from negative to nonnegative we notice that
 * 48 dB => 63095 factor ~= 2^16 so we virtually pre-multiply everything by 2^(-16)
 * and add 48dB to the input volume. We cannot go lower -43dB because several
 * values between -48dB and -43dB would require a fractional multiplier, which is
 * stupid to implement for such very low volume. */
static int dig_vol_mult = 2 ^ 16; /* multiplicative factor to apply to each sample */

void pcm_alsa_set_digital_volume(int vol_db)
{
    if(vol_db > 0 || vol_db < -43)
        panicf("invalid pcm alsa volume");
    if(format != SND_PCM_FORMAT_S32_LE)
        panicf("this function assumes 32-bit sample size");
    vol_db += 48; /* -42dB .. 0dB => 5dB .. 48dB */
    /* NOTE if vol_dB = 5 then vol_shift = 1 but r = 1 so we do vol_shift - 1 >= 0
     * otherwise vol_dB >= 0 implies vol_shift >= 2 so vol_shift - 2 >= 0 */
    int vol_shift = vol_db / 3;
    int r = vol_db % 3;
    if(r == 0)
        dig_vol_mult = 1 << vol_shift;
    else if(r == 1)
        dig_vol_mult = 1 << vol_shift | 1 << (vol_shift - 2);
    else
        dig_vol_mult = 1 << vol_shift | 1 << (vol_shift - 1);
    printf("%d dB -> factor = %d\n", vol_db - 48, dig_vol_mult);
}

/* copy pcm samples to a spare buffer, suitable for snd_pcm_writei() */
static bool fill_frames(void)
{
    ssize_t copy_n, frames_left = period_size;
    bool new_buffer = false;

    while (frames_left > 0)
    {
        if (!pcm_size)
        {
            new_buffer = true;
            if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data,
                                                &pcm_size))
            {
                return false;
            }
        }

        if (pcm_size % 4)
            panicf("Wrong pcm_size");
        /* the compiler will optimize this test away */
        copy_n = MIN((ssize_t)pcm_size/4, frames_left);
        if (format == SND_PCM_FORMAT_S32_LE)
        {
            /* We have to convert 16-bit to 32-bit, the need to multiply the
             * sample by some value so the sound is not too low */
            const short *pcm_ptr = pcm_data;
            sample_t *sample_ptr = &frames[2*(period_size-frames_left)];
            for (int i = 0; i < copy_n*2; i++)
                *sample_ptr++ = *pcm_ptr++ * dig_vol_mult;
        }
        else
        {
            /* Rockbox and PCM have same format: memcopy */
            memcpy(&frames[2*(period_size-frames_left)], pcm_data, copy_n * 4);
        }
        pcm_data += copy_n*4;
        pcm_size -= copy_n*4;
        frames_left -= copy_n;

        if (new_buffer)
        {
            new_buffer = false;
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
        }
    }
    return true;
}

#ifdef USE_ASYNC_CALLBACK
static void async_callback(snd_async_handler_t *ahandler)
{
    snd_pcm_t *handle = snd_async_handler_get_pcm(ahandler);

    if (pthread_mutex_trylock(&pcm_mtx) != 0)
        return;
#else
static void pcm_tick(void)
{
    if (snd_pcm_state(handle) != SND_PCM_STATE_RUNNING)
        return;
#endif

    while (snd_pcm_avail_update(handle) >= period_size)
    {
        if (fill_frames())
        {
            int err = snd_pcm_writei(handle, frames, period_size);
            if (err < 0 && err != period_size && err != -EAGAIN)
            {
                printf("Write error: written %i expected %li\n", err, period_size);
                break;
            }
        }
        else
        {
            DEBUGF("%s: No Data.\n", __func__);
            break;
        }
    }
#ifdef USE_ASYNC_CALLBACK
    pthread_mutex_unlock(&pcm_mtx);
#endif
}

static int async_rw(snd_pcm_t *handle)
{
    int err;
    snd_pcm_sframes_t sample_size;
    sample_t *samples;

#ifdef USE_ASYNC_CALLBACK
    /* assign alternative stack for the signal handlers */
    stack_t ss = {
        .ss_sp = signal_stack,
        .ss_size = sizeof(signal_stack),
        .ss_flags = 0
    };
    struct sigaction sa;

    err = sigaltstack(&ss, NULL);
    if (err < 0)
    {
        DEBUGF("Unable to install alternative signal stack: %s", strerror(err));
        return err;
    }
    
    err = snd_async_add_pcm_handler(&ahandler, handle, async_callback, NULL);
    if (err < 0)
    {
        DEBUGF("Unable to register async handler: %s\n", snd_strerror(err));
        return err;
    }

    /* only modify the stack the handler runs on */
    sigaction(SIGIO, NULL, &sa);
    sa.sa_flags |= SA_ONSTACK;
    err = sigaction(SIGIO, &sa, NULL);
    if (err < 0)
    {
        DEBUGF("Unable to install alternative signal stack: %s", strerror(err));
        return err;
    }
#endif

    /* fill buffer with silence to initiate playback without noisy click */
    sample_size = buffer_size;
    samples = malloc(sample_size * channels * sizeof(sample_t));

    snd_pcm_format_set_silence(format, samples, sample_size);
    err = snd_pcm_writei(handle, samples, sample_size);
    free(samples);

    if (err < 0)
    {
            DEBUGF("Initial write error: %s\n", snd_strerror(err));
            return err;
    }
    if (err != (ssize_t)sample_size)
    {
            DEBUGF("Initial write error: written %i expected %li\n", err, sample_size);
            return err;
    }
    if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED)
    {
            err = snd_pcm_start(handle);
            if (err < 0)
            {
                DEBUGF("Start error: %s\n", snd_strerror(err));
                return err;
            }
    }
    return 0;
}


void cleanup(void)
{
    free(frames);
    frames = NULL;
    snd_pcm_close(handle);
}


void pcm_play_dma_init(void)
{
    int err;
    audiohw_preinit();

    if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
        panicf("%s(): Cannot open device %s: %s\n", __func__, device, snd_strerror(err));
    }

    if ((err = snd_pcm_nonblock(handle, 1)))
        panicf("Could not set non-block mode: %s\n", snd_strerror(err));

    if ((err = set_hwparams(handle, rate)) < 0)
    {
        panicf("Setting of hwparams failed: %s\n", snd_strerror(err));
    }
    if ((err = set_swparams(handle)) < 0)
    {
        panicf("Setting of swparams failed: %s\n", snd_strerror(err));
    }

    pcm_dma_apply_settings();

#ifdef USE_ASYNC_CALLBACK
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);
#else
    tick_add_task(pcm_tick);
#endif

    atexit(cleanup);
    return;
}


void pcm_play_lock(void)
{
#ifdef USE_ASYNC_CALLBACK
    pthread_mutex_lock(&pcm_mtx);
#else
    if (recursion++ == 0)
        tick_remove_task(pcm_tick);
#endif
}

void pcm_play_unlock(void)
{
#ifdef USE_ASYNC_CALLBACK
    pthread_mutex_unlock(&pcm_mtx);
#else
    if (--recursion == 0)
        tick_add_task(pcm_tick);
#endif
}

static void pcm_dma_apply_settings_nolock(void)
{
    snd_pcm_drop(handle);
    set_hwparams(handle, pcm_sampr);
#if defined(HAVE_NWZ_LINUX_CODEC)
    /* Sony NWZ linux driver uses a nonstandard mecanism to set the sampling rate */
    audiohw_set_frequency(pcm_sampr);
#endif
}

void pcm_dma_apply_settings(void)
{
    pcm_play_lock();
    pcm_dma_apply_settings_nolock();
    pcm_play_unlock();
}


void pcm_play_dma_pause(bool pause)
{
    snd_pcm_pause(handle, pause);
}


void pcm_play_dma_stop(void)
{
    snd_pcm_drain(handle);
}

void pcm_play_dma_start(const void *addr, size_t size)
{
    pcm_dma_apply_settings_nolock();

    pcm_data = addr;
    pcm_size = size;

    while (1)
    {
        snd_pcm_state_t state = snd_pcm_state(handle);
        switch (state)
        {
            case SND_PCM_STATE_RUNNING:
                return;
            case SND_PCM_STATE_XRUN:
            {
                DEBUGF("Trying to recover from error\n");
                int err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0)
                    DEBUGF("Recovery failed: %s\n", snd_strerror(err));
                continue;
            }
            case SND_PCM_STATE_SETUP:
            {
                int err = snd_pcm_prepare(handle);
                if (err < 0)
                    printf("Prepare error: %s\n", snd_strerror(err));
                /* fall through */
            }
            case SND_PCM_STATE_PREPARED:
            {   /* prepared state, we need to fill the buffer with silence before
                 * starting */
                int err = async_rw(handle);
                if (err < 0)
                    printf("Start error: %s\n", snd_strerror(err));
                return;
            }
            case SND_PCM_STATE_PAUSED:
            {   /* paused, simply resume */
                pcm_play_dma_pause(0);
                return;
            }
            case SND_PCM_STATE_DRAINING:
                /* run until drained */
                continue;
            default:
                DEBUGF("Unhandled state: %s\n", snd_pcm_state_name(state));
                return;
        }
    }
}

size_t pcm_get_bytes_waiting(void)
{
    return pcm_size;
}

const void * pcm_play_dma_get_peak_buffer(int *count)
{
    uintptr_t addr = (uintptr_t)pcm_data;
    *count = pcm_size / 4;
    return (void *)((addr + 3) & ~3);
}

void pcm_play_dma_postinit(void)
{
    audiohw_postinit();
}


void pcm_set_mixer_volume(int volume)
{
    (void)volume;
}
#ifdef HAVE_RECORDING
void pcm_rec_lock(void)
{
}

void pcm_rec_unlock(void)
{
}

void pcm_rec_dma_init(void)
{
}

void pcm_rec_dma_close(void)
{
}

void pcm_rec_dma_start(void *start, size_t size)
{
    (void)start;
    (void)size;
}

void pcm_rec_dma_stop(void)
{
}

const void * pcm_rec_dma_get_peak_buffer(void)
{
    return NULL;
}

void audiohw_set_recvol(int left, int right, int type)
{
    (void)left;
    (void)right;
    (void)type;
}

#endif /* HAVE_RECORDING */
