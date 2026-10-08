#pragma once
// dsp.h - simple headphone "3D" virtualizer (stereo float32 interleaved)
//
// Signal chain per frame:
//   1) Mid/Side widening            -> wider stage
//   2) Crossfeed with ITD + head-shadow low-pass -> out-of-head feeling
//   3) Early reflections (3 taps)   -> small "room" around the listener
//
// Normal mode uses identity parameters, so the signal passes through unchanged.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Order MUST match the order of items in the "Audio Effect" dropdown.
enum class Effect
{
    Normal = 0,
    ThreeD = 1,
    Balanced = 2
};

struct FxParams
{
    float width; // side-channel gain (1.0 = original stereo width)
    float cross; // crossfeed amount
    float room;  // early reflection amount
    float gain;  // output gain (to avoid clipping)
};

// Tune these numbers to get closer to the Histen feel you like.
inline FxParams paramsFor(Effect e)
{
    switch (e)
    {
    case Effect::ThreeD:
        return {2.00f, 0.60f, 0.40f, 0.55f};
    case Effect::Balanced:
        return {1.40f, 0.30f, 0.18f, 0.70f};
    default:
        return {1.00f, 0.00f, 0.00f, 1.00f};
    }
}

// Transparent below 0.85, then smoothly bends towards +-1.0 (no harsh digital clipping).
inline float softLimit(float x)
{
    const float knee = 0.85f;
    const float a = std::fabs(x);
    if (a <= knee)
        return x;
    const float over = (a - knee) / (1.0f - knee);          // 0..inf
    const float y = knee + (1.0f - knee) * std::tanh(over); // < 1.0
    return x < 0.0f ? -y : y;
}

class Spatializer
{
public:
    explicit Spatializer(float sampleRate = 48000.0f) { init(sampleRate); }

    void init(float sr)
    {
        const float kPi = 3.14159265f;
        lpA_ = 1.0f - std::exp(-2.0f * kPi * 1400.0f / sr); // ~1.4 kHz head shadow
        itd_ = (int)(0.00027f * sr);                        // ~0.27 ms interaural delay
        d1_ = (int)(0.0173f * sr);
        d2_ = (int)(0.0231f * sr);
        d3_ = (int)(0.0311f * sr);
        dl_.assign(kN, 0.0f);
        dr_.assign(kN, 0.0f);
        w_ = 0;
        lpL_ = lpR_ = 0.0f;
        cur_ = paramsFor(Effect::Normal);
    }

    // data: interleaved stereo float samples, 'frames' = number of L/R pairs
    void process(float *data, size_t frames, Effect e)
    {
        const FxParams t = paramsFor(e);
        const float k = 0.0004f; // parameter smoothing (no clicks when switching)

        for (size_t i = 0; i < frames; ++i)
        {
            cur_.width += (t.width - cur_.width) * k;
            cur_.cross += (t.cross - cur_.cross) * k;
            cur_.room += (t.room - cur_.room) * k;
            cur_.gain += (t.gain - cur_.gain) * k;

            const float L = data[2 * i];
            const float R = data[2 * i + 1];

            // 1) widening
            const float M = 0.5f * (L + R);
            const float S = 0.5f * (L - R) * cur_.width;
            const float l = M + S;
            const float r = M - S;

            dl_[w_] = l;
            dr_[w_] = r;

            // 2) crossfeed: opposite ear gets delayed + low-passed signal
            const float cl = dr_[(w_ - itd_) & kMask];
            const float cr = dl_[(w_ - itd_) & kMask];
            lpL_ += lpA_ * (cl - lpL_);
            lpR_ += lpA_ * (cr - lpR_);

            float oL = l + cur_.cross * lpL_;
            float oR = r + cur_.cross * lpR_;

            // 3) early reflections
            const float eL = 0.70f * dr_[(w_ - d1_) & kMask] + 0.50f * dl_[(w_ - d2_) & kMask] + 0.35f * dr_[(w_ - d3_) & kMask];
            const float eR = 0.70f * dl_[(w_ - d1_) & kMask] + 0.50f * dr_[(w_ - d2_) & kMask] + 0.35f * dl_[(w_ - d3_) & kMask];
            oL += cur_.room * eL;
            oR += cur_.room * eR;

            oL *= cur_.gain;
            oR *= cur_.gain;

            oL = softLimit(oL);
            oR = softLimit(oR);

            data[2 * i] = oL;
            data[2 * i + 1] = oR;

            w_ = (w_ + 1) & kMask;
        }
    }

private:
    static constexpr int kN = 4096; // power of two, ~85 ms at 48 kHz
    static constexpr int kMask = kN - 1;

