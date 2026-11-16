// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Audio - the `:reverb` partition.
//
// A Freeverb-style Schroeder reverberator (public-domain topology: 8 parallel damped
// combs and 4 series allpasses per channel, stereo-spread on the right) as pure DSP with
// no miniaudio contact, so the state machine unit-tests headless. AudioEngineImpl.cpp wraps
// it in a custom ma_node for bus-effect chains and per-scene zone reverb.

module;
#include "Core/Prelude.h"

export module foundation.audio:reverb;

import foundation.core;

using namespace foundation::core;

export namespace foundation::audio
{
    struct AudioReverbParams
    {
        f32 roomSize = 0.6f; // 0..1 (comb feedback)
        f32 damping = 0.4f;  // 0..1 (high-frequency decay inside the tail)
        f32 wet = 0.4f;      // 0..1 mix (0 = fully dry passthrough)
        // Dry passthrough level: < 0 = the classic insert mix (1 - wet); an explicit
        // 0 makes a wet-only node (aux-send reverb: per-voice sends feed it in
        // parallel with the dry path, so the dry signal must not pass through again).
        f32 dry = -1.0f;
    };

    /// Stereo Freeverb state. Buffer lengths scale from the canonical 44.1 kHz tunings.
    class FreeverbState
    {
    public:
        void Initialize(u32 sampleRate)
        {
            static constexpr u32 kCombTunings[kCombCount] = {1116, 1188, 1277, 1356,
                                                             1422, 1491, 1557, 1617};
            static constexpr u32 kAllpassTunings[kAllpassCount] = {556, 441, 341, 225};
            static constexpr u32 kStereoSpread = 23;

            const f32 scale = static_cast<f32>(sampleRate) / 44100.0f;
            for (u32 channel = 0; channel < 2; ++channel)
            {
                const u32 spread = channel == 1 ? kStereoSpread : 0;
                for (u32 i = 0; i < kCombCount; ++i)
                {
                    const u32 length = Max<u32>(
                        4, static_cast<u32>(static_cast<f32>(kCombTunings[i] + spread) * scale));
                    m_comb[channel][i].buffer.Resize(length);
                    for (f32& sample : m_comb[channel][i].buffer)
                    {
                        sample = 0.0f;
                    }
                    m_comb[channel][i].cursor = 0;
                    m_comb[channel][i].filterStore = 0.0f;
                }
                for (u32 i = 0; i < kAllpassCount; ++i)
                {
                    const u32 length = Max<u32>(
                        2, static_cast<u32>(static_cast<f32>(kAllpassTunings[i] + spread) * scale));
                    m_allpass[channel][i].buffer.Resize(length);
                    for (f32& sample : m_allpass[channel][i].buffer)
                    {
                        sample = 0.0f;
                    }
                    m_allpass[channel][i].cursor = 0;
                }
            }
            m_initialized = true;
        }

        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        // Set from the game thread while the audio thread processes: each value is atomic, and the
        // processing loads them once per block (plain floats written under a running callback
        // were a data race).
        void SetParams(const AudioReverbParams& params) noexcept
        {
            const f32 wet = Clamp(params.wet, 0.0f, 1.0f);
            m_feedback.store(0.7f + Clamp(params.roomSize, 0.0f, 1.0f) * 0.28f, MemoryOrder::Relaxed);
            m_damp.store(Clamp(params.damping, 0.0f, 1.0f) * 0.4f, MemoryOrder::Relaxed);
            m_wet.store(wet, MemoryOrder::Relaxed);
            m_dry.store(params.dry < 0.0f ? 1.0f - wet : Clamp(params.dry, 0.0f, 1.0f), MemoryOrder::Relaxed);
        }

        [[nodiscard]] f32 Wet() const noexcept { return m_wet.load(MemoryOrder::Relaxed); }
        [[nodiscard]] f32 Dry() const noexcept { return m_dry.load(MemoryOrder::Relaxed); }

        /// Interleaved-stereo in-place processing: out = in * dry + tail * wet (dry
        /// defaults to 1 - wet, the classic insert mix; 0 = wet-only send node).
        /// Mono callers duplicate the channel. Denormal-flushed.
        void ProcessStereo(const f32* input, f32* output, u32 frameCount)
        {
            constexpr f32 kFixedGain = 0.015f; // canonical freeverb input gain
            const f32 dry = m_dry.load(MemoryOrder::Relaxed);
            const f32 wet = m_wet.load(MemoryOrder::Relaxed);
            const f32 damp = m_damp.load(MemoryOrder::Relaxed);
            const f32 feedback = m_feedback.load(MemoryOrder::Relaxed);
            for (u32 frame = 0; frame < frameCount; ++frame)
            {
                const f32 inL = input[frame * 2 + 0];
                const f32 inR = input[frame * 2 + 1];
                const f32 feed = (inL + inR) * kFixedGain;
                f32 outL = 0.0f;
                f32 outR = 0.0f;
                for (u32 i = 0; i < kCombCount; ++i)
                {
                    outL += CombProcess(m_comb[0][i], feed, damp, feedback);
                    outR += CombProcess(m_comb[1][i], feed, damp, feedback);
                }
                for (u32 i = 0; i < kAllpassCount; ++i)
                {
                    outL = AllpassProcess(m_allpass[0][i], outL);
                    outR = AllpassProcess(m_allpass[1][i], outR);
                }
                output[frame * 2 + 0] = inL * dry + outL * wet * 3.0f;
                output[frame * 2 + 1] = inR * dry + outR * wet * 3.0f;
            }
        }

    private:
        static constexpr u32 kCombCount = 8;
        static constexpr u32 kAllpassCount = 4;

        struct Comb
        {
            Array<f32> buffer;
            u32 cursor = 0;
            f32 filterStore = 0.0f;
        };
        struct Allpass
        {
            Array<f32> buffer;
            u32 cursor = 0;
        };

        [[nodiscard]] static f32 Undenormal(f32 value) noexcept
        {
            return (value > -1.0e-18f && value < 1.0e-18f) ? 0.0f : value;
        }

        f32 CombProcess(Comb& comb, f32 input, f32 damp, f32 feedback) noexcept
        {
            const f32 output = comb.buffer[comb.cursor];
            comb.filterStore = Undenormal(output * (1.0f - damp) + comb.filterStore * damp);
            comb.buffer[comb.cursor] = Undenormal(input + comb.filterStore * feedback);
            if (++comb.cursor >= comb.buffer.Size())
            {
                comb.cursor = 0;
            }
            return output;
        }

        f32 AllpassProcess(Allpass& allpass, f32 input) noexcept
        {
            const f32 buffered = allpass.buffer[allpass.cursor];
            const f32 output = buffered - input;
            allpass.buffer[allpass.cursor] = Undenormal(input + buffered * 0.5f);
            if (++allpass.cursor >= allpass.buffer.Size())
            {
                allpass.cursor = 0;
            }
            return output;
        }

        Comb m_comb[2][kCombCount];
        Allpass m_allpass[2][kAllpassCount];
        Atomic<f32> m_feedback{0.84f};
        Atomic<f32> m_damp{0.2f};
        Atomic<f32> m_wet{0.4f};
        Atomic<f32> m_dry{0.6f}; // tracks 1 - wet unless params.dry pins it (send mode)
        bool m_initialized = false;
    };
}
