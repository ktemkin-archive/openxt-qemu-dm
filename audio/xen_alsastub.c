#include "qemu-common.h"
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <libv4v.h>
#include <alsa/asoundlib.h>
#include "audio.h"

#define AUDIO_CAP "xen_alsa"
#include "audio_int.h"

typedef struct ALSAVoiceOut {
    HWVoiceOut hw;
    void *pcm_buf;
    snd_pcm_t *handle;
} ALSAVoiceOut;

typedef struct ALSAVoiceIn {
    HWVoiceIn hw;
    snd_pcm_t *handle;
    void *pcm_buf;
} ALSAVoiceIn;

static struct {
    int size_in_usec_in;
    int size_in_usec_out;
    const char *pcm_name_in;
    const char *pcm_name_out;
    unsigned int buffer_size_in;
    unsigned int period_size_in;
    unsigned int buffer_size_out;
    unsigned int period_size_out;
    unsigned int threshold;

    int buffer_size_in_overridden;
    int period_size_in_overridden;

    int buffer_size_out_overridden;
    int period_size_out_overridden;
    int verbose;

    const char *volume_control;
} conf = {
    .buffer_size_out = 1024,
    .pcm_name_out = "default",
    .pcm_name_in = "default",
    .volume_control = "Master",
};

struct alsa_params_req {
    int freq;
    snd_pcm_format_t fmt;
    int nchannels;
    int size_in_usec;
    int override_mask;
    unsigned int buffer_size;
    unsigned int period_size;
};

struct alsa_params_obt {
    int freq;
    audfmt_e fmt;
    int endianness;
    int nchannels;
    snd_pcm_uframes_t samples;
};

#define V4V_TYPE 'W'
#define V4VIOCSETRINGSIZE       _IOW(V4V_TYPE,  1, uint32_t)

#define AUDIO_PORT 5001

#define V4V_AUDIO_RING_SIZE \
  (V4V_ROUNDUP((((4096)*64) - sizeof(v4v_ring_t)-V4V_ROUNDUP(1))))

/* Messages definition */
#define AUDIO_INIT                             0x00
#define AUDIO_ALSA_OPEN                        0x01
#define AUDIO_VOLUME                           0x02
#define AUDIO_VOLUME_MONO                      0x03

#define AUDIO_SND_PCM_CLOSE                    0x04
#define AUDIO_SND_PCM_PREPARE                  0x05
#define AUDIO_SND_PCM_DROP                     0x06
#define AUDIO_SND_PCM_AVAIL_UPDATE             0x07
#define AUDIO_SND_PCM_STATE                    0x08
#define AUDIO_SND_PCM_WRITEI                   0x09
#define AUDIO_SND_PCM_READI                    0x10
#define AUDIO_SND_PCM_RESUME                   0x11

#define MAX_V4V_MSG_SIZE (V4V_AUDIO_RING_SIZE)

struct audio_helper {
    int fd;
    v4v_addr_t remote_addr;
    v4v_addr_t local_addr;
    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    int stubdom_id;
};

struct audio_helper *ah;



static int common_snd_pcm_op(snd_pcm_t *handle, uint8_t op)
{
    uint8_t *v4v_buf = ah->io_buf;
    int r;

    v4v_buf[0] = op;
    v4v_buf += 1;

    memcpy(v4v_buf, &handle, sizeof(handle));
    v4v_buf += sizeof(handle);

    r = v4v_sendto(ah->fd, ah->io_buf, v4v_buf - ah->io_buf,
                   0, &ah->remote_addr);

    r = v4v_recvfrom(ah->fd, ah->io_buf, MAX_V4V_MSG_SIZE, 0, &ah->remote_addr);

    v4v_buf = ah->io_buf;
    v4v_buf++;

    memcpy(&r, v4v_buf, sizeof(r));

    return r;
}

static int snd_pcm_close_wrapper(snd_pcm_t *handle)
{
    return common_snd_pcm_op(handle, AUDIO_SND_PCM_CLOSE);
}

