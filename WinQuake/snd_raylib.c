/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_raylib.c -- raylib AudioStream DMA driver for Quake

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>

#include "raylib.h"

//
// Quake DMA driver interface & structures (plain C types, no quakedef.h)
//
typedef int qboolean;

typedef struct
{
	int				gamealive;
	int				soundalive;
	int				splitbuffer;
	int				channels;
	int				samples;				// mono samples in buffer
	int				submission_chunk;		// don't mix less than this #
	int				samplepos;				// in mono samples
	int				samplebits;
	int				speed;
	unsigned char	*buffer;
} qdma_t;

typedef struct cvar_s
{
	char	*name;
	char	*string;
	qboolean archive;		// set to true to cause it to be saved to vars.rc
	qboolean server;		// notifies players when changed
	float	value;
	struct cvar_s *next;
} cvar_t;

typedef void (*xcommand_t)(void);

extern volatile qdma_t *shm;
extern volatile qdma_t sn;
extern int paintedtime;

extern void Con_Printf (const char *fmt, ...);
extern int COM_CheckParm (const char *parm);
extern char **com_argv;
extern int com_argc;
extern void Cmd_AddCommand (char *cmd_name, xcommand_t function);
extern void Cvar_RegisterVariable (cvar_t *variable);
extern float Cvar_VariableValue (char *var_name);
extern char *Cvar_VariableString (char *var_name);

// Buffer: 16384 stereo frames = 32768 mono samples = 65536 bytes
#define DMA_BUFFER_FRAMES	16384
#define DMA_BUFFER_SAMPLES	(DMA_BUFFER_FRAMES * 2)
#define DMA_BUFFER_BYTES	(DMA_BUFFER_SAMPLES * (int)sizeof(short))
#define DMA_FRAME_MASK		(DMA_BUFFER_FRAMES - 1)

static atomic_int		snd_inited;		// also read by the audio thread
static short			*dma_buffer = NULL;
static AudioStream		audio_stream;

// Pacing and synchronization
static atomic_uint_fast64_t	consumed_frames;
static atomic_uint_fast64_t	painted_frames_count;
static struct timespec		start_time;

// Underrun and diagnostic counters
// Written by the audio thread, read by the main thread (snd_stats, shutdown),
// so atomic: plain loads would be a data race, and arm64 won't hide it.
static atomic_uint_fast32_t	underrun_count;
static atomic_uint_fast64_t	underrun_frames;
static int				in_underrun = 0;		// audio thread only

// Debug WAV capture
static pthread_mutex_t	capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE				*capture_fp = NULL;
static uint32_t			capture_data_bytes = 0;
static int				capture_from_cmdline = 0;
static int				capture_rate = 11025;

static cvar_t			snd_capture_cvar = {"snd_capture", "", 0, 0, 0.0f, NULL};

// Helpers for WAV capture
static void WriteWavHeader(FILE *fp, int rate, int channels, int bits, uint32_t data_bytes)
{
	uint32_t riff_size = 36 + data_bytes;
	uint32_t fmt_size = 16;
	uint16_t format = 1; // PCM
	uint16_t num_ch = (uint16_t)channels;
	uint32_t sample_rate = (uint32_t)rate;
	uint16_t bits_per_sample = (uint16_t)bits;
	uint16_t block_align = (uint16_t)(channels * (bits / 8));
	uint32_t byte_rate = sample_rate * block_align;

	fseek(fp, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, fp);
	fwrite(&riff_size, 4, 1, fp);
	fwrite("WAVE", 1, 4, fp);
	fwrite("fmt ", 1, 4, fp);
	fwrite(&fmt_size, 4, 1, fp);
	fwrite(&format, 2, 1, fp);
	fwrite(&num_ch, 2, 1, fp);
	fwrite(&sample_rate, 4, 1, fp);
	fwrite(&byte_rate, 4, 1, fp);
	fwrite(&block_align, 2, 1, fp);
	fwrite(&bits_per_sample, 2, 1, fp);
	fwrite("data", 1, 4, fp);
	fwrite(&data_bytes, 4, 1, fp);
}

