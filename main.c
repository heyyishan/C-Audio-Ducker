#include "webrtc_vad.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

enum {
  SAMPLE_RATE = 16000,
  BITS_PER_SAMPLE = 16,
  FRAME_DURATION_MS = 20,
  FRAME_SAMPLES = (SAMPLE_RATE * FRAME_DURATION_MS) / 1000, /* 320 */
};

enum {
  DEFAULT_ONSET_FRAMES = 3,
  DEFAULT_OFFSET_FRAMES = 25,
};

static const double DEFAULT_ENERGY_THRESHOLD = 800.0;
static const double NOISE_FLOOR_ALPHA = 0.001;
static const double DEFAULT_GATE_RATIO = 6.0;

enum {
  VOLUME_UPDATE_INTERVAL_MS = 40,
  VAD_MODE = 3,
};

enum {
  TUI_REFRESH_INTERVAL_MS = 50,
  TUI_METER_WIDTH = 50,
  TUI_HISTORY_LEN = 60,
  TUI_LOG_LINES = 8,
  TUI_LOG_LINE_LEN = 120,
};

/* TUI color pairs */
enum {
  CLR_NORMAL = 1,
  CLR_TITLE,
  CLR_BORDER,
  CLR_STATE_IDLE,
  CLR_STATE_DUCKING,
  CLR_STATE_DUCKED,
  CLR_STATE_RESTORING,
  CLR_METER_LOW,
  CLR_METER_MID,
  CLR_METER_HIGH,
  CLR_METER_CLIP,
  CLR_GATE_LINE,
  CLR_NOISE_FLOOR,
  CLR_VAD_ON,
  CLR_VAD_OFF,
  CLR_VOLUME,
  CLR_HELP,
  CLR_INPUT_ACTIVE,
  CLR_WAVEFORM,
  CLR_HEADER,
  CLR_DIM,
  CLR_SPEECH_IND,
};

typedef enum {
  STATE_IDLE,
  STATE_DUCKING,
  STATE_DUCKED,
  STATE_RESTORING,
} duck_state_t;

static const char *state_name(duck_state_t s) {
  switch (s) {
  case STATE_IDLE:
    return "IDLE";
  case STATE_DUCKING:
    return "DUCKING";
  case STATE_DUCKED:
    return "DUCKED";
  case STATE_RESTORING:
    return "RESTORING";
  }
  return "UNKNOWN";
}

typedef struct {
  const char *capture_device;
  const char *sink_id;
  double ducked_volume;
  double normal_volume;
  double volume_step;
  int onset_frames;
  int offset_frames;
  double energy_threshold;
  double gate_ratio;
  bool adaptive_gate;
  bool verbose;
  bool daemonize;
  bool calibrate;
  bool tui_mode;
} config_t;

static config_t g_config = {
    .capture_device = "default",
    .sink_id = "@DEFAULT_AUDIO_SINK@",
    .ducked_volume = 0.10,
    .normal_volume = 1.00,
    .volume_step = 0.03,
    .onset_frames = DEFAULT_ONSET_FRAMES,
    .offset_frames = DEFAULT_OFFSET_FRAMES,
    .energy_threshold = DEFAULT_ENERGY_THRESHOLD,
    .gate_ratio = DEFAULT_GATE_RATIO,
    .adaptive_gate = true,
    .verbose = false,
    .daemonize = false,
    .calibrate = false,
    .tui_mode = true,
};

typedef enum {
  TUI_FIELD_NONE = 0,
  TUI_FIELD_ENERGY_THRESHOLD,
  TUI_FIELD_GATE_RATIO,
  TUI_FIELD_ONSET,
  TUI_FIELD_OFFSET,
  TUI_FIELD_DUCK_LEVEL,
  TUI_FIELD_NORMAL_LEVEL,
  TUI_FIELD_VOLUME_STEP,
  TUI_FIELD_COUNT
} tui_field_t;

typedef struct {
  double rms;
  double rms_db;
  double peak_rms;
  double peak_rms_db;
  double noise_floor;
  double noise_floor_db;
  double effective_threshold;
  double effective_threshold_db;
  double current_volume;
  duck_state_t state;
  bool is_speech;
  bool energy_above_gate;
  int speech_counter;
  int silence_counter;
  int frame_count;
  int speech_detect_count;
  double rms_history[TUI_HISTORY_LEN];
  int rms_history_idx;

  double peak_hold;
  int64_t peak_hold_time;

  tui_field_t selected_field;
  bool editing;
  char edit_buf[32];
  int edit_pos;

  char log_lines[TUI_LOG_LINES][TUI_LOG_LINE_LEN];
  int log_head;
  int log_count;
  pthread_mutex_t mutex;

  bool needs_refresh;
} tui_state_t;

static tui_state_t g_tui = {
    .rms = 0,
    .peak_rms = 0,
    .noise_floor = 500.0,
    .current_volume = 1.0,
    .state = STATE_IDLE,
    .selected_field = TUI_FIELD_NONE,
    .editing = false,
    .edit_pos = 0,
    .log_head = 0,
    .log_count = 0,
    .rms_history_idx = 0,
    .peak_hold = 0,
    .needs_refresh = true,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static volatile sig_atomic_t g_running = 1;
static int g_capture_channels = 1;
static pid_t g_volume_child = -1;
static int64_t g_last_volume_update_ms = 0;
static double g_current_volume = 1.0;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double rms_to_dbfs(double rms) {
  if (rms < 1.0)
    return -96.0;
  double db = 20.0 * log10(rms / 32768.0);
  if (db < -96.0)
    db = -96.0;
  return db;
}

static void tui_log(const char *fmt, ...) {
  pthread_mutex_lock(&g_tui.mutex);

  va_list ap;
  va_start(ap, fmt);

  int idx = (g_tui.log_head + g_tui.log_count) % TUI_LOG_LINES;
  if (g_tui.log_count >= TUI_LOG_LINES) {
    g_tui.log_head = (g_tui.log_head + 1) % TUI_LOG_LINES;
  } else {
    g_tui.log_count++;
  }

  vsnprintf(g_tui.log_lines[idx], TUI_LOG_LINE_LEN, fmt, ap);
  va_end(ap);

  g_tui.needs_refresh = true;
  pthread_mutex_unlock(&g_tui.mutex);
}

static double compute_rms(const int16_t *samples, int count) {
  int64_t sum_sq = 0;
  for (int i = 0; i < count; i++) {
    int64_t s = samples[i];
    sum_sq += s * s;
  }
  return sqrt((double)sum_sq / count);
}

static void reap_volume_child(void) {
  if (g_volume_child > 0) {
    int status;
    pid_t result = waitpid(g_volume_child, &status, WNOHANG);
    if (result == g_volume_child || result == -1) {
      g_volume_child = -1;
    }
  }
}

static bool set_volume(const char *sink_id, double volume) {
  int64_t now = now_ms();
  if (now - g_last_volume_update_ms < VOLUME_UPDATE_INTERVAL_MS)
    return false;

  if (g_volume_child > 0) {
    reap_volume_child();
    if (g_volume_child > 0)
      return false;
  }

  if (volume < 0.0)
    volume = 0.0;
  if (volume > 1.5)
    volume = 1.5;

  char vol_str[16];
  snprintf(vol_str, sizeof(vol_str), "%.2f", volume);

  g_last_volume_update_ms = now;
  g_current_volume = volume;

  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("wpctl", "wpctl", "set-volume", sink_id, vol_str, (char *)NULL);
    _exit(127);
  } else if (pid > 0) {
    g_volume_child = pid;
    return true;
  } else {
    return false;
  }
}