static int snd_pcm_prepare_wrapper(snd_pcm_t *handle)
{
    return common_snd_pcm_op(handle, AUDIO_SND_PCM_PREPARE);
}

static int snd_pcm_resume_wrapper(snd_pcm_t *handle)
{
    return common_snd_pcm_op(handle, AUDIO_SND_PCM_RESUME);
}

static int snd_pcm_drop_wrapper(snd_pcm_t *handle)
{
    return common_snd_pcm_op(handle, AUDIO_SND_PCM_DROP);
}

static int snd_pcm_avail_update_wrapper(snd_pcm_t *handle)
{
    return common_snd_pcm_op(handle, AUDIO_SND_PCM_AVAIL_UPDATE);
}

static int snd_pcm_state_wrapper(snd_pcm_t *handle)
{
    return common_snd_pcm_op(handle, AUDIO_SND_PCM_STATE);
}

static int audio_helper_open(void)
{
    int ret;
    uint32_t v4v_ring_size = V4V_AUDIO_RING_SIZE;
    ah = malloc(sizeof(struct audio_helper));
    if (ah == NULL) {
        fprintf(stderr,
                "audio_stubdom_open: no memory to allocate stubdom_helper\n");
    }

    ah->fd = v4v_socket(SOCK_DGRAM);
    if (ah->fd == -1) {
        fprintf(stderr, "audio_stubdom_open: failed to open socket\n");
        ret = -1;
        return ret;
    }

    ah->local_addr.port = V4V_PORT_NONE;
    ah->local_addr.domain = V4V_DOMID_ANY;

    ah->remote_addr.port = AUDIO_PORT;
    ah->remote_addr.domain = 0;

    ret = ioctl(ah->fd, V4VIOCSETRINGSIZE, &v4v_ring_size);
    fprintf(stderr, "%s:%d ioctl=%d\n", __func__, __LINE__, ret);

    ret = v4v_bind(ah->fd, &ah->local_addr, 0);

    if (ret == -1) {
        fprintf(stderr, "%s:%d v4v_bind=%d\n", __func__, __LINE__, ret);
        return ret;
    }

    return 0;
}

static int snd_pcm_writei_wrapper(snd_pcm_t *handle, uint8_t *dst, int len)
{
    uint8_t *v4v_buf = ah->io_buf;
    int r;

    v4v_buf[0] = AUDIO_SND_PCM_WRITEI;
    v4v_buf += 1;

    memcpy(v4v_buf, &handle, sizeof(handle));
    v4v_buf += sizeof(handle);

    memcpy(v4v_buf, &len, sizeof(int));
    v4v_buf += sizeof(int);

    memcpy(v4v_buf, dst, (len*4));
    v4v_buf += (len*4);

    r = v4v_sendto(ah->fd, ah->io_buf, v4v_buf - ah->io_buf,
                   0, &ah->remote_addr);

    r = v4v_recvfrom(ah->fd, ah->io_buf, MAX_V4V_MSG_SIZE, 0, &ah->remote_addr);

    v4v_buf = ah->io_buf;
    v4v_buf++;

    memcpy(&r, v4v_buf, sizeof(r));

    return r;
}

static int snd_pcm_readi_wrapper(snd_pcm_t *handle, uint8_t *src, int len)
{
    uint8_t *v4v_buf = ah->io_buf;
    int r;

    v4v_buf[0] = AUDIO_SND_PCM_READI;
    v4v_buf += 1;

    memcpy(v4v_buf, &handle, sizeof(handle));
    v4v_buf += sizeof(handle);

    memcpy(v4v_buf, &len, sizeof(int));
    v4v_buf += sizeof(int);

    r = v4v_sendto(ah->fd, ah->io_buf, v4v_buf - ah->io_buf,
                   0, &ah->remote_addr);

    r = v4v_recvfrom(ah->fd, ah->io_buf, MAX_V4V_MSG_SIZE, 0, &ah->remote_addr);

    v4v_buf = ah->io_buf;
    v4v_buf++;

    memcpy(&r, v4v_buf, sizeof(r));
    v4v_buf += sizeof(int);

    if (r >= 0) {
        memcpy(src, v4v_buf, (r*4));
    }

    return r;
}

