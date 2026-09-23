#include "audio/ac3/ac3_imdct.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace openmedia::ac3 {

namespace {

// Zeroth order modified Bessel function of the first kind.
auto besselI0(double x) -> double {
  const double q = x * x / 4.0;
  double term = 1.0;
  double sum = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

// Output gain applied on top of the unnormalized transform.
constexpr double OUTPUT_SCALE = -2.0;

}

template<int M>
void Imdct::Dct4<M>::init() {
  constexpr int n = M / 2;
  const double pi = std::numbers::pi;
  for (int p = 0; p < n; ++p) {
    pre[p] = std::polar(1.0f, static_cast<float>(-pi * (4 * p + 1) / (4.0 * M)));
    post[p] = std::polar(1.0f, static_cast<float>(-pi * p / M));
  }
  for (int k = 0; k < n / 2; ++k) {
    roots[k] = std::polar(1.0f, static_cast<float>(-2.0 * pi * k / n));
  }
  const int bits = std::countr_zero(static_cast<unsigned>(n));
  for (int i = 0; i < n; ++i) {
    unsigned r = 0;
    for (int b = 0; b < bits; ++b) {
      if (i & (1 << b)) r |= 1u << (bits - 1 - b);
    }
    bitrev[i] = static_cast<uint16_t>(r);
  }
}

template<int M>
void Imdct::Dct4<M>::run(const float* x, float* y) const {
  constexpr int n = M / 2;
  std::complex<float> buf[n];

  // Pack even and reversed odd coefficients into complex values, pre-twiddle
  // and store in bit reversed order for the in-place FFT.
  for (int p = 0; p < n; ++p) {
    buf[bitrev[p]] = std::complex<float>(x[2 * p], x[M - 1 - 2 * p]) * pre[p];
  }

  // Radix-2 decimation in time FFT of size n.
  for (int size = 2; size <= n; size <<= 1) {
    const int half = size >> 1;
    const int step = n / size;
    for (int start = 0; start < n; start += size) {
      for (int k = 0; k < half; ++k) {
        const std::complex<float> t = buf[start + k + half] * roots[k * step];
        const std::complex<float> u = buf[start + k];
        buf[start + k] = u + t;
        buf[start + k + half] = u - t;
      }
    }
  }

  for (int q = 0; q < n; ++q) {
    const std::complex<float> d = buf[q] * post[q];
    y[2 * q] = d.real();
    y[M - 1 - 2 * q] = -d.imag();
  }
}

Imdct::Imdct() {
  long_.init();
  short_.init();

  // Kaiser-Bessel derived window, alpha = 5 (A/52 Section 7.9.4.1).
  constexpr int WINDOW_LENGTH = 2 * BLOCK_SIZE;
  constexpr double KBD_ALPHA = 5.0;
  double kernel[BLOCK_SIZE + 1];
  double total = 0.0;
  for (int j = 0; j <= BLOCK_SIZE; ++j) {
    const double t = 2.0 * j / BLOCK_SIZE - 1.0;
    kernel[j] = besselI0(std::numbers::pi * KBD_ALPHA * std::sqrt(std::max(0.0, 1.0 - t * t)));
    total += kernel[j];
  }
  double acc = 0.0;
  for (int n = 0; n < BLOCK_SIZE; ++n) {
    acc += kernel[n];
    const auto w = static_cast<float>(std::sqrt(acc / total) * OUTPUT_SCALE);
    window_[n] = w;
    window_[WINDOW_LENGTH - 1 - n] = w;
  }
}

void Imdct::overlapAdd(const float* x, float* delay, float* out, int stride) const {
  for (int n = 0; n < BLOCK_SIZE; ++n) {
    out[n * stride] = x[n] * window_[n] + delay[n];
    delay[n] = x[BLOCK_SIZE + n] * window_[BLOCK_SIZE + n];
  }
}

void Imdct::longBlock(const float* coefs, float* delay, float* out, int stride) const {
  constexpr int M = BLOCK_SIZE;
  float y[M];
  long_.run(coefs, y);

  // Unfold the DCT-IV into the 512 IMDCT outputs
  // x[n] = sum_k X[k] cos(2 pi / 512 * (n + 1/2 + 128) * (k + 1/2)).
  float x[2 * M];
  for (int n = 0; n < M / 2; ++n) {
    x[n] = y[n + M / 2];
    x[3 * M / 2 + n] = -y[n];
  }
  for (int n = M / 2; n < 3 * M / 2; ++n) {
    x[n] = -y[3 * M / 2 - 1 - n];
  }
  overlapAdd(x, delay, out, stride);
}

void Imdct::shortBlocks(const float* coefs, float* delay, float* out, int stride) const {
  constexpr int M = BLOCK_SIZE / 2;
  float in1[M];
  float in2[M];
  for (int k = 0; k < M; ++k) {
    in1[k] = coefs[2 * k];
    in2[k] = coefs[2 * k + 1];
  }
  float y1[M];
  float y2[M];
  short_.run(in1, y1);
  short_.run(in2, y2);

  // First transform uses a phase offset of 1/2, the second one of 128 + 1/2
  // (A/52 Section 7.9.4.2, alpha = -1 and +1).
  float x[4 * M];
  for (int n = 0; n < M; ++n) {
    x[n] = y1[n];
    x[M + n] = -y1[M - 1 - n];
    x[2 * M + n] = -y2[M - 1 - n];
    x[3 * M + n] = -y2[n];
  }
  overlapAdd(x, delay, out, stride);
}

} // namespace openmedia::ac3
