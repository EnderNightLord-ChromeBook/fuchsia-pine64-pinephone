// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_MIXER_TEST_AUDIO_ANALYSIS_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_MIXER_TEST_AUDIO_ANALYSIS_H_

#include <zircon/types.h>

#include <cmath>

namespace media::audio::test {

// Numerically compare two buffers of integers. A bool (default true) represents
// whether we expect the comparison to fail (for error logging purposes).
template <typename T>
bool CompareBuffers(const T* actual, const T* expected, uint32_t buf_size,
                    bool expect_to_pass = true, bool float_tolerance = false);

// Numerically compare buffer of integers to a specific value. A bool represents
// whether we expect the comparison to fail (for error logging purposes).
template <typename T>
bool CompareBufferToVal(const T* buf, T val, uint32_t buf_size, bool expect_to_pass = true,
                        bool float_tolerance = false);

// Print values of an array -- used during debugging, not test-runs
template <typename T>
void DisplayVals(const T* buf, uint32_t buf_size);

// Write sinusoidal values into a given buffer & length, determined by equation
// "buffer[idx] = magn * cosine(idx*freq/buf_size*2*M_PI + phase)".
// Restated: 'buffer' is the destination for these values; 'buf_size' is the
// number of values generated and written; 'freq' is the number of **complete
// sinusoidal periods** that should perfectly fit into the buffer; 'magn' is a
// multiplier applied to the output (default value is 1.0); 'phase' is an offset
// (default value 0.0) which shifts the signal along the x-axis (value expressed
// in radians, so runs from -M_PI to +M_PI); 'accum' represents whether to add
// the results to current contents of the buffer, or to overwrite it.
template <typename T>
void GenerateCosine(T* buffer, uint32_t buf_size, double freq, bool accumulate, double magn = 1.0,
                    double phase = 0.0);

template <typename T>
void AccumulateCosine(T* buffer, uint32_t buf_size, double freq, double magn = 1.0,
                      double phase = 0.0) {
  GenerateCosine(buffer, buf_size, freq, true, magn, phase);
}

template <typename T>
void OverwriteCosine(T* buffer, uint32_t buf_size, double freq, double magn = 1.0,
                     double phase = 0.0) {
  GenerateCosine(buffer, buf_size, freq, false, magn, phase);
}

// Perform a Fast Fourier Transform on the provided data arrays.
//
// On input, real[] and imag[] contain 'buf_size' number of double-float values
// in the time domain (such as audio samples); buf_size must be a power-of-two.
//
// On output, real[] and imag[] contain 'buf_size' number of double-float values
// in frequency domain, but generally used only through buf_size/2 (per Nyquist)
void FFT(double* real, double* imag, uint32_t buf_size);

// Calculate phase in radians for the complex pair. Correctly handles negative
// or zero values: range of return value is [-PI,PI], not just [-PI/2,PI/2].
double GetPhase(double real, double imag);

// Convert provided real-imag (cartesian) data into magn-phase (polar) format.
// This is done with 2 in-buffers 2 two out-buffers -- NOT 2 in-out-buffers.
// TODO(mpuryear): will clients (tests) want this transformed in-place?
void RectangularToPolar(const double* real, const double* imag, uint32_t buf_size, double* magn,
                        double* phase = nullptr);

void RealDFT(const double* reals, uint32_t len, double* r_freq, double* i_freq);

void InverseDFT(double* real, double* imag, uint32_t buf_size, double* real_out);

void InverseFFT(double* real, double* imag, uint32_t buf_size);

// For specified audio buffer & length, analyze contents and return the
// magnitude (and phase) at given frequency (as above, that sinusoid for which
// 'freq' periods fit perfectly within buffer length). Also return magnitude of
// all other content. Useful for frequency response and signal-to-noise.
// Internally uses an FFT, so buf_size must be a power-of-two.
template <typename T>
void MeasureAudioFreq(T* audio, uint32_t buf_size, uint32_t freq, double* magn_signal,
                      double* magn_other = nullptr, double* phase_signal = nullptr);

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_MIXER_TEST_AUDIO_ANALYSIS_H_