static void GCC_FMT_ATTR(2, 3) alsa_logerr(int err, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    AUD_vlog(AUDIO_CAP, fmt, ap);
    va_end(ap);

#if 0
    AUD_log(AUDIO_CAP, "Reason: %s\n", snd_strerror(err));
#endif
}

static void alsa_anal_close(snd_pcm_t **handlep)
{
    int err = snd_pcm_close_wrapper(*handlep);
    if (err) {
        alsa_logerr(err, "Failed to close PCM handle %p\n", *handlep);
    }
    *handlep = NULL;
}

static int alsa_write(SWVoiceOut *sw, void *buf, int len)
{
    return audio_pcm_sw_write(sw, buf, len);
}

static snd_pcm_format_t aud_to_alsafmt(audfmt_e fmt)
{
    switch (fmt) {
    case AUD_FMT_S8:
        return SND_PCM_FORMAT_S8;

    case AUD_FMT_U8:
        return SND_PCM_FORMAT_U8;

    case AUD_FMT_S16:
        return SND_PCM_FORMAT_S16_LE;

    case AUD_FMT_U16:
        return SND_PCM_FORMAT_U16_LE;

    case AUD_FMT_S32:
        return SND_PCM_FORMAT_S32_LE;

    case AUD_FMT_U32:
        return SND_PCM_FORMAT_U32_LE;

    default:
        dolog("Internal logic error: Bad audio format %d\n", fmt);
#ifdef DEBUG_AUDIO
        abort();
#endif
        return SND_PCM_FORMAT_U8;
    }
}

static int alsa_open(int in, struct alsa_params_req *req,
                     struct alsa_params_obt *obt, snd_pcm_t **handlep)
{
    uint8_t *v4v_buf = ah->io_buf;
    snd_pcm_t *handle;
    int r;

    v4v_buf[0] = AUDIO_ALSA_OPEN;
    v4v_buf += 1;

    memcpy(v4v_buf, &in, sizeof(in));
    v4v_buf += sizeof(in);

    memcpy(v4v_buf, req, sizeof(*req));
    v4v_buf += sizeof(*req);

    memcpy(v4v_buf, obt, sizeof(*obt));
    v4v_buf += sizeof(*obt);

    r = v4v_sendto(ah->fd, ah->io_buf, v4v_buf - ah->io_buf,
                   0, &ah->remote_addr);

    r = v4v_recvfrom(ah->fd, ah->io_buf, MAX_V4V_MSG_SIZE, 0, &ah->remote_addr);

    v4v_buf = ah->io_buf;
    v4v_buf++;

    memcpy(obt, v4v_buf, sizeof(*obt));
    v4v_buf += sizeof(*obt);

    memcpy(&handle, v4v_buf, sizeof(handle));
    v4v_buf += sizeof(handle);

    memcpy(&r, v4v_buf, sizeof(r));
    v4v_buf += sizeof(r);

    *handlep = handle;

    return r;
}


static int alsa_recover(snd_pcm_t *handle)
{
    int err = snd_pcm_prepare_wrapper(handle);
    if (err < 0) {
        alsa_logerr(err, "Failed to prepare handle %p\n", handle);
        return -1;
    }
    return 0;
}

static snd_pcm_sframes_t alsa_get_avail(snd_pcm_t *handle)
{
    snd_pcm_sframes_t avail;

    avail = snd_pcm_avail_update_wrapper(handle);
    if (avail < 0) {
        if (avail == -EPIPE) {
            if (!alsa_recover(handle)) {
                avail = snd_pcm_avail_update_wrapper(handle);
            }
        }

        if (avail < 0) {
            alsa_logerr(avail,
                        "Could not obtain number of available frames\n");
            return -1;
        }
    }

    return avail;
}

static void alsa_resume(snd_pcm_t *h)
{
    snd_pcm_drop_wrapper(h);
    snd_pcm_prepare_wrapper(h);
    snd_pcm_resume_wrapper(h);
    if (snd_pcm_state_wrapper(h) == SND_PCM_STATE_SUSPENDED) {
        fprintf(stderr, "alsa: try to resume but failed\n");
        exit(1);
    }
}

