/* 
 * Gmu Music Player
 *
 * Copyright (c) 2006-2021 Johannes Heimansberg (wej.k.vu)
 *
 * File: reader.c  Created: 110406
 *
 * Description: File/Stream reader functions
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of
 * the License. See the file COPYING in the Gmu's main directory
 * for details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#ifdef URL_WITH_CURL
#include <curl/curl.h> // Use tiny-curl lib for https stream support.
#endif
#include "util.h" /* for assign_signal_handler() */
#include "reader.h"
#include "ringbuffer.h"
#include "debug.h"
#include "core.h" /* for VERSION_NUMBER and DEFAULT_THREAD_STACK_SIZE */
#include "pthread_helper.h"

static size_t http_cache_size           = 512 * 1024;
static size_t http_cache_prebuffer_size = 256 * 1024;

size_t reader_set_cache_size_kb(size_t size, size_t prebuffer_size)
{
	size = size < HTTP_CACHE_SIZE_MIN_KB ? HTTP_CACHE_SIZE_MIN_KB : size;
	size = size > HTTP_CACHE_SIZE_MAX_KB ? HTTP_CACHE_SIZE_MAX_KB : size;
	http_cache_size = size * 1024;
	prebuffer_size *= 1024;
	if (prebuffer_size > 0 && prebuffer_size <= http_cache_size / 2 + http_cache_size / 4)
		http_cache_prebuffer_size = prebuffer_size;
	wdprintf(V_INFO, "reader", "Cache size: %d kB\n", size);
	wdprintf(V_INFO, "reader", "Cache prebuffer size: %d kB\n", prebuffer_size / 1024);
	return size;
}

size_t reader_get_cache_fill(Reader *r)
{
	return ringbuffer_get_fill(&(r->rb_http));
}

#ifdef URL_WITH_CURL
#ifndef PARSE_META_IN_READER  // ICY parsing in reader.c is a dream...
static size_t gmu_curl_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
	size_t total_bytes = size * nmemb;
	Reader *r = (Reader *)userdata;
	int write_okay = 0;

	// Handle 0-byte edge cases cleanly
	if (total_bytes == 0) return 0;

	/* 
	 * 1. THE WRITE & THROTTLE LOOP
	 * Keep looping until Gmu's buffer accepts the data chunk, 
	 * or until the main thread flags an exit request (r->eof).
	 */
	while (!write_okay && !r->eof) {
		pthread_mutex_lock(&(r->mutex));
		// Note: Gmu's native logic passes the whole buffer chunk at once
		write_okay = ringbuffer_write(&(r->rb_http), (char *)ptr, total_bytes);
		pthread_mutex_unlock(&(r->mutex));

		// Inside your original write callback loop where ringbuffer_write succeeds:
		if (!write_okay) {
			// Buffer is full. Yield CPU to let the decoder catch up.
			// Using Gmu's original 1500us (1.5ms) delay timing.
			usleep(1500);
		}
	}

	/* 
	 * 2. INTERRUPT CHECK
	 * If the loop broke because the user pressed stop (r->eof became 1),
	 * return 0 to tell cURL to immediately abort its active network stream.
	 */
	if (r->eof) {
		return 0; 
	}

	/* 
	 * 3. PRE-BUFFERING HANDSHAKE
	 * If Gmu is waiting for the initial buffer to fill, check if we hit the limit yet.
	 */
	if (!r->is_ready && ringbuffer_get_fill(&(r->rb_http)) >= http_cache_prebuffer_size) {
		r->is_ready = 1;
	}

	/* 
	 * 4. LIVE LOGGING
	 * Keep Gmu's original console buffer tracker functional.
	 */
	wdprintf(V_DEBUG, "reader", "buf fill: %d bytes\r", ringbuffer_get_fill(&(r->rb_http)));
	fflush(stdout);

	// Tell cURL we processed all incoming bytes successfully
	return total_bytes; 
}
#else // ZIPIT_Z2 CURL
static void parse_and_update_stream_title(const char *meta, Reader *r)
{
	// Meta matches the standard ICY layout: StreamTitle='Artist - Title';StreamUrl='';
	char *title_start = strstr(meta, "StreamTitle='");
	if (title_start) {
		title_start += 13; // Jump past StreamTitle='
		char *title_end = strchr(title_start, '\'');
		if (title_end) {
			size_t len = title_end - title_start;
			if (len > 0 && len < 256) {
				char clean_title[256];
				memcpy(clean_title, title_start, len);
				clean_title[len] = '\0';
				
				wdprintf(V_INFO, "reader", "New ICY Stream Title: %s\n", clean_title);
				
				// Keep Gmu's internal configuration state updated
				cfg_add_key(r->streaminfo, "title", clean_title);

				// NOTE: If Gmu requires an event signal to force the frontend
				// screen to redraw immediately, add that UI broadcast macro here.
				
				// NOTE: mpg123.c puts this in trackinfo and sets updated flag.  Need equivalent...
				//   trackinfo_set_title(&ti, stitle_utf8);
				//   trackinfo_set_updated(&ti);
				
			}
		}
	}
}

