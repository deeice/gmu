/* 
 * Gmu Music Player
 *
 * Copyright (c) 2006-2010 Johannes Heimansberg (wejp.k.vu)
 *
 * File: mpg123.c  Created: 090606
 *
 * Description: mpg123 MPEG decoder
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of
 * the License. See the file COPYING in the Gmu's main directory
 * for details.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mpg123.h>
#include "../gmudecoder.h"
#include "../trackinfo.h"
#include "../util.h"
#include "../id3.h"
#include "../reader.h"

static mpg123_handle *player;
static int            init = 0;
static long           seek_to_sample_offset;
static int            sample_rate, channels = 0, bitrate = 0;
static TrackInfo      ti, ti_metaonly;
static Reader        *r;

static const char *get_name(void)
{
	return "mpg123 MPEG decoder v1.0";
}

static int decode_data(char *target, int max_size)
{
	int                     ret = 1;
	struct mpg123_frameinfo mi;
	size_t                  decsize = 1;

	if (r) {
		if (reader_read_bytes(r, 2048)) {
			int size = reader_get_number_of_bytes_in_buffer(r);
			if (size > 0) mpg123_feed(player, (unsigned char *)reader_get_buffer(r), size);
		}
	}
	mpg123_info(player, &mi);
	bitrate = 1000 * (mi.abr_rate ? mi.abr_rate : mi.bitrate);
	ret = mpg123_read(player, (unsigned char*)target, max_size, &decsize);
	if (ret == MPG123_DONE) decsize = 0;
	/*while (!r && item_status != STOPPED && ret != MPG123_DONE && decsize > 0) {
		r = audio_fill_buffer((char *)membuf, decsize);
		SDL_Delay(10);
	}*/
	if (seek_to_sample_offset) {
		if (mpg123_tell(player) + seek_to_sample_offset >= 0) {
			mpg123_seek(player, seek_to_sample_offset, SEEK_SET);
		}
		seek_to_sample_offset = 0;
	}
	return decsize;
}

static int mpg123_play_file(char *mpeg_file)
{
	int                      result = 1;
	struct mpg123_frameinfo  mi;

	seek_to_sample_offset = 0;

	if (!init) {
		printf("mpg123: Initializing.\n");
		if (mpg123_init() != MPG123_OK)
			printf("mpg123: Init failed.\n");
		printf("mpg123: Creating decoder.\n");	
		player = mpg123_new(NULL, NULL);
		init = 1;
	}

	if (player) {
		int  encoding = 0;
		long rate = 0;

		printf("mpg123: Opening %s...\n", mpeg_file);
		trackinfo_clear(&ti);
		id3_read_tag(mpeg_file, &ti, "MP3");
		/*strncpy(ti->file_name, mpeg_file, SIZE_FILE_NAME-1);*/

		if (!r && (mpg123_open(player, mpeg_file) != MPG123_OK || 
		    mpg123_getformat(player, &rate, &channels, &encoding) != MPG123_OK)) { /* Use normal file read stuff */
			printf("mpg123: Error opening file.\n");
			channels = 0;
		} else if (r) { /* Use stream reader */
			printf("mpg123: Opening stream...\n");
			if (mpg123_open_feed(player) == MPG123_OK) {
				int status;
				int size = reader_get_number_of_bytes_in_buffer(r); /* There are some bytes in the buffer already, that should be used first */
				do {
					if (size > 0) mpg123_feed(player, (unsigned char *)reader_get_buffer(r), size);
					status = mpg123_getformat(player, &rate, &channels, &encoding);
					if (status == MPG123_NEED_MORE) {
						reader_read_bytes(r, 2048);
						size = reader_get_number_of_bytes_in_buffer(r);
					}
				} while (status == MPG123_NEED_MORE && size > 0);
				if (status != MPG123_OK) {
					printf("mpg123: Error opening stream.\n");
					channels = 0;
				}
			} else {
				printf("mpg123: Failed opening feed.\n");
				channels = 0;
			}
		} else {
			printf("mpg123: ERROR: Could not open stream/file.\n");
			channels = 0;
		}
		if (channels > 0) {
			size_t        dummy;
			unsigned char dumbuf[1024];
			
			printf("mpg123: Found stream with %d channels and %ld bps.\n", channels, rate);
			mpg123_format_none(player);
			mpg123_format(player, rate, channels, encoding);
			mpg123_info(player, &mi);
			sample_rate = mi.rate;
			bitrate = 1000 * (mi.abr_rate ? mi.abr_rate : mi.bitrate);
			/*ti->samplerate = mi.rate;
			ti->channels   = mi.mode == MPG123_M_MONO ? 1 : 2;
			ti->bitrate    = 1000 * (mi.abr_rate ? mi.abr_rate : mi.bitrate);
			ti->length     = mpg123_length(player) / ti->samplerate;
			ti->vbr        = mi.vbr == MPG123_CBR ? 0 : 1;
			printf("mpg123: Bitstream is %ld kbps, %d channel(s), %d Hz\n", 
			       ti->bitrate / 1000, ti->channels, ti->samplerate);*/

			if (mpg123_read(player, dumbuf, 1024, &dummy) != MPG123_NEW_FORMAT) {
				printf("mpg123: No new format.\n");
			}
		} else {
			printf("mpg123: Problem with stream.\n");
			mpg123_delete(player);
			mpg123_exit();
			player = NULL;
			init = 0;
			result = 0;
		}
	}
	return result;
}

