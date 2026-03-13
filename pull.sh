#!/bin/bash
# fetch_vad_sources.sh
# Run this from the audio-duck/ directory.
#
# We pull the minimal VAD + signal processing files from the WebRTC project.
# These are the same files that py-webrtcvad uses -- no C++, no rtc_base,
# no Chromium build system.

set -euo pipefail

WEBRTC_REPO="https://raw.githubusercontent.com/niccokunzmann/webrtc-audio-processing/refs/heads/master"
# Alternative: use the official WebRTC repo at a known-good commit
# WEBRTC_REPO="https://chromium.googlesource.com/external/webrtc/+/refs/heads/main"

# We use the py-webrtcvad approach: grab from wiseman's bundled copy.
# This is the cleanest standalone extraction that exists.

PY_WEBRTCVAD_REPO="https://github.com/wiseman/py-webrtcvad"
PY_WEBRTCVAD_COMMIT="master"

echo "=== Cloning py-webrtcvad (contains pre-extracted standalone VAD sources) ==="
TMPDIR=$(mktemp -d)
git clone --depth=1 "${PY_WEBRTCVAD_REPO}" "${TMPDIR}/py-webrtcvad"

SRC="${TMPDIR}/py-webrtcvad/cbits"

echo "=== Copying VAD sources ==="
mkdir -p webrtc_vad/include
mkdir -p webrtc_vad/src/signal_processing

# --- Public header ---
cp "${SRC}/webrtc/common_audio/vad/include/webrtc_vad.h" webrtc_vad/include/

# --- VAD core ---
cp "${SRC}/webrtc/common_audio/vad/vad_core.c" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_core.h" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_filterbank.c" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_filterbank.h" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_gmm.c" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_gmm.h" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_sp.c" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/vad_sp.h" webrtc_vad/src/
cp "${SRC}/webrtc/common_audio/vad/webrtc_vad.c" webrtc_vad/src/

# --- Signal processing library (minimal subset used by VAD) ---
for f in \
  division_operations.c \
  energy.c \
  get_scaling_square.c \
  resample_48khz.c \
  resample_by_2_internal.c \
  resample_by_2_internal.h \
  resample_fractional.c \
  spl_init.c \
  spl_inl.c \
  real_fft.c \
  complex_fft.c \
  cross_correlation.c \
  downsample_fast.c \
  vector_scaling_operations.c \
  dot_product_with_scale.h \
  spl_inl.h \
  real_fft.h \
  signal_processing_library.h \
  complex_fft_tables.h; do
  SRC_FILE="${SRC}/webrtc/common_audio/signal_processing/${f}"
  if [ -f "${SRC_FILE}" ]; then
    cp "${SRC_FILE}" webrtc_vad/src/signal_processing/
  else
    echo "WARNING: ${f} not found, may not be needed"
  fi
done

# --- Type definitions header ---
cp "${SRC}/webrtc/typedefs.h" webrtc_vad/include/ 2>/dev/null || true

# --- Common header that VAD internals reference ---
if [ -f "${SRC}/webrtc/common_audio/signal_processing/include/signal_processing_library.h" ]; then
  cp "${SRC}/webrtc/common_audio/signal_processing/include/signal_processing_library.h" \
    webrtc_vad/src/signal_processing/
fi

echo "=== Cleaning up ==="
rm -rf "${TMPDIR}"

echo "=== Done. VAD sources are in webrtc_vad/ ==="
echo "You may need to adjust #include paths in the copied files."
echo "See the patching instructions in README.md."