// Thread-safe wrapper that retains your original gmu throttling, mutex, and pre-buffering
static int gmu_write_audio_to_ringbuffer(Reader *r, const char *data, size_t len)
{
	if (len == 0) return 1;
	int write_okay = 0;

	while (!write_okay && !r->eof) {
		pthread_mutex_lock(&(r->mutex));
		write_okay = ringbuffer_write(&(r->rb_http), (char *)data, len);
		pthread_mutex_unlock(&(r->mutex));

		if (!write_okay) {
			usleep(1500); // Gmu's original 1.5ms delay timing
		}
	}

	if (r->eof) return 0; // Signal failure to break the parent curl process

	if (!r->is_ready && ringbuffer_get_fill(&(r->rb_http)) >= http_cache_prebuffer_size) {
		r->is_ready = 1;
	}

	return 1;
}

size_t gmu_curl_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t total_bytes = size * nmemb;
	Reader *r = (Reader *)userdata;
	size_t consumed = 0;
	char *char_ptr = (char *)ptr;

	if (total_bytes == 0) return 0;

	// Fallback: If no ICY stream metadata header was detected, route raw bytes directly
	if (r->icy_metaint <= 0) {
		if (!gmu_write_audio_to_ringbuffer(r, char_ptr, total_bytes)) return 0;
		wdprintf(V_DEBUG, "reader", "buf fill: %d bytes\r", ringbuffer_get_fill(&(r->rb_http)));
		fflush(stdout);
		return total_bytes;
	}

	// ICY Parsing State Machine Loop
	while (consumed < total_bytes && !r->eof) {
		
		// State 1: Writing pure audio data
		if (r->bytes_until_meta > 0) {
			size_t bytes_to_write = total_bytes - consumed;
			if (bytes_to_write > (size_t)r->bytes_until_meta) {
				bytes_to_write = r->bytes_until_meta;
			}

			if (!gmu_write_audio_to_ringbuffer(r, char_ptr + consumed, bytes_to_write)) {
				return 0; 
			}

			r->bytes_until_meta -= bytes_to_write;
			consumed += bytes_to_write;
		} 
		
		// State 2: Reading the Metadata Length byte
		else if (r->meta_length == -1) {
			unsigned char len_byte = (unsigned char)char_ptr[consumed];
			r->meta_length = len_byte * 16;
			r->meta_read_bytes = 0;
			consumed++;

			if (r->meta_length == 0) {
				// No metadata update at this interval, reset countdown for next audio chunk
				r->bytes_until_meta = r->icy_metaint;
				r->meta_length = -1;
			}
		} 
		
		// State 3: Extracting the actual Metadata Block
		else {
			size_t bytes_to_read = total_bytes - consumed;
			if (bytes_to_read > (size_t)(r->meta_length - r->meta_read_bytes)) {
				bytes_to_read = r->meta_length - r->meta_read_bytes;
			}

			// Accumulate metadata string safely checking buffer constraints
			if (r->meta_read_bytes + bytes_to_read < sizeof(r->meta_buffer) - 1) {
				memcpy(r->meta_buffer + r->meta_read_bytes, char_ptr + consumed, bytes_to_read);
			}

			r->meta_read_bytes += bytes_to_read;
			consumed += bytes_to_read;

			// Finished gathering the whole metadata block
			if (r->meta_read_bytes >= r->meta_length) {
				int final_len = (r->meta_length < (int)sizeof(r->meta_buffer) - 1) ? r->meta_length : (int)sizeof(r->meta_buffer) - 1;
				r->meta_buffer[final_len] = '\0'; 
				
				parse_and_update_stream_title(r->meta_buffer, r);

				// Reset counters for the next cycle
				r->bytes_until_meta = r->icy_metaint;
				r->meta_length = -1;
			}
		}
	}

	wdprintf(V_DEBUG, "reader", "buf fill: %d bytes\r", ringbuffer_get_fill(&(r->rb_http)));
	fflush(stdout);

	return r->eof ? 0 : total_bytes;
}
#endif  // ZIPIT_Z2 CURL