static snd_pcm_t *setup_alsa_capture(const char *device) {
  snd_pcm_t *pcm = NULL;
  int err;

  err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    fprintf(stderr, "ALSA: cannot open '%s': %s\n", device, snd_strerror(err));
    return NULL;
  }

  snd_pcm_hw_params_t *params;
  snd_pcm_hw_params_alloca(&params);
  snd_pcm_hw_params_any(pcm, params);

  snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);

  g_capture_channels = 1;
  err = snd_pcm_hw_params_set_channels(pcm, params, 1);
  if (err < 0) {
    g_capture_channels = 2;
    err = snd_pcm_hw_params_set_channels(pcm, params, 2);
    if (err < 0) {
      goto fail;
    }
  }

  unsigned int rate = SAMPLE_RATE;
  snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0);

  snd_pcm_uframes_t period_size = FRAME_SAMPLES;
  snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, 0);

  snd_pcm_uframes_t buffer_size = period_size * 4;
  snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_size);

  err = snd_pcm_hw_params(pcm, params);
  if (err < 0) {
    goto fail;
  }

  snd_pcm_prepare(pcm);
  return pcm;

fail:
  snd_pcm_close(pcm);
  return NULL;
}

static int recover_alsa(snd_pcm_t *pcm, int err) {
  if (err == -EPIPE) {
    return snd_pcm_prepare(pcm);
  }
  if (err == -ESTRPIPE) {
    while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
      usleep(100000);
    if (err < 0)
      return snd_pcm_prepare(pcm);
    return 0;
  }
  return err;
}

// vad setup
static VadInst *setup_vad(void) {
  VadInst *vad = WebRtcVad_Create();
  if (!vad)
    return NULL;
  if (WebRtcVad_Init(vad) != 0) {
    WebRtcVad_Free(vad);
    return NULL;
  }
  if (WebRtcVad_set_mode(vad, VAD_MODE) != 0) {
    WebRtcVad_Free(vad);
    return NULL;
  }
  return vad;
}

// tui
static void draw_box(int y, int x, int h, int w, int color_pair,
                     const char *title) {
  attron(COLOR_PAIR(color_pair));

  mvaddch(y, x, ACS_ULCORNER);
  mvaddch(y, x + w - 1, ACS_URCORNER);
  mvaddch(y + h - 1, x, ACS_LLCORNER);
  mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

  for (int i = 1; i < w - 1; i++) {
    mvaddch(y, x + i, ACS_HLINE);
    mvaddch(y + h - 1, x + i, ACS_HLINE);
  }

  for (int i = 1; i < h - 1; i++) {
    mvaddch(y + i, x, ACS_VLINE);
    mvaddch(y + i, x + w - 1, ACS_VLINE);
  }

  if (title && title[0] != '\0') {
    int title_x = x + 2;
    mvaddch(y, title_x - 1, ACS_RTEE);
    attron(A_BOLD | COLOR_PAIR(CLR_TITLE));
    mvprintw(y, title_x, " %s ", title);
    attroff(A_BOLD);
    attron(COLOR_PAIR(color_pair));
    mvaddch(y, title_x + (int)strlen(title) + 2, ACS_LTEE);
  }

  attroff(COLOR_PAIR(color_pair));
}

static void draw_horizontal_line(int y, int x, int w, int color_pair) {
  attron(COLOR_PAIR(color_pair));
  mvaddch(y, x, ACS_LTEE);
  for (int i = 1; i < w - 1; i++) {
    mvaddch(y, x + i, ACS_HLINE);
  }
  mvaddch(y, x + w - 1, ACS_RTEE);
  attroff(COLOR_PAIR(color_pair));
}

static void draw_db_meter(int y, int x, int width, double db_value,
                          double db_gate, double db_floor, double db_peak,
                          const char *label, bool show_markers) {
  int label_len = (int)strlen(label);
  attron(COLOR_PAIR(CLR_NORMAL));
  mvprintw(y, x, "%s", label);

  int meter_x = x + label_len + 1;
  int meter_w = width - label_len - 12;
  if (meter_w < 10)
    meter_w = 10;
  if (meter_w > TUI_METER_WIDTH)
    meter_w = TUI_METER_WIDTH;

  double db_min = -96.0;
  double db_max = 0.0;
  double db_range = db_max - db_min;

  int fill = (int)((db_value - db_min) / db_range * meter_w);
  if (fill < 0)
    fill = 0;
  if (fill > meter_w)
    fill = meter_w;

  int gate_pos = (int)((db_gate - db_min) / db_range * meter_w);
  int floor_pos = (int)((db_floor - db_min) / db_range * meter_w);
  int peak_pos = (int)((db_peak - db_min) / db_range * meter_w);

  attron(COLOR_PAIR(CLR_NORMAL));
  mvaddch(y, meter_x, '[');

  for (int i = 0; i < meter_w; i++) {
    int pos_x = meter_x + 1 + i;

    double pos_db = db_min + (double)i / meter_w * db_range;
    int clr;
    if (pos_db < -36.0)
      clr = CLR_METER_LOW;
    else if (pos_db < -12.0)
      clr = CLR_METER_MID;
    else if (pos_db < -3.0)
      clr = CLR_METER_HIGH;
    else
      clr = CLR_METER_CLIP;

    if (i < fill) {
      attron(COLOR_PAIR(clr) | A_BOLD);
      mvaddch(y, pos_x, ACS_BLOCK);
      attroff(A_BOLD);
    } else {
      bool marked = false;
      if (show_markers) {
        if (i == gate_pos && gate_pos >= 0 && gate_pos < meter_w) {
          attron(COLOR_PAIR(CLR_GATE_LINE) | A_BOLD);
          mvaddch(y, pos_x, '|');
          attroff(A_BOLD);
          marked = true;
        } else if (i == floor_pos && floor_pos >= 0 && floor_pos < meter_w) {
          attron(COLOR_PAIR(CLR_NOISE_FLOOR));
          mvaddch(y, pos_x, ':');
          marked = true;
        } else if (i == peak_pos && peak_pos >= 0 && peak_pos < meter_w) {
          attron(COLOR_PAIR(CLR_METER_HIGH) | A_BOLD);
          mvaddch(y, pos_x, '|');
          attroff(A_BOLD);
          marked = true;
        }
      }
      if (!marked) {
        attron(COLOR_PAIR(CLR_DIM));
        mvaddch(y, pos_x, ACS_BULLET);
        attroff(COLOR_PAIR(CLR_DIM));
      }
    }
  }

  attron(COLOR_PAIR(CLR_NORMAL));
  mvaddch(y, meter_x + meter_w + 1, ']');

  if (db_value <= -95.0) {
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(y, meter_x + meter_w + 3, " -inf dB");
  } else {
    int readout_clr = CLR_METER_LOW;
    if (db_value > -36.0)
      readout_clr = CLR_METER_MID;
    if (db_value > -12.0)
      readout_clr = CLR_METER_HIGH;
    if (db_value > -3.0)
      readout_clr = CLR_METER_CLIP;
    attron(COLOR_PAIR(readout_clr) | A_BOLD);
    mvprintw(y, meter_x + meter_w + 3, "%+6.1f dB", db_value);
    attroff(A_BOLD);
  }
}