    std::vector<float> dl_, dr_;
    int w_ = 0;
    int itd_ = 13, d1_ = 830, d2_ = 1109, d3_ = 1492;
    float lpA_ = 0.15f, lpL_ = 0.0f, lpR_ = 0.0f;
    FxParams cur_{1.0f, 0.0f, 0.0f, 1.0f};
};

// ---------------------------------------------------------------------------
// ToneControl: bass shelf (150 Hz) + treble shelf (5 kHz) + master volume + soft limiter.
// Runs AFTER the spatial effect. At bass = treble = 0 and volume = 100 it is transparent.
// ---------------------------------------------------------------------------
class ToneControl
{
public:
    explicit ToneControl(float sampleRate = 48000.0f) { init(sampleRate); }

    void init(float sr)
    {
        sr_ = sr;
        bassDb_ = trebleDb_ = 0;
        loC_ = Coef();
        hiC_ = Coef();
        for (int c = 0; c < 2; ++c)
        {
            lo_[c] = State();
            hi_[c] = State();
        }
        gain_ = 1.0f;
    }

    // bassDb / trebleDb: about -12..+12 dB, volumePct: 0..100
    void process(float *d, size_t frames, int bassDb, int trebleDb, int volumePct)
    {
        if (bassDb != bassDb_)
        {
            bassDb_ = bassDb;
            loC_ = shelf(false, 150.0, bassDb);
        }
        if (trebleDb != trebleDb_)
        {
            trebleDb_ = trebleDb;
            hiC_ = shelf(true, 5000.0, trebleDb);
        }

        const float v = volumePct <= 0 ? 0.0f : (volumePct >= 100 ? 1.0f : volumePct / 100.0f);
        // Perceptual volume curve: 50 % is about -12 dB.
        // When boosting bass/treble, give back some headroom so loud music does not hit the limiter.
        const int boost = std::max(0, std::max(bassDb, trebleDb));
        const float comp = std::pow(10.0f, -0.5f * (float)boost / 20.0f);
        const float target = v * v * comp;

        const bool doLo = bassDb != 0;
        const bool doHi = trebleDb != 0;

        for (size_t i = 0; i < frames; ++i)
        {
            gain_ += (target - gain_) * 0.002f; // smooth volume changes (no clicks)
            for (int ch = 0; ch < 2; ++ch)
            {
                double x = d[2 * i + ch];
                if (doLo)
                    x = lo_[ch].run(loC_, x);
                if (doHi)
                    x = hi_[ch].run(hiC_, x);
                d[2 * i + ch] = softLimit((float)x * gain_);
            }
        }
    }

private:
    struct Coef
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct State
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double run(const Coef &c, double x)
        {
            const double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    // RBJ audio-EQ-cookbook shelving filter (slope S = 1)
    Coef shelf(bool high, double f0, int db) const
    {
        const double A = std::pow(10.0, db / 40.0);
        const double w0 = 2.0 * 3.14159265358979 * f0 / sr_;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / 2.0 * std::sqrt(2.0);
        const double beta = 2.0 * std::sqrt(A) * alpha;

        double b0, b1, b2, a0, a1, a2;
        if (!high)
        { // low shelf
            b0 = A * ((A + 1) - (A - 1) * cw + beta);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - beta);
            a0 = (A + 1) + (A - 1) * cw + beta;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - beta;
        }
        else
        { // high shelf
            b0 = A * ((A + 1) + (A - 1) * cw + beta);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - beta);
            a0 = (A + 1) - (A - 1) * cw + beta;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - beta;
        }
        Coef c;
        c.b0 = b0 / a0;
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }

    float sr_ = 48000.0f;
    int bassDb_ = 0, trebleDb_ = 0;
    Coef loC_, hiC_;
    State lo_[2], hi_[2];
    float gain_ = 1.0f;
};