static size_t gmu_curl_header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	size_t total_bytes = size * nitems;
	Reader *r = (Reader *)userdata;
	
	// Libcurl passes blank lines ("\r\n") at the end of headers; skip them
	if (total_bytes <= 2 || buffer[0] == '\r' || buffer[0] == '\n') {
		return total_bytes;
	}

	// Find the colon separating key and value
	char *colon = memchr(buffer, ':', total_bytes);
	if (colon) {
		char key[256];
		char value[512];
		
		// Calculate lengths safely within bounds
		size_t key_len = colon - buffer;
		if (key_len > 255) key_len = 255;
		
		// Extract and null-terminate the key
		memcpy(key, buffer, key_len);
		key[key_len] = '\0';
		
		// Skip colon and any leading spaces for the value
		char *val_start = colon + 1;
		while (val_start < buffer + total_bytes && *val_start == ' ') {
			val_start++;
		}
		
		// Calculate value length, cutting off trailing \r or \n
		char *val_end = buffer + total_bytes;
		while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == '\n')) {
			val_end--;
		}
		
		size_t val_len = val_end - val_start;
		if (val_len > 511) val_len = 511;
		
		if (val_len > 0) {
			memcpy(value, val_start, val_len);
			value[val_len] = '\0';
			
			// Replicate Gmu's original config management behavior
			wdprintf(V_DEBUG, "reader", "key=[%s]\n", key);
			wdprintf(V_DEBUG, "reader", "value=[%s]\n", value);
			cfg_add_key(r->streaminfo, key, value);

#ifndef PARSE_META_IN_READER  // ICY parsing in reader.c is a dream...
#else // ZIPIT_Z2 CURL
			// Intercept the metadata interval.  Check case-insensitively for the ICY interval header
			if (strcasecmp(key, "icy-metaint") == 0) {
				r->icy_metaint = strtol(value, NULL, 10);
				r->bytes_until_meta = r->icy_metaint; // Initialize the byte countdown timer
				wdprintf(V_INFO, "reader", "ICY Metadata every %ld bytes.\n", r->icy_metaint);
			}
#endif			
		}
	}
	
	wdprintf(V_INFO, "reader", "total_bytes=%d.\n", total_bytes);
	return total_bytes;
}

// Libcurl progress callback (Runs multiple times per second during transfer/handshakes)
static int gmu_curl_progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	Reader *r = (Reader *)clientp;

	// If Gmu main thread flagged an abort or EOF, return non-zero to terminate cURL instantly
	if (r && r->eof) {
		wdprintf(V_DEBUG, "reader", "Progress callback intercepted r->eof. Aborting curl stream.\n");
		return 1; 
	}
	
	return 0; // 0 means continue running normally
}

