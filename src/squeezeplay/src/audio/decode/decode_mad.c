/*
** Copyright 2007-2008 Logitech. All Rights Reserved.
**
** This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.
*/

#define RUNTIME_DEBUG 1

#include "common.h"

#include "audio/streambuf.h"
#include "audio/decode/decode.h"
#include "audio/decode/decode_priv.h"

#include <mad.h>


/* The input buffer size is at the theoretical maximum frame
 * size - MPEG 2.5 Layer II 8KHz @ 160kbps with padding slot
 */
#define INPUT_BUFFER_SIZE 2890

#define OUTPUT_BUFFER_FRAMES 2048
#define OUTPUT_BUFFER_BYTES (OUTPUT_BUFFER_FRAMES * sizeof(sample_t))

#define ID3_TAG_FLAG_FOOTERPRESENT 0x10


struct decode_mad {
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	u8_t *input_buffer;
	sample_t *output_buffer;
	u8_t *guard_pointer;

	u32_t frames;
	u32_t encoder_delay;
	u32_t encoder_padding;

	enum {
		MAD_STATE_OK = 0,
		MAD_STATE_PCM_READY,
		MAD_STATE_END_OF_FILE,
		MAD_STATE_ERROR,
	} state;

	u32_t sample_rate;
};


#define XING_MAGIC      ( ('X' << 24) | ('i' << 16) | ('n' << 8) | 'g' )
#define INFO_MAGIC      ( ('I' << 24) | ('n' << 16) | ('f' << 8) | 'o' )
#define LAME_MAGIC      ( ('L' << 24) | ('A' << 16) | ('M' << 8) | 'E' )

#define XING_FRAMES 	0x01
#define XING_BYTES  	0x02
#define XING_TOC    	0x04
#define XING_SCALE  	0x08

/* Not much documentation exists about the MAD decoder delay, but
   we apparently need to skip the first 529 samples
   http://www.hydrogenaudio.org/forums/lofiversion/index.php/t20083.html

   See also
   http://lame.sourceforge.net/tech-FAQ.txt
*/
#define MAD_DECODER_DELAY 529


static void xing_parse(struct decode_mad *self) {
	struct mad_bitptr ptr = self->stream.anc_ptr;
	unsigned int bitlen = self->stream.anc_bitlen;
	u32_t magic, flags;

	if (bitlen < 64) {
		DEBUG_TRACE("no xing header");
		return;
	}

	magic = mad_bit_read(&ptr, 32);
	DEBUG_TRACE("xing magic %x", magic);
	if (magic != XING_MAGIC && magic != INFO_MAGIC) {
		return;
	}

	flags = mad_bit_read(&ptr, 32);
	bitlen -= 64;

	// skip traditional xing vbr tag data
	if (flags & XING_FRAMES) {
		DEBUG_TRACE("skipping xing frames");
		if (bitlen < 32) {
			return;
		}
		mad_bit_skip(&ptr, 32);
		bitlen -= 32;
	}
	if (flags & XING_BYTES) {
		DEBUG_TRACE("skipping xing bytes");
		if (bitlen < 32) {
			return;
		}
		mad_bit_skip(&ptr, 32);
		bitlen -= 32;
	}
	if (flags & XING_TOC) {
		DEBUG_TRACE("skipping xing toc");
		if (bitlen < 800) {
			return;
		}
		mad_bit_skip(&ptr, 800);
		bitlen -= 800;
	}
	if (flags & XING_SCALE) {
		DEBUG_TRACE("skipping xing scale");
		if (bitlen < 32) {
			return;
		}
		mad_bit_skip(&ptr, 32);
		bitlen -= 32;
	}

	if (bitlen < 72) {
		DEBUG_TRACE("no lame header");
		return;
	}

	magic = mad_bit_read(&ptr, 32);
	mad_bit_skip(&ptr, 40);
	bitlen -= 72;

	DEBUG_TRACE("lame magic %x bitlen %d", magic, bitlen);
	if (magic != LAME_MAGIC) {
		return;
	}

	if (bitlen < 120) {
		return;
	}

	mad_bit_skip(&ptr, 96);

	self->encoder_delay += mad_bit_read(&ptr, 12);
	self->encoder_padding = mad_bit_read(&ptr, 12);

	DEBUG_TRACE("encoder delay %d", self->encoder_delay - MAD_DECODER_DELAY);
	DEBUG_TRACE("encoder padding %d", self->encoder_padding);

	/* Remove MAD decoder delay of 529 samples from the end too */
	if (self->encoder_padding > MAD_DECODER_DELAY) {
		self->encoder_padding -= MAD_DECODER_DELAY;
	}
	else {
		self->encoder_padding = 0;
	}
}