static void draw_volume_bar(int y, int x, int width, double volume) {
  attron(COLOR_PAIR(CLR_NORMAL));
  mvprintw(y, x, "Volume:");

  int bar_x = x + 9;
  int bar_w = width - 20;
  if (bar_w < 10)
    bar_w = 10;

  int fill = (int)(volume / 1.5 * bar_w);
  if (fill < 0)
    fill = 0;
  if (fill > bar_w)
    fill = bar_w;

  mvaddch(y, bar_x, '[');

  for (int i = 0; i < bar_w; i++) {
    if (i < fill) {
      double pos_vol = (double)i / bar_w * 1.5;
      int clr;
      if (pos_vol < 0.3)
        clr = CLR_METER_HIGH;
      else if (pos_vol < 0.8)
        clr = CLR_METER_MID;
      else
        clr = CLR_METER_LOW;

      attron(COLOR_PAIR(clr) | A_BOLD);
      mvaddch(y, bar_x + 1 + i, ACS_BLOCK);
      attroff(A_BOLD);
    } else {
      attron(COLOR_PAIR(CLR_DIM));
      mvaddch(y, bar_x + 1 + i, ACS_BULLET);
      attroff(COLOR_PAIR(CLR_DIM));
    }
  }

  attron(COLOR_PAIR(CLR_NORMAL));
  mvaddch(y, bar_x + bar_w + 1, ']');

  int pct = (int)(volume * 100.0);
  attron(COLOR_PAIR(CLR_VOLUME) | A_BOLD);
  mvprintw(y, bar_x + bar_w + 3, "%3d%%", pct);
  attroff(A_BOLD);
}

static void draw_waveform(int y, int x, int width, int height,
                          const double *history, int hist_len, int hist_idx,
                          double gate_db) {
  double db_min = -96.0;
  double db_max = 0.0;
  double db_range = db_max - db_min;

  int graph_w = width - 2;
  if (graph_w > hist_len)
    graph_w = hist_len;

  int gate_h = (int)((gate_db - db_min) / db_range * (height - 1));
  if (gate_h < 0)
    gate_h = 0;
  if (gate_h >= height)
    gate_h = height - 1;

  for (int col = 0; col < graph_w; col++) {
    int idx = (hist_idx - graph_w + col + hist_len) % hist_len;
    double rms = history[idx];
    double db = rms_to_dbfs(rms);

    int bar_h = (int)((db - db_min) / db_range * (height - 1));
    if (bar_h < 0)
      bar_h = 0;
    if (bar_h >= height)
      bar_h = height - 1;

    for (int row = 0; row < height; row++) {
      int draw_row = y + height - 1 - row;
      int draw_col = x + 1 + col;

      if (row <= bar_h) {
        double row_db = db_min + (double)row / (height - 1) * db_range;
        int clr;
        if (row_db < -36.0)
          clr = CLR_METER_LOW;
        else if (row_db < -12.0)
          clr = CLR_METER_MID;
        else
          clr = CLR_METER_HIGH;
        attron(COLOR_PAIR(clr));
        mvaddch(draw_row, draw_col, ACS_BLOCK);
        attroff(COLOR_PAIR(clr));
      } else if (row == gate_h) {
        attron(COLOR_PAIR(CLR_GATE_LINE));
        mvaddch(draw_row, draw_col, '-');
        attroff(COLOR_PAIR(CLR_GATE_LINE));
      } else {
        mvaddch(draw_row, draw_col, ' ');
      }
    }
  }
}

static void draw_state_indicator(int y, int x, duck_state_t state,
                                 bool is_speech, bool energy_above_gate) {
  int clr;
  const char *icon;
  const char *desc;

  switch (state) {
  case STATE_IDLE:
    clr = CLR_STATE_IDLE;
    icon = "  IDLE  ";
    desc = "Monitoring...";
    break;
  case STATE_DUCKING:
    clr = CLR_STATE_DUCKING;
    icon = " DUCK! ";
    desc = "Ducking volume";
    break;
  case STATE_DUCKED:
    clr = CLR_STATE_DUCKED;
    icon = " DUCKED ";
    desc = "Volume ducked";
    break;
  case STATE_RESTORING:
    clr = CLR_STATE_RESTORING;
    icon = " RESTOR ";
    desc = "Restoring vol";
    break;
  default:
    clr = CLR_NORMAL;
    icon = " ????? ";
    desc = "Unknown";
    break;
  }

  attron(COLOR_PAIR(clr) | A_BOLD);
  mvprintw(y, x, "  State: %-12s", state_name(state));
  attroff(A_BOLD);

  attron(COLOR_PAIR(clr) | A_BOLD | A_REVERSE);
  mvprintw(y, x + 26, " %s ", icon);
  attroff(A_BOLD | A_REVERSE);

  y++;
  if (is_speech) {
    attron(COLOR_PAIR(CLR_SPEECH_IND) | A_BOLD);
    mvprintw(y, x, "  Speech: YES ***");
    attroff(A_BOLD);
  } else {
    attron(COLOR_PAIR(CLR_VAD_OFF));
    mvprintw(y, x, "  Speech: no      ");
  }

  if (energy_above_gate) {
    attron(COLOR_PAIR(CLR_GATE_LINE) | A_BOLD);
    mvprintw(y, x + 22, "Gate: OPEN  ");
  } else {
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(y, x + 22, "Gate: closed");
  }
  attroff(A_BOLD);

  y++;
  attron(COLOR_PAIR(CLR_DIM));
  mvprintw(y, x, "  %s", desc);
  attroff(COLOR_PAIR(CLR_DIM));
}

