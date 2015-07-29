/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "player.h"
#include <groove/queue.h>

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <soundio/soundio.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>

struct GroovePlayerPrivate {
    struct GroovePlayer externals;
    struct GroovePlayerContextPrivate *pc;

    struct GrooveBuffer *audio_buf;
    size_t audio_buf_size; // in frames
    size_t audio_buf_index; // in frames

    // this mutex applies to the variables in this block
    pthread_mutex_t play_head_mutex;
    bool play_head_mutex_inited;
    // pointer to current item where the buffered audio is reaching the device
    struct GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;

    struct SoundIo *dummy_soundio;
    struct SoundIoDevice *dummy_device;

    struct SoundIoOutStream *outstream;
    struct GrooveSink *sink;

    struct GrooveQueue *eventq;

    // watchdog thread for opening and closing audio device
    bool abort_request;
    pthread_t device_thread_id;
    int device_thread_inited;
    pthread_cond_t device_thread_cond;
    bool device_thread_cond_inited;
    int silence_frames_left;
    bool request_device_reopen;
    struct GrooveAudioFormat device_format;
};

struct GroovePlayerContextPrivate {
    struct GroovePlayerContext externals;
    struct SoundIo *soundio;
};

// TODO refactor this
__attribute__ ((cold))
__attribute__ ((noreturn))
__attribute__ ((format (printf, 1, 2)))
static void panic(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

static int open_audio_device(struct GroovePlayer *player,
        const struct GrooveAudioFormat *target_format, struct GrooveAudioFormat *actual_format,
        int use_exact_audio_format);

static enum SoundIoFormat to_soundio_fmt(enum GrooveSampleFormat fmt) {
    switch (fmt) {
        case GROOVE_SAMPLE_FMT_NONE:
            return SoundIoFormatInvalid;

        case GROOVE_SAMPLE_FMT_U8:
        case GROOVE_SAMPLE_FMT_U8P:
            return SoundIoFormatU8;

        case GROOVE_SAMPLE_FMT_S16:
        case GROOVE_SAMPLE_FMT_S16P:
            return SoundIoFormatS16NE;

        case GROOVE_SAMPLE_FMT_S32:
        case GROOVE_SAMPLE_FMT_S32P:
            return SoundIoFormatS32NE;

        case GROOVE_SAMPLE_FMT_FLT:
        case GROOVE_SAMPLE_FMT_FLTP:
            return SoundIoFormatFloat32NE;

        case GROOVE_SAMPLE_FMT_DBL:
        case GROOVE_SAMPLE_FMT_DBLP:
            return SoundIoFormatFloat64NE;
    }
    return SoundIoFormatInvalid;
}

static void to_soundio_layout(uint64_t in_layout, struct SoundIoChannelLayout *out_layout) {
    if (in_layout != GROOVE_CH_LAYOUT_STEREO)
        panic("TODO libroove's channel layout handling needs to be revamped using genesis as a guide");
    out_layout->name = "Stereo";
    out_layout->channel_count = 2;
    out_layout->channels[0] = SoundIoChannelIdFrontLeft;
    out_layout->channels[1] = SoundIoChannelIdFrontRight;
}

static void emit_event(struct GrooveQueue *queue, enum GroovePlayerEventType type) {
    union GroovePlayerEvent *evt = av_malloc(sizeof(union GroovePlayerEvent));
    if (!evt) {
        av_log(NULL, AV_LOG_ERROR, "unable to create event: out of memory\n");
        return;
    }
    evt->type = type;
    if (groove_queue_put(queue, evt) < 0)
        av_log(NULL, AV_LOG_ERROR, "unable to put event on queue: out of memory\n");
}

static void close_audio_device(struct GroovePlayerPrivate *p) {
    if (p->outstream) {
        soundio_outstream_destroy(p->outstream);
        p->outstream = NULL;
    }
}

static void *device_thread_run(void *arg) {
    struct GroovePlayer *player = arg;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    for (;;) {
        if (p->abort_request)
            break;

        pthread_mutex_lock(&p->play_head_mutex);
        if (!p->request_device_reopen) {
            pthread_cond_wait(&p->device_thread_cond, &p->play_head_mutex);
            pthread_mutex_unlock(&p->play_head_mutex);
            continue;
        }

        p->request_device_reopen = false;

        close_audio_device(p);

        p->device_format = p->audio_buf->format;
        int err;
        if ((err = open_audio_device(player, &p->audio_buf->format, NULL, player->use_exact_audio_format))) {
            pthread_mutex_unlock(&p->play_head_mutex);
            emit_event(p->eventq, GROOVE_EVENT_DEVICE_REOPEN_ERROR);
        } else {
            pthread_mutex_unlock(&p->play_head_mutex);
            // unlock the mutex before calling start since start might call the audio callback
            soundio_outstream_start(p->outstream);
            emit_event(p->eventq, GROOVE_EVENT_DEVICEREOPENED);
        }
    }

    return NULL;
}

static bool is_planar(enum GrooveSampleFormat fmt) {
    switch (fmt) {
        default:
            return false;
        case GROOVE_SAMPLE_FMT_U8P:
        case GROOVE_SAMPLE_FMT_S16P:
        case GROOVE_SAMPLE_FMT_S32P:
        case GROOVE_SAMPLE_FMT_FLTP:
        case GROOVE_SAMPLE_FMT_DBLP:
            return true;
    }
}

static void error_callback(struct SoundIoOutStream *outstream, int err) {
    // TODO destroy stream and emit error
    panic("stream error: %s", soundio_strerror(err));
}

static void underflow_callback(struct SoundIoOutStream *outstream) {
    struct GroovePlayerPrivate *p = outstream->userdata;
    emit_event(p->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
}

static void audio_callback(struct SoundIoOutStream *outstream, int requested_frame_count) {
    struct GroovePlayerPrivate *p = outstream->userdata;

    struct GrooveSink *sink = p->sink;
    struct GroovePlaylist *playlist = sink->playlist;

    int paused = !groove_playlist_playing(playlist);
    int err;

    pthread_mutex_lock(&p->play_head_mutex);

    for (;;) {
        struct SoundIoChannelArea *areas;
        int write_frame_count = requested_frame_count;
        if ((err = soundio_outstream_begin_write(outstream, &areas, &write_frame_count)))
            panic("%s", soundio_strerror(err));

        if (!write_frame_count)
            break;

        const struct SoundIoChannelLayout *layout = &outstream->layout;

        bool waiting_for_silence = (p->silence_frames_left > 0);
        if (!p->request_device_reopen && !waiting_for_silence &&
            !paused && p->audio_buf_index >= p->audio_buf_size)
        {
            groove_buffer_unref(p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            // TODO problematic when this function blocks for a long time
            int ret = groove_sink_buffer_get(p->sink, &p->audio_buf, 1);
            if (ret == GROOVE_BUFFER_END) {
                p->play_head = NULL;
                p->play_pos = -1.0;
                // emit the event after setting play head so user can check
                // the play head
                // TODO emit this event when the sound is coming out of the speakers,
                // not when we've queued it up.
                emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
            } else if (ret == GROOVE_BUFFER_YES) {
                if (p->play_head != p->audio_buf->item)
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->play_head = p->audio_buf->item;
                p->play_pos = p->audio_buf->pos;
                p->audio_buf_size = p->audio_buf->frame_count;

                if (p->device_thread_inited &&
                    !groove_audio_formats_equal(&p->audio_buf->format, &p->device_format))
                {
                    // TODO use latency instead of buffer length?
                    p->silence_frames_left = outstream->buffer_duration * outstream->sample_rate;
                    waiting_for_silence = true;
                }
            }
        }
        if (p->request_device_reopen || waiting_for_silence || paused || !p->audio_buf) {
            // fill with silence
            for (int frame = 0; frame < write_frame_count; frame += 1) {
                for (int channel = 0; channel < layout->channel_count; channel += 1) {
                    memset(areas[channel].ptr, 0, outstream->bytes_per_sample);
                    areas[channel].ptr += areas[channel].step;
                }
            }

            if (waiting_for_silence) {
                p->silence_frames_left -= write_frame_count;
                if (p->silence_frames_left <= 0) {
                    p->request_device_reopen = true;
                    pthread_cond_signal(&p->device_thread_cond);
                }
            }
            if ((err = soundio_outstream_end_write(outstream, write_frame_count)))
                panic("%s", soundio_strerror(err));
            break;
        }
        size_t read_frame_count = p->audio_buf_size - p->audio_buf_index;
        int frame_count = (read_frame_count < write_frame_count) ? read_frame_count : write_frame_count;

        if (is_planar(p->audio_buf->format.sample_fmt)) {
            int end_frame = p->audio_buf_index + frame_count;
            for (; p->audio_buf_index < end_frame; p->audio_buf_index += 1) {
                for (int ch = 0; ch < layout->channel_count; ch += 1) {
                    uint8_t *source = &p->audio_buf->data[ch][p->audio_buf_index * outstream->bytes_per_sample];
                    memcpy(areas[ch].ptr, source, outstream->bytes_per_sample);
                    areas[ch].ptr += areas[ch].step;
                }
            }
        } else {
            uint8_t *source = p->audio_buf->data[0] + p->audio_buf_index * outstream->bytes_per_frame;
            int end_frame = p->audio_buf_index + frame_count;
            for (; p->audio_buf_index < end_frame; p->audio_buf_index += 1) {
                for (int ch = 0; ch < layout->channel_count; ch += 1) {
                    memcpy(areas[ch].ptr, source, outstream->bytes_per_sample);
                    source += outstream->bytes_per_sample;
                    areas[ch].ptr += areas[ch].step;
                }
            }
        }
        p->play_pos += frame_count / (double) outstream->sample_rate;

        if ((err = soundio_outstream_end_write(outstream, frame_count)))
            panic("%s", soundio_strerror(err));
        requested_frame_count -= frame_count;

        if (requested_frame_count <= 0)
            break;
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    if (p->play_head == item) {
        p->play_head = NULL;
        p->play_pos = -1.0;
        groove_buffer_unref(p->audio_buf);
        p->audio_buf = NULL;
        p->audio_buf_index = 0;
        p->audio_buf_size = 0;
        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_pause(struct GrooveSink *sink) {
    struct GroovePlayer *player = sink->userdata;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    soundio_outstream_pause(p->outstream, true);
}

static void sink_play(struct GrooveSink *sink) {
    struct GroovePlayer *player = sink->userdata;
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    soundio_outstream_pause(p->outstream, false);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;
    p->play_pos = -1.0;
    p->play_head = NULL;

    pthread_mutex_unlock(&p->play_head_mutex);
}

struct GroovePlayer *groove_player_create(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    struct GroovePlayerPrivate *p = av_mallocz(sizeof(struct GroovePlayerPrivate));

    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }
    struct GroovePlayer *player = &p->externals;
    p->pc = pc;

    p->sink = groove_sink_create();
    if (!p->sink) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create sink: out of memory\n");
        return NULL;
    }

    p->sink->userdata = player;
    p->sink->purge = sink_purge;
    p->sink->flush = sink_flush;

    if (pthread_mutex_init(&p->play_head_mutex, NULL) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create play head mutex: out of memory\n");
        return NULL;
    }
    p->play_head_mutex_inited = true;

    p->eventq = groove_queue_create();
    if (!p->eventq) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create event queue: out of memory\n");
        return NULL;
    }

    if (pthread_cond_init(&p->device_thread_cond, NULL) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex condition\n");
        return NULL;
    }
    p->device_thread_cond_inited = true;

    // set some nice defaults
    player->target_audio_format.sample_rate = 44100;
    player->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    player->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    // small because there is no way to clear the buffer.
    player->sink_buffer_size = 8192;
    player->gain = p->sink->gain;

    return player;
}

void groove_player_destroy(struct GroovePlayer *player) {
    if (!player)
        return;

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    if (p->device_thread_cond_inited)
        pthread_cond_destroy(&p->device_thread_cond);

    if (p->play_head_mutex_inited)
        pthread_mutex_destroy(&p->play_head_mutex);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    av_free(p);
}

static int open_audio_device(struct GroovePlayer *player,
        const struct GrooveAudioFormat *target_format,
        struct GrooveAudioFormat *actual_format,
        int use_exact_audio_format)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    struct SoundIoDevice *device;
    if (player->use_dummy_device) {
        device = p->dummy_device;
        soundio_device_ref(device);
    } else {
        if (player->device) {
            device = (struct SoundIoDevice *) player->device;
            soundio_device_ref(device);
        } else {
            int default_index = soundio_default_output_device_index(p->pc->soundio);
            device = soundio_get_output_device(p->pc->soundio, default_index);
        }
    }

    assert(!p->outstream);
    p->outstream = soundio_outstream_create(device);
    if (!p->outstream) {
        soundio_device_unref(device);
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: out of memory\n");
        return -1;
    }

    soundio_device_unref(device);

    // support best sample format
    enum SoundIoFormat fmt = to_soundio_fmt(target_format->sample_fmt);
    if (!soundio_device_supports_format(device, fmt)) {
        panic("TODO: sample format conversion if necessary");
    }
    p->outstream->format = to_soundio_fmt(target_format->sample_fmt);

    if (target_format->sample_rate < device->sample_rate_min ||
        target_format->sample_rate > device->sample_rate_max)
    {
        panic("TODO: resample if necessary");
    }
    p->outstream->sample_rate = target_format->sample_rate;

    struct SoundIoChannelLayout layout;
    to_soundio_layout(target_format->channel_layout, &layout);
    if (!soundio_device_supports_layout(device, &layout)) {
        panic("TODO: remap if necessary");
    }
    p->outstream->layout = layout;

    p->outstream->userdata = player;
    p->outstream->error_callback = error_callback;
    p->outstream->underflow_callback = underflow_callback;
    p->outstream->write_callback = audio_callback;

    int err;
    if ((err = soundio_outstream_open(p->outstream))) {
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", soundio_strerror(err));
        return -1;
    }

    if (actual_format) {
        actual_format->sample_rate = target_format->sample_rate;
        actual_format->channel_layout = target_format->channel_layout;
        actual_format->sample_fmt = target_format->sample_fmt;
    }

    return 0;
}

int groove_player_attach(struct GroovePlayer *player, struct GroovePlaylist *playlist) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    p->sink->gain = player->gain;
    p->sink->buffer_size = player->sink_buffer_size;
    p->sink->pause = sink_pause;
    p->sink->play = sink_play;

    int err;

    // for dummy devices we create a new soundio instance for each one and
    // connect to the dummy backend.
    // for normal devices we use the one soundio instance that is connected
    // to the best backend.
    if (player->use_dummy_device) {
        p->dummy_soundio = soundio_create();
        if (!p->dummy_soundio) {
            groove_player_detach(player);
            return -1;
        }
        if ((err = soundio_connect_backend(p->dummy_soundio, SoundIoBackendDummy))) {
            groove_player_detach(player);
            return -1;
        }

        int dummy_device_index = soundio_default_output_device_index(p->dummy_soundio);
        p->dummy_device = soundio_get_output_device(p->dummy_soundio, dummy_device_index);
    }

    if (open_audio_device(player, &player->target_audio_format, &player->actual_audio_format,
                player->use_exact_audio_format))
    {
        groove_player_detach(player);
        return -1;
    }

    // based on spec that we got, attach a sink with those properties
    p->sink->audio_format = player->actual_audio_format;

    if (p->sink->audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return -1;
    }

    if (player->use_exact_audio_format) {
        p->sink->disable_resample = 1;

        if (pthread_create(&p->device_thread_id, NULL, device_thread_run, player) != 0) {
            groove_player_detach(player);
            av_log(NULL, AV_LOG_ERROR, "unable to create device thread\n");
            return -1;
        }
        p->device_thread_inited = true;
    }

    err = groove_sink_attach(p->sink, playlist);
    if (err < 0) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;

    groove_queue_reset(p->eventq);


    if (!groove_playlist_playing(playlist))
        sink_pause(p->sink);

    soundio_outstream_start(p->outstream);

    return 0;
}

int groove_player_detach(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    p->abort_request = true;
    if (p->device_thread_inited) {
        pthread_mutex_lock(&p->play_head_mutex);
        pthread_cond_signal(&p->device_thread_cond);
        pthread_mutex_unlock(&p->play_head_mutex);
        pthread_join(p->device_thread_id, NULL);
        p->device_thread_inited = false;
    }
    if (p->eventq) {
        groove_queue_flush(p->eventq);
        groove_queue_abort(p->eventq);
    }
    if (p->sink->playlist) {
        groove_sink_detach(p->sink);
    }
    close_audio_device(p);

    soundio_device_unref(p->dummy_device);
    p->dummy_device = NULL;

    soundio_destroy(p->dummy_soundio);
    p->dummy_soundio = NULL;

    player->playlist = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    p->request_device_reopen = false;
    p->silence_frames_left = 0;
    p->abort_request = false;

    return 0;
}

void groove_player_position(struct GroovePlayer *player,
        struct GroovePlaylistItem **item, double *seconds)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    pthread_mutex_lock(&p->play_head_mutex);

    if (item)
        *item = p->play_head;

    if (seconds)
        *seconds = p->play_pos;

    pthread_mutex_unlock(&p->play_head_mutex);
}

