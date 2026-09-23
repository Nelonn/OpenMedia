#pragma once

#include <audio/ac3/ac3_tables.hpp>
#include <complex>

namespace openmedia::ac3 {

// Inverse MDCT with windowing and overlap-add (A/52 Section 7.9).
//
// Every call consumes the 256 transform coefficients of one audio block and
// emits 256 PCM samples to `out`, `stride` floats apart, so a channel can be
// written straight into an interleaved buffer. `delay` carries the second
// windowed half of the previous block for the channel.
class Imdct {
public:
  Imdct();

  // 512-sample transform (blksw = 0).
  void longBlock(const float* coefs, float* delay, float* out, int stride) const;

  // Two interleaved 256-sample transforms (blksw = 1).
  void shortBlocks(const float* coefs, float* delay, float* out, int stride) const;

private:
  template<int M>
  struct Dct4 {
    std::complex<float> pre[M / 2];
    std::complex<float> post[M / 2];
    std::complex<float> roots[M / 4];
    uint16_t bitrev[M / 2];

    void init();
    // y[j] = sum_k x[k] * cos(pi / M * (j + 1/2) * (k + 1/2))
    void run(const float* x, float* y) const;
  };

  void overlapAdd(const float* x, float* delay, float* out, int stride) const;

  Dct4<256> long_;
  Dct4<128> short_;
  float window_[2 * BLOCK_SIZE]; // symmetric KBD window, output scale folded in
};

} // namespace openmedia::ac3