static u32_t tagtype(const unsigned char *data, u32_t length) {
        if (length >= 3 && data[0] == 'T' && data[1] == 'A' && data[2] == 'G') {
                DEBUG_TRACE("ID3v1 tag detected");
                return 128;
        }
        
        if (length >= 10 &&
                (data[0] == 'I' && data[1] == 'D' && data[2] == '3') &&
                data[3] < 0xff && data[4] < 0xff &&
                data[6] < 0x80 && data[7] < 0x80 && data[8] < 0x80 && data[9] < 0x80)
        {
                u32_t size;
				
				DEBUG_TRACE("ID3v2 tag detected");
                
                size = 10 + (data[6]<<21) + (data[7]<<14) + (data[8]<<7) + data[9];
                if (data[5] & ID3_TAG_FLAG_FOOTERPRESENT) {
                        size += 10;
                }
                for (; size < length && !data[size]; ++size);  /* Consume padding */
                return size;
        }
        
        return 0;
}


static bool_t consume_id3_tags(struct decode_mad *self) {
        bool_t rc = FALSE;      
        u32_t tagsize;
        u32_t remaining = self->stream.bufend - self->stream.next_frame;
        
        if ( (tagsize = tagtype(self->stream.this_frame, remaining)) ) {
                DEBUG_TRACE("ID3 tag detected, skipping %d bytes before next frame", tagsize);
                mad_stream_skip(&self->stream, tagsize);
                rc = TRUE;
        }
        
        /* We know that a valid frame hasn't been found yet
          * so help libmad out and go back into frame seek mode.
          * This is true whether an ID3 tag was found or not.
          */
        mad_stream_sync(&self->stream);
        
        return rc;
}


static void decode_mad_frame(struct decode_mad *self) {
	size_t read_max, read_num, remaining;
	u8_t *read_start;
	bool_t streaming;

	do {
		/* The input stream must be filled if it's the first
		 * execution of the loop or it becomes empty
		 */
		if (self->stream.buffer == NULL ||
		    self->stream.error == MAD_ERROR_BUFLEN) {

			/* If there's data left from the last time,
			 * copy it to the beginning of the input buffer
			 */
			if (self->stream.next_frame) {
				remaining = self->stream.bufend - self->stream.next_frame;
				memcpy(self->input_buffer, self->stream.next_frame, remaining);
			}
			/* Otherwise fill the input buffer */
			else {
				remaining = 0;
			}

			read_start = self->input_buffer + remaining;
			read_max = INPUT_BUFFER_SIZE - remaining;


			read_num = streambuf_read(read_start, 0, read_max, &streaming);

			if (!read_num) {
				current_decoder_state |= DECODE_STATE_UNDERRUN;
				if (streaming) {
					return;
				}

				/* If we're at the end of the input file, write
				 * out the buffer guard.
				 */
				self->guard_pointer = read_start;
				memset(self->guard_pointer, 0, MAD_BUFFER_GUARD);
				read_num += MAD_BUFFER_GUARD;
			}
			else {
				current_decoder_state &= ~DECODE_STATE_UNDERRUN;
			}

			/* Send the new content to libmad's stream decoder
			 */
			mad_stream_buffer(&self->stream,
					  self->input_buffer,
					  read_num + remaining);

			self->stream.error = MAD_ERROR_NONE;
		}

		if (mad_frame_decode(&self->frame, &self->stream)) {
			if (MAD_RECOVERABLE(self->stream.error)) {
				if (consume_id3_tags(self)) {
					continue;
				}

				if (self->stream.error != MAD_ERROR_LOSTSYNC ||
				    self->stream.this_frame != self->guard_pointer) {
					continue;
				}
			}
			else {
				/* Need more data, try again */
				if (self->stream.error == MAD_ERROR_BUFLEN) {
					continue;
				}

				// XXXX unrecoverable error
				DEBUG_ERROR("Unrecoverable frame error %d", self->stream.error);
				self->state = MAD_STATE_ERROR;
				current_decoder_state |= DECODE_STATE_ERROR;
				return;
			}
		}

		mad_synth_frame(&self->synth, &self->frame);

		/* pcm is now ready  */
		self->state = MAD_STATE_PCM_READY;
	} while (0);
}		


static inline sample_t mad_fixed_to_32bit(mad_fixed_t fixed)
{
	fixed += (1L << (MAD_F_FRACBITS - 24));

	/* Clipping */
	if(fixed >= MAD_F_ONE)
		fixed = MAD_F_ONE - 1;
	if(fixed <= -MAD_F_ONE)
		fixed = -MAD_F_ONE;

	/* Conversion */
	fixed = fixed >> (MAD_F_FRACBITS - 23);
	return ((sample_t)fixed) << 8;
}


