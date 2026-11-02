#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bps::production::dsp {
// One peaking-EQ band (RBJ cookbook), direct form I, one instance per channel.
class PeakingBand {
public:
    // Sets the band. freqHz is clamped under Nyquist, q to a usable range, gainDb to ±24.
    void Configure(double sampleRateHz, double freqHz, double gainDb, double q)
    {
        const double nyquist = sampleRateHz * 0.5;
        freqHz = std::clamp(freqHz, 10.0, nyquist * 0.95);
        q = std::clamp(q, 0.1, 20.0);
        gainDb = std::clamp(gainDb, -24.0, 24.0);
        if (configured_ && sampleRateHz == rate_ && freqHz == freq_ && gainDb == gain_ && q == q_)
            return;
        rate_ = sampleRateHz;
        freq_ = freqHz;
        gain_ = gainDb;
        q_ = q;

        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * freqHz / sampleRateHz;
        const double cosW0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double b0 = 1.0 + alpha * a;
        const double b1 = -2.0 * cosW0;
        const double b2 = 1.0 - alpha * a;
        const double a0 = 1.0 + alpha / a;
        const double a1 = -2.0 * cosW0;
        const double a2 = 1.0 - alpha / a;
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
        configured_ = true;
    }

    float Process(float x)
    {
        const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return static_cast<float>(y);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    bool configured_ = false;
    double rate_ = 0.0, freq_ = 0.0, gain_ = 0.0, q_ = 0.0;
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

class NoiseReducer {
public:
    void Process(float &l, float &r, double amount, double sampleRateHz)
    {
        if (sampleRateHz <= 0.0)
            return;
        const double depth = std::clamp(amount, 0.0, 1.0);
        const double level = std::max(std::fabs(static_cast<double>(l)), std::fabs(static_cast<double>(r)));

        // Envelope: quick to follow a rise, slower to let go.
        const double attackCoef = std::exp(-1.0 / (0.002 * sampleRateHz));
        const double releaseCoef = std::exp(-1.0 / (0.050 * sampleRateHz));
        envelope_ = level > envelope_ ? attackCoef * envelope_ + (1.0 - attackCoef) * level
                                      : releaseCoef * envelope_ + (1.0 - releaseCoef) * level;

        // Target gain: unity above a threshold a few dB over the floor, falling
        // linearly toward (1 - depth) at the floor itself.
        constexpr double kThresholdRatio = 3.0;   // about +9.5 dB over the floor

        if (initSamples_ < static_cast<uint32_t>(0.05 * sampleRateHz)) {
            initSum_ += level;
            ++initSamples_;
            floor_ = initSum_ / static_cast<double>(initSamples_);
        } else if (envelope_ < floor_) {
            floor_ = envelope_;
        } else {
            const double maxRise = std::pow(3.0, 1.0 / sampleRateHz);   // per sample
            floor_ = std::min(envelope_, floor_ * maxRise);
        }

        const double threshold = std::max(floor_ * kThresholdRatio, 1e-9);
        const double target = envelope_ >= threshold
                                  ? 1.0
                                  : std::max(1.0 - depth, envelope_ / threshold);

        const double gainCoef = std::exp(-1.0 / (0.005 * sampleRateHz));
        gain_ = gainCoef * gain_ + (1.0 - gainCoef) * target;
        l = static_cast<float>(l * gain_);
        r = static_cast<float>(r * gain_);
    }

private:
    double envelope_ = 0.0;
    double floor_ = 0.0;
    double gain_ = 1.0;
    // The floor's starting point, averaged over the first 50 ms (see Process).
    uint32_t initSamples_ = 0;
    double initSum_ = 0.0;
};

class Reverb {
public:
    void Configure(double sampleRateHz)
    {
        if (configured_ && sampleRateHz == rate_)
            return;
        rate_ = sampleRateHz;
        const double scale = sampleRateHz / 44100.0;
        static constexpr int kComb[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static constexpr int kAllpass[kAllpasses] = {556, 441, 341, 225};
        for (int i = 0; i < kCombs; ++i) {
            combs_[i].buffer.assign(static_cast<size_t>(kComb[i] * scale) + 1, 0.0f);
            combs_[i].index = 0;
            combs_[i].store = 0.0f;
        }
        for (int i = 0; i < kAllpasses; ++i) {
            allpasses_[i].buffer.assign(static_cast<size_t>(kAllpass[i] * scale) + 1, 0.0f);
            allpasses_[i].index = 0;
        }
        configured_ = true;
    }

    // One mono sample in, one reverberant mono sample out.
    float Process(float input)
    {
        const float in = input * kFixedGain;
        float wet = 0.0f;
        for (auto &c : combs_) {
            const float out = c.buffer[c.index];
            c.store = out * (1.0f - kDamp) + c.store * kDamp;
            c.buffer[c.index] = in + c.store * kFeedback;
            if (++c.index >= c.buffer.size())
                c.index = 0;
            wet += out;
        }
        for (auto &a : allpasses_) {
            const float buffered = a.buffer[a.index];
            const float out = -wet + buffered;
            a.buffer[a.index] = wet + buffered * 0.5f;
            if (++a.index >= a.buffer.size())
                a.index = 0;
            wet = out;
        }
        return wet;
    }

private:
    static constexpr int kCombs = 8;
    static constexpr int kAllpasses = 4;
    static constexpr float kFixedGain = 0.015f;
    static constexpr float kFeedback = 0.84f;
    static constexpr float kDamp = 0.2f;

    struct Comb {
        std::vector<float> buffer;
        size_t index = 0;
        float store = 0.0f;
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t index = 0;
    };

    bool configured_ = false;
    double rate_ = 0.0;
    Comb combs_[kCombs];
    Allpass allpasses_[kAllpasses];
};
} // namespace bps::production::dsp