static void draw_config_panel(int y, int x, int width) {
  (void)width;

  struct {
    tui_field_t field;
    const char *label;
    char value[32];
  } fields[] = {
      {TUI_FIELD_ENERGY_THRESHOLD, "Energy Threshold", {0}},
      {TUI_FIELD_GATE_RATIO, "Gate Ratio", {0}},
      {TUI_FIELD_ONSET, "Onset Frames", {0}},
      {TUI_FIELD_OFFSET, "Offset Frames", {0}},
      {TUI_FIELD_DUCK_LEVEL, "Duck Level", {0}},
      {TUI_FIELD_NORMAL_LEVEL, "Normal Level", {0}},
      {TUI_FIELD_VOLUME_STEP, "Volume Step", {0}},
  };
  int nfields = (int)(sizeof(fields) / sizeof(fields[0]));

  if (g_config.adaptive_gate) {
    snprintf(fields[0].value, sizeof(fields[0].value), "ADAPTIVE");
  } else {
    snprintf(fields[0].value, sizeof(fields[0].value), "%.0f",
             g_config.energy_threshold);
  }
  snprintf(fields[1].value, sizeof(fields[1].value), "%.1f",
           g_config.gate_ratio);
  snprintf(fields[2].value, sizeof(fields[2].value), "%d (%dms)",
           g_config.onset_frames, g_config.onset_frames * FRAME_DURATION_MS);
  snprintf(fields[3].value, sizeof(fields[3].value), "%d (%dms)",
           g_config.offset_frames, g_config.offset_frames * FRAME_DURATION_MS);
  snprintf(fields[4].value, sizeof(fields[4].value), "%.2f",
           g_config.ducked_volume);
  snprintf(fields[5].value, sizeof(fields[5].value), "%.2f",
           g_config.normal_volume);
  snprintf(fields[6].value, sizeof(fields[6].value), "%.3f",
           g_config.volume_step);

  for (int i = 0; i < nfields; i++) {
    int row = y + i;
    bool selected = (g_tui.selected_field == fields[i].field);
    bool is_editing = selected && g_tui.editing;

    if (selected) {
      attron(COLOR_PAIR(CLR_INPUT_ACTIVE) | A_BOLD);
      mvprintw(row, x, " > ");
    } else {
      attron(COLOR_PAIR(CLR_NORMAL));
      mvprintw(row, x, "   ");
    }

    attron(COLOR_PAIR(selected ? CLR_INPUT_ACTIVE : CLR_NORMAL));
    mvprintw(row, x + 3, "%-18s", fields[i].label);

    if (is_editing) {
      attron(COLOR_PAIR(CLR_INPUT_ACTIVE) | A_REVERSE);
      mvprintw(row, x + 22, "%-14s", g_tui.edit_buf);
      attroff(A_REVERSE);
    } else {
      attron(COLOR_PAIR(selected ? CLR_INPUT_ACTIVE : CLR_VOLUME) | A_BOLD);
      mvprintw(row, x + 22, "%-14s", fields[i].value);
      attroff(A_BOLD);
    }

    attroff(COLOR_PAIR(CLR_INPUT_ACTIVE));
  }
}

static void draw_help_bar(int y, int width) {
  attron(COLOR_PAIR(CLR_HELP) | A_REVERSE);
  char help[256];
  if (g_tui.editing) {
    snprintf(help, sizeof(help),
             " EDITING: Type value, Enter=confirm, Esc=cancel "
             "| 0=adaptive threshold ");
  } else {
    snprintf(help, sizeof(help),
             " j/k=select  Enter=edit  Tab=next  "
             "A=adaptive  +/-=ratio  [/]=thresh  q=quit ");
  }
  int len = (int)strlen(help);
  int max_len = width < (int)sizeof(help) - 1 ? width : (int)sizeof(help) - 1;
  for (int i = len; i < max_len; i++) {
    help[i] = ' ';
  }
  help[max_len] = '\0';
  mvprintw(y, 0, "%s", help);
  attroff(A_REVERSE);
}

static void draw_log_panel(int y, int x, int width, int height) {
  pthread_mutex_lock(&g_tui.mutex);

  int lines_to_show = height - 2;
  if (lines_to_show > g_tui.log_count)
    lines_to_show = g_tui.log_count;
  if (lines_to_show > TUI_LOG_LINES)
    lines_to_show = TUI_LOG_LINES;

  for (int i = 0; i < lines_to_show; i++) {
    int idx =
        (g_tui.log_head + g_tui.log_count - lines_to_show + i) % TUI_LOG_LINES;
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(y + 1 + i, x + 1, " %-*.*s", width - 3, width - 3,
             g_tui.log_lines[idx]);
    attroff(COLOR_PAIR(CLR_DIM));
  }

  pthread_mutex_unlock(&g_tui.mutex);
}

static void draw_stats(int y, int x) {
  attron(COLOR_PAIR(CLR_NORMAL));
  mvprintw(y, x, "  Frames:     %d", g_tui.frame_count);

  mvprintw(y + 1, x, "  Detections: %d", g_tui.speech_detect_count);

  mvprintw(y + 2, x, "  Noise Floor: ");
  attron(COLOR_PAIR(CLR_NOISE_FLOOR) | A_BOLD);
  printw("%.0f", g_tui.noise_floor);
  attroff(A_BOLD);
  attron(COLOR_PAIR(CLR_NORMAL));
  printw(" (%.1fdB)", g_tui.noise_floor_db);

  mvprintw(y + 3, x, "  Gate Thresh: ");
  attron(COLOR_PAIR(CLR_GATE_LINE) | A_BOLD);
  printw("%.0f", g_tui.effective_threshold);
  attroff(A_BOLD);
  attron(COLOR_PAIR(CLR_NORMAL));
  printw(" (%.1fdB)", g_tui.effective_threshold_db);

  mvprintw(y + 4, x, "  Mode: ");
  attron(COLOR_PAIR(CLR_VOLUME) | A_BOLD);
  printw("%s", g_config.adaptive_gate ? "ADAPTIVE" : "FIXED");
  attroff(A_BOLD);
}