static void decode_mad_output(struct decode_mad *self) {
	struct mad_pcm *pcm;
	sample_t *buf, *buf_end;
	mad_fixed_t *left, *right;
	int i, offset = 0;

	pcm = &self->synth.pcm;

	if (!decode_output_can_write(SAMPLES_TO_BYTES(pcm->length), self->sample_rate)) {
		self->state = MAD_STATE_PCM_READY;
		return;
	}

	/* parse xing header */
	if (self->frames++ == 0) {
		/* Bug 5720, files with CRC will have the ptr in the
		 * wrong place
		 */
		if (self->frame.header.flags & MAD_FLAG_PROTECTION) {
			if (self->stream.anc_ptr.byte > self->stream.buffer + 2) {
				self->stream.anc_ptr.byte = self->stream.anc_ptr.byte - 2;
			}
		}

		xing_parse(self);
	}
	else {
		/* Bug 9046, don't allow sample rate to change mid stream */
		if (self->frames > 2 && self->sample_rate != self->frame.header.samplerate) {
			DEBUG_TRACE("Sample rate changed from %d to %d, discarding PCM", self->sample_rate, self->frame.header.samplerate);
			return;
		}
		self->sample_rate = self->frame.header.samplerate;

		buf = self->output_buffer;
		buf_end = self->output_buffer + OUTPUT_BUFFER_FRAMES;

		left = pcm->samples[0];

		if (pcm->channels == 2) {
			/* stereo */
			right = pcm->samples[1];
		}
		else {
			/* mono */
			right = pcm->samples[0];
		}

		/* skip samples for the encoder delay */
		if (self->encoder_delay) {
			offset = self->encoder_delay;
			if (offset > pcm->length) {
				offset = pcm->length;
			}
			self->encoder_delay -= offset;

			DEBUG_TRACE("Skip encoder_delay=%d pcm->length=%d offset=%d", self->encoder_delay, pcm->length, offset);
		}

		for (i=offset; i<pcm->length; i++) {
			*buf++ = mad_fixed_to_32bit(*left++);
			*buf++ = mad_fixed_to_32bit(*right++);

			if (buf == buf_end) {
				decode_output_samples(self->output_buffer,
						      (buf - self->output_buffer) / 2,
						      self->sample_rate,
						      FALSE);

				buf = self->output_buffer;
			}
		}

		decode_output_samples(self->output_buffer,
				      (buf - self->output_buffer) / 2,
				      self->sample_rate,
				      FALSE);
	}

	/* If we've come to the guard pointer, we're done */
	if (self->stream.this_frame == self->guard_pointer) {
		DEBUG_TRACE("Reached end of stream");

		self->state = MAD_STATE_END_OF_FILE;

		if (self->encoder_padding) {
			DEBUG_TRACE("Remove encoder padding=%d", self->encoder_padding);
			decode_output_remove_padding(self->encoder_padding, self->sample_rate);
		}
	}
	else {
		self->state = MAD_STATE_OK;
	}
}


static bool_t decode_mad_callback(void *data) {
	struct decode_mad *self = (struct decode_mad *) data;


	/* End of file? */
	if (self->state == MAD_STATE_END_OF_FILE ||
	    self->state == MAD_STATE_ERROR) {
		return FALSE;
	}

	if (!decode_output_can_write(OUTPUT_BUFFER_BYTES, self->sample_rate)) {
		return 0;
	}

	if (self->state == MAD_STATE_OK) {
		decode_mad_frame(self);
	}

	if (self->state == MAD_STATE_PCM_READY) {
		decode_mad_output(self);
	}

	return TRUE;
}


static u32_t decode_mad_period(void *data) {
	struct decode_mad *self = (struct decode_mad *) data;

	if (self->sample_rate <= 48000) {
		return 8;
	}
	else {
		return 4;
	}
}


static void *decode_mad_start(u8_t *params, u32_t num_params) {
	struct decode_mad *self;

	DEBUG_TRACE("decode_mad_start()");

	self = malloc(sizeof(struct decode_mad));
	memset(self, 0, sizeof(struct decode_mad));

	self->input_buffer = malloc(INPUT_BUFFER_SIZE + MAD_BUFFER_GUARD);
	self->output_buffer = malloc(OUTPUT_BUFFER_BYTES);
	self->guard_pointer = NULL;

	mad_stream_init(&self->stream);
	mad_frame_init(&self->frame);
	mad_synth_init(&self->synth);

	/* Assume we aren't changing sample rates until proven wrong */
	self->sample_rate = decode_output_samplerate();
	self->encoder_delay = MAD_DECODER_DELAY;

	/* Don't check for CRC errors (bug #2527) */
	// XXXX this needs a patch to libmad
	//decoder->stream.options = MAD_OPTION_ZEROCRC;
	
	return self;
}


static void decode_mad_stop(void *data) {
	struct decode_mad *self = (struct decode_mad *) data;

	DEBUG_TRACE("decode_mad_stop()");

	mad_stream_finish(&self->stream);
	mad_frame_finish(&self->frame);
	mad_synth_finish(&self->synth);
	
	free(self->input_buffer);
	free(self->output_buffer);
	free(self);
}


struct decode_module decode_mad = {
	'm',
	"mp3",
	decode_mad_start,
	decode_mad_stop,
	decode_mad_period,
	decode_mad_callback,
};
