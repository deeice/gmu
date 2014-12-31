/* 
 * Gmu Music Player
 *
 * Copyright (c) 2006-2014 Johannes Heimansberg (wejp.k.vu)
 *
 * File: fileplayer.c  Created: 070107
 *
 * Description: Functions for audio file playback
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of
 * the License. See the file COPYING in the Gmu's main directory
 * for details.
 */
#include "SDL/SDL.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "fileplayer.h"
#include "audio.h"
#include "trackinfo.h"
#include "id3.h"
#include "util.h"
#include "pbstatus.h"
#include "gmudecoder.h"
#include "decloader.h"
#include "charset.h"
#include "debug.h"
#include "reader.h"
#include "core.h"
#include "eventqueue.h"
#include "gmuerror.h"

#define BUF_SIZE 65536

static char      lyrics_file_pattern[256];
static long      seek_second;
static int       thread_running = 0;
static int       file_player_shut_down = 0;

/**
 * Overall playback status. When this is set to PLAYING, it indicates that
 * the file player should continue playing with the next track in the
 * playlist, after the current one has finished.
 */
static PB_Status playback_status = STOPPED;

/**
 * Playback status for current item. This is set to PLAYING as soon as a
 * track starts playing (decode_audio_thread is started) and is set to
 * STOPPED as soon as the track finishes. When setting this to STOPPED
 * the decoder thread stops as soon as possible. Use 
 * file_player_stop_playback() to stop playback.
 */
/* DEPRECATED/TODO To be removed. We only want one playback status and one 
 * request for a playback status (see below). */
static PB_Status item_status;

/**
 * The requested playback status. This changes when the user requests
 * a status change, e.g. pushes the 'play', 'pause' oder 'stop' button.
 * It is only ever to be set by a user interaction (which can include the
 * user's request to resume playback on program start, but other than
 * that usually corresponds directly to a button press/mouse click of
 * the user).
 */
static PB_Status_Request user_pb_request = PBRQ_NONE;

static char      *file = NULL;
static TrackInfo *ti;

static pthread_mutex_t mutex;


void file_player_set_lyrics_file_pattern(const char *pattern)
{
	strncpy(lyrics_file_pattern, pattern ? pattern : "", 255);
}

int file_player_playback_get_time(void)
{
 	return audio_get_playtime();
}

PB_Status file_player_get_playback_status(void)
{
	return playback_status;
}

PB_Status file_player_get_item_status(void)
{
	return item_status;
}

int file_player_is_thread_running(void)
{
	return thread_running;
}

static void *decode_audio_thread(void *udata);
static pthread_t thread;

void file_player_shutdown(void)
{
	playback_status = STOPPED;
	item_status = STOPPED;
	file_player_shut_down = 1;
	file_player_set_filename(NULL);
	file_player_start_playback(); /* Release waiting lock in thread */
	pthread_join(thread, NULL);
	if (file) free(file);
	pthread_mutex_destroy(&mutex);
}

static int locked = 0;

void file_player_set_filename(char *filename)
{
	if (!locked) pthread_mutex_lock(&mutex);
	locked = 1;
	if (filename) {
		int len = strlen(filename);
		if (len > 0) {
			file = realloc(file, len+1);
			if (file) memcpy(file, filename, len+1);
		}
	}
}

void file_player_start_playback(void)
{
	pthread_mutex_unlock(&mutex);
	locked = 0;
}

int file_player_init(TrackInfo *ti_ref)
{
	pthread_mutex_init(&mutex, NULL);
	file_player_set_filename(NULL);
	ti = ti_ref;
	pthread_create(&thread, NULL, decode_audio_thread, NULL);
	return 0;
}

void file_player_stop_playback(void)
{
	playback_status = STOPPED;
	item_status = STOPPED;
	wdprintf(V_INFO, "fileplayer", "Stop playback!\n");
	file_player_set_filename(NULL);
}


