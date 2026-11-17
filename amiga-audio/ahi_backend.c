/*
 * OpenOMF audio back end for AmigaOS 3: ahi.device.
 *
 * A mixer process feeds ahi.device with two linked CMD_WRITE requests
 * (double buffered). Each block: the music source renders 16-bit frames
 * into the block (as SDL_mixer's music hook does), then the three sound
 * channels are mixed on top. Sound sources are 8-bit unsigned mono at their
 * own (pitched) rate; they are resampled with a 16.16 fixed-point step and
 * mixed with integer gains, so the inner loop has no FPU instructions.
 *
 * The game's calls (play, stop, fade, pan, volume, music) take the
 * context's semaphore briefly; the mixer holds it while it fills a block.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "audio/backends/ahi/ahi_backend.h"
#include "audio/backends/audio_backend.h"
#include "audio/music_sources/music_source.h"
#include "utils/allocator.h"
#include "utils/c_array_util.h"
#include "utils/log.h"

#include <exec/exec.h>
#include <devices/ahi.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#define AHI_BLOCK_FRAMES 1024

static const audio_sample_rate supported_sample_rates[] = {
    {11025, 0, "11025Hz"},
    {22050, 1, "22050Hz"},
    {44100, 0, "44100Hz"},
};

typedef struct ahi_channel {
    unsigned char *data;       /* our copy of the source's bytes (8-bit unsigned) */
    unsigned long len;         /* frames */
    unsigned long pos;         /* 16.16 */
    unsigned long step;        /* 16.16 */
    int volume;                /* 0 .. 127 */
    int gain_l, gain_r;        /* 0 .. 256, from the panning */
    long fade;                 /* 0 .. 65536 */
    long fade_step;            /* per frame; < 0 fades out */
    int playing;
} ahi_channel;

typedef struct ahi_context {
    struct SignalSemaphore lock;
    unsigned rate;
    unsigned channels; /* 1 or 2 */
    int resampler;
    int sound_volume; /* 0 .. 256 */
    float music_volume;
    music_source music;
    ahi_channel ch[SOUND_CHANNEL_COUNT];

    struct Process *mixer;
    struct Task *parent;
    BYTE parent_sig;
    volatile int quit;
    volatile int started; /* 1 running, -1 failed */
} ahi_context;

static ahi_context *g_ctx; /* the mixer process's context: there is one back end */

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void pan_gains(int panning, int *l, int *r) {
    panning = clampi(panning, -100, 100);
    *l = panning > 0 ? (100 - panning) * 256 / 100 : 256;
    *r = panning < 0 ? (100 + panning) * 256 / 100 : 256;
}

/* Fill one block: music, then the sound channels on top. Called with the lock. */
static void mix_block(ahi_context *ctx, WORD *out, unsigned frames) {
    const unsigned nch = ctx->channels;
    memset(out, 0, frames * nch * sizeof(WORD));
    if(ctx->music.context != NULL) {
        music_source_render(&ctx->music, (char *)out, (int)(frames * nch * sizeof(WORD)));
    }
    for(int c = 0; c < SOUND_CHANNEL_COUNT; c++) {
        ahi_channel *ch = &ctx->ch[c];
        if(!ch->playing) {
            continue;
        }
        const long base = (long)ch->volume * ctx->sound_volume; /* 0 .. 32512 */
        WORD *o = out;
        for(unsigned f = 0; f < frames; f++) {
            unsigned long i = ch->pos >> 16;
            if(i >= ch->len) {
                ch->playing = 0;
                break;
            }
            long s = ((long)ch->data[i] - 128) << 8; /* -32768 .. 32512 */
            long g = (base * (ch->fade >> 8)) >> 8;  /* 0 .. 32512 */
            if(ch->fade_step != 0) {
                ch->fade += ch->fade_step;
                if(ch->fade >= 65536) {
                    ch->fade = 65536;
                    ch->fade_step = 0;
                } else if(ch->fade <= 0) {
                    ch->playing = 0;
                    break;
                }
            }
            long v = (s * (g >> 7)) >> 8; /* g>>7: 0 .. 254 */
            if(nch == 2) {
                long l = o[0] + ((v * ch->gain_l) >> 8);
                long r = o[1] + ((v * ch->gain_r) >> 8);
                o[0] = (WORD)clampi((int)l, -32768, 32767);
                o[1] = (WORD)clampi((int)r, -32768, 32767);
                o += 2;
            } else {
                long m = o[0] + v;
                o[0] = (WORD)clampi((int)m, -32768, 32767);
                o += 1;
            }
            ch->pos += ch->step;
        }
    }
}