int groove_player_event_get(struct GroovePlayer *player,
        union GroovePlayerEvent *event, int block)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    union GroovePlayerEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        av_free(tmp);
    }
    return err;
}

int groove_player_event_peek(struct GroovePlayer *player, int block) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    return groove_queue_peek(p->eventq, block);
}

int groove_player_set_gain(struct GroovePlayer *player, double gain) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    player->gain = gain;
    return groove_sink_set_gain(p->sink, gain);
}

struct GrooveAudioFormat groove_player_get_device_audio_format(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    struct GrooveAudioFormat result;
    pthread_mutex_lock(&p->play_head_mutex);
    result = p->device_format;
    pthread_mutex_unlock(&p->play_head_mutex);
    return result;
}

static void on_devices_change(struct SoundIo *soundio) {
    struct GroovePlayerContextPrivate *pc = soundio->userdata;
    struct GroovePlayerContext *player_context = &pc->externals;
    if (player_context->on_devices_change)
        player_context->on_devices_change(player_context);
}

static void on_events_signal(struct SoundIo *soundio) {
    struct GroovePlayerContextPrivate *pc = soundio->userdata;
    struct GroovePlayerContext *player_context = &pc->externals;
    if (player_context->on_events_signal)
        player_context->on_events_signal(player_context);
}