/* Return 1 when new meta data differs from previous data, 0 otherwise */
static int update_metadata(GmuDecoder *gd, TrackInfo *ti, GmuCharset charset)
{
	TrackInfo ti_tmp;
	int       differ = 0;

	if (*gd->get_meta_data) {
		trackinfo_copy(&ti_tmp, ti);
		trackinfo_set_artist(&ti_tmp, "");
		trackinfo_set_title(&ti_tmp, "");
		trackinfo_set_album(&ti_tmp, "");
		if ((*gd->get_meta_data)(GMU_META_ARTIST, 1))
			strncpy_charset_conv(ti_tmp.artist,  (*gd->get_meta_data)(GMU_META_ARTIST, 1), SIZE_ARTIST-1, 0, charset);
		if ((*gd->get_meta_data)(GMU_META_TITLE, 1))
			strncpy_charset_conv(ti_tmp.title,   (*gd->get_meta_data)(GMU_META_TITLE, 1), SIZE_TITLE-1, 0, charset);
		if ((*gd->get_meta_data)(GMU_META_ALBUM, 1))
			strncpy_charset_conv(ti_tmp.album,   (*gd->get_meta_data)(GMU_META_ALBUM, 1), SIZE_ALBUM-1, 0, charset);
		if ((*gd->get_meta_data)(GMU_META_TRACKNR, 1))
			strncpy_charset_conv(ti_tmp.tracknr, (*gd->get_meta_data)(GMU_META_TRACKNR, 1), SIZE_TRACKNR-1, 0, charset);
		if ((*gd->get_meta_data)(GMU_META_DATE, 1))
			strncpy_charset_conv(ti_tmp.date,    (*gd->get_meta_data)(GMU_META_DATE, 1), SIZE_DATE-1, 0, charset);

		if (ti_tmp.title[0] == '\0') {
			char *filename_without_path = strrchr(ti_tmp.file_name, '/');
			if (filename_without_path != NULL)
				filename_without_path++;
			else
				filename_without_path = (char *)ti_tmp.file_name;

			if (!strncpy_charset_conv(ti_tmp.title, filename_without_path, SIZE_TITLE-1, 0, M_CHARSET_AUTODETECT))
				ti_tmp.title[0] = '\0';
		}

		/* Check if new dataset differs from old dataset: */
		if (strlen(ti->title) == 0 || strncmp(ti_tmp.title, ti->title, SIZE_TITLE) != 0)
			differ = 1;
		else if (strlen(ti->artist) == 0 || strncmp(ti_tmp.artist, ti->artist, SIZE_ARTIST) != 0)
			differ = 1;
		else if (strlen(ti->album) == 0 || strncmp(ti_tmp.album, ti->album, SIZE_ALBUM) != 0)
			differ = 1;

		if (differ) {
			trackinfo_copy(ti, &ti_tmp);
			if (*gd->get_meta_data_int) {
				if ((*gd->get_meta_data_int)(GMU_META_IMAGE_DATA_SIZE, 1) &&
				   ((*gd->get_meta_data)(GMU_META_IMAGE_DATA, 1)) &&
				   (*gd->get_meta_data)(GMU_META_IMAGE_MIME_TYPE, 1))
					trackinfo_set_image(ti, (char *)((*gd->get_meta_data)(GMU_META_IMAGE_DATA, 1)),
										(*gd->get_meta_data_int)(GMU_META_IMAGE_DATA_SIZE, 1),
										(char *)((*gd->get_meta_data)(GMU_META_IMAGE_MIME_TYPE, 1)));
			}
			trackinfo_set_updated(ti);
		}
	}
	return differ;
}