static void StartCapture(const char *filename, int rate)
{
	pthread_mutex_lock(&capture_mutex);
	if (capture_fp)
	{
		WriteWavHeader(capture_fp, capture_rate, 2, 16, capture_data_bytes);
		fclose(capture_fp);
		capture_fp = NULL;
	}

	if (!filename || !filename[0])
	{
		pthread_mutex_unlock(&capture_mutex);
		return;
	}

	capture_fp = fopen(filename, "wb");
	if (!capture_fp)
	{
		pthread_mutex_unlock(&capture_mutex);
		Con_Printf("snd_capture: could not open '%s' for writing\n", filename);
		return;
	}

	capture_rate = rate > 0 ? rate : ((shm && shm->speed) ? shm->speed : 11025);
	capture_data_bytes = 0;
	WriteWavHeader(capture_fp, capture_rate, 2, 16, 0);
	pthread_mutex_unlock(&capture_mutex);
	Con_Printf("snd_capture: recording audio to '%s' (%d Hz 16-bit stereo)\n", filename, capture_rate);
}

static void StopCapture(void)
{
	pthread_mutex_lock(&capture_mutex);
	if (!capture_fp)
	{
		pthread_mutex_unlock(&capture_mutex);
		return;
	}

	WriteWavHeader(capture_fp, capture_rate, 2, 16, capture_data_bytes);
	fclose(capture_fp);
	capture_fp = NULL;
	uint32_t saved_bytes = capture_data_bytes;
	capture_data_bytes = 0;
	pthread_mutex_unlock(&capture_mutex);

	Con_Printf("snd_capture: capture closed (%u bytes, %.2f s)\n",
		saved_bytes,
		capture_rate > 0 ? (double)saved_bytes / (capture_rate * 4.0) : 0.0);
}

static void Snd_Capture_f(void)
{
	// Inspect command line argument if any
	const char *arg = Cvar_VariableString("snd_capture");
	int rate = (shm && shm->speed) ? shm->speed : capture_rate;
	if (capture_fp)
	{
		StopCapture();
	}
	else if (arg && arg[0])
	{
		StartCapture(arg, rate);
	}
	else
	{
		StartCapture("quake_audio_capture.wav", rate);
	}
}

static void Snd_Stats_f(void)
{
	if (!snd_inited || !shm)
	{
		Con_Printf("Sound driver not initialized\n");
		return;
	}

	uint64_t consumed = atomic_load_explicit(&consumed_frames, memory_order_relaxed);
	uint64_t painted = atomic_load_explicit(&painted_frames_count, memory_order_relaxed);
	double lead_ms = shm->speed > 0 && painted >= consumed ?
		((double)(painted - consumed) * 1000.0 / shm->speed) : 0.0;

	Con_Printf("--- raylib AudioStream Stats ---\n");
	Con_Printf("  Rate: %d Hz, 16-bit stereo\n", shm->speed);
	Con_Printf("  Buffer size: %d frames (%.1f ms)\n", DMA_BUFFER_FRAMES,
		(double)DMA_BUFFER_FRAMES * 1000.0 / shm->speed);
	Con_Printf("  Frames consumed: %llu (%.2f s)\n", (unsigned long long)consumed,
		(double)consumed / shm->speed);
	Con_Printf("  Mix lead: %.1f ms (%llu frames ahead)\n", lead_ms,
		(unsigned long long)(painted > consumed ? painted - consumed : 0));
	Con_Printf("  Underrun count: %u (%llu frames dropped)\n",
		(unsigned)atomic_load_explicit(&underrun_count, memory_order_relaxed),
		(unsigned long long)atomic_load_explicit(&underrun_frames, memory_order_relaxed));
	if (capture_fp)
		Con_Printf("  WAV capture: active (%u bytes written)\n", capture_data_bytes);
	else
		Con_Printf("  WAV capture: inactive\n");
}

static double GetWallTime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)(ts.tv_sec - start_time.tv_sec) +
	       (double)(ts.tv_nsec - start_time.tv_nsec) * 1e-9;
}