static void tui_render(void) {
  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);

  if (max_x < 60 || max_y < 24) {
    erase();
    attron(COLOR_PAIR(CLR_METER_HIGH) | A_BOLD);
    mvprintw(max_y / 2, max_x / 2 - 12, "Terminal too small!");
    mvprintw(max_y / 2 + 1, max_x / 2 - 12, "Need at least 60x24");
    attroff(A_BOLD);
    refresh();
    return;
  }

  erase();

  attron(COLOR_PAIR(CLR_HEADER) | A_BOLD | A_REVERSE);
  char title_bar[256];
  snprintf(title_bar, sizeof(title_bar),
           " AUDIO DUCK --- Real-time Audio Ducking Daemon "
           "--- Device: %s ",
           g_config.capture_device);
  int tlen = (int)strlen(title_bar);
  int max_title =
      max_x < (int)sizeof(title_bar) - 1 ? max_x : (int)sizeof(title_bar) - 1;
  for (int i = tlen; i < max_title; i++)
    title_bar[i] = ' ';
  title_bar[max_title] = '\0';
  mvprintw(0, 0, "%s", title_bar);
  attroff(A_BOLD | A_REVERSE);

  int left_w = max_x * 2 / 3;
  if (left_w < 45)
    left_w = 45;
  int right_w = max_x - left_w;
  if (right_w < 20)
    right_w = 20;

  int panel_h = max_y - 2;
  draw_box(1, 0, panel_h, left_w, CLR_BORDER, "Meters & State");

  int row = 3;

  draw_state_indicator(row, 2, g_tui.state, g_tui.is_speech,
                       g_tui.energy_above_gate);
  row += 4;

  draw_horizontal_line(row, 0, left_w, CLR_BORDER);
  row += 2;

  draw_db_meter(row, 2, left_w - 4, g_tui.rms_db, g_tui.effective_threshold_db,
                g_tui.noise_floor_db, g_tui.peak_rms_db, "RMS", true);
  row++;

  attron(COLOR_PAIR(CLR_NOISE_FLOOR));
  mvprintw(row, 8, ": floor  ");
  attron(COLOR_PAIR(CLR_GATE_LINE) | A_BOLD);
  printw("| gate  ");
  attroff(A_BOLD);
  attron(COLOR_PAIR(CLR_METER_HIGH) | A_BOLD);
  printw("| peak");
  attroff(A_BOLD);
  row += 2;

  draw_volume_bar(row, 2, left_w - 4, g_tui.current_volume);
  row += 2;

  draw_horizontal_line(row, 0, left_w, CLR_BORDER);
  row++;

  int waveform_h = panel_h - row - 2;
  if (waveform_h > 12)
    waveform_h = 12;
  if (waveform_h < 4)
    waveform_h = 4;

  attron(COLOR_PAIR(CLR_BORDER));
  mvprintw(row, 2, " Waveform History ");
  attroff(COLOR_PAIR(CLR_BORDER));
  row++;

  int waveform_w = left_w - 4;
  if (waveform_w > TUI_HISTORY_LEN)
    waveform_w = TUI_HISTORY_LEN;

  draw_waveform(row, 1, waveform_w, waveform_h, g_tui.rms_history,
                TUI_HISTORY_LEN, g_tui.rms_history_idx,
                g_tui.effective_threshold_db);

  attron(COLOR_PAIR(CLR_DIM));
  mvprintw(row, left_w - 7, " 0 dB");
  if (waveform_h > 2) {
    mvprintw(row + waveform_h / 2, left_w - 7, "-48dB");
  }
  mvprintw(row + waveform_h - 1, left_w - 7, "-96dB");
  attroff(COLOR_PAIR(CLR_DIM));

  draw_box(1, left_w, panel_h, right_w, CLR_BORDER, "Configuration");

  int rrow = 3;
  draw_stats(rrow, left_w + 1);
  rrow += 6;

  draw_horizontal_line(rrow, left_w, right_w, CLR_BORDER);
  rrow++;

  attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
  mvprintw(rrow, left_w + 2, "Settings (Enter to edit):");
  attroff(A_BOLD);
  rrow++;

  draw_config_panel(rrow, left_w + 1, right_w - 2);
  rrow += 8;

  if (rrow < panel_h - 3) {
    draw_horizontal_line(rrow, left_w, right_w, CLR_BORDER);
    rrow++;

    int log_h = panel_h - rrow;
    if (log_h > TUI_LOG_LINES + 2)
      log_h = TUI_LOG_LINES + 2;
    if (log_h > 2) {
      attron(COLOR_PAIR(CLR_BORDER));
      mvprintw(rrow, left_w + 2, " Event Log ");
      attroff(COLOR_PAIR(CLR_BORDER));
      draw_log_panel(rrow, left_w, right_w, log_h);
    }
  }

  draw_help_bar(max_y - 1, max_x);

  refresh();
}

static void tui_apply_edit(void) {
  double val = atof(g_tui.edit_buf);

  switch (g_tui.selected_field) {
  case TUI_FIELD_ENERGY_THRESHOLD:
    if (val == 0.0) {
      g_config.adaptive_gate = true;
      tui_log("Gate mode: ADAPTIVE");
    } else if (val > 0.0) {
      g_config.adaptive_gate = false;
      g_config.energy_threshold = val;
      tui_log("Energy threshold: %.0f (FIXED mode)", val);
    }
    break;

  case TUI_FIELD_GATE_RATIO:
    if (val > 0.0 && val < 100.0) {
      g_config.gate_ratio = val;
      tui_log("Gate ratio: %.1f", val);
    }
    break;

  case TUI_FIELD_ONSET:
    if ((int)val > 0 && (int)val < 100) {
      g_config.onset_frames = (int)val;
      tui_log("Onset frames: %d (%dms)", g_config.onset_frames,
              g_config.onset_frames * FRAME_DURATION_MS);
    }
    break;

  case TUI_FIELD_OFFSET:
    if ((int)val > 0 && (int)val < 500) {
      g_config.offset_frames = (int)val;
      tui_log("Offset frames: %d (%dms)", g_config.offset_frames,
              g_config.offset_frames * FRAME_DURATION_MS);
    }
    break;

  case TUI_FIELD_DUCK_LEVEL:
    if (val >= 0.0 && val <= 1.0) {
      g_config.ducked_volume = val;
      tui_log("Duck level: %.2f", val);
    }
    break;

  case TUI_FIELD_NORMAL_LEVEL:
    if (val > 0.0 && val <= 1.5) {
      g_config.normal_volume = val;
      tui_log("Normal level: %.2f", val);
    }
    break;

  case TUI_FIELD_VOLUME_STEP:
    if (val > 0.0 && val < 1.0) {
      g_config.volume_step = val;
      tui_log("Volume step: %.3f", val);
    }
    break;

  case TUI_FIELD_NONE:
  case TUI_FIELD_COUNT:
    break;
  }
}

