/*
** Copyright 2007-2008 Logitech. All Rights Reserved.
**
** This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.
*/


#ifndef AUDIO_DECODE_PRIV
#define AUDIO_DECODE_PRIV

#include "audio/fifo.h"


/* Audio sample, 32-bits.
 */
typedef s32_t sample_t;

#define SAMPLE_MAX (sample_t)0x7FFFFFFF
#define SAMPLE_MIN (sample_t)0x80000000


#define DECODER_MAX_PARAMS 32


/* Decode interface */
struct decode_module {
	u32_t id;
	void *(*start)(u8_t *params, u32_t num_params);
	void (*stop)(void *data);
	u32_t (*period)(void *data);
	bool_t (*callback)(void *data);
};


/* Built-in decoders */
extern struct decode_module decode_tones;
extern struct decode_module decode_pcm;
extern struct decode_module decode_flac;
extern struct decode_module decode_mad;
extern struct decode_module decode_vorbis;
#ifdef _WIN32
extern struct decode_module decode_wma_win;
#endif


/* Private decoder api */
extern u32_t current_decoder_state;
extern u32_t current_audio_state;

extern bool_t decode_output_can_write(u32_t buffer_size, u32_t sample_rate);

extern void decode_output_samples(sample_t *buffer, u32_t samples, int sample_rate,
				   bool_t copyright_asserted);

extern void decode_output_remove_padding(u32_t nsamples, u32_t sample_rate);

extern int decode_output_samplerate(void);

extern void decode_output_song_ended(void);

extern void decode_output_set_transition(u32_t type, u32_t period);

extern void decode_output_set_track_gain(u32_t replay_gain);

extern void decode_set_track_polarity_inversion(u8_t inversion);


/* Stream metadata */

struct decode_metadata {
	enum {
		SHOUTCAST = 0,
		WMA_GUID = 1,
	} type;

	u32_t timestamp;
	size_t fullness;

	size_t len;
	u8_t data;
};

extern void decode_queue_metadata(struct decode_metadata *metadata);


/* Audio output api */
struct decode_audio {
	int (*init)(void);
	void (*start)(void);
	void (*stop)(void);
};

extern struct decode_audio decode_portaudio;
extern struct decode_audio *decode_audio;


/* Decode output api */
extern void decode_output_begin(void);
extern void decode_output_end(void);
extern void decode_output_flush(void);
extern bool_t decode_check_start_point(void);


/* Internal state */

#define DECODE_FIFO_SIZE (10 * 2 * 44100 * sizeof(sample_t)) 
#define SAMPLES_TO_BYTES(n)  (2 * (n) * sizeof(sample_t))
#define BYTES_TO_SAMPLES(n)  (n / (2 * sizeof(sample_t)))

/* State variables for the current track */
extern u32_t decode_num_tracks_started;
extern u32_t decode_elapsed_samples;
extern bool_t decode_first_buffer;
extern u32_t current_sample_rate;

/* The fifo used to store decoded samples */
extern u8_t decode_fifo_buf[DECODE_FIFO_SIZE];
extern struct fifo decode_fifo;

/* Decode message queue */
extern struct mqueue decode_mqueue;


#endif // AUDIO_DECODE_PRIV