//
// Audio callback: invoked by raylib/miniaudio audio thread
//
static void RaylibAudioCallback(void *buffer, unsigned int frames)
{
	short *out = (short *)buffer;
	if (!snd_inited || !dma_buffer)
	{
		memset(out, 0, frames * 2 * sizeof(short));
		return;
	}

	uint64_t read_start = atomic_load_explicit(&consumed_frames, memory_order_relaxed);
	uint64_t painted = atomic_load_explicit(&painted_frames_count, memory_order_acquire);

	int underrun_occurred = 0;
	uint64_t missed = 0;

	for (unsigned int i = 0; i < frames; i++)
	{
		uint64_t f = read_start + i;
		if (f < painted)
		{
			int idx = (int)(f & DMA_FRAME_MASK) * 2;
			out[i * 2]     = dma_buffer[idx];
			out[i * 2 + 1] = dma_buffer[idx + 1];

			// Clear consumed samples to prevent repetition on underrun
			dma_buffer[idx]     = 0;
			dma_buffer[idx + 1] = 0;
		}
		else
		{
			// Underrun: output silence
			out[i * 2]     = 0;
			out[i * 2 + 1] = 0;
			missed++;
			underrun_occurred = 1;
		}
	}

	if (underrun_occurred)
	{
		atomic_fetch_add_explicit(&underrun_frames, missed, memory_order_relaxed);
		if (!in_underrun)
		{
			atomic_fetch_add_explicit(&underrun_count, 1, memory_order_relaxed);
			in_underrun = 1;
		}
	}
	else
	{
		in_underrun = 0;
	}

	atomic_store_explicit(&consumed_frames, read_start + frames, memory_order_release);

	// Write to capture file if active (non-blocking trylock so realtime callback is never stalled)
	if (pthread_mutex_trylock(&capture_mutex) == 0)
	{
		if (capture_fp)
		{
			size_t bytes = frames * 2 * sizeof(short);
			fwrite(out, 1, bytes, capture_fp);
			capture_data_bytes += (uint32_t)bytes;
		}
		pthread_mutex_unlock(&capture_mutex);
	}
}

//
// SNDDMA_Init: initialize raylib audio device and AudioStream
//
int SNDDMA_Init(void)
{
	int speed = 11025; // Quake default
	int p;
	char *env_speed;

	snd_inited = 0;
	atomic_store_explicit(&consumed_frames, 0, memory_order_relaxed);
	atomic_store_explicit(&painted_frames_count, 0, memory_order_relaxed);
	atomic_store_explicit(&underrun_count, 0, memory_order_relaxed);
	atomic_store_explicit(&underrun_frames, 0, memory_order_relaxed);
	in_underrun = 0;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	// Check command-line or environment speed override
	p = COM_CheckParm("-sndspeed");
	if (p && p < com_argc - 1)
	{
		speed = atoi(com_argv[p + 1]);
	}
	else if ((env_speed = getenv("QUAKE_SOUND_SPEED")) != NULL)
	{
		speed = atoi(env_speed);
	}

	// Clamp to supported rates: 11025, 22050, 44100 (or keep positive valid rate)
	if (speed <= 0)
		speed = 11025;
	else if (speed < 16000)
		speed = 11025;
	else if (speed < 32000)
		speed = 22050;
	else
		speed = 44100;

	// Initialize raylib audio device if not already running
	if (!IsAudioDeviceReady())
	{
		SetTraceLogLevel(LOG_WARNING);
		InitAudioDevice();
		if (!IsAudioDeviceReady())
		{
			Con_Printf("SNDDMA_Init: raylib InitAudioDevice failed\n");
			return 0;
		}
	}

	// Allocate DMA ring buffer
	dma_buffer = (short *)calloc(DMA_BUFFER_FRAMES * 2, sizeof(short));
	if (!dma_buffer)
	{
		Con_Printf("SNDDMA_Init: failed to allocate %d bytes DMA buffer\n", DMA_BUFFER_BYTES);
		return 0;
	}

	// Setup DMA descriptor
	shm = &sn;
	shm->splitbuffer = 0;
	shm->channels = 2;              // 16-bit stereo
	shm->samplebits = 16;
	shm->speed = speed;
	capture_rate = speed;
	shm->samples = DMA_BUFFER_SAMPLES;
	shm->submission_chunk = 1;
	shm->samplepos = 0;
	shm->soundalive = 1;
	shm->gamealive = 1;
	shm->buffer = (unsigned char *)dma_buffer;

	// Load raylib AudioStream
	SetAudioStreamBufferSizeDefault(512);
	audio_stream = LoadAudioStream(speed, 16, 2);
	if (!IsAudioStreamValid(audio_stream))
	{
		Con_Printf("SNDDMA_Init: LoadAudioStream failed\n");
		free(dma_buffer);
		dma_buffer = NULL;
		return 0;
	}

	SetAudioStreamCallback(audio_stream, RaylibAudioCallback);
	PlayAudioStream(audio_stream);

	snd_inited = 1;

	// Register console commands & cvars
	Cmd_AddCommand("snd_info", Snd_Stats_f);
	Cmd_AddCommand("snd_stats", Snd_Stats_f);
	Cmd_AddCommand("snd_capture_toggle", Snd_Capture_f);
	Cvar_RegisterVariable(&snd_capture_cvar);

	// Check if capture was requested via command line -sndcapture <file>
	p = COM_CheckParm("-sndcapture");
	if (p && p < com_argc - 1)
	{
		capture_from_cmdline = 1;
		StartCapture(com_argv[p + 1], speed);
	}
	else
	{
		char *env_cap = getenv("QUAKE_SOUND_CAPTURE");
		if (env_cap && env_cap[0])
		{
			capture_from_cmdline = 1;
			StartCapture(env_cap, speed);
		}
	}

	Con_Printf("raylib audio driver initialized (%d Hz, 16-bit stereo, %d frame buffer)\n",
		speed, DMA_BUFFER_FRAMES);

	return 1;
}