static void *http_reader_thread(void *arg)
{
	Reader *r = (Reader *)arg;

	CURL *curl = curl_easy_init();
	if (!curl) {
		r->eof = 1;
		return NULL;
	}

	// Replicate Gmu's original custom User-Agent
	char user_agent_buf[64];
	snprintf(user_agent_buf, sizeof(user_agent_buf), "Gmu/%s", VERSION_NUMBER);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_buf);

	// Inject the custom ICY metadata request header
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Icy-MetaData: 1");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	// Configure connection settings
	curl_easy_setopt(curl, CURLOPT_URL, r->url); // Ensure r->url is populated in _reader_open
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	// Fast disconnect & dead stream timeouts
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);   // Max 6 seconds to connect
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // Below 1 byte/sec...
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);   // ...for 5 seconds = dead stream.

	// Progress function for instant user interrupt 
	curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, gmu_curl_progress_callback);
	curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, r);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);       // Must be 0L to activate callback!

	// Register the header callback
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, gmu_curl_header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, r);

	// Route audio data directly to your original ring buffer writer
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gmu_curl_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);

#ifndef PARSE_META_IN_READER  // ICY parsing in reader.c is a dream...
#else // ZIPIT_Z2 CURL
	// Init Icy meta parser
	r->icy_metaint = 0; // Will be populated by the header callback automatically
	r->bytes_until_meta = 0;
	r->meta_length = -1;
	r->meta_read_bytes = 0;
	memset(r->meta_buffer, 0, sizeof(r->meta_buffer));
#endif

	// Run the connection blocking loop
	CURLcode res = curl_easy_perform(curl);

	// If the stream was successfully parsed, populate r->file_size
	if (res == CURLE_OK) {
		char *val = cfg_get_key_value_ignore_case(r->streaminfo, "Content-Length");
		if (val) {
			r->file_size = atol(val);
			wdprintf(V_DEBUG, "reader", "Stream size = %d bytes.\n", r->file_size);
		}
	}

	// Clean up allocations safely for the Zipit Z2
	curl_easy_cleanup(curl);
	if (headers) {
		curl_slist_free_all(headers);
	}
	
	wdprintf(V_DEBUG, "reader", "thread done.\n");
	r->eof = 1; // Mark EOF only after cURL is completely finished
	
	return NULL;
}
#else

/* get sockaddr, IPv4 or IPv6 */
static void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET)
		return &(((struct sockaddr_in*)sa)->sin_addr);
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

static int http_url_split_alloc(const char *url, char **hostname, unsigned short *port, char **path)
{
	size_t len = url ? strlen(url) : 0;

	if (len > 7) {
		const char  *host_begin = url+7;
		char        *port_begin = strchr(host_begin, ':');
		char        *path_tmp = NULL;
		size_t       path_len = 0, host_len;

		if (port_begin) {
			*port = (short unsigned)atoi(port_begin+1);
		} else {
			port_begin = strchr(host_begin, '/');
			*port = 80; /* default http port */
		}
		if (port_begin) path_tmp = strchr(port_begin, '/');
		host_len = (port_begin ? (size_t)(port_begin - host_begin) : len-7);

		*hostname = malloc(host_len+1);
		if (*hostname) {
			strncpy(*hostname, host_begin, host_len);
			(*hostname)[host_len] = '\0';
		}
		if (!path_tmp) path_tmp = "/";
		path_len = strlen(path_tmp);
		*path = malloc(path_len+1);
		if (*path) {
			strncpy(*path, path_tmp, path_len+1);
			(*path)[path_len] = '\0';
		}
	}
	return 0;
}

static void *http_reader_thread(void *arg)
{
	Reader *r = (Reader *)arg;
	ssize_t numbytes = 1;
	int     err = 0;
	char    buf[4096];

	while (numbytes != -1 && numbytes > 0 && !r->eof) {
		do {
			numbytes = recv(r->sockfd, buf, 4096, 0);
			err = errno;
			if (numbytes > 0) { /* write to ringbuffer */
				int write_okay = 0;
				while (!write_okay && !r->eof) {
					pthread_mutex_lock(&(r->mutex));
					write_okay = ringbuffer_write(&(r->rb_http), buf, (size_t)numbytes);
					pthread_mutex_unlock(&(r->mutex));
					usleep(1500);
				}
			} else {
				usleep(300000);
				if (err == 0) {
					if (numbytes == 0) r->eof = 1;
				} else {
					wdprintf(V_DEBUG, "reader", "Network problem: %s (%d)\n", strerror(err), err);
					wdprintf(V_DEBUG, "reader", "Retrying...\n");
				}
			}
		} while (numbytes <= 0 && !r->eof && ringbuffer_get_fill(&(r->rb_http)) > 4000);
		wdprintf(V_DEBUG, "reader", "buf fill: %d bytes\r", ringbuffer_get_fill(&(r->rb_http)));
		if (!r->is_ready && ringbuffer_get_fill(&(r->rb_http)) >= http_cache_prebuffer_size)
			r->is_ready = 1;
		fflush(stdout);
	}
	wdprintf(V_DEBUG, "reader", "thread done.\n");
	r->eof = 1;
	return NULL;
}
#endif

