/*
 * linuxdvb output/writer handling.
 *
 * konfetti 2010 based on linuxdvb.c code from libeplayer2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* ***************************** */
/* Includes                      */
/* ***************************** */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/stm_ioctls.h>
#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */
#define PCM_DEBUG

#ifdef PCM_DEBUG

static short debug_level = 0;

#define pcm_printf(level, fmt, x...) do { \
if (debug_level >= level) printf("[%s:%s] " fmt, __FILE__, __FUNCTION__, ## x); } while (0)
#else
#define pcm_printf(level, fmt, x...)
#endif

#ifndef PCM_SILENT
#define pcm_err(fmt, x...) do { printf("[%s:%s] " fmt, __FILE__, __FUNCTION__, ## x); } while (0)
#else
#define pcm_err(fmt, x...)
#endif

/* ***************************** */
/* Types                         */
/* ***************************** */

/* ***************************** */
/* Varaibles                     */
/* ***************************** */

static int initialHeader = 1;

static unsigned int SubFrameLen = 0;
static unsigned int SubFramesPerPES = 0;

// reference: search for TypeLpcmDVDAudio in player/frame_parser/frame_parser_audio_lpcm.cpp
static const unsigned char clpcm_prv[14] = { 0xA0,	//sub_stream_id
    0, 0,			//resvd and UPC_EAN_ISRC stuff, unused
    0x0A,			//private header length
    0, 9,			//first_access_unit_pointer
    0x00,			//emph,rsvd,stereo,downmix
    0x0F,			//quantisation word length 1,2
    0x0F,			//audio sampling freqency 1,2
    0,				//resvd, multi channel type
    0,				//bit shift on channel GR2, assignment
    0x80,			//dynamic range control
    0, 0			//resvd for copyright management
};

static unsigned char lpcm_prv[14];
static unsigned char breakBuffer[8192];
static unsigned int breakBufferFillSize = 0;

/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/* MISC Functions                */
/* ***************************** */

static int prepareClipPlay(int uNoOfChannels, int uSampleRate,
			   int uBitsPerSample, int bLittleEndian
			   __attribute__ ((unused)))
{
    printf("rate: %d ch: %d bits: %d (%d bps)\n",
	   uSampleRate /*Format->dwSamplesPerSec */ ,
	   uNoOfChannels /*Format->wChannels */ ,
	   uBitsPerSample /*Format->wBitsPerSample */ ,
	   (uBitsPerSample /*Format->wBitsPerSample */  / 8)
	);

    SubFrameLen = 0;
    SubFramesPerPES = 0;
    breakBufferFillSize = 0;

    memcpy(lpcm_prv, clpcm_prv, sizeof(lpcm_prv));

    // figure out size of subframe and set up sample rate
    switch (uSampleRate) {
    case 48000:
	SubFrameLen = 40;
	break;
    case 96000:
	lpcm_prv[8] |= 0x10;
	SubFrameLen = 80;
	break;
    case 192000:
	lpcm_prv[8] |= 0x20;
	SubFrameLen = 160;
	break;
    case 44100:
	lpcm_prv[8] |= 0x80;
	SubFrameLen = 40;
	break;
    case 88200:
	lpcm_prv[8] |= 0x90;
	SubFrameLen = 80;
	break;
    case 176400:
	lpcm_prv[8] |= 0xA0;
	SubFrameLen = 160;
	break;
    default:
	break;
    }

    SubFrameLen *= uNoOfChannels;
    SubFrameLen *= (uBitsPerSample / 8);

    //rewrite PES size to have as many complete subframes per PES as we can
    // FIXME: PES header size was hardcoded to 18 in previous code. Actual size returned by InsertPesHeader is 14.
    SubFramesPerPES = ((2048 - 18) - sizeof(lpcm_prv)) / SubFrameLen;
    SubFrameLen *= SubFramesPerPES;

    //set number of channels
    lpcm_prv[10] = uNoOfChannels - 1;

    switch (uBitsPerSample) {
    case 24:
	lpcm_prv[7] |= 0x20;
    case 16:
	break;
    default:
	printf("inappropriate bits per sample (%d) - must be 16 or 24\n", uBitsPerSample);
	return 1;
    }

    return 0;
}

static int reset()
{
    initialHeader = 1;
    return 0;
}

static int writeData(WriterAVCallData_t *call)
{
    unsigned char PesHeader[PES_MAX_HEADER_SIZE];

    pcm_printf(10, "\n");

    if (!call) {
	pcm_err("call data is NULL...\n");
	return 0;
    }

    pcm_printf(10, "AudioPts %lld\n", call->Pts);

    if (!call->data || (call->len <= 0)) {
	pcm_err("parsing NULL Data. ignoring...\n");
	return 0;
    }

    if (call->fd < 0) {
	pcm_err("file pointer < 0. ignoring ...\n");
	return 0;
    }

    if (initialHeader) {
	initialHeader = 0;
	prepareClipPlay(call->uNoOfChannels,
			call->uSampleRate,
			call->uBitsPerSample,
			call->bLittleEndian);
    }

    unsigned char *buffer = call->data;
    unsigned int size = call->len;

    unsigned int n;
    unsigned char *injectBuffer = (unsigned char *) malloc(SubFrameLen);
    unsigned int pos;

    for (pos = 0; pos < size;) {
	//printf("PCM %s - Position=%d\n", __FUNCTION__, pos);
	if ((size - pos) < SubFrameLen) {
	    breakBufferFillSize = size - pos;
	    memcpy(breakBuffer, &buffer[pos],
		   sizeof(unsigned char) * breakBufferFillSize);
	    //printf("PCM %s - Unplayed=%d\n", __FUNCTION__, breakBufferFillSize);
	    break;
	}
	//get first PES's worth
	if (breakBufferFillSize > 0) {
	    memcpy(injectBuffer, breakBuffer, sizeof(unsigned char) * breakBufferFillSize);
	    memcpy(&injectBuffer[breakBufferFillSize], &buffer[pos], sizeof(unsigned char) * (SubFrameLen - breakBufferFillSize));
	    pos += (SubFrameLen - breakBufferFillSize);
	    breakBufferFillSize = 0;
	} else {
	    memcpy(injectBuffer, &buffer[pos], sizeof(unsigned char) * SubFrameLen);
	    pos += SubFrameLen;
	}

	struct iovec iov[3];
	iov[0].iov_base = PesHeader;
	iov[1].iov_base = lpcm_prv;
	iov[1].iov_len = sizeof(lpcm_prv);

	iov[2].iov_base = injectBuffer;
	iov[2].iov_len = SubFrameLen;

	//write the PCM data
	if (call->uBitsPerSample == 16) {
	    for (n = 0; n < SubFrameLen; n += 2) {
		unsigned char tmp;
		tmp = injectBuffer[n];
		injectBuffer[n] = injectBuffer[n + 1];
		injectBuffer[n + 1] = tmp;
	    }
	} else {
	    //      0   1   2   3   4   5   6   7   8   9  10  11
	    //    A1c A1b A1a-B1c B1b B1a-A2c A2b A2a-B2c B2b B2a
	    // to A1a A1b B1a B1b.A2a A2b B2a B2b-A1c B1c A2c B2c
	    for (n = 0; n < SubFrameLen; n += 12) {
		unsigned char t, *p = &injectBuffer[n];
		t = p[0];
		p[0] = p[2];
		p[2] = p[5];
		p[5] = p[7];
		p[7] = p[11];
		p[11] = p[9];
		p[9] = p[3];
		p[3] = p[4];
		p[4] = p[8];
		p[8] = t;
	    }
	}

	//increment err... subframe count?
	lpcm_prv[1] = ((lpcm_prv[1] + SubFramesPerPES) & 0x1F);

	iov[0].iov_len = InsertPesHeader(PesHeader, iov[1].iov_len + iov[2].iov_len, PCM_PES_START_CODE, call->Pts, 0);
	int len = writev(call->fd, iov, 3);
	if (len < 0)
	    break;
    }
    free(injectBuffer);

    return size;
}


SwrContext *swr = NULL;
AVFrame *decoded_frame = NULL;
int out_sample_rate = 44100;
int out_channels = 2;
uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
int restart_audio_resampling = 0;

static int resetIpcm()
{
    if (swr)
	swr_free(&swr);
    if (decoded_frame)
	av_frame_free(&decoded_frame);

    return 0;
}

int64_t calcPts(AVFormatContext *, AVStream *, int64_t);

static int writeDataIpcm(WriterAVCallData_t *call)
{
	AVCodecContext *c = call->stream->codec;
	AVPacket *packet = call->packet;
	uint8_t *packet_data = packet->data;
	unsigned int packet_size = packet->size;

	if (call->restart_audio_resampling)
		call->restart_audio_resampling = 1;	

	if (restart_audio_resampling) {
		restart_audio_resampling = 0;
		if (swr) {
			swr_free(&swr);
			swr = NULL;
		}
		if (decoded_frame) {
			av_frame_free(&decoded_frame);
			decoded_frame = NULL;
		}
		call->context->output->Command(call->context, OUTPUT_CLEAR, NULL);
		call->context->output->Command(call->context, OUTPUT_PLAY, NULL);

		AVCodec *codec = avcodec_find_decoder(c->codec_id);

		if (!codec || avcodec_open2(c, codec, NULL))
			fprintf(stderr, "%s %d: avcodec_open2 failed\n", __func__, __LINE__);
		}

		while (packet_size > 0) {
			int got_frame = 0;
			if (!decoded_frame) {
			if (!(decoded_frame = av_frame_alloc())) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	} else
		av_frame_unref(decoded_frame);

		int len = avcodec_decode_audio4(c, decoded_frame, &got_frame, packet);
		if (len < 0) {
			restart_audio_resampling = 1;
			break;
		}

		packet_data += len;
		packet_size -= len;

		if (!got_frame)
			continue;

		int e;
		if (!swr) {
			int rates[] = { 48000, 96000, 192000, 44100, 88200, 176400, 0 };
			int *rate = rates;
			int in_rate = c->sample_rate;
			while (*rate && ((*rate / in_rate) * in_rate != *rate) && (in_rate / *rate) * *rate != in_rate)
				rate++;
			out_sample_rate = *rate ? *rate : 44100;
			swr = swr_alloc();
			out_channels = c->channels;
			if (c->channel_layout == 0) {
				// FIXME -- need to guess, looks pretty much like a bug in the FFMPEG WMA decoder
				c->channel_layout = AV_CH_LAYOUT_STEREO;
			}

			out_channel_layout = c->channel_layout;
			// player2 won't play mono
			if (out_channel_layout == AV_CH_LAYOUT_MONO) {
				out_channel_layout = AV_CH_LAYOUT_STEREO;
				out_channels = 2;
			}

			av_opt_set_int(swr, "in_channel_layout", c->channel_layout, 0);
			av_opt_set_int(swr, "out_channel_layout", out_channel_layout, 0);
			av_opt_set_int(swr, "in_sample_rate", c->sample_rate, 0);
			av_opt_set_int(swr, "out_sample_rate", out_sample_rate, 0);
			av_opt_set_int(swr, "in_sample_fmt", c->sample_fmt, 0);
			av_opt_set_int(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

			e = swr_init(swr);
			if (e < 0) {
				fprintf(stderr,
					"swr_init: %d (icl=%d ocl=%d isr=%d osr=%d isf=%d osf=%d\n",
					-e, (int) c->channel_layout,
					(int) out_channel_layout, c->sample_rate, out_sample_rate, c->sample_fmt, AV_SAMPLE_FMT_S16);
				swr_free(&swr);
				swr = NULL;
			}
		}

		uint8_t *output = NULL;
		int in_samples = decoded_frame->nb_samples;
		int out_samples = av_rescale_rnd(swr_get_delay(swr, c->sample_rate) + in_samples, out_sample_rate, c->sample_rate, AV_ROUND_UP);
		e = av_samples_alloc(&output, NULL, out_channels, out_samples, AV_SAMPLE_FMT_S16, 1);
		if (e < 0) {
			fprintf(stderr, "av_samples_alloc: %d\n", -e);
			continue;
		}
		// FIXME. PTS calculation is probably broken.
		int64_t pts;
		int64_t next_in_pts =  av_rescale(av_frame_get_best_effort_timestamp(decoded_frame),
			call->stream->time_base.num * (int64_t) out_sample_rate * c->sample_rate,
			call->stream->time_base.den);
		int64_t next_out_pts = av_rescale(swr_next_pts(swr, next_in_pts),
			call->stream->time_base.den,
			call->stream->time_base.num * (int64_t) out_sample_rate * c->sample_rate);
			*(call->context->currentAudioPtsP) = /* audioTrack->pts = */ pts = calcPts(call->avfc, call->stream, next_out_pts);
			out_samples = swr_convert(swr, &output, out_samples, (const uint8_t **)
			&decoded_frame->data[0], in_samples);

		WriterAVCallData_t pcmOut;
		pcmOut.fd = call->fd;
		pcmOut.uSampleRate = out_sample_rate;
		pcmOut.uNoOfChannels = av_get_channel_layout_nb_channels(out_channel_layout);
		pcmOut.uBitsPerSample = 16;
		pcmOut.bLittleEndian = 1;

		pcmOut.data = output;
		pcmOut.len = out_samples * sizeof(short) * out_channels;

		pcmOut.Pts = pts; // FIXME  videoTrack ? pts : 0;
		pcmOut.stream = call->stream;
		pcmOut.avfc = call->avfc;
		pcmOut.packet = NULL;

		writeData(&pcmOut);

		av_freep(&output);
	}
	return packet->size;
}

/* ***************************** */
/* Writer  Definition            */
/* ***************************** */

static WriterCaps_t caps_pcm = {
	"pcm",
	eAudio,
	"A_PCM",
	AUDIO_ENCODING_LPCMA
};

struct Writer_s WriterAudioPCM = {
	&reset,
	&writeData,
	NULL,
	&caps_pcm
};

static WriterCaps_t caps_ipcm = {
	"ipcm",
	eAudio,
	"A_IPCM",
	AUDIO_ENCODING_LPCMA
};

struct Writer_s WriterAudioIPCM = {
	&resetIpcm,
	&writeDataIpcm,
	NULL,
	&caps_ipcm
};