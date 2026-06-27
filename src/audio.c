/* 
 * Gmu Music Player
 *
 * Copyright (c) 2006-2012 Johannes Heimansberg (wejp.k.vu)
 *
 * File: audio.c  Created: 061110
 *
 * Description: Audio output functions
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of
 * the License. See the file COPYING in the Gmu's main directory
 * for details.
 */
#include <math.h>
#include "SDL.h"
#include "ringbuffer.h"
#include "audio.h"
#include "fmath.h"
#include "debug.h"
#include "eventqueue.h"
#include "gmuerror.h"
#include "core.h"
#include FILE_HW_H
#if 1 // ZIPIT_Z2 INTERPOLATION
#define RINGBUFFER_SIZE 262144  // Allow some interpolation space to avoid blocking.
#else
#define RINGBUFFER_SIZE 131072
#endif

static RingBuffer    audio_rb;
static unsigned int  volume_fade_percent = 100;

static unsigned long buf_read_counter;
static int           done;
static int           have_samplerate, have_channels;
static SDL_mutex    *audio_mutex2;

static int           paused;
static SDL_mutex    *pause_mutex;

static size_t        spectrum_reg = 0;
static int16_t       amplitudes[16];
static SDL_mutex    *spectrum_mutex;

static int           device_open;

static unsigned int  volume, volume_internal;

#if 1 // ZIPIT_Z2 INTERPOLATION
// Pre-allocate an output buffer that has a little bit of breathing room.
// Since we are stretching by 1.18%, the output can be up to ~1.2% larger than the input chunk.
// If your standard 'size' is usually 4096 or 8192 bytes, 65536 bytes provides massive headroom.
// Um yeah, fileplayer requests 65536 bytes from decode_data, so we need > 32768 int16.
static int16_t interp_out_buf[36000];  
// static int16_t interp_out_buf[2][36000]; // I might need ping pong buffer.
// static int     buf_toggle = 0;           
static int interpolation_needed = 0;

#if 0
// FIXED-POINT CONFIGURATION FOR 1.18% HARDWARE DRIFT
// -------------------------------------------------------------
// If playback runs at 100.47% speed.
// Target Correction Scale (with 16.16 Fixed Point numbers) =  65536 / 1.0047 = 65229
// -------------------------------------------------------------
#define STEP_441K_FAMILY_STRETCH  65229  // Use 65229 for 0.47% (theoretical) or 65151 for 0.59%
#define STEP_PASSTHROUGH          65536  // 1:1 behavior

uint32_t step_fp = STEP_PASSTHROUGH;

// Apply your fixed-point stretch purely to the 44.1kHz family 
if (stream_sample_rate == 44100 || stream_sample_rate == 22050 || stream_sample_rate == 11025) {
    step_fp = STEP_441K_FAMILY_STRETCH;
} else {
    step_fp = STEP_PASSTHROUGH; // Leave 48k family un-interpolated
}
#endif

static void apply_linear_correction(int16_t *in_buf, int16_t *out_buf, int in_samples, int *out_samples, uint32_t step_fp) {
    static uint32_t fp_index = 0;
    
    // Persistent static registers acting as a shift register across blocks
    static int32_t s0_l = 0; 
    static int32_t s0_r = 0; 
    
    int out_idx = 0;

    // The loop runs natively through every single sample frame in the block
    while ((fp_index >> 16) < (uint32_t)in_samples) {
        uint32_t sample_idx = fp_index >> 16;
        uint32_t frac = fp_index & 0xFFFF;

        // 1. Fetch the active current target into s1
        int32_t s1_l = in_buf[sample_idx * 2];     
        int32_t s1_r = in_buf[sample_idx * 2 + 1]; 

        // 2. Interpolate between the persistent s0 and our fresh s1
        int32_t diff_l = s1_l - s0_l;
        out_buf[out_idx * 2] = (int16_t)(s0_l + (((int64_t)diff_l * frac) >> 16));

        int32_t diff_r = s1_r - s0_r;
        out_buf[out_idx * 2 + 1] = (int16_t)(s0_r + (((int64_t)diff_r * frac) >> 16));

        fp_index += step_fp;
	out_idx++;
	if ((fp_index >> 16) > sample_idx) {
	  s0_l = s1_l; 
	  s0_r = s1_r; 
	}
    }
    
    fp_index &= 0xFFFF; 
    *out_samples = out_idx; // Return the interpolated sample count
}