//
// SNDDMA_GetDMAPos: returns current playback position in mono samples (0 .. shm->samples - 1)
//
int SNDDMA_GetDMAPos(void)
{
	if (!snd_inited || !shm)
		return 0;

	uint64_t consumed = atomic_load_explicit(&consumed_frames, memory_order_acquire);

	// Fallback pacing in case audio stream callback is delayed or stalled
	double elapsed = GetWallTime();
	uint64_t wall_f = (uint64_t)(elapsed * shm->speed);
	if (consumed < wall_f && (!IsAudioStreamPlaying(audio_stream) || !IsAudioDeviceReady()))
	{
		consumed = wall_f;
	}

	int frame_pos = (int)(consumed & DMA_FRAME_MASK);
	int samplepos = frame_pos * shm->channels;

	shm->samplepos = samplepos;
	return samplepos;
}

//
// SNDDMA_Submit: called by S_Update_ right after S_PaintChannels
//
void SNDDMA_Submit(void)
{
	if (!snd_inited)
		return;

	// Update painted_frames_count so the audio callback knows the safe limit
	atomic_store_explicit(&painted_frames_count, (uint64_t)paintedtime, memory_order_release);

	// Check if snd_capture cvar was modified at console
	if (!capture_from_cmdline)
	{
		if (snd_capture_cvar.string && snd_capture_cvar.string[0])
		{
			if (!capture_fp)
				StartCapture(snd_capture_cvar.string, shm ? shm->speed : 11025);
		}
		else if (capture_fp)
		{
			StopCapture();
		}
	}
}

//
// SNDDMA_Shutdown: stops audio stream and cleans up
//
void SNDDMA_Shutdown(void)
{
	if (!snd_inited)
		return;

	snd_inited = 0;

	if (capture_fp)
		StopCapture();

	if (IsAudioStreamValid(audio_stream))
	{
		StopAudioStream(audio_stream);
		UnloadAudioStream(audio_stream);
	}

	if (dma_buffer)
	{
		free(dma_buffer);
		dma_buffer = NULL;
	}

	if (shm)
	{
		shm->buffer = NULL;
		shm->soundalive = 0;
	}

	uint64_t consumed = atomic_load_explicit(&consumed_frames, memory_order_relaxed);
	Con_Printf("raylib audio driver shutdown: %llu frames consumed, %u underruns (%llu frames)\n",
		(unsigned long long)consumed,
		(unsigned)atomic_load_explicit(&underrun_count, memory_order_relaxed),
		(unsigned long long)atomic_load_explicit(&underrun_frames, memory_order_relaxed));
}