static void tui_handle_input(int ch) {
  if (g_tui.editing) {
    switch (ch) {
    case '\n':
    case '\r':
    case KEY_ENTER:
      g_tui.edit_buf[g_tui.edit_pos] = '\0';
      tui_apply_edit();
      g_tui.editing = false;
      break;

    case 27: /* Escape */
      g_tui.editing = false;
      break;

    case KEY_BACKSPACE:
    case 127:
    case '\b':
      if (g_tui.edit_pos > 0) {
        g_tui.edit_pos--;
        g_tui.edit_buf[g_tui.edit_pos] = '\0';
      }
      break;

    default:
      if (ch >= 32 && ch < 127 && g_tui.edit_pos < 30) {
        g_tui.edit_buf[g_tui.edit_pos] = (char)ch;
        g_tui.edit_pos++;
        g_tui.edit_buf[g_tui.edit_pos] = '\0';
      }
      break;
    }
    return;
  }

  switch (ch) {
  case 'q':
  case 'Q':
    g_running = 0;
    break;

  case KEY_UP:
  case 'k':
    if (g_tui.selected_field > TUI_FIELD_ENERGY_THRESHOLD) {
      g_tui.selected_field--;
    } else {
      g_tui.selected_field = TUI_FIELD_VOLUME_STEP;
    }
    break;

  case KEY_DOWN:
  case 'j':
    if (g_tui.selected_field < TUI_FIELD_VOLUME_STEP) {
      g_tui.selected_field++;
    } else {
      g_tui.selected_field = TUI_FIELD_ENERGY_THRESHOLD;
    }
    if (g_tui.selected_field == TUI_FIELD_NONE) {
      g_tui.selected_field = TUI_FIELD_ENERGY_THRESHOLD;
    }
    break;

  case '\t':
    g_tui.selected_field++;
    if (g_tui.selected_field >= TUI_FIELD_COUNT) {
      g_tui.selected_field = TUI_FIELD_ENERGY_THRESHOLD;
    }
    break;

  case '\n':
  case '\r':
  case KEY_ENTER:
    if (g_tui.selected_field != TUI_FIELD_NONE) {
      g_tui.editing = true;
      g_tui.edit_buf[0] = '\0';
      g_tui.edit_pos = 0;
    }
    break;

  case 'a':
  case 'A':
    g_config.adaptive_gate = !g_config.adaptive_gate;
    if (g_config.adaptive_gate) {
      tui_log("Switched to ADAPTIVE gate mode");
    } else {
      tui_log("Switched to FIXED gate mode (threshold=%.0f)",
              g_config.energy_threshold);
    }
    break;

  case '+':
  case '=':
    g_config.gate_ratio += 0.5;
    if (g_config.gate_ratio > 50.0)
      g_config.gate_ratio = 50.0;
    tui_log("Gate ratio: %.1f", g_config.gate_ratio);
    break;

  case '-':
    g_config.gate_ratio -= 0.5;
    if (g_config.gate_ratio < 1.0)
      g_config.gate_ratio = 1.0;
    tui_log("Gate ratio: %.1f", g_config.gate_ratio);
    break;

  case '[':
    g_config.energy_threshold -= 50.0;
    if (g_config.energy_threshold < 50.0)
      g_config.energy_threshold = 50.0;
    if (!g_config.adaptive_gate) {
      tui_log("Energy threshold: %.0f", g_config.energy_threshold);
    }
    break;

  case ']':
    g_config.energy_threshold += 50.0;
    if (g_config.energy_threshold > 20000.0)
      g_config.energy_threshold = 20000.0;
    if (!g_config.adaptive_gate) {
      tui_log("Energy threshold: %.0f", g_config.energy_threshold);
    }
    break;

  default:
    break;
  }
}

static void tui_init(void) {
  setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  nonl();
  intrflush(stdscr, FALSE);
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);

  if (has_colors()) {
    start_color();
    use_default_colors();

    init_pair(CLR_NORMAL, COLOR_WHITE, -1);
    init_pair(CLR_TITLE, COLOR_CYAN, -1);
    init_pair(CLR_BORDER, COLOR_BLUE, -1);
    init_pair(CLR_STATE_IDLE, COLOR_GREEN, -1);
    init_pair(CLR_STATE_DUCKING, COLOR_YELLOW, -1);
    init_pair(CLR_STATE_DUCKED, COLOR_RED, -1);
    init_pair(CLR_STATE_RESTORING, COLOR_MAGENTA, -1);
    init_pair(CLR_METER_LOW, COLOR_GREEN, -1);
    init_pair(CLR_METER_MID, COLOR_YELLOW, -1);
    init_pair(CLR_METER_HIGH, COLOR_RED, -1);
    init_pair(CLR_METER_CLIP, COLOR_RED, -1);
    init_pair(CLR_GATE_LINE, COLOR_CYAN, -1);
    init_pair(CLR_NOISE_FLOOR, COLOR_BLUE, -1);
    init_pair(CLR_VAD_ON, COLOR_GREEN, -1);
    init_pair(CLR_VAD_OFF, COLOR_WHITE, -1);
    init_pair(CLR_VOLUME, COLOR_CYAN, -1);
    init_pair(CLR_HELP, COLOR_WHITE, COLOR_BLUE);
    init_pair(CLR_INPUT_ACTIVE, COLOR_BLACK, COLOR_CYAN);
    init_pair(CLR_WAVEFORM, COLOR_GREEN, -1);
    init_pair(CLR_HEADER, COLOR_WHITE, COLOR_BLUE);
    init_pair(CLR_DIM, COLOR_WHITE, -1);
    init_pair(CLR_SPEECH_IND, COLOR_GREEN, -1);
  }

  memset(g_tui.rms_history, 0, sizeof(g_tui.rms_history));
  g_tui.rms_history_idx = 0;
  g_tui.selected_field = TUI_FIELD_ENERGY_THRESHOLD;
}

static void tui_cleanup(void) { endwin(); }

static void print_usage(const char *prog) {
  fprintf(
      stderr,
      "Usage: %s [OPTIONS]\n\n"
      "  -d, --device DEV       ALSA capture device (default: \"default\")\n"
      "  -s, --sink ID          PipeWire sink (default: "
      "\"@DEFAULT_AUDIO_SINK@\")\n"
      "  -l, --duck-level F     Ducked volume 0.0-1.0 (default: 0.20)\n"
      "  -n, --normal-level F   Normal volume 0.0-1.5 (default: 1.00)\n"
      "  -S, --step F           Volume step per update (default: 0.03)\n"
      "  -O, --onset N          Speech onset frames (default: 3)\n"
      "  -F, --offset N         Speech offset frames (default: 25)\n"
      "  -E, --energy F         Fixed energy threshold, 0=adaptive (default: "
      "0=adaptive)\n"
      "  -R, --ratio F          Adaptive gate ratio (default: 6.0)\n"
      "  -C, --calibrate        Print energy values for 10s and exit\n"
      "  -T, --no-tui           Disable TUI (use verbose text mode)\n"
      "  -v, --verbose          Print state/energy to stderr (no-tui mode)\n"
      "  -D, --daemonize        Fork to background (disables TUI)\n"
      "  -h, --help             This help\n\n"
      "TUI Controls:\n"
      "  Up/Down or j/k  Select configuration field\n"
      "  Enter            Edit selected field\n"
      "  Tab              Next field\n"
      "  A                Toggle adaptive/fixed gate mode\n"
      "  +/-              Adjust gate ratio by 0.5\n"
      "  [/]              Adjust energy threshold by 50\n"
      "  q                Quit\n\n",
      prog);
}