static void mixer_entry(void) {
    ahi_context *ctx = g_ctx;
    struct MsgPort *port = CreateMsgPort();
    struct AHIRequest *req[2] = {NULL, NULL};
    WORD *buf[2] = {NULL, NULL};
    const ULONG bytes = AHI_BLOCK_FRAMES * ctx->channels * sizeof(WORD);
    BOOL opened = FALSE;

    if(port != NULL) {
        req[0] = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    }
    if(req[0] != NULL) {
        req[0]->ahir_Version = 4;
        opened = OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT, (struct IORequest *)req[0], 0) == 0;
    }
    if(opened) {
        req[1] = AllocVec(sizeof(struct AHIRequest), MEMF_PUBLIC);
        buf[0] = AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        buf[1] = AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    }
    if(!opened || req[1] == NULL || buf[0] == NULL || buf[1] == NULL) {
        ctx->started = -1;
        Signal(ctx->parent, 1UL << ctx->parent_sig);
        goto out;
    }
    CopyMem(req[0], req[1], sizeof(struct AHIRequest));
    ctx->started = 1;
    Signal(ctx->parent, 1UL << ctx->parent_sig);

    struct AHIRequest *last = NULL;
    int cur = 0;
    while(!ctx->quit && !(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
        ObtainSemaphore(&ctx->lock);
        mix_block(ctx, buf[cur], AHI_BLOCK_FRAMES);
        ReleaseSemaphore(&ctx->lock);

        struct AHIRequest *r = req[cur];
        r->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
        r->ahir_Std.io_Command = CMD_WRITE;
        r->ahir_Std.io_Data = buf[cur];
        r->ahir_Std.io_Length = bytes;
        r->ahir_Std.io_Offset = 0;
        r->ahir_Frequency = ctx->rate;
        r->ahir_Type = ctx->channels == 2 ? AHIST_S16S : AHIST_M16S;
        r->ahir_Volume = 0x10000;
        r->ahir_Position = 0x8000;
        r->ahir_Link = last;
        SendIO((struct IORequest *)r);

        if(last != NULL) {
            /* the block before this one: once it has played, its buffer is free */
            WaitIO((struct IORequest *)last);
        }
        last = r;
        cur ^= 1;
    }
    if(last != NULL) {
        AbortIO((struct IORequest *)last);
        WaitIO((struct IORequest *)last);
    }
out:
    if(opened) {
        CloseDevice((struct IORequest *)req[0]);
    }
    FreeVec(buf[0]);
    FreeVec(buf[1]);
    FreeVec(req[1]);
    if(req[0] != NULL) {
        DeleteIORequest((struct IORequest *)req[0]);
    }
    if(port != NULL) {
        DeleteMsgPort(port);
    }
    Forbid(); /* the parent frees ctx only after this signal, and we exit before it runs */
    ctx->mixer = NULL;
    Signal(ctx->parent, 1UL << ctx->parent_sig);
}

static bool is_available(void) {
    return true; /* setup fails cleanly when ahi.device is not installed */
}

static const char *get_description(void) {
    return "Audio output through ahi.device";
}

static const char *get_name(void) {
    return "AHI";
}