static int alsa_run_out(HWVoiceOut *hw, int live)
{
    ALSAVoiceOut *alsa = (ALSAVoiceOut *) hw;
    int rpos, decr;
    int samples;
    uint8_t *dst;
    struct st_sample *src;
    snd_pcm_sframes_t avail;

    if (snd_pcm_state_wrapper(alsa->handle) == SND_PCM_STATE_SUSPENDED) {
        alsa_resume(alsa->handle);
    }

    live = xc_audio_pcm_hw_get_live_out(hw);
    if (!live) {
        return 0;
    }

    avail = alsa_get_avail(alsa->handle);
    if (avail < 0) {
        dolog("Could not get number of available playback frames\n");
        return 0;
    }

    decr = audio_MIN(live, avail);
    samples = decr;
    rpos = hw->rpos;
    while (samples) {
        int left_till_end_samples = hw->samples - rpos;
        int len = audio_MIN(samples, left_till_end_samples);
        snd_pcm_sframes_t written;

        src = hw->mix_buf + rpos;
        dst = advance(alsa->pcm_buf, rpos << hw->info.shift);

        hw->clip(dst, src, len);
        while (len) {
            written = snd_pcm_writei_wrapper(alsa->handle, dst, len);

            if (written <= 0) {
                switch (written) {
                case 0:
                    if (conf.verbose) {
                        dolog("Failed to write %d frames (wrote zero)\n", len);
                    }
                    goto exit;

                case -EPIPE:
                    if (alsa_recover(alsa->handle)) {
                        alsa_logerr(written, "Failed to write %d frames\n",
                                    len);
                        goto exit;
                    }
                    if (conf.verbose) {
                        dolog("Recovering from playback xrun\n");
                    }
                    continue;
                case -ESTRPIPE:
                    alsa_resume(alsa->handle);
                    goto exit;

                case -EAGAIN:
                    goto exit;

                default:
                    alsa_logerr(written, "Failed to write %d frames to %p\n",
                                len, dst);
                    goto exit;
                }
            }

            rpos = (rpos + written) % hw->samples;
            samples -= written;
            len -= written;
            dst = advance(dst, written << hw->info.shift);
            src += written;
        }
    }

 exit:
    hw->rpos = rpos;
    return decr;
}

static void alsa_fini_out(HWVoiceOut *hw)
{
    ALSAVoiceOut *alsa = (ALSAVoiceOut *) hw;

    ldebug("alsa_fini\n");
    alsa_anal_close(&alsa->handle);

    if (alsa->pcm_buf) {
        g_free(alsa->pcm_buf);
        alsa->pcm_buf = NULL;
    }
}

static int alsa_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    ALSAVoiceOut *alsa = (ALSAVoiceOut *) hw;
    struct alsa_params_req req;
    struct alsa_params_obt obt;
    snd_pcm_t *handle;
    struct audsettings obt_as;

    req.fmt = aud_to_alsafmt(as->fmt);
    req.freq = as->freq;
    req.nchannels = as->nchannels;
    req.period_size = conf.period_size_out;
    req.buffer_size = conf.buffer_size_out;
    req.size_in_usec = conf.size_in_usec_out;
    req.override_mask = (!!conf.period_size_out_overridden)
        | ((!!conf.buffer_size_out_overridden) << 1);

    if (alsa_open(0, &req, &obt, &handle)) {
        return -1;
    }

    obt_as.freq = obt.freq;
    obt_as.nchannels = obt.nchannels;
    obt_as.fmt = obt.fmt;
    obt_as.endianness = obt.endianness;

    audio_pcm_init_info(&hw->info, &obt_as);
    hw->samples = obt.samples;

    alsa->pcm_buf = audio_calloc(AUDIO_FUNC, obt.samples, 1 << hw->info.shift);
    if (!alsa->pcm_buf) {
        dolog("Could not allocate DAC buffer (%d samples, each %d bytes)\n",
              hw->samples, 1 << hw->info.shift);
        alsa_anal_close(&handle);
        return -1;
    }

    alsa->handle = handle;
    return 0;
}