static void parse_args(int argc, char *argv[]) {
  static struct option long_opts[] = {
      {"device", required_argument, NULL, 'd'},
      {"sink", required_argument, NULL, 's'},
      {"duck-level", required_argument, NULL, 'l'},
      {"normal-level", required_argument, NULL, 'n'},
      {"step", required_argument, NULL, 'S'},
      {"onset", required_argument, NULL, 'O'},
      {"offset", required_argument, NULL, 'F'},
      {"energy", required_argument, NULL, 'E'},
      {"ratio", required_argument, NULL, 'R'},
      {"calibrate", no_argument, NULL, 'C'},
      {"no-tui", no_argument, NULL, 'T'},
      {"verbose", no_argument, NULL, 'v'},
      {"daemonize", no_argument, NULL, 'D'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "d:s:l:n:S:O:F:E:R:CTvDh", long_opts,
                            NULL)) != -1) {
    switch (opt) {
    case 'd':
      g_config.capture_device = optarg;
      break;
    case 's':
      g_config.sink_id = optarg;
      break;
    case 'l':
      g_config.ducked_volume = atof(optarg);
      break;
    case 'n':
      g_config.normal_volume = atof(optarg);
      break;
    case 'S':
      g_config.volume_step = atof(optarg);
      break;
    case 'O':
      g_config.onset_frames = atoi(optarg);
      break;
    case 'F':
      g_config.offset_frames = atoi(optarg);
      break;
    case 'E':
      g_config.energy_threshold = atof(optarg);
      g_config.adaptive_gate = (atof(optarg) == 0.0);
      break;
    case 'R':
      g_config.gate_ratio = atof(optarg);
      break;
    case 'C':
      g_config.calibrate = true;
      g_config.tui_mode = false;
      break;
    case 'T':
      g_config.tui_mode = false;
      break;
    case 'v':
      g_config.verbose = true;
      break;
    case 'D':
      g_config.daemonize = true;
      g_config.tui_mode = false;
      break;
    case 'h':
      print_usage(argv[0]);
      exit(0);
    default:
      print_usage(argv[0]);
      exit(1);
    }
  }
}

static void run_calibration(snd_pcm_t *pcm) {
  int16_t capture_buf[FRAME_SAMPLES * 2];
  int16_t frame[FRAME_SAMPLES];

  fprintf(stderr,
          "\n=== CALIBRATION MODE ===\n"
          "Let music play and stay quiet for 5 seconds,\n"
          "then speak normally for 5 seconds.\n"
          "Press Ctrl+C when done.\n\n"
          "%-8s  %-10s  %-36s\n",
          "Frame", "RMS", "Level");

  int frame_num = 0;
  double min_rms = 1e9, max_rms = 0;
  double quiet_sum = 0, loud_sum = 0;
  int quiet_count = 0, loud_count = 0;

  while (g_running && frame_num < 500) {
    snd_pcm_sframes_t n = snd_pcm_readi(pcm, capture_buf, FRAME_SAMPLES);
    if (n < 0) {
      recover_alsa(pcm, (int)n);
      continue;
    }
    if (n != FRAME_SAMPLES)
      continue;

    if (g_capture_channels == 2) {
      for (int i = 0; i < FRAME_SAMPLES; i++) {
        frame[i] = (int16_t)(((int32_t)capture_buf[i * 2] +
                              (int32_t)capture_buf[i * 2 + 1]) /
                             2);
      }
    } else {
      memcpy(frame, capture_buf, FRAME_SAMPLES * sizeof(int16_t));
    }

    double rms = compute_rms(frame, FRAME_SAMPLES);
    if (rms < min_rms)
      min_rms = rms;
    if (rms > max_rms)
      max_rms = rms;

    if (frame_num < 250) {
      quiet_sum += rms;
      quiet_count++;
    } else {
      loud_sum += rms;
      loud_count++;
    }

    int bar_len = 0;
    if (rms > 1.0) {
      bar_len = (int)(log10(rms) * 9.0);
      if (bar_len > 36)
        bar_len = 36;
      if (bar_len < 0)
        bar_len = 0;
    }

    char bar[37];
    memset(bar, '#', (size_t)bar_len);
    memset(bar + bar_len, ' ', (size_t)(36 - bar_len));
    bar[36] = '\0';

    fprintf(stderr, "%-8d  %8.1f  |%s|\n", frame_num, rms, bar);
    frame_num++;
  }

  double quiet_avg = quiet_count > 0 ? quiet_sum / quiet_count : 0;
  double loud_avg = loud_count > 0 ? loud_sum / loud_count : 0;

  fprintf(stderr,
          "\n=== RESULTS ===\n"
          "Min RMS:            %8.1f\n"
          "Max RMS:            %8.1f\n"
          "Avg first 5s:       %8.1f  (should be music bleed / silence)\n"
          "Avg last 5s:        %8.1f  (should include speech)\n"
          "Ratio:              %8.1f\n\n",
          min_rms, max_rms, quiet_avg, loud_avg,
          quiet_avg > 0 ? loud_avg / quiet_avg : 0);

  double suggested = quiet_avg * 4.0;
  if (suggested < 300)
    suggested = 300;

  fprintf(stderr,
          "Suggested settings:\n"
          "  Adaptive (default):  ./audio-duck -d %s -R %.1f\n"
          "  Fixed threshold:     ./audio-duck -d %s -E %.0f\n\n",
          g_config.capture_device,
          loud_avg > 0 && quiet_avg > 0 ? (loud_avg / quiet_avg) * 0.3 : 6.0,
          g_config.capture_device, suggested);
}