static void apply_cubic_correction(int16_t *in_buf, int16_t *out_buf, int in_samples, int *out_samples, uint32_t step_fp) {
    static uint32_t fp_index = 0;
    
    // 4-stage persistent shift registers for Left and Right channels
    static int32_t s0_l = 0, s1_l = 0, s2_l = 0, s3_l = 0; 
    static int32_t s0_r = 0, s1_r = 0, s2_r = 0, s3_r = 0; 
    
    int out_idx = 0;

    while ((fp_index >> 16) < (uint32_t)in_samples) {
        uint32_t sample_idx = fp_index >> 16;
        int64_t  f = fp_index & 0xFFFF; // 16-bit fraction (0 to 65535) 

        // Fetch the upcoming future look-ahead target from the buffer
        s3_l = in_buf[sample_idx * 2];     
        s3_r = in_buf[sample_idx * 2 + 1]; 

        // --- LEFT CHANNEL CUBIC SPLINE (Horner's Method) ---
        // Pre-calculate standard Catmull-Rom coefficients scaled to prevent 32-bit clip
        int64_t a_l = (-s0_l + 3 * s1_l - 3 * s2_l + s3_l) / 2;
        int64_t b_l = 2 * s0_l - 5 * s1_l + 4 * s2_l - s3_l;
        int64_t c_l = (-s0_l + s2_l) / 2;

        // Nested multiplication reduces hardware cycles: ((A*f + B)*f + C)*f + s1
        int64_t out_l = (((((a_l * f) >> 16) + b_l) * f) >> 16) + c_l;
        out_buf[out_idx * 2] = (int16_t)(s1_l + ((out_l * f) >> 16)); // 

        // --- RIGHT CHANNEL CUBIC SPLINE ---
        int64_t a_r = (-s0_r + 3 * s1_r - 3 * s2_r + s3_r) / 2;
        int64_t b_r = 2 * s0_r - 5 * s1_r + 4 * s2_r - s3_r;
        int64_t c_r = (-s0_r + s2_r) / 2;

        int64_t out_r = (((((a_r * f) >> 16) + b_r) * f) >> 16) + c_r;
        out_buf[out_idx * 2 + 1] = (int16_t)(s1_r + ((out_r * f) >> 16)); // 

        fp_index += step_fp;
        out_idx++; 

        // 4-STAGE PIPELINE SHIFT: Only step the timeline forward when index changes
        if ((fp_index >> 16) > sample_idx) { 
            s0_l = s1_l; s1_l = s2_l; s2_l = s3_l; // 
            s0_r = s1_r; s1_r = s2_r; s2_r = s3_r; // 
        }
    }

    fp_index &= 0xFFFF; 
    *out_samples = out_idx; 
}
#endif

int audio_fill_buffer(char *data, size_t size)
{
	int result = 0;
#if 1 // ZIPIT_Z2 INTERPOLATION
	// 2. Apply interpolation ONLY if we are playing the problematic 44.1kHz family
	if (interpolation_needed) {
		int16_t *in_samples = (int16_t *)data;
		int in_sample_count = size / 4; // 4 bytes per stereo frame (16-bit L + 16-bit R)
		int samples_generated = 0;

		// Ensure we have plenty of room for expanded samples BEFORE the interpolation.
		if (audio_buffer_get_size() - audio_buffer_get_fill() < size * 5 / 4)
		  return 0;

		// Apply your hardware slowdown multiplier
		//apply_linear_correction(in_samples, interp_out_buf, in_sample_count, &samples_generated, 65229);
		apply_cubic_correction(in_samples, interp_out_buf, in_sample_count, &samples_generated, 65229);
		// Map the new sample timeline payload back to an absolute byte size
		//wdprintf(V_DEBUG, "audio", "INTERPOLATION (%d ==> %d)\n", size, (samples_generated * 4));
		size = (size_t)samples_generated * 4;
		data = (char *)interp_out_buf;
	}
	// 3. Native pass-through mode: 48kHz, 32kHz, etc. run with 0% interpolation CPU overhead
#endif    
	SDL_LockAudio();
	result = ringbuffer_write(&audio_rb, data, size);
	SDL_UnlockAudio();
	return result;
}

