#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SPECS_STEREO 2

typedef struct {
  void *start;
  size_t length;
} buffer_t;

typedef struct {
  int fd;
  const char *dev;
  int width;
  int height;
  struct v4l2_format *fmt;
  struct v4l2_requestbuffers *req;
  buffer_t **buffers;
  enum v4l2_buf_type *type;
  SDL_Window **win;
  SDL_Renderer **ren;
  SDL_Texture **tex;
  size_t *rgb_size;
  uint8_t **rgb;
  int *running; // shared running flag
  SDL_Event *e;
  fd_set *fds;
  struct timeval *tv;
  int *r;
  struct v4l2_buffer *buf;
} proc_video_args_t;

typedef struct {
  const char *dev; // recording device selector (substring or index string)
  int sample_rate;
  int out;      // playback device index, -1 = default
  int *running; // shared running flag
} proc_audio_args_t;

static volatile sig_atomic_t g_stop = 0;

static void signalHandler(int sig) {
  (void)sig;
  g_stop = 1;
}

static int xioctl(int fd, unsigned long req, void *arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

static inline uint8_t clamp_u8(int x) {
  if (x < 0)
    return 0;
  if (x > 255)
    return 255;
  return (uint8_t)x;
}

// Convert one YUYV frame to RGB24
static void yuyv_to_rgb24(const uint8_t *yuyv, uint8_t *rgb, int w, int h) {
  int npix = w * h;
  for (int i = 0, j = 0; i < npix; i += 2, j += 4) {
    int y0 = yuyv[j + 0];
    int u = yuyv[j + 1] - 128;
    int y1 = yuyv[j + 2];
    int v = yuyv[j + 3] - 128;

    int c0 = y0 - 16;
    int c1 = y1 - 16;

    int r0 = (298 * c0 + 409 * v + 128) >> 8;
    int g0 = (298 * c0 - 100 * u - 208 * v + 128) >> 8;
    int b0 = (298 * c0 + 516 * u + 128) >> 8;

    int r1 = (298 * c1 + 409 * v + 128) >> 8;
    int g1 = (298 * c1 - 100 * u - 208 * v + 128) >> 8;
    int b1 = (298 * c1 + 516 * u + 128) >> 8;

    int out0 = i * 3;
    rgb[out0 + 0] = clamp_u8(r0);
    rgb[out0 + 1] = clamp_u8(g0);
    rgb[out0 + 2] = clamp_u8(b0);

    int out1 = (i + 1) * 3;
    rgb[out1 + 0] = clamp_u8(r1);
    rgb[out1 + 1] = clamp_u8(g1);
    rgb[out1 + 2] = clamp_u8(b1);
  }
}

static SDL_AudioDeviceID pick_recording_device(const char *selector) {
  int count = 0;
  SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&count);

  for (int i = 0; devices && i < count; i++) {
    const char *name = SDL_GetAudioDeviceName(devices[i]);
    printf("recording[%d]=%s\n", i, name ? name : "(unknown)");
  }

  SDL_AudioDeviceID chosen = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;

  if (selector && selector[0] != '\0') {
    char *end = NULL;
    long idx = strtol(selector, &end, 10);
    if (end && *end == '\0') {
      if (devices && idx >= 0 && idx < count) {
        chosen = devices[idx];
      } else {
        fprintf(stderr,
                "Requested recording index %ld unavailable, using default.\n",
                idx);
      }
    } else if (devices) {
      int matched = 0;
      for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(devices[i]);
        if (name && strstr(name, selector)) {
          chosen = devices[i];
          matched = 1;
          printf("Selected recording device: %s (id=%d)\n", name, (int)chosen);
          break;
        }
      }
      if (!matched) {
        fprintf(stderr,
                "Requested recording \"%s\" not found, using default.\n",
                selector);
      }
    }
  }

  if (devices)
    SDL_free(devices);
  return chosen;
}