struct GroovePlayerContext *groove_player_context_create(void) {
    struct GroovePlayerContextPrivate *pc = av_mallocz(sizeof(struct GroovePlayerContextPrivate));
    if (!pc)
        return NULL;

    struct GroovePlayerContext *player_context = &pc->externals;

    pc->soundio = soundio_create();
    if (!pc->soundio) {
        groove_player_context_destroy(player_context);
        return NULL;
    }

    pc->soundio->on_devices_change = on_devices_change;
    pc->soundio->on_events_signal = on_events_signal;
    pc->soundio->userdata = pc;

    return player_context;
}

void groove_player_context_destroy(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    if (!pc)
        return;

    soundio_destroy(pc->soundio);

    av_free(pc);
}

int groove_player_context_connect(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    int err;
    if ((err = soundio_connect(pc->soundio)))
        return -1;
    return 0;
}

void groove_player_context_disconnect(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    soundio_disconnect(pc->soundio);
}

void groove_player_context_flush_events(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    soundio_flush_events(pc->soundio);
}

void groove_player_context_wait(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    soundio_wait_events(pc->soundio);
}

void groove_player_context_wakeup(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    soundio_wakeup(pc->soundio);
}

int groove_player_context_device_count(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    return soundio_output_device_count(pc->soundio);
}

