#ifndef WEBRTC_COMMON_AUDIO_SIGNAL_PROCESSING_REAL_FFT_H_
#define WEBRTC_COMMON_AUDIO_SIGNAL_PROCESSING_REAL_FFT_H_

/*
 * Real-valued FFT wrapper used by the VAD filterbank.
 * Wraps the Ooura complex FFT for real-only input.
 */

struct RealFFT;

struct RealFFT* WebRtcSpl_CreateRealFFT(int order);
void WebRtcSpl_FreeRealFFT(struct RealFFT* self);

/* Compute forward real FFT. output_complex has length 2 + order. */
int WebRtcSpl_RealForwardFFT(struct RealFFT* self,
                              const int16_t* real_data_in,
                              int16_t* complex_data_out);

int WebRtcSpl_RealInverseFFT(struct RealFFT* self,
                              const int16_t* complex_data_in,
                              int16_t* real_data_out);

#endif /* WEBRTC_COMMON_AUDIO_SIGNAL_PROCESSING_REAL_FFT_H_ */