static void calculate_dft(int16_t *input_signal, int input_signal_size, int *rex, int *imx)
{
	size_t res_size = (size_t)input_signal_size / 2 + 1;

	if (rex && imx) {
		int i, j;
		size_t rs = res_size * sizeof(int);
		memset(rex, 0, rs);
		memset(imx, 0, rs);
		for (j = 0; j < res_size; j++) {
			for (i = 0; i < input_signal_size; i++) {
				/*if (rex) rex[j] = rex[j] + input_signal[i] * cos(2*M_PI*j*i/input_signal_size);
				if (imx) imx[j] = imx[j] + input_signal[i] * sin(2*M_PI*j*i/input_signal_size);*/
				if (rex) rex[j] = rex[j] + input_signal[i] * fcos(F_PI2*j*i/input_signal_size);
				if (imx) imx[j] = imx[j] + input_signal[i] * fsin(F_PI2*j*i/input_signal_size);
			}
		}
		for (j = 0; j < res_size; j++) {
			if (rex) rex[j] /= 10000;
			if (imx) imx[j] /= 10000;
		}
	}
}

int16_t *audio_spectrum_get_current_amplitudes(void)
{
	return amplitudes;
}

void audio_spectrum_register_for_access(void)
{
	spectrum_reg++;
}

void audio_spectrum_unregister(void)
{
	if (spectrum_reg > 0) spectrum_reg--;
}

int audio_spectrum_read_lock(void)
{
	return !SDL_LockMutex(spectrum_mutex);
}

void audio_spectrum_read_unlock(void)
{
	SDL_UnlockMutex(spectrum_mutex);
}

/* len has to be of type int here, because SDL expects this. */
static void fill_audio(void *udata, Uint8 *stream, int len)
{
	static Uint8 buf[65536];
	size_t       add = 0;

	if (ringbuffer_read(&audio_rb, (char *)buf, (size_t)len)) {
		add = (size_t)len;
	} else {
		size_t avail = ringbuffer_get_fill(&audio_rb);
		memset(buf, 0, 65536);
		if (avail > 0 && ringbuffer_read(&audio_rb, (char *)buf, avail))
			add = avail;
	}

	if (SDL_LockMutex(audio_mutex2) == 0) {
		buf_read_counter += add;
		SDL_UnlockMutex(audio_mutex2);
	}
	SDL_memset(stream, 0, (size_t)len);
	SDL_MixAudio(stream, buf, (Uint32)len, (int)(volume * volume_fade_percent) / 100);

	/* When requested, run DFT on a few samples of each block of data for visualization purposes */
	if (spectrum_reg > 0) {
		int     rex[9], imx[9];
		size_t  i, j;
		int16_t samples_l[16];
		int     channels = 0;

		if (SDL_LockMutex(audio_mutex2) != -1) {
			channels = have_channels;
			SDL_UnlockMutex(audio_mutex2);
		}

		if (channels > 0) {
			for (i = 0, j = 0; j < 16; i += (2 * (unsigned)channels), j++) {
				samples_l[j] = (buf[i+1] << 8) + buf[i];
			}
			calculate_dft(samples_l, 16, rex, imx);
		}
		SDL_LockMutex(spectrum_mutex);
		if (channels > 0)
			for (i = 1; i < 9; i++) amplitudes[i-1] = (imx[i] < 0 ? -imx[i] : imx[i]);
		SDL_UnlockMutex(spectrum_mutex);
	}
}

