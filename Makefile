CC      = cc
CFLAGS  = -std=c2x -Wall -Wextra -Wpedantic -Wno-unused-parameter \
          -O2 -march=native -pipe \
          -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -DWEBRTC_POSIX \
          -Iwebrtc_vad/shims -Iwebrtc_vad/include -Iwebrtc_vad/src \
          -Iwebrtc_vad/src/signal_processing

LDFLAGS = -lasound -lm -lncursesw -lpthread

VAD_SRCS = \
    webrtc_vad/src/webrtc_vad.c \
    webrtc_vad/src/vad_core.c \
    webrtc_vad/src/vad_filterbank.c \
    webrtc_vad/src/vad_gmm.c \
    webrtc_vad/src/vad_sp.c \
    webrtc_vad/src/signal_processing/energy.c \
    webrtc_vad/src/signal_processing/get_scaling_square.c \
    webrtc_vad/src/signal_processing/resample_48khz.c \
    webrtc_vad/src/signal_processing/resample_by_2_internal.c \
    webrtc_vad/src/signal_processing/resample_fractional.c \
    webrtc_vad/src/signal_processing/division_operations.c \
    webrtc_vad/src/signal_processing/cross_correlation.c \
    webrtc_vad/src/signal_processing/downsample_fast.c \
    webrtc_vad/src/signal_processing/min_max_operations.c \
    webrtc_vad/src/signal_processing/vector_scaling_operations.c \
    webrtc_vad/src/signal_processing/spl_init.c \
    webrtc_vad/src/signal_processing/spl_inl.c \
    webrtc_vad/src/signal_processing/complex_fft.c

VAD_OBJS = $(VAD_SRCS:.c=.o)
ALL_OBJS = main.o $(VAD_OBJS)
TARGET   = audio-duck

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(ALL_OBJS) $(TARGET)