int groove_player_context_device_default(struct GroovePlayerContext *player_context) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    return soundio_default_output_device_index(pc->soundio);
}

struct GrooveDevice *groove_player_context_get_device(struct GroovePlayerContext *player_context, int index) {
    struct GroovePlayerContextPrivate *pc = (struct GroovePlayerContextPrivate *)player_context;
    return (struct GrooveDevice *)soundio_get_output_device(pc->soundio, index);
}

const char *groove_device_id(struct GrooveDevice *device) {
    struct SoundIoDevice *sio_device = (struct SoundIoDevice *)device;
    return sio_device->name;
}

const char *groove_device_name(struct GrooveDevice *device) {
    struct SoundIoDevice *sio_device = (struct SoundIoDevice *)device;
    return sio_device->description;
}

int groove_device_is_raw(struct GrooveDevice *device) {
    struct SoundIoDevice *sio_device = (struct SoundIoDevice *)device;
    return sio_device->is_raw;
}

void groove_device_ref(struct GrooveDevice *device) {
    struct SoundIoDevice *sio_device = (struct SoundIoDevice *)device;
    soundio_device_ref(sio_device);
}

void groove_device_unref(struct GrooveDevice *device) {
    struct SoundIoDevice *sio_device = (struct SoundIoDevice *)device;
    soundio_device_unref(sio_device);
}