static unsigned int get_sample_rates(const audio_sample_rate **sample_rates) {
    *sample_rates = supported_sample_rates;
    return N_ELEMENTS(supported_sample_rates);
}

static void create_backend(audio_backend *backend) {
    backend->ctx = omf_calloc(1, sizeof(ahi_context));
}

static void destroy_backend(audio_backend *backend) {
    omf_free(backend->ctx);
}

static void get_info(void *userdata, unsigned *sample_rate, unsigned *channels, unsigned *resampler) {
    const ahi_context *ctx = userdata;
    if(sample_rate != NULL) {
        *sample_rate = ctx->rate;
    }
    if(channels != NULL) {
        *channels = ctx->channels;
    }
    if(resampler != NULL) {
        *resampler = (unsigned)ctx->resampler;
    }
}

static void set_sound_volume(void *userdata, float volume) {
    ahi_context *ctx = userdata;
    int v = (int)(volume * 256.0f);
    ObtainSemaphore(&ctx->lock);
    ctx->sound_volume = clampi(v, 0, 256);
    ReleaseSemaphore(&ctx->lock);
}

static void set_music_volume(void *userdata, float volume) {
    ahi_context *ctx = userdata;
    ObtainSemaphore(&ctx->lock);
    ctx->music_volume = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
    music_source_set_volume(&ctx->music, ctx->music_volume);
    ReleaseSemaphore(&ctx->lock);
}

static bool play_pcm_sound(void *userdata, int channel, const sound_source *src, int volume, int panning,
                           int fade_in_ms) {
    ahi_context *ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT || src == NULL || src->buf == NULL || src->len == 0 ||
       src->freq <= 0) {
        return false;
    }
    /* our own copy: the caller may free the source once this returns */
    unsigned char *copy = omf_malloc(src->len);
    memcpy(copy, src->buf, src->len);
    ObtainSemaphore(&ctx->lock);
    ahi_channel *ch = &ctx->ch[channel];
    unsigned char *old = ch->data;
    ch->data = copy;
    ch->len = src->len;
    ch->pos = 0;
    ch->step = ((unsigned long)src->freq << 16) / ctx->rate;
    ch->volume = clampi(volume, 0, 127);
    pan_gains(panning, &ch->gain_l, &ch->gain_r);
    if(fade_in_ms > 0) {
        long frames = (long)ctx->rate * fade_in_ms / 1000;
        ch->fade = 0;
        ch->fade_step = frames > 0 ? 65536 / frames + 1 : 65536;
    } else {
        ch->fade = 65536;
        ch->fade_step = 0;
    }
    ch->playing = 1;
    ReleaseSemaphore(&ctx->lock);
    omf_free(old); /* the mixer only reads a channel's data with the lock held */
    return true;
}

static bool is_channel_playing(void *userdata, int channel) {
    ahi_context *ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return false;
    }
    return ctx->ch[channel].playing != 0;
}

static void stop_channel(void *userdata, int channel) {
    ahi_context *ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return;
    }
    ObtainSemaphore(&ctx->lock);
    ctx->ch[channel].playing = 0;
    ReleaseSemaphore(&ctx->lock);
}

static void fade_out_channel(void *userdata, int channel, int ms) {
    ahi_context *ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return;
    }
    ObtainSemaphore(&ctx->lock);
    ahi_channel *ch = &ctx->ch[channel];
    long frames = (long)ctx->rate * (ms > 0 ? ms : 1) / 1000;
    ch->fade_step = -(65536 / (frames > 0 ? frames : 1) + 1);
    ReleaseSemaphore(&ctx->lock);
}

static void set_channel_panning(void *userdata, int channel, int panning) {
    ahi_context *ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return;
    }
    ObtainSemaphore(&ctx->lock);
    pan_gains(panning, &ctx->ch[channel].gain_l, &ctx->ch[channel].gain_r);
    ReleaseSemaphore(&ctx->lock);
}