static SDL_AudioDeviceID pick_playback_device(int out_index) {
  int count = 0;
  SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);

  for (int i = 0; devices && i < count; i++) {
    const char *name = SDL_GetAudioDeviceName(devices[i]);
    printf("sink[%d]=%s\n", i, name ? name : "(unknown)");
  }

  SDL_AudioDeviceID chosen = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

  if (out_index >= 0) {
    if (devices && out_index < count) {
      chosen = devices[out_index];
      const char *name = SDL_GetAudioDeviceName(chosen);
      printf("Selected sink device: %s (id=%d)\n", name ? name : "(unknown)",
             (int)chosen);
    } else {
      fprintf(stderr, "Requested sink index %d unavailable, using default.\n",
              out_index);
    }
  }

  if (devices)
    SDL_free(devices);
  return chosen;
}

// Add near the top (after includes), SDL3 integer-scaling helper:
static SDL_FRect integer_fit_rect(int src_w, int src_h, int dst_w, int dst_h) {
  int sx = dst_w / src_w;
  int sy = dst_h / src_h;
  int s = (sx < sy) ? sx : sy;
  if (s < 1)
    s = 1;

  SDL_FRect r;
  r.w = (float)(src_w * s);
  r.h = (float)(src_h * s);
  r.x = (float)(dst_w - (int)r.w) * 0.5f;
  r.y = (float)(dst_h - (int)r.h) * 0.5f;
  return r;
}