/* ─────────────────────────── Main ──────────────────────────────── */

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  /* Daemonize */
  if (g_config.daemonize) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid > 0) {
      fprintf(stderr, "PID %d\n", pid);
      return 0;
    }
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      if (!g_config.verbose) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
      }
      close(devnull);
    }
  }

  snd_pcm_t *pcm = setup_alsa_capture(g_config.capture_device);
  if (!pcm)
    return 1;

  /* Calibration mode */
  if (g_config.calibrate) {
    run_calibration(pcm);
    snd_pcm_close(pcm);
    return 0;
  }

  VadInst *vad = setup_vad();
  if (!vad) {
    snd_pcm_close(pcm);
    return 1;
  }

  if (g_config.tui_mode) {
    tui_init();
    tui_log("Audio duck started");
    tui_log("Device: %s", g_config.capture_device);
    tui_log("Gate: %s", g_config.adaptive_gate ? "ADAPTIVE" : "FIXED");
  } else {
    if (g_config.adaptive_gate) {
      fprintf(stderr, "Energy gate: ADAPTIVE (ratio=%.1f)\n",
              g_config.gate_ratio);
    } else {
      fprintf(stderr, "Energy gate: FIXED (threshold=%.0f)\n",
              g_config.energy_threshold);
    }
    fprintf(stderr,
            "audio-duck: running (device=%s sink=%s duck=%.2f "
            "normal=%.2f)\n",
            g_config.capture_device, g_config.sink_id, g_config.ducked_volume,
            g_config.normal_volume);
  }

  int speech_counter = 0;
  int silence_counter = 0;
  g_current_volume = g_config.normal_volume;

  double noise_floor = 500.0;
  int frame_count = 0;
  int speech_detect_count = 0;

  int16_t capture_buf[FRAME_SAMPLES * 2];
  int16_t frame[FRAME_SAMPLES];

  int64_t last_tui_refresh = 0;

  while (g_running) {
    reap_volume_child();

    if (g_config.tui_mode) {
      int ch;
      while ((ch = getch()) != ERR) {
        tui_handle_input(ch);
      }
    }

    snd_pcm_sframes_t frames_read =
        snd_pcm_readi(pcm, capture_buf, FRAME_SAMPLES);
    if (frames_read < 0) {
      if (!g_running)
        break;
      if (recover_alsa(pcm, (int)frames_read) < 0)
        break;
      continue;
    }
    if (frames_read != FRAME_SAMPLES)
      continue;

    /* Downmix stereo -> mono */
    if (g_capture_channels == 2) {
      for (int i = 0; i < FRAME_SAMPLES; i++) {
        frame[i] = (int16_t)(((int32_t)capture_buf[i * 2] +
                              (int32_t)capture_buf[i * 2 + 1]) /
                             2);
      }
    } else {
      memcpy(frame, capture_buf, FRAME_SAMPLES * sizeof(int16_t));
    }

    frame_count++;

    double rms = compute_rms(frame, FRAME_SAMPLES);
    double rms_db = rms_to_dbfs(rms);

    double effective_threshold;
    if (g_config.adaptive_gate) {
      effective_threshold = noise_floor * g_config.gate_ratio;
      if (effective_threshold < 200.0)
        effective_threshold = 200.0;
    } else {
      effective_threshold = g_config.energy_threshold;
    }

    bool energy_above_gate = (rms >= effective_threshold);
    bool is_speech = false;

    if (energy_above_gate) {
      int vad_result =
          WebRtcVad_Process(vad, SAMPLE_RATE, frame, FRAME_SAMPLES);
      is_speech = (vad_result == 1);

      if (is_speech) {
        speech_detect_count++;
      }
    }

    if (g_config.adaptive_gate && !is_speech && !energy_above_gate) {
      noise_floor =
          (1.0 - NOISE_FLOOR_ALPHA) * noise_floor + NOISE_FLOOR_ALPHA * rms;
      if (noise_floor < 30.0)
        noise_floor = 30.0;
    }

    if (is_speech) {
      speech_counter++;
      silence_counter = 0;
    } else {
      silence_counter++;
      speech_counter = 0;
    }

    // state machine transitions
    duck_state_t prev_state = state;

    switch (state) {
    case STATE_IDLE:
      if (speech_counter >= g_config.onset_frames)
        state = STATE_DUCKING;
      break;

    case STATE_DUCKING:
      if (silence_counter >= g_config.offset_frames)
        state = STATE_RESTORING;
      break;

    case STATE_DUCKED:
      if (silence_counter >= g_config.offset_frames)
        state = STATE_RESTORING;
      break;

    case STATE_RESTORING:
      if (speech_counter >= g_config.onset_frames)
        state = STATE_DUCKING;
      break;
    }

    if (state != prev_state) {
      if (g_config.tui_mode) {
        tui_log("[%d] %s -> %s (rms=%.0f gate=%.0f)", frame_count,
                state_name(prev_state), state_name(state), rms,
                effective_threshold);
      } else if (g_config.verbose) {
        fprintf(stderr,
                "[%6d] STATE: %s -> %s  rms=%.0f  floor=%.0f  "
                "gate=%.0f  speech=%d  silence=%d\n",
                frame_count, state_name(prev_state), state_name(state), rms,
                noise_floor, effective_threshold, speech_counter,
                silence_counter);
      }
    }

    if (!g_config.tui_mode && g_config.verbose && (frame_count % 250) == 0) {
      fprintf(stderr,
              "[%6d] status: state=%s  rms=%.0f  floor=%.0f  "
              "gate=%.0f  vol=%.2f  detections=%d\n",
              frame_count, state_name(state), rms, noise_floor,
              effective_threshold, g_current_volume, speech_detect_count);
    }

    switch (state) {
    case STATE_IDLE:
      break;

    case STATE_DUCKING: {
      double target = g_config.ducked_volume;
      if (g_current_volume > target) {
        double nv = g_current_volume - g_config.volume_step;
        if (nv < target)
          nv = target;
        set_volume(g_config.sink_id, nv);
      }
      if (g_current_volume <= target + 0.005)
        state = STATE_DUCKED;
      break;
    }

    case STATE_DUCKED:
      break;

    case STATE_RESTORING: {
      double target = g_config.normal_volume;
      if (g_current_volume < target) {
        double nv = g_current_volume + g_config.volume_step;
        if (nv > target)
          nv = target;
        set_volume(g_config.sink_id, nv);
      }
      if (g_current_volume >= target - 0.005) {
        state = STATE_IDLE;
        speech_counter = 0;
        silence_counter = 0;
      }
      break;
    }
    }

    // fuck this shit i need a marlboro and fine rye
    if (g_config.tui_mode) {
      int64_t now = now_ms();

      if (rms > g_tui.peak_hold || now - g_tui.peak_hold_time > 2000) {
        g_tui.peak_hold = rms;
        g_tui.peak_hold_time = now;
      }

      if (rms > g_tui.peak_rms) {
        g_tui.peak_rms = rms;
        g_tui.peak_rms_db = rms_db;
      }

      g_tui.rms_history[g_tui.rms_history_idx] = rms;
      g_tui.rms_history_idx = (g_tui.rms_history_idx + 1) % TUI_HISTORY_LEN;

      // update the fucking state fuck this
      g_tui.rms = rms;
      g_tui.rms_db = rms_db;
      g_tui.noise_floor = noise_floor;
      g_tui.noise_floor_db = rms_to_dbfs(noise_floor);
      g_tui.effective_threshold = effective_threshold;
      g_tui.effective_threshold_db = rms_to_dbfs(effective_threshold);
      g_tui.current_volume = g_current_volume;
      g_tui.state = state;
      g_tui.is_speech = is_speech;
      g_tui.energy_above_gate = energy_above_gate;
      g_tui.speech_counter = speech_counter;
      g_tui.silence_counter = silence_counter;
      g_tui.frame_count = frame_count;
      g_tui.speech_detect_count = speech_detect_count;

      if (now - last_tui_refresh >= TUI_REFRESH_INTERVAL_MS) {
        tui_render();
        last_tui_refresh = now;
      }
    }
  }

  if (g_config.tui_mode) {
    tui_cleanup();
  }

  fprintf(stderr, "\naudio-duck: restoring volume...\n");
  {
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%.2f", g_config.normal_volume);
    pid_t pid = fork();
    if (pid == 0) {
      execlp("wpctl", "wpctl", "set-volume", g_config.sink_id, vol_str,
             (char *)NULL);
      _exit(127);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
  }

  if (g_volume_child > 0) {
    int status;
    waitpid(g_volume_child, &status, 0);
  }

  WebRtcVad_Free(vad);
  snd_pcm_drop(pcm);
  snd_pcm_close(pcm);
  fprintf(stderr, "audio-duck: stopped\n");
  return 0;
}