static void stop_music(void *userdata) {
    ahi_context *ctx = userdata;
    ObtainSemaphore(&ctx->lock);
    music_source_close(&ctx->music);
    ReleaseSemaphore(&ctx->lock);
}

static void play_music(void *userdata, const music_source *src) {
    ahi_context *ctx = userdata;
    ObtainSemaphore(&ctx->lock);
    music_source_close(&ctx->music);
    memcpy(&ctx->music, src, sizeof(music_source));
    music_source_set_volume(&ctx->music, ctx->music_volume);
    ReleaseSemaphore(&ctx->lock);
}

static bool setup_context(void *userdata, unsigned sample_rate, bool mono, int resampler, float music_volume,
                          float sound_volume) {
    ahi_context *ctx = userdata;
    memset(ctx, 0, sizeof(*ctx));
    InitSemaphore(&ctx->lock);
    ctx->rate = sample_rate >= 8000 && sample_rate <= 48000 ? sample_rate : 22050;
    ctx->channels = mono ? 1 : 2;
    ctx->resampler = resampler;
    ctx->music_volume = music_volume;
    ctx->sound_volume = clampi((int)(sound_volume * 256.0f), 0, 256);

    ctx->parent = FindTask(NULL);
    ctx->parent_sig = AllocSignal(-1);
    if(ctx->parent_sig < 0) {
        return false;
    }
    g_ctx = ctx;
    ctx->mixer = CreateNewProcTags(NP_Entry, (ULONG)mixer_entry, NP_Name, (ULONG) "OpenOMF audio", NP_Priority, 10,
                                   NP_StackSize, 65536, TAG_DONE);
    if(ctx->mixer == NULL) {
        FreeSignal(ctx->parent_sig);
        log_error("AHI: could not start the mixer");
        return false;
    }
    while(ctx->started == 0) {
        Wait(1UL << ctx->parent_sig);
    }
    if(ctx->started < 0) {
        while(ctx->mixer != NULL) {
            Wait(1UL << ctx->parent_sig);
        }
        FreeSignal(ctx->parent_sig);
        log_error("AHI: could not open ahi.device (is AHI installed and a unit set in AHI prefs?)");
        return false;
    }
    log_info("AHI: %u Hz, %s, %d frames a block", ctx->rate, ctx->channels == 2 ? "stereo" : "mono",
             AHI_BLOCK_FRAMES);
    return true;
}

static void close_context(void *userdata) {
    ahi_context *ctx = userdata;
    if(ctx->mixer != NULL) {
        ctx->quit = 1;
        Signal((struct Task *)ctx->mixer, SIGBREAKF_CTRL_C);
        while(ctx->mixer != NULL) {
            Wait(1UL << ctx->parent_sig);
        }
    }
    if(ctx->parent_sig >= 0) {
        FreeSignal(ctx->parent_sig);
        ctx->parent_sig = -1;
    }
    music_source_close(&ctx->music);
    for(int c = 0; c < SOUND_CHANNEL_COUNT; c++) {
        omf_free(ctx->ch[c].data);
        ctx->ch[c].playing = 0;
    }
    g_ctx = NULL;
}

void ahi_audio_backend_set_callbacks(audio_backend *backend) {
    backend->is_available = is_available;
    backend->get_description = get_description;
    backend->get_name = get_name;
    backend->get_sample_rates = get_sample_rates;
    backend->get_info = get_info;
    backend->create = create_backend;
    backend->destroy = destroy_backend;
    backend->set_music_volume = set_music_volume;
    backend->set_sound_volume = set_sound_volume;
    backend->setup_context = setup_context;
    backend->close_context = close_context;
    backend->play_pcm_sound = play_pcm_sound;
    backend->is_channel_playing = is_channel_playing;
    backend->stop_channel = stop_channel;
    backend->fade_out_channel = fade_out_channel;
    backend->set_channel_panning = set_channel_panning;
    backend->play_music = play_music;
    backend->stop_music = stop_music;
}