int proc_audio(const proc_audio_args_t *args) {
  int rc = 1;

  SDL_AudioStream *rec_stream = NULL;
  SDL_AudioStream *out_stream = NULL;
  SDL_AudioDeviceID rec_dev = 0;
  SDL_AudioDeviceID out_dev = 0;

  SDL_AudioDeviceID rec_id = pick_recording_device(args->dev);
  SDL_AudioDeviceID out_id = pick_playback_device(args->out);

  SDL_AudioSpec want;
  SDL_zero(want);
  want.freq = args->sample_rate;
  want.format = SDL_AUDIO_S16;
  want.channels = 2;

  rec_dev = SDL_OpenAudioDevice(rec_id, &want);
  if (!rec_dev) {
    fprintf(stderr, "Open recording failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  out_dev = SDL_OpenAudioDevice(out_id, &want);
  if (!out_dev) {
    fprintf(stderr, "Open playback failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  // App-side format: what we read/write in the loop.
  SDL_AudioSpec appspec = want;

  // recording: device -> stream -> app
  rec_stream = SDL_CreateAudioStream(NULL, &appspec);
  if (!rec_stream || !SDL_BindAudioStream(rec_dev, rec_stream)) {
    fprintf(stderr, "Bind rec_stream failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  // playback: app -> stream -> device
  out_stream = SDL_CreateAudioStream(&appspec, NULL);
  if (!out_stream || !SDL_BindAudioStream(out_dev, out_stream)) {
    fprintf(stderr, "Bind out_stream failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  SDL_ResumeAudioDevice(rec_dev);
  SDL_ResumeAudioDevice(out_dev);

  Uint8 buf[4096];

  while (*args->running && !g_stop) {
    int avail = SDL_GetAudioStreamAvailable(rec_stream);
    if (avail <= 0) {
      SDL_Delay(1);
      continue;
    }

    if (avail > (int)sizeof(buf))
      avail = (int)sizeof(buf);

    int got = SDL_GetAudioStreamData(rec_stream, buf, avail);
    if (got < 0) {
      fprintf(stderr, "GetAudioStreamData: %s\n", SDL_GetError());
      break;
    }

    if (got > 0) {
      if (!SDL_PutAudioStreamData(out_stream, buf, got)) {
        fprintf(stderr, "PutAudioStreamData: %s\n", SDL_GetError());
        break;
      }
    }
  }

  rc = 0;

cleanup:
  if (rec_stream)
    SDL_DestroyAudioStream(rec_stream);
  if (out_stream)
    SDL_DestroyAudioStream(out_stream);
  if (out_dev)
    SDL_CloseAudioDevice(out_dev);
  if (rec_dev)
    SDL_CloseAudioDevice(rec_dev);
  return rc;
}

int proc_video(const proc_video_args_t *args) {
  uint32_t i;

  memset(args->fmt, 0, sizeof(*args->fmt));
  args->fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  args->fmt->fmt.pix.width = args->width;
  args->fmt->fmt.pix.height = args->height;
  args->fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  args->fmt->fmt.pix.field = V4L2_FIELD_NONE;

  if (xioctl(args->fd, VIDIOC_S_FMT, args->fmt) < 0) {
    fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
    return 1;
  }

  memset(args->req, 0, sizeof(*args->req));
  args->req->count = 4;
  args->req->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  args->req->memory = V4L2_MEMORY_MMAP;

  if (xioctl(args->fd, VIDIOC_REQBUFS, args->req) < 0 || args->req->count < 2) {
    fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
    return 1;
  }

  *args->buffers = calloc(args->req->count, sizeof(buffer_t));
  if (!*args->buffers) {
    perror("calloc(buffers)");
    return 1;
  }

  for (i = 0; i < args->req->count; i++) {
    memset(args->buf, 0, sizeof(*args->buf));
    args->buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    args->buf->memory = V4L2_MEMORY_MMAP;
    args->buf->index = i;

    if (xioctl(args->fd, VIDIOC_QUERYBUF, args->buf) < 0) {
      fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
      return 1;
    }

    (*args->buffers)[i].length = args->buf->length;
    (*args->buffers)[i].start =
        mmap(NULL, args->buf->length, PROT_READ | PROT_WRITE, MAP_SHARED,
             args->fd, args->buf->m.offset);
    if ((*args->buffers)[i].start == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", strerror(errno));
      return 1;
    }
  }

  for (i = 0; i < args->req->count; i++) {
    memset(args->buf, 0, sizeof(*args->buf));
    args->buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    args->buf->memory = V4L2_MEMORY_MMAP;
    args->buf->index = i;

    if (xioctl(args->fd, VIDIOC_QBUF, args->buf) < 0) {
      fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
      return 1;
    }
  }

  *args->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(args->fd, VIDIOC_STREAMON, args->type) < 0) {
    fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
    return 1;
  }

  *args->win = SDL_CreateWindow(args->dev, args->fmt->fmt.pix.width,
                                args->fmt->fmt.pix.height, 0);
  if (!*args->win) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetWindowResizable(*args->win, 1);

  *args->ren = SDL_CreateRenderer(*args->win, NULL);
  if (!*args->ren) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return 1;
  }

  *args->tex = SDL_CreateTexture(
      *args->ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
      args->fmt->fmt.pix.width, args->fmt->fmt.pix.height);
  if (!*args->tex) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetTextureScaleMode(*args->tex, SDL_SCALEMODE_NEAREST);

  *args->rgb_size =
      (size_t)args->fmt->fmt.pix.width * (size_t)args->fmt->fmt.pix.height * 3;
  *args->rgb = malloc(*args->rgb_size);
  if (!*args->rgb) {
    perror("malloc(rgb)");
    return 1;
  }

  while (*args->running && !g_stop) {
    while (SDL_PollEvent(args->e)) {
      if (args->e->type == SDL_EVENT_QUIT)
        *args->running = 0;
      if (args->e->type == SDL_EVENT_KEY_DOWN &&
          args->e->key.key == SDLK_ESCAPE)
        *args->running = 0;
    }

    FD_ZERO(args->fds);
    FD_SET(args->fd, args->fds);
    args->tv->tv_sec = 2;
    args->tv->tv_usec = 0;

    *args->r = select(args->fd + 1, args->fds, NULL, NULL, args->tv);
    if (*args->r < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "select failed: %s\n", strerror(errno));
      break;
    }
    if (*args->r == 0)
      continue;

    memset(args->buf, 0, sizeof(*args->buf));
    args->buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    args->buf->memory = V4L2_MEMORY_MMAP;

    if (xioctl(args->fd, VIDIOC_DQBUF, args->buf) < 0) {
      if (errno == EAGAIN)
        continue;
      fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
      break;
    }

    // Convert + draw
    yuyv_to_rgb24((const uint8_t *)(*args->buffers)[args->buf->index].start,
                  *args->rgb, args->fmt->fmt.pix.width,
                  args->fmt->fmt.pix.height);

    // Upload frame
    SDL_UpdateTexture(*args->tex, NULL, *args->rgb,
                      args->fmt->fmt.pix.width * 3);

    // Integer scaling: render into a centered integer-multiple rect
    int out_w = 0, out_h = 0;
    SDL_GetRenderOutputSize(*args->ren, &out_w, &out_h);

    SDL_FRect dst = integer_fit_rect(args->fmt->fmt.pix.width,
                                     args->fmt->fmt.pix.height, out_w, out_h);

    SDL_RenderClear(*args->ren);
    SDL_RenderTexture(*args->ren, *args->tex, NULL, &dst);
    SDL_RenderPresent(*args->ren);

    if (xioctl(args->fd, VIDIOC_QBUF, args->buf) < 0) {
      fprintf(stderr, "VIDIOC_QBUF (requeue) failed: %s\n", strerror(errno));
      break;
    }
  }

  // ensure other thread exits too
  *args->running = 0;
  return 0;
}

static void *proc_video_thread(void *arg) {
  return (void *)(intptr_t)proc_video((proc_video_args_t *)arg);
}
static void *proc_audio_thread(void *arg) {
  return (void *)(intptr_t)proc_audio((proc_audio_args_t *)arg);
}

int main(int argc, char **argv) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  int a = 1;
  int width = (argc > ++a) ? atoi(argv[a]) : 640;
  int height = (argc > ++a) ? atoi(argv[a]) : 480;
  const char *video_dev = (argc > ++a) ? argv[a] : "/dev/video0";
  const char *audio_sel =
      (argc > ++a) ? argv[a] : "USB3. 0 capture Stereo analogico";
  int out_idx = -1; // sink index; -1 default

  int fdv = open(video_dev, O_RDWR | O_NONBLOCK, 0);
  if (fdv < 0) {
    fprintf(stderr, "open(%s) failed: %s\n", video_dev, strerror(errno));
    return 1;
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    close(fdv);
    return 1;
  }

  // shared state
  int running = 1;

  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  buffer_t *buffers = NULL;
  enum v4l2_buf_type type;
  SDL_Window *win = NULL;
  SDL_Renderer *ren = NULL;
  SDL_Texture *tex = NULL;
  size_t rgb_size = 0;
  uint8_t *rgb = NULL;
  SDL_Event e;
  fd_set fds;
  struct timeval tv;
  int r = 0;
  struct v4l2_buffer buf;

  proc_video_args_t video_args = {
      .fd = fdv,
      .dev = video_dev,
      .width = width,
      .height = height,
      .fmt = &fmt,
      .req = &req,
      .buffers = &buffers,
      .type = &type,
      .win = &win,
      .ren = &ren,
      .tex = &tex,
      .rgb_size = &rgb_size,
      .rgb = &rgb,
      .running = &running,
      .e = &e,
      .fds = &fds,
      .tv = &tv,
      .r = &r,
      .buf = &buf,
  };

  proc_audio_args_t audio_args = {
      .dev = audio_sel,
      .sample_rate = 44100,
      .out = out_idx,
      .running = &running,
  };

  pthread_t video_thread, audio_thread;

  if (pthread_create(&video_thread, NULL, proc_video_thread, &video_args) !=
      0) {
    fprintf(stderr, "pthread_create(video) failed\n");
    running = 0;
    SDL_Quit();
    close(fdv);
    return 1;
  }

  if (pthread_create(&audio_thread, NULL, proc_audio_thread, &audio_args) !=
      0) {
    fprintf(stderr, "pthread_create(audio) failed\n");
    running = 0;
    pthread_join(video_thread, NULL);
    SDL_Quit();
    close(fdv);
    return 1;
  }

  pthread_join(video_thread, NULL);
  running = 0; // in case video exits first
  pthread_join(audio_thread, NULL);

  // Cleanup V4L2 + SDL video objects (created in video thread)
  xioctl(fdv, VIDIOC_STREAMOFF, &type);

  if (buffers) {
    for (uint32_t i = 0; i < req.count; i++) {
      munmap(buffers[i].start, buffers[i].length);
    }
    free(buffers);
  }
  free(rgb);

  if (tex)
    SDL_DestroyTexture(tex);
  if (ren)
    SDL_DestroyRenderer(ren);
  if (win)
    SDL_DestroyWindow(win);

  SDL_Quit();
  close(fdv);
  return 0;
}