int reader_is_ready(Reader *r)
{
	return r->is_ready;
}

/* Opens a local file or HTTP URL for reading */
static Reader *_reader_open(const char *url, int max_redirects)
{
	Reader *r = malloc(sizeof(Reader));
	if (r) {
		r->eof = 0;
		r->file = NULL;
		r->sockfd = 0;
		r->seekable = 0;
		r->buf = NULL;
		r->buf_size = 0;
		r->buf_data_size = 0;
		r->file_size = 0;
		r->is_ready = 0;
		r->stream_pos = 0;
		pthread_mutex_init(&(r->mutex), NULL);

		r->streaminfo = cfg_init();

#ifdef URL_WITH_CURL
		/* SAVE THE URL RIGHT AWAY */
		strncpy(r->url, url, sizeof(r->url) - 1);
		r->url[sizeof(r->url) - 1] = '\0'; // Ensure null-termination

		if (IS_URL(url)) { /* Got a HTTP URL */			
			/* Start reader thread... */
			// NOTE:  512K bytes is well over 10 secs for a 320K bps stream.  Longer for most radio streams.
			// if (ringbuffer_init(&(r->rb_http), 32768)) { // AI wanted 32K (much less than 512K)
			if (ringbuffer_init(&(r->rb_http), http_cache_size)) { 
				// if (pthread_create(&(r->thread), NULL, http_reader_thread, r) != 0) { // AI wanted this
				if (pthread_create_with_stack_size(&(r->thread), DEFAULT_THREAD_STACK_SIZE, http_reader_thread, r) == 0) {
					return r;
				}
				wdprintf(V_ERROR, "reader", "pthread_create failed.\n");
			} else {
				wdprintf(V_ERROR, "reader", "Out of memory.\n");
			}
			pthread_mutex_destroy(&(r->mutex));
			cfg_free(r->streaminfo);
			free(r);
			r = NULL;
			return NULL;
#else
		if (strncasecmp(url, "http://", 7) == 0) { /* Got a HTTP URL */
			char          *hostname = NULL, *path = NULL;
			unsigned short port = 80;
			/* open http stream... */
			/* 1) Split URL into host, port and path */
			http_url_split_alloc(url, &hostname, &port, &path);
			/* 2) open connection to host on port */
			assign_signal_handler(SIGPIPE, SIG_IGN);
			wdprintf(V_INFO, "reader", "Opening connection to host %s on port %d. Reading from %s.\n", hostname, port, path);
			if (hostname && path && port > 0) {
				struct addrinfo hints, *servinfo, *p;
				int    rv;
				char   s[INET6_ADDRSTRLEN];
				char   port_str[6];

				snprintf(port_str, 5, "%hu", port);
				memset(&hints, 0, sizeof hints);
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;

				if ((rv = getaddrinfo(hostname, port_str, &hints, &servinfo)) != 0) {
					wdprintf(V_ERROR, "reader",  "getaddrinfo: %s\n", gai_strerror(rv));
					free(r);
					r = NULL;
				} else {
					int err = 0;
					int flags = 0;
					/* loop through all the results and connect to the first we can */
					for (p = servinfo; p != NULL; p = p->ai_next) {
						if ((r->sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
							wdprintf(V_INFO, "reader", "socket: %s\n", strerror(errno));
							continue;
						} else { /* Set socket timeout to 2 seconds */
							struct timeval tv;
							tv.tv_sec = 2;
							tv.tv_usec = 0;
							if (setsockopt(r->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,  sizeof tv)) {
								wdprintf(V_INFO, "reader", "setsockopt: %s\n", strerror(errno));
							}
						}

						flags = fcntl(r->sockfd, F_GETFL, 0);
						fcntl(r->sockfd, F_SETFL, flags | O_NONBLOCK);
						if (connect(r->sockfd, p->ai_addr, p->ai_addrlen) == -1) {
							err = errno;
							wdprintf(V_INFO, "reader", "connect: %s\n", strerror(err));
							if (err == EINPROGRESS) {
								wdprintf(V_DEBUG, "reader", "Connection okay; continuing...\n");
								break;
							} else {
								wdprintf(V_DEBUG, "reader", "Connection unusable; closing.\n");
								close(r->sockfd);
							}
							continue;
						}
						break;
					}

					if (err == EINPROGRESS) {
						fd_set myset;
						struct timeval tv; 

						wdprintf(V_DEBUG, "reader", "Connection in progress. select()ing...\n");
						do {
							int res;

							tv.tv_sec = 5; 
							tv.tv_usec = 0; 
							FD_ZERO(&myset); 
							FD_SET(r->sockfd, &myset); 
							res = select((r->sockfd)+1, NULL, &myset, NULL, &tv); 
							if (res < 0 && errno != EINTR) {
								wdprintf(V_DEBUG, "reader", "Error while connecting: %d - %s\n", errno, strerror(errno));
								break;
							} else if (res > 0) {
								int valopt = 0;
								socklen_t lon = sizeof(int);
								if (getsockopt(r->sockfd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon) < 0) {
									wdprintf(V_DEBUG, "reader", "Error in getsockopt(): %d - %s\n", errno, strerror(errno));
									p = NULL;
									break;
								}
								if (valopt) {
									wdprintf(V_DEBUG, "reader", "Error in delayed connection(): %d - %s\n", valopt, strerror(valopt));
									p = NULL;
								}
								break;
							} else {
								wdprintf(V_DEBUG, "reader", "Timeout in select() - Cancelling!\n");
								p = NULL;
								break;
							}
						} while (1);
						flags = fcntl(r->sockfd, F_GETFL, 0);
						fcntl(r->sockfd, F_SETFL, flags & (~O_NONBLOCK));
					}

					if (p == NULL) {
						wdprintf(V_ERROR, "reader", "Failed to connect.\n");
						free(r);
						r = NULL;
					} else {
						inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
						wdprintf(V_INFO, "reader", "Connected to %s:%d.\n", s, port);
						/* Send HTTP GET request */
						{
							char http_request[512];
							snprintf(http_request, 511,
							         "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: Gmu/%s\r\nIcy-MetaData: 1\r\n\r\n",
							         path, hostname, VERSION_NUMBER);
							wdprintf(V_DEBUG, "reader", "Sending request: %s\n", http_request);
							send(r->sockfd, http_request, strlen(http_request), 0);
						}

						/* Start reader thread... */
						if (ringbuffer_init(&(r->rb_http), http_cache_size)) {
							pthread_create_with_stack_size(&(r->thread), DEFAULT_THREAD_STACK_SIZE, http_reader_thread, r);
						} else {
							wdprintf(V_ERROR, "reader", "Out of memory.\n");
						}

						/* Skip http response header */
						{
							int    header_end_found = 0;
							size_t cnt = 0, i = 0;
							char   ch, key[256], value[512];

							key[0] = '\0'; value[0] = '\0';
							/* Search for http header end sequence "\r\n\r\n" (or "\n\n"); I assume http headers to be no longer than 32 kB. */
							while (!header_end_found && !reader_is_eof(r) && cnt < 32768) {
								/* extract key */
								ch = 0;
								while (ch != '\r' && ch != '\n' && ch != ':' && !reader_is_eof(r)) {
									ch = reader_read_byte(r);
									if (i < 255 && ch != ':' && ch != '=' && ch != '\r' && ch != '\n') key[i++] = ch;
									cnt++;
								}
								key[i] = '\0';
								wdprintf(V_DEBUG, "reader", "key=[%s]\n", key);

								/* extract value */
								i = 0;
								while ((ch = reader_read_byte(r)) == ' ' && !reader_is_eof(r)) cnt++; /* skip spaces after ":" */
								if (ch != '\r' && ch != '\n') value[i++] = ch;
								while (ch != '\r' && ch != '\n' && !reader_is_eof(r)) {
									ch = reader_read_byte(r);
									if (i < 511 && ch != '\r' && ch != '\n') value[i++] = ch;
									cnt++;
								}
								value[i] = '\0';
								wdprintf(V_DEBUG, "reader", "value=[%s]\n", value);
								if (key[0] && value[0])
									cfg_add_key(r->streaminfo, key, value);

								i = 0;
								if (ch == '\n' || (ch = reader_read_byte(r)) == '\n') {
									ch = reader_read_byte(r);
									if ((ch == '\r' && (ch = reader_read_byte(r)) == '\n') || ch == '\n')
										header_end_found = 1;
									else
										key[i++] = ch;
									cnt+=3;
								} else {
									key[i++] = ch;
								}
							}
							wdprintf(V_DEBUG, "reader", "HTTP header skipped: %s (%d bytes)\n", header_end_found ? "yes" : "no", cnt);
							/* Try to figure out stream length */
							if (header_end_found) {
								char *val = cfg_get_key_value_ignore_case(r->streaminfo, "Content-Length");
								if (val) {
									r->file_size = (size_t)atol(val);
									wdprintf(V_DEBUG, "reader", "Stream size = %d bytes.\n", r->file_size);
								}
							}
						}
					}
					freeaddrinfo(servinfo);
				}
			}
			if (hostname) free(hostname);
			if (path)     free(path);
			/* Check for 302 redirect (Location) */
			if (r) {
				char *v = cfg_get_key_value_ignore_case(r->streaminfo, "Location");
				if (v) {
					size_t len = strlen(v);
					char  *vc = NULL;
					
					if (len > 0 && (vc = malloc(len+1))) {
						strncpy(vc, v, len+1);
						vc[len] = '\0';
					}
					wdprintf(V_INFO, "reader", "302 Redirect found: %s\n", vc ? vc : "unknown");
					reader_close(r);
					r = NULL;
					if (max_redirects > 0 && vc) {
						r = _reader_open(vc, max_redirects-1);
					} else {
						wdprintf(V_WARNING, "reader", "Too many HTTP redirects.\n");
					}
					if (vc) free(vc);
				}
			}
#endif		
		} else { /* Treat everything else as a local file (for now) */
			wdprintf(V_INFO, "reader", "Opening file %s.\n", url);
			r->file = fopen(url, "r");
			if (r->file) {
				struct stat st;
				r->seekable = 1;
				if (stat(url, &st) == 0) {
					r->file_size = (size_t)st.st_size;
					wdprintf(V_DEBUG, "reader", "File size = %d bytes.\n", r->file_size);
				}
				r->is_ready = 1;
			} else {
				wdprintf(V_ERROR, "reader", "Unable to open file '%s'.\n", url);
				free(r);
				r = NULL;
			}
		}
	}
	return r;
}

Reader *reader_open(const char *url)
{
	return _reader_open(url, 3);
}

int reader_close(Reader *r)
{
	if (r) {
		if (r->file) { /* local file */
			fclose(r->file);
		} else if (r->sockfd > 0) { /* http stream */
			/* close http stream */
			close(r->sockfd);
			r->eof = 1;
			wdprintf(V_DEBUG, "reader", "Waiting for reader thread to finish.\n");
			pthread_join(r->thread, NULL);
			wdprintf(V_DEBUG, "reader", "Reader thread joined.\n");
			ringbuffer_free(&(r->rb_http));
		}
		pthread_mutex_destroy(&(r->mutex));
		if (r->buf) free(r->buf);
		cfg_free(r->streaminfo);
		free(r);
		r = NULL;
	}
	return 0;
}

int reader_is_eof(Reader *r)
{
	return r->file ? r->eof : ringbuffer_get_fill(&(r->rb_http)) > 0 ? 0 : r->eof;
}

char reader_read_byte(Reader *r)
{
	int ch = 0;
	if (r->file) {
		ch = fgetc(r->file);
		if (ch == EOF) r->eof = 1;
	} else {
		int read_okay = 0;
		while (!read_okay && !reader_is_eof(r)) {
			char buf[1];
			pthread_mutex_lock(&(r->mutex));
			read_okay = ringbuffer_read(&(r->rb_http), buf, 1);
			pthread_mutex_unlock(&(r->mutex));
			if (!read_okay && r->eof) break;
			if (!read_okay) usleep(150);
			ch = buf[0];
		}
		if (read_okay) r->stream_pos++;
	}
	return (char)ch;
}

size_t reader_get_number_of_bytes_in_buffer(Reader *r)
{
	return r->buf_data_size;
}

int reader_read_bytes(Reader *r, size_t size)
{
	int read_okay = 0;

	if (size > 0) {
		if (size > r->buf_size) r->buf = realloc(r->buf, size+1);
		if (r->buf) r->buf_size = size;
		if (r->file) {
			if (r->buf) {
				long pos = ftell(r->file);
				memset(r->buf, 0, r->buf_size);
				if (fread(r->buf, size, 1, r->file) < 1) {
					do { /* Not the most elegant solution.. ;) */
						size--;
						if (fseek(r->file, pos, SEEK_SET) != 0)
							wdprintf(V_ERROR, "reader", "Unable to seek to pos %d:(\n", pos);
					} while (fread(r->buf, size, 1, r->file) < 1 && size > 0);
					r->eof = feof(r->file);
					if (size > 0)
						read_okay = 1;
					else
						r->eof = 1;
					r->buf_data_size = size;
				} else {
					r->buf[size] = '\0';
					read_okay = 1;
					r->buf_data_size = size;
					r->stream_pos += size;
				}
			}
		} else {
			while (!read_okay) {
				pthread_mutex_lock(&(r->mutex));
				read_okay = ringbuffer_read(&(r->rb_http), r->buf, size);
				pthread_mutex_unlock(&(r->mutex));
				if (read_okay) r->buf_data_size = size; else r->buf_data_size = 0;
				if (!read_okay && r->eof) break;
				r->buf[size] = '\0';
				if (!read_okay) usleep(150);
			}
		}
	}
	return read_okay;
}

char *reader_get_buffer(Reader *r)
{
	return r->buf;
}

size_t reader_get_file_size(Reader *r)
{
	return r->file_size;
}

size_t reader_get_stream_position(Reader *r)
{
	return r->stream_pos;
}

/* Resets the stream to the beginning (if possible), returns 1 on success, 0 otherwise */
int reader_reset_stream(Reader *r)
{
	int res = 0;
	if (r->file) { /* Only possible for local files */
		rewind(r->file);
		r->stream_pos = 0;
		res = 1;
	}
	return res;
}

int reader_is_seekable(Reader *r)
{
	return r->seekable;
}

int reader_seek_whence(Reader *r, ssize_t byte_offset, int whence)
{
	int res = 0;
	if (r->file) {
		if (fseek(r->file, byte_offset, whence) == 0) {
			long int ftres = ftell(r->file);
			r->buf_data_size = 0;
			if (ftres > 0) {
				r->stream_pos = (size_t)ftres;
				res = 1;
			} else {
				wdprintf(V_INFO, "reader", "Seeking failed during ftell(). :(\n");
			}
		} else {
			wdprintf(V_INFO, "reader", "Seeking failed. :(\n");
		}
	} else {
		/* Seeking not possible in HTTP streams */
		res = 0;
	}
	return res;
}

int reader_seek(Reader *r, ssize_t byte_offset)
{
	return reader_seek_whence(r, byte_offset, SEEK_SET);
}

void reader_clear_buffer(Reader *r)
{
	r->buf_data_size = 0;
}