int audio_device_open(int samplerate, int channels)
{
	static SDL_AudioSpec wanted, obtained;
	int                  result = -1;

	/* Keep audio device open unless sampling rate or number of channels change */
	if (SDL_LockMutex(audio_mutex2) != -1) {
		buf_read_counter = 0;
		wdprintf(V_DEBUG, "audio", "Device already open: %s\n", device_open ? "yes" : "no");
		if (device_open)
			wdprintf(V_DEBUG, "audio", "Samplerate: have=%d want=%d Channels: have=%d want=%d\n",
					 have_samplerate, samplerate, have_channels, channels);
		if (!device_open || samplerate != have_samplerate || channels != have_channels) {
			if (device_open) {
				SDL_UnlockMutex(audio_mutex2);
				audio_device_close();
				SDL_LockMutex(audio_mutex2);
			}
			wdprintf(V_INFO, "audio", "Opening audio device...\n");
			wanted.freq     = samplerate;
			wanted.format   = AUDIO_S16;
			wanted.channels = (Uint8)channels; /* 1 = mono, 2 = stereo */
			wanted.samples  = SAMPLE_BUFFER_SIZE;
			wanted.callback = fill_audio;
			wanted.userdata = NULL;
			SDL_ClearError();
			if (SDL_OpenAudio(&wanted, &obtained) < 0) {
				wdprintf(V_ERROR, "audio", "Could not open audio: %s\n", SDL_GetError());
				event_queue_push_with_parameter(gmu_core_get_event_queue(),
				                                GMU_ERROR,
				                                GMU_ERROR_CANNOT_OPEN_AUDIO_DEVICE);
				result = -3;
			} else {
				result = 0;
				device_open = 1;
				have_samplerate = samplerate;
				have_channels   = channels;
#if 1 // ZIPIT_Z2 INTERPOLATION
				// Calculate the interpolation_needed once here
				// Maybe add IS_URL check(url/filename) for net stream only?
				if (samplerate == 44100 || samplerate == 22050 || samplerate == 11025) {
					interpolation_needed = 1;
				} else {
					interpolation_needed = 0;
				}
				wdprintf(V_DEBUG, "audio", "INTERPOLATION = %d.\n", interpolation_needed);
#endif
				wdprintf(V_INFO, "audio", "Device opened with %d Hz, %d channels and sample buffer w/ %d samples.\n",
						 obtained.freq, obtained.channels, obtained.samples);
			}
			if (SDL_UnlockMutex(audio_mutex2) != -1) {
				SDL_LockAudio();
				ringbuffer_clear(&audio_rb);
				SDL_UnlockAudio();
				SDL_LockMutex(audio_mutex2);
			}
		} else {
			wdprintf(V_INFO, "audio", "Using already opened audio device with the same settings...\n");
			result = 0;
		}

		if (result == 0) {
			done = 0;
		}
		SDL_UnlockMutex(audio_mutex2);
	}
	return result;
}

int audio_get_status(void)
{
	int res = 0;
	if (SDL_LockMutex(pause_mutex) != -1) {
		res = SDL_GetAudioStatus();
		SDL_UnlockMutex(pause_mutex);
	}
	return res;
}

void audio_force_pause(int pause)
{
	if (SDL_LockMutex(pause_mutex) != -1) {
		SDL_PauseAudio(pause);
		SDL_UnlockMutex(pause_mutex);
	}
}

int audio_set_pause(int pause_state)
{
	int res = 0;
	if (device_open) {
		wdprintf(V_DEBUG, "audio", "%s\n", pause_state ? "Pause!" : "Play!");
		if (SDL_LockMutex(pause_mutex) != -1) {
			if (paused != pause_state) {
				paused = pause_state;
				if (paused) memset(amplitudes, 0, sizeof(int16_t) * 16);
				res = paused;
				SDL_PauseAudio(paused);
			}
			SDL_UnlockMutex(pause_mutex);
		}
	} else {
		wdprintf(V_WARNING, "audio", "Device not opened. Cannot set pause state!\n");
	}
	return res;
}

void audio_set_done(void)
{
	if (SDL_LockMutex(audio_mutex2) != -1) {
		done = 1;
		SDL_UnlockMutex(audio_mutex2);
	}
}

int audio_get_pause(void)
{
	int res = 0;
	if (SDL_LockMutex(pause_mutex) != -1) {
		res = paused;
		SDL_UnlockMutex(pause_mutex);
	}
	return res;
}

size_t audio_get_playtime(void)
{
	size_t res = 0;
	if (SDL_LockMutex(audio_mutex2) != -1) {
		res = buf_read_counter / (size_t)(have_samplerate * 2 * have_channels) * 1000;
		SDL_UnlockMutex(audio_mutex2);
	}
	return res;
}

size_t audio_buffer_get_fill(void)
{
	size_t res = 0;
	SDL_LockAudio();
	res = ringbuffer_get_fill(&audio_rb);
	SDL_UnlockAudio();
	return res;
}

size_t audio_buffer_get_size(void)
{
	size_t res = 0;
	SDL_LockAudio();
	res = ringbuffer_get_size(&audio_rb);
	SDL_UnlockAudio();
	return res;
}

