// Denormal handling for the render thread.
//
// Feedback-heavy patches (loopers with high feedback, reverb-ish VST chains)
// decay into denormal territory and, without flush-to-zero, a single node can
// cost two orders of magnitude more CPU. The audio callback installs a
// ScopedNoDenormals for the duration of the block and restores the caller's MXCSR
// on the way out so we never leak the mode into host/plugin code that might
// depend on IEEE behaviour.
#pragma once

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define ACOMPOSTER_HAS_SSE 1
#  include <xmmintrin.h>
#  include <pmmintrin.h>
#else
#  define ACOMPOSTER_HAS_SSE 0
#endif

#include <cmath>

namespace acm {

class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if ACOMPOSTER_HAS_SSE
        saved_ = _mm_getcsr();
        // 0x8000 = flush-to-zero, 0x0040 = denormals-are-zero.
        _mm_setcsr((saved_ & ~0x8040u) | 0x8040u);
#endif
    }

    ~ScopedNoDenormals() noexcept {
#if ACOMPOSTER_HAS_SSE
        _mm_setcsr(saved_);
#endif
    }

    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
#if ACOMPOSTER_HAS_SSE
    unsigned saved_ = 0;
#endif
};

// Belt-and-braces scrub for values that leave the engine (meters, state saved to
// disk) where a denormal or NaN would be visible rather than merely slow.
inline float sanitise(float v) noexcept {
    if (!std::isfinite(v)) return 0.0f;
    if (v > -1.0e-25f && v < 1.0e-25f) return 0.0f;
    return v;
}

} // namespace acm