static int alsa_voice_ctl(snd_pcm_t *handle, const char *typ, int pause)
{
    int err;

    if (pause) {
        err = snd_pcm_drop_wrapper(handle);
        if (err < 0) {
            alsa_logerr(err, "Could not stop %s\n", typ);
            return -1;
        }
    } else {
        err = snd_pcm_prepare_wrapper(handle);
        if (err < 0) {
            alsa_logerr(err, "Could not prepare handle for %s\n", typ);
            return -1;
        }
    }

    return 0;
}

static int alsa_volume(int rvol, int lvol, int mute)
{
    int *v4v_buf = (void*)ah->io_buf;
    int rc;

    v4v_buf[0] = AUDIO_VOLUME;
    v4v_buf[1] = rvol;
    v4v_buf[2] = lvol;
    v4v_buf[3] = mute;

    rc = v4v_sendto(ah->fd, ah->io_buf, 4 * sizeof (int), 0, &ah->remote_addr);
    if (rc != (4 * sizeof (int))) {
        return -errno;
    }
    return 0;
}


static int alsa_ctl_out(HWVoiceOut *hw, int cmd, ...)
{
    ALSAVoiceOut *alsa = (ALSAVoiceOut *) hw;

    switch (cmd) {
    case VOICE_ENABLE:
        ldebug("enabling voice\n");
        return alsa_voice_ctl(alsa->handle, "playback", 0);

    case VOICE_DISABLE:
        ldebug("disabling voice\n");
        return alsa_voice_ctl(alsa->handle, "playback", 1);
    case VOICE_VOLUME:
        {
            va_list ap;
            SWVoiceOut *sw;

            va_start(ap, cmd);
            sw = va_arg(ap, SWVoiceOut*);
            va_end(ap);
            ldebug("change volume level\n");
            return alsa_volume(sw->vol.r, sw->vol.l, sw->vol.mute);
        }
    }
    return -1;
}

static int alsa_init_in(HWVoiceIn *hw, struct audsettings *as)
{
    ALSAVoiceIn *alsa = (ALSAVoiceIn *) hw;
    struct alsa_params_req req;
    struct alsa_params_obt obt;
    snd_pcm_t *handle;
    struct audsettings obt_as;

    req.fmt = aud_to_alsafmt(as->fmt);
    req.freq = as->freq;
    req.nchannels = as->nchannels;
    req.period_size = conf.period_size_in;
    req.buffer_size = conf.buffer_size_in;
    req.size_in_usec = conf.size_in_usec_in;
    req.override_mask = (!!conf.period_size_in_overridden)
        | ((!!conf.buffer_size_in_overridden) << 1);

    if (alsa_open(1, &req, &obt, &handle)) {
        return -1;
    }

    obt_as.freq = obt.freq;
    obt_as.nchannels = obt.nchannels;
    obt_as.fmt = obt.fmt;
    obt_as.endianness = obt.endianness;

    audio_pcm_init_info(&hw->info, &obt_as);
    hw->samples = obt.samples;

    alsa->pcm_buf = audio_calloc(AUDIO_FUNC, hw->samples, 1 << hw->info.shift);
    if (!alsa->pcm_buf) {
        dolog("Could not allocate ADC buffer (%d samples, each %d bytes)\n",
              hw->samples, 1 << hw->info.shift);
        alsa_anal_close(&handle);
        return -1;
    }

    alsa->handle = handle;
    return 0;
}

static void alsa_fini_in(HWVoiceIn *hw)
{
    ALSAVoiceIn *alsa = (ALSAVoiceIn *) hw;

    alsa_anal_close(&alsa->handle);

    if (alsa->pcm_buf) {
        g_free(alsa->pcm_buf);
        alsa->pcm_buf = NULL;
    }
}