void audio_buffer_init(void)
{
	volume = SDL_MIX_MAXVOLUME;
	volume_internal = 15;
	paused = 1;
	done = 0;
	device_open = 0;
	have_samplerate = 1;
	have_channels = 1;
	ringbuffer_init(&audio_rb, RINGBUFFER_SIZE);
	spectrum_mutex = SDL_CreateMutex();
	audio_mutex2 = SDL_CreateMutex();
	pause_mutex = SDL_CreateMutex();
}

void audio_buffer_clear(void)
{
	audio_set_pause(1);
	SDL_LockAudio();
	ringbuffer_clear(&audio_rb);
	SDL_UnlockAudio();
}

void audio_buffer_free(void)
{
	ringbuffer_free(&audio_rb);
	SDL_DestroyMutex(pause_mutex);
	SDL_DestroyMutex(spectrum_mutex);
	if (audio_mutex2) SDL_DestroyMutex(audio_mutex2);
}

void audio_device_close(void)
{
	if (device_open) {
		wdprintf(V_DEBUG, "audio", "Closing device.\n");
		audio_set_pause(1);
		device_open = 0;
		SDL_CloseAudio();
		wdprintf(V_INFO, "audio", "Device closed.\n");
	}
}

static const unsigned int volume_array[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 32, 48, 64, 96, 128 };

void audio_set_volume(unsigned int vol) /* 0..AUDIO_MAX_SW_VOLUME */
{
	volume_internal = (vol < AUDIO_MAX_SW_VOLUME ? vol : AUDIO_MAX_SW_VOLUME-1);
	volume_internal = (volume_internal > 0 ? volume_internal : 0);
	volume = volume_array[volume_internal]; /*(SDL_MIX_MAXVOLUME / AUDIO_MAX_SW_VOLUME) * volume_internal;*/
	wdprintf(V_DEBUG, "audio", "volume=%d (%d/%d)\n", volume, SDL_MIX_MAXVOLUME, AUDIO_MAX_SW_VOLUME);
}

unsigned int audio_get_volume(void)
{
	return volume_internal;
}

size_t audio_set_sample_counter(size_t sample)
{
	size_t res = 0;
	if (SDL_LockMutex(audio_mutex2) != -1) {
		res = buf_read_counter = (sample * 2 * (size_t)have_channels);
		SDL_UnlockMutex(audio_mutex2);
	}
	return res;
}

size_t audio_increase_sample_counter(size_t sample_offset)
{
	size_t res = 0;
	if (SDL_LockMutex(audio_mutex2) != -1) {
		buf_read_counter += (sample_offset * 2 * (size_t)have_channels);
		res = buf_read_counter;
		SDL_UnlockMutex(audio_mutex2);
	}
	return res;
}

size_t audio_get_sample_count(void)
{
	size_t res = 0;
	if (SDL_LockMutex(audio_mutex2) != -1) {
		res = buf_read_counter / (2 * (size_t)have_channels);
		SDL_UnlockMutex(audio_mutex2);
	}
	return res;
}

void audio_set_fade_volume(unsigned int percent)
{
	SDL_LockAudio();
	if (percent >= 0 && percent <= 100)
		volume_fade_percent = percent;
	SDL_UnlockAudio();
}

/**
 * Fades out one step. To be called multiple times until the volume has
 * reached 0. Returns 1 when the volume is zero, and 0 otherwise.
 */
int audio_fade_out_step(unsigned int step_size)
{
	int res;
	SDL_LockAudio();
	if (volume_fade_percent > 0 && volume_fade_percent >= step_size)
		volume_fade_percent -= step_size;
	else
		volume_fade_percent = 0;
	wdprintf(V_DEBUG, "audio", "fadeout: %d\n", volume_fade_percent);
	res = (volume_fade_percent == 0 ? 1 : 0);
	SDL_UnlockAudio();
	return res;
}

void audio_reset_fade_volume(void)
{
	SDL_LockAudio();
	volume_fade_percent = 100;
	SDL_UnlockAudio();
}

int audio_fade_out_in_progress(void)
{
	int res;
	SDL_LockAudio();
	res = (volume_fade_percent < 100 && volume_fade_percent > 0) ? 1 : 0;
	SDL_UnlockAudio();
	return res;
}