static int mpg123_seek_to(int offset_seconds)
{
	seek_to_sample_offset = offset_seconds * sample_rate;
	return seek_to_sample_offset;
}

static int close_file(void)
{
	printf("mpg123: Closing file.\n");
	mpg123_close(player);
	mpg123_delete(player);
	mpg123_exit();
	init = 0;
	return 0;
}

static int get_decoder_buffer_size(void)
{
	return 256;
}

static const char* get_file_extensions(void)
{
	return ".mp3;.mp2;.mp1";
}

static int get_current_bitrate(void)
{
	return bitrate;
}

static int get_length(void)
{
	return mpg123_length(player) / sample_rate;
}

static int get_samplerate(void)
{
	return sample_rate;
}

static int get_channels(void)
{
	return channels;
}

static int get_bitrate(void)
{
	return bitrate;
}

static int get_meta_data_int(GmuMetaDataType gmdt, int for_current_file)
{
	int result = 0;
	if (for_current_file) {
		switch (gmdt) {
			case GMU_META_IMAGE_DATA_SIZE:
				result = trackinfo_get_image_data_size(&ti);
				break;
			default:
				break;
		}
	} else {
		switch (gmdt) {
			case GMU_META_IMAGE_DATA_SIZE:
				result = trackinfo_get_image_data_size(&ti);
				break;
			default:
				break;
		}
	}
	return result;
}

static const char *get_meta_data(GmuMetaDataType gmdt, int for_current_file)
{
	char *result = NULL;

	if (for_current_file) {
		switch (gmdt) {
			case GMU_META_ARTIST:
				result = trackinfo_get_artist(&ti);
				break;
			case GMU_META_TITLE:
				result = trackinfo_get_title(&ti);
				break;
			case GMU_META_ALBUM:
				result = trackinfo_get_album(&ti);
				break;
			case GMU_META_TRACKNR:
				result = trackinfo_get_tracknr(&ti);
				break;
			case GMU_META_DATE:
				result = trackinfo_get_date(&ti);
				break;
			case GMU_META_IMAGE_DATA:
				result = trackinfo_get_image_data(&ti);
				break;
			case GMU_META_IMAGE_MIME_TYPE:
				result = trackinfo_get_image_mime_type(&ti);
				break;
			default:
				break;
		}
	} else {
		switch (gmdt) {
			case GMU_META_ARTIST:
				result = trackinfo_get_artist(&ti_metaonly);
				break;
			case GMU_META_TITLE:
				result = trackinfo_get_title(&ti_metaonly);
				break;
			case GMU_META_ALBUM:
				result = trackinfo_get_album(&ti_metaonly);
				break;
			case GMU_META_TRACKNR:
				result = trackinfo_get_tracknr(&ti_metaonly);
				break;
			case GMU_META_DATE:
				result = trackinfo_get_date(&ti_metaonly);
				break;
			case GMU_META_IMAGE_DATA:
				result = trackinfo_get_image_data(&ti_metaonly);
				break;
			case GMU_META_IMAGE_MIME_TYPE:
				result = trackinfo_get_image_mime_type(&ti_metaonly);
				break;
			default:
				break;
		}
	}
	return result;
}

static const char *get_file_type(void)
{
	return "MPEG Audio";
}

static int meta_data_load(const char *filename)
{
	return id3_read_tag(filename, &ti_metaonly, "MP3");
}

static int meta_data_close(void)
{
	trackinfo_clear(&ti_metaonly);
	return 1;
}

static GmuCharset meta_data_get_charset(void)
{
	return M_CHARSET_ISO_8859_1;
}

static int data_check_mime_type(const char *data, int size)
{
	int i, id3 = 0, sync = 0;
	if (size >= 3) {
		if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
			id3 = 1;
		} else {
			for (i = 0; i < size-1; i++) { /* Search for mpeg sync bits */
				if (((unsigned char)data[i]) == 0xff && (((unsigned char)data[i+1]) & 0xf0) == 0xf0) {
					sync = 1;
					break;
				}
			}
		}
	}
	printf("\nid3:%d sync:%d\n", id3, sync);
	return id3 || sync;
}

static void set_reader_handle(Reader *reader)
{
	r = reader;
}

static GmuDecoder gd = {
	"mpg123_decoder",
	NULL,
	NULL,
	get_name,
	NULL,
	get_file_extensions,
	NULL,
	mpg123_play_file,
	close_file,
	decode_data,
	mpg123_seek_to,
	get_current_bitrate,
	get_meta_data,
	get_meta_data_int,
	get_samplerate,
	get_channels,
	get_length,
	get_bitrate,
	get_file_type,
	get_decoder_buffer_size,
	meta_data_load,
	meta_data_close,
	meta_data_get_charset,
	data_check_mime_type,
	set_reader_handle
};

GmuDecoder *gmu_register_decoder(void)
{
	return &gd;
}