static void *decode_audio_thread(void *udata)
{
	GmuDecoder *gd = NULL;
	Reader     *r;
	long        ret  = 1;
	static char pcmout[BUF_SIZE];
	GmuCharset  charset = M_CHARSET_AUTODETECT;

	thread_running = 1;

	wdprintf(V_INFO, "fileplayer", "File player thread initialized.\n");
	while (thread_running && !file_player_shut_down) {
		char *filename = NULL;
		int   len = 0;
		pthread_mutex_lock(&mutex);
		if (file) len = strlen(file);
		if (len > 0) {
			filename = malloc(len+1);
			if (filename) {
				strncpy(filename, file, len);
				filename[len] = '\0';
				item_status = PLAYING;
			}
			free(file); file = NULL;
		}
		pthread_mutex_unlock(&mutex);
		wdprintf(V_DEBUG, "fileplayer", "Here we go...\n");
		r = NULL;
		if (!file_player_shut_down && filename && item_status == PLAYING) {
			const char *tmp = get_file_extension(filename);
			wdprintf(V_INFO, "fileplayer", "Playing %s...\n", filename);
			if (tmp) gd = decloader_get_decoder_for_extension(tmp);
			if (!(gd && gd->identifier)) { /* No decoder found by extension, try mime-type check by data chunk */
				wdprintf(V_WARNING, "fileplayer", "No suitable decoder available for extension %s. Trying mime type check.\n", tmp);
				r = reader_open(filename);
				if (r && reader_read_bytes(r, 4096)) {
					char *mime_type = cfg_get_key_value_ignore_case(r->streaminfo, "content-type");
					if (mime_type)
						gd = decloader_get_decoder_for_mime_type(mime_type);
					else
						gd = decloader_get_decoder_for_data_chunk(reader_get_buffer(r), reader_get_number_of_bytes_in_buffer(r));
				}
			}
			if (gd && gd->identifier && !file_player_shut_down) {
				wdprintf(V_INFO, "fileplayer", "Selected decoder: %s\n", gd->identifier);
				if (gd->set_reader_handle) {
					if (!r) {
						r = reader_open(filename);
						if (r) reader_read_bytes(r, 4096);
					}
				} else {
					if (r) {
						reader_close(r);
						r = NULL;
					}
				}

				if (*gd->meta_data_get_charset) charset = (*gd->meta_data_get_charset)();
				if (gd->set_reader_handle) (*gd->set_reader_handle)(r);
				playback_status = PLAYING;

				audio_reset_fade_volume();
				if (item_status == PLAYING && !file_player_shut_down && (*gd->open_file)(filename)) {
					int channels = 0;
					if (trackinfo_acquire_lock(ti)) {
						trackinfo_clear(ti);
						if (charset_is_valid_utf8_string(filename))
							strncpy(ti->file_name, filename, SIZE_FILE_NAME-1);
						else
							charset_iso8859_1_to_utf8(ti->file_name, filename, SIZE_FILE_NAME-1);

						/* Assume 44.1 kHz stereo as default */
						ti->samplerate = 44100;
						ti->channels   = 2;
						ti->bitrate    = 0;

						if (*gd->get_samplerate)
							ti->samplerate = (*gd->get_samplerate)();
						if (*gd->get_channels)
							ti->channels   = (*gd->get_channels)();
						if (*gd->get_bitrate)
							ti->bitrate    = (*gd->get_bitrate)();
						if (*gd->get_length)
							ti->length     = (*gd->get_length)();
						if (*gd->get_file_type)
							strncpy_charset_conv(ti->file_type, (*gd->get_file_type)(),
												 SIZE_FILE_TYPE-1, 0, charset);
						channels = ti->channels;
						trackinfo_release_lock(ti);
					}

					if (channels > 0 && trackinfo_acquire_lock(ti)) {
						wdprintf(V_INFO, "fileplayer", "Found %s stream w/ %d channel(s), %d Hz, %ld bps, %d seconds\n",
								 ti->file_type, ti->channels, ti->samplerate, ti->bitrate, ti->length);

						if (!trackinfo_has_lyrics(ti)) {
							char *lyrics_file = get_file_matching_given_pattern_alloc(filename, lyrics_file_pattern);
							if (lyrics_file) {
								wdprintf(V_DEBUG, "fileplayer", "Trying to load lyrics from file %s...\n", lyrics_file);
								if (trackinfo_load_lyrics_from_file(ti, lyrics_file))
									wdprintf(V_DEBUG, "fileplayer", "Loading lyrics was successful.\n");
								else
									wdprintf(V_WARNING, "fileplayer", "Loading lyrics from file failed.\n");
								free(lyrics_file);
							}
							/*wdprintf(V_DEBUG, "fileplayer", "LYRICS:%s\n",ti->lyrics);*/
						}

						if (audio_device_open(ti->samplerate, ti->channels) < 0) {
							wdprintf(V_ERROR, "fileplayer", "Couldn't open audio: %s\n", SDL_GetError());
						} else {
							wdprintf(V_DEBUG, "fileplayer", "Audio device ready!\n");
						}

						/* read meta data */
						if (update_metadata(gd, ti, charset))
							event_queue_push(gmu_core_get_event_queue(), GMU_TRACKINFO_CHANGE);
						trackinfo_release_lock(ti);

						if (user_pb_request == PBRQ_PLAY) audio_set_pause(0);

						if (r && !reader_is_ready(r)) {
							int check_count = 20, prev_buf_fill = 0;
							/* Wait for the reader to pre-buffer the requested amount of data (if necessary) */
							wdprintf(V_DEBUG, "fileplayer", "Prebuffering...\n");
							event_queue_push(gmu_core_get_event_queue(), GMU_BUFFERING);
							while (r && !reader_is_ready(r) && !reader_is_eof(r) && item_status == PLAYING && check_count > 0) {
								int buf_fill = reader_get_cache_fill(r);
								if (prev_buf_fill != buf_fill) {
									prev_buf_fill = buf_fill;
									check_count = 20;
								} else {
									check_count--;
								}
								SDL_Delay(200);
							}
							if (check_count <= 0) {
								item_status = FINISHED;
								wdprintf(V_DEBUG, "fileplayer", "Prebuffering failed.\n");
								event_queue_push(gmu_core_get_event_queue(), GMU_BUFFERING_FAILED);
							} else {
								event_queue_push(gmu_core_get_event_queue(), GMU_BUFFERING_DONE);
							}
						}

						while (ret && (item_status == PLAYING || audio_fade_out_in_progress()) && !file_player_shut_down) {
							int size = 0, ret = 1, br = 0;

							if (seek_second) {
								if (item_status == PLAYING && (!gd->set_reader_handle || reader_is_seekable(r))) {
									if (seek_second < 0) seek_second = 0;
									if (*gd->seek && (*gd->seek)(seek_second))
										audio_set_sample_counter(seek_second * ti->samplerate);
								}
								seek_second = 0;
							}
							if (audio_fade_out_in_progress()) {
								if (audio_fade_out_step(12)) item_status = STOPPED;
							}
							while (ret > 0 && size < BUF_SIZE / 2 && item_status == PLAYING) {
								ret = (*gd->decode_data)(pcmout+size, BUF_SIZE-size);
								size += ret;
							}
							if (gd->get_current_bitrate) br = (*gd->get_current_bitrate)();
							if (br > 0) {
								if (trackinfo_acquire_lock(ti)) {
									ti->recent_bitrate = br;
									trackinfo_release_lock(ti);
								}
							}
							if (ret == 0) {
								item_status = FINISHED;
								break;
							} else if (ret < 0) {
								wdprintf(V_ERROR, "fileplayer", "Error. Code: %d\n", ret);
								audio_set_pause(1);
								item_status = FINISHED;
								break;
							} else {
								int ret = 0;
								while (!ret && item_status == PLAYING) {
									if (audio_buffer_get_fill() > MIN_BUFFER_FILL * 3)
										audio_wait_until_more_data_is_needed();
									ret = audio_fill_buffer(pcmout, size);
									if (!ret) SDL_Delay(10);
									if (item_status == PLAYING && user_pb_request == PBRQ_PLAY && audio_get_pause()) {
										wdprintf(V_DEBUG, "fileplayer", "Unpause audio due to user request...\n");
										audio_set_pause(0);
									}
								}
								if (SDL_GetAudioStatus() != SDL_AUDIO_PLAYING &&
									!audio_get_pause() && playback_status == PLAYING &&
									audio_buffer_get_fill() > audio_buffer_get_size() / 2 &&
									user_pb_request == PBRQ_PLAY) {
									wdprintf(V_DEBUG, "fileplayer", "Unpausing audio...\n");
									SDL_PauseAudio(0);
								}
							}
							if (*gd->get_meta_data_int) {
								if ((*gd->get_meta_data_int)(GMU_META_IS_UPDATED, 1)) {
									if (trackinfo_acquire_lock(ti)) {
										if (update_metadata(gd, ti, charset)) {
											event_queue_push(gmu_core_get_event_queue(), GMU_TRACKINFO_CHANGE);
											wdprintf(V_DEBUG, "fileplayer", "Meta data change detected!\n");
										}
										trackinfo_release_lock(ti);
									}
								}
							}
						}
						wdprintf(V_INFO, "fileplayer", "Playback stopped.\n");
						seek_second = 0;
					} else {
						wdprintf(V_WARNING, "fileplayer", "Broken audio stream.\n");
					}
					(*gd->close_file)();
				} else {
					wdprintf(V_DEBUG, "fileplayer", "Unable to open file.\n");
					event_queue_push_with_parameter(gmu_core_get_event_queue(),
													GMU_ERROR,
													GMU_ERROR_CANNOT_OPEN_FILE);
				}
			}
			if (item_status == STOPPED) audio_buffer_clear();
			if (item_status != STOPPED) item_status = FINISHED;
			wdprintf(V_DEBUG, "fileplayer", "Decoder thread: Playback done.\n");
			audio_set_done();
		}
		if (filename) {
			free(filename);
			filename = NULL;
		}
		if (r) reader_close(r);
		if (gd && gd->set_reader_handle) {
			(*gd->set_reader_handle)(NULL);
		}
		usleep(100000);
	}
	wdprintf(V_DEBUG, "fileplayer", "Decoder thread finished.\n");
	thread_running = 0;
	return NULL;
}

int file_player_play_file(char *filename, int skip_current, int fade_out_on_skip)
{
	if (skip_current && fade_out_on_skip)
		audio_fade_out_step(16); /* Initiate fade-out and decrease volume by 16 % */
	else
		item_status = skip_current ? STOPPED : FINISHED;
	playback_status = PLAYING;

	wdprintf(V_INFO, "fileplayer", "Trying to play %s...\n", filename);
	file_player_set_filename(filename);
	file_player_start_playback();
	return playback_status;
}

int file_player_seek(long offset)
{
	seek_second = audio_get_playtime() / 1000 + offset;
	return 0;
}

TrackInfo *file_player_get_trackinfo_ref(void)
{
	return ti;
}

int fileplayer_request_playback_state_change(PB_Status_Request request)
{
	int res = 0;
	user_pb_request = request;
	wdprintf(V_DEBUG, "fileplayer", "Requested playback state: %d\n", request);
	switch (user_pb_request) {
		case PBRQ_PAUSE:
			audio_set_pause(1);
			break;
		case PBRQ_STOP:
			file_player_stop_playback();
			audio_set_pause(1);
			break;
		case PBRQ_PLAY:
		default:
			break;
	}
	return res;
}