static int alsa_run_in(HWVoiceIn *hw)
{
    ALSAVoiceIn *alsa = (ALSAVoiceIn *) hw;
    int hwshift = hw->info.shift;
    int i;
    int live = audio_pcm_hw_get_live_in(hw);
    int dead = hw->samples - live;
    int decr;
    struct {
        int add;
        int len;
    } bufs[2] = {
        { hw->wpos, 0 },
        { 0, 0 }
    };
    snd_pcm_sframes_t avail;
    snd_pcm_uframes_t read_samples = 0;

    if (!dead) {
        return 0;
    }

    avail = alsa_get_avail(alsa->handle);
    if (avail < 0) {
        dolog("Could not get number of captured frames\n");
        return 0;
    }

    if (!avail &&
        (snd_pcm_state_wrapper(alsa->handle) == SND_PCM_STATE_PREPARED)) {
        avail = hw->samples;
    }

    decr = audio_MIN(dead, avail);
    if (!decr) {
        return 0;
    }

    if (hw->wpos + decr > hw->samples) {
        bufs[0].len = (hw->samples - hw->wpos);
        bufs[1].len = (decr - (hw->samples - hw->wpos));
    } else {
        bufs[0].len = decr;
    }

    for (i = 0; i < 2; ++i) {
        void *src;
        struct st_sample *dst;
        snd_pcm_sframes_t nread;
        snd_pcm_uframes_t len;

        len = bufs[i].len;

        src = advance(alsa->pcm_buf, bufs[i].add << hwshift);
        dst = hw->conv_buf + bufs[i].add;

        while (len) {
            nread = snd_pcm_readi_wrapper(alsa->handle, src, len);

            if (nread <= 0) {
                switch (nread) {
                case 0:
                    if (conf.verbose) {
                        dolog("Failed to read %ld frames (read zero)\n", len);
                    }
                    goto exit;

                case -EPIPE:
                    if (alsa_recover(alsa->handle)) {
                        alsa_logerr(nread, "Failed to read %ld frames\n", len);
                        goto exit;
                    }
                    if (conf.verbose) {
                        dolog("Recovering from capture xrun\n");
                    }
                    continue;

                case -EAGAIN:
                    goto exit;

                default:
                    alsa_logerr(nread, "Failed to read %ld frames from %p\n",
                                len, src);
                    goto exit;
                }
            }

            hw->conv(dst, src, nread);

            src = advance(src, nread << hwshift);
            dst += nread;

            read_samples += nread;
            len -= nread;
        }
    }

 exit:
    hw->wpos = (hw->wpos + read_samples) % hw->samples;
    return read_samples;
}

static int alsa_read(SWVoiceIn *sw, void *buf, int size)
{
    return audio_pcm_sw_read(sw, buf, size);
}

static int alsa_ctl_in(HWVoiceIn *hw, int cmd, ...)
{
    ALSAVoiceIn *alsa = (ALSAVoiceIn *) hw;

    switch (cmd) {
    case VOICE_ENABLE:
        ldebug("enabling voice\n");
        return alsa_voice_ctl(alsa->handle, "capture", 0);

    case VOICE_DISABLE:
        ldebug("disabling voice\n");
        return alsa_voice_ctl(alsa->handle, "capture", 1);
    }

    return -1;
}

static void send_conf(void)
{
    uint8_t *v4v_buf = ah->io_buf;

    v4v_buf[0] = AUDIO_INIT;
    v4v_buf += 1;

    memcpy(v4v_buf, &conf, sizeof(conf));
    v4v_buf += sizeof(conf);

    /* memcpy(v4v_buf, conf.pcm_name_in, strlen(conf.pcm_name_in) + 1); */
    /* v4v_buf += (strlen(conf.pcm_name_in) + 1); */

    /* memcpy(v4v_buf, conf.pcm_name_out, strlen(conf.pcm_name_out) + 1); */
    /* v4v_buf += (strlen(conf.pcm_name_out) + 1); */

    /* memcpy(v4v_buf, conf.volume_control, strlen(conf.volume_control) + 1); */
    /* v4v_buf += (strlen(conf.volume_control) + 1); */

    v4v_sendto(ah->fd, ah->io_buf, v4v_buf - ah->io_buf,
               0, &ah->remote_addr);
}

