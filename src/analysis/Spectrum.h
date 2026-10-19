#pragma once
// A display spectrum for the page's EQ view: Hann-windowed FFT folded into 1/12-octave bands from 20 Hz to 20 kHz.
// Each band shows its strongest FFT bin, scaled so a full-scale sine reads 0 dB in its band.
#include <vector>

namespace im
{
// The band centres in Hz: 20 * 2^(k/12), k = 0..119 (20 Hz .. 19.4 kHz).
const std::vector<float>& spectrumBandHz();
// x: n samples (n a power of two, 512..16384), oldest first. out: one dB value per band (floor -120).
void spectrumDb (const float* x, int n, double sampleRate, std::vector<float>& out);
} // namespace im