static void *alsa_audio_init(void)
{
    audio_helper_open();
    send_conf();
    return &conf;
}

static void alsa_audio_fini(void *opaque)
{
    (void) opaque;
}

static struct audio_option alsa_options[] = {
    {
        .name        = "DAC_SIZE_IN_USEC",
        .tag         = AUD_OPT_BOOL,
        .valp        = &conf.size_in_usec_out,
        .descr       = "DAC period/buffer size in microseconds "
                       "(otherwise in frames)"
    },
    {
        .name        = "DAC_PERIOD_SIZE",
        .tag         = AUD_OPT_INT,
        .valp        = &conf.period_size_out,
        .descr       = "DAC period size (0 to go with system default)",
        .overriddenp = &conf.period_size_out_overridden
    },
    {
        .name        = "DAC_BUFFER_SIZE",
        .tag         = AUD_OPT_INT,
        .valp        = &conf.buffer_size_out,
        .descr       = "DAC buffer size (0 to go with system default)",
        .overriddenp = &conf.buffer_size_out_overridden
    },
    {
        .name        = "ADC_SIZE_IN_USEC",
        .tag         = AUD_OPT_BOOL,
        .valp        = &conf.size_in_usec_in,
        .descr       =
        "ADC period/buffer size in microseconds (otherwise in frames)"
    },
    {
        .name        = "ADC_PERIOD_SIZE",
        .tag         = AUD_OPT_INT,
        .valp        = &conf.period_size_in,
        .descr       = "ADC period size (0 to go with system default)",
        .overriddenp = &conf.period_size_in_overridden
    },
    {
        .name        = "ADC_BUFFER_SIZE",
        .tag         = AUD_OPT_INT,
        .valp        = &conf.buffer_size_in,
        .descr       = "ADC buffer size (0 to go with system default)",
        .overriddenp = &conf.buffer_size_in_overridden
    },
    {
        .name        = "THRESHOLD",
        .tag         = AUD_OPT_INT,
        .valp        = &conf.threshold,
        .descr       = "(undocumented)"
    },
    {
        .name        = "DAC_DEV",
        .tag         = AUD_OPT_STR,
        .valp        = &conf.pcm_name_out,
        .descr       = "DAC device name (for instance dmix)"
    },
    {
        .name        = "ADC_DEV",
        .tag         = AUD_OPT_STR,
        .valp        = &conf.pcm_name_in,
        .descr       = "ADC device name"
    },
    {
        .name        = "VERBOSE",
        .tag         = AUD_OPT_BOOL,
        .valp        = &conf.verbose,
        .descr       = "Behave in a more verbose way"
    },
    {
        .name        = "VOL_CTRL",
        .tag         = AUD_OPT_STR,
        .valp        = &conf.volume_control,
        .descr       = "Volume control voice name"
    },
    { /* End of list */ }
};

static struct audio_pcm_ops alsa_pcm_ops = {
    .init_out = alsa_init_out,
    .fini_out = alsa_fini_out,
    .run_out  = alsa_run_out,
    .write    = alsa_write,
    .ctl_out  = alsa_ctl_out,

    .init_in  = alsa_init_in,
    .fini_in  = alsa_fini_in,
    .run_in   = alsa_run_in,
    .read     = alsa_read,
    .ctl_in   = alsa_ctl_in,
};

struct audio_driver xen_alsa_audio_driver = {
    .name           = "xen-alsa",
    .descr          = "ALSA forwarding with V4V for XenClient XT",
    .options        = alsa_options,
    .init           = alsa_audio_init,
    .fini           = alsa_audio_fini,
    .pcm_ops        = &alsa_pcm_ops,
    .can_be_default = 1,
    .max_voices_out = INT_MAX,
    .max_voices_in  = INT_MAX,
    .voice_size_out = sizeof(ALSAVoiceOut),
    .voice_size_in  = sizeof(ALSAVoiceIn),
};
