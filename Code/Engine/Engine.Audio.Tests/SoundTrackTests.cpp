// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A sequence's sound track on a playing scene: a section entered part way starts its voice there,
// a voice that wanders is moved back to the playhead and a jump moves it at once, the voice pauses
// with the play and plays on at a hold, and the play letting go stops it.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <cmath>

import foundation.core;
import foundation.scene;
import foundation.audio;
import foundation.sequence;
import engine.audio;

using namespace foundation::core;
using namespace engine::audio;
using namespace foundation::audio;
namespace scene = foundation::scene;
namespace sequence = foundation::sequence;

namespace
{
    [[nodiscard]] RefPtr<AudioClip> Tone(f32 seconds)
    {
        constexpr u32 kRate = 8000;
        Array<i16> samples;
        const usize frames = static_cast<usize>(seconds * static_cast<f32>(kRate));
        for (usize frame = 0; frame < frames; ++frame)
        {
            const f32 t = static_cast<f32>(frame) / static_cast<f32>(kRate);
            samples.PushBack(static_cast<i16>(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * t) * 32000.0f));
        }
        Array<byte> wav;
        REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 1, kRate, wav));
        RefPtr<AudioClip> clip = MakeRef<AudioClip>(DefaultAllocator());
        AudioClipMetadata metadata;
        REQUIRE(ProbeAudioClipMetadata(Span<const byte>(wav.Data(), wav.Size()), metadata));
        clip->channels = metadata.channels;
        clip->sampleRate = metadata.sampleRate;
        clip->frameCount = metadata.frameCount;
        clip->durationSeconds = metadata.durationSeconds;
        clip->encodedData = wav;
        return clip;
    }

    struct SoundStage
    {
        AudioEngine engine{DefaultAllocator(), []
                           {
                               AudioEngineSettings settings;
                               settings.headless = true;
                               settings.dedupeWindowSeconds = 0.0f;
                               return settings;
                           }()};
        scene::Scene scene{DefaultAllocator(), u8"sounds"};
        RefPtr<AudioClip> line = Tone(2.0f);

        SoundStage()
        {
            RegisterAudioComponentReflection();
            scene.AddSystem<AudioSourceComponentManager>();
            scene.AddSystem<AudioListenerComponentManager>();
            scene.AddSystem<AudioReverbZoneComponentManager>();
            scene.AddSystem<AudioSceneSystem>()->SetEngine(&engine);
            scene.Start();
            scene.SetSimulationEnabled(true);
        }

        // The one voice playing (the track's); invalid when none.
        [[nodiscard]] VoiceHandle Voice() const
        {
            Array<VoiceHandle> voices;
            engine.ActiveVoices(voices);
            return voices.Size() == 1u ? voices[0] : VoiceHandle{};
        }
        [[nodiscard]] f32 Cursor() const
        {
            VoiceStatus status;
            REQUIRE(engine.GetVoiceStatus(Voice(), status));
            return status.cursorSeconds;
        }
        // Mixes `seconds` in small steps, as a frame loop would.
        void Mix(f32 seconds)
        {
            for (f32 t = 0.0f; t < seconds - 1.0e-4f; t += 0.05f)
            {
                engine.Update(0.05f);
            }
        }
        // Plays the sequence and the mixer on together.
        void Run(sequence::SequencePlay& play, f32 seconds)
        {
            for (f32 t = 0.0f; t < seconds - 1.0e-4f; t += 0.05f)
            {
                play.Advance(0.05f);
                engine.Update(0.05f);
            }
        }
    };

    // The line from 1 s on the timeline, from its start.
    sequence::Sequence Line(const RefPtr<AudioClip>& clip, f32 start = 1.0f)
    {
        RefPtr<SoundSequenceTrack> track = MakeRef<SoundSequenceTrack>(DefaultAllocator());
        SoundSection section;
        section.clip.SetDirect(clip);
        section.start = start;
        track->sections.PushBack(section);
        sequence::Sequence sequence;
        sequence.tracks.PushBack(track);
        sequence.duration = track->Duration();
        return sequence;
    }
}

TEST_CASE("sound track: a section's voice starts with it, part way in when entered part way")
{
    SoundStage stage;
    sequence::Sequence sequence = Line(stage.line);
    CHECK(sequence.duration == doctest::Approx(3.0f));
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    play.Play();
    stage.Run(play, 0.5f);
    CHECK_FALSE(stage.Voice().IsValid()); // before the section
    play.Seek(2.0f); // a second into the line
    REQUIRE(stage.Voice().IsValid());
    CHECK(stage.Cursor() == doctest::Approx(1.0f).epsilon(0.03));
    stage.Run(play, 0.5f);
    CHECK(stage.Cursor() == doctest::Approx(1.5f).epsilon(0.05));
    play.Stop();
    stage.engine.Update(0.05f);
    CHECK_FALSE(stage.Voice().IsValid());
}

TEST_CASE("sound track: a voice that wanders from the playhead is moved back")
{
    SoundStage stage;
    sequence::Sequence sequence = Line(stage.line, 0.0f);
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    play.Play();
    stage.Run(play, 0.5f);
    REQUIRE(stage.Voice().IsValid());
    stage.engine.SeekVoice(stage.Voice(), 1.5f); // a second ahead of the playhead
    play.Advance(0.05f);
    CHECK(stage.Cursor() == doctest::Approx(0.55f).epsilon(0.05));
}

TEST_CASE("sound track: the voice pauses with the play, and plays on at a hold")
{
    SoundStage stage;
    sequence::Sequence sequence = Line(stage.line, 0.0f);
    sequence.markers.PushBack(sequence::SequenceMarker{0.5f, String(u8"wait"), true});
    sequence::SequencePlay play(DefaultAllocator(), stage.scene, sequence);
    play.Play();
    stage.Run(play, 0.25f);
    play.Pause();
    stage.engine.Update(0.05f); // the declick lands
    const f32 pausedAt = stage.Cursor();
    stage.Mix(0.5f);
    CHECK(stage.Cursor() == doctest::Approx(pausedAt).epsilon(0.01));
    play.Resume();
    stage.Run(play, 0.5f); // reaches the hold at 0.5
    REQUIRE(play.IsHeld());
    const f32 heldAt = stage.Cursor();
    stage.Mix(0.5f);
    CHECK(stage.Cursor() > heldAt + 0.4f); // the line carries on while the game waits
}

TEST_CASE("sound track: a sequence reads its sound sections back by their kind")
{
    RegisterAudioComponentReflection();
    RefPtr<SoundSequenceTrack> track = MakeRef<SoundSequenceTrack>(DefaultAllocator());
    track->binding = 0;
    SoundSection section;
    section.clip.SetId(Guid{0x56, 0x78});
    section.start = 0.5f;
    section.length = 4.0f;
    section.offset = 0.25f;
    section.volume = 0.75f;
    section.bus = AudioBus::Music;
    section.loop = true;
    track->sections.PushBack(section);
    sequence::Sequence original;
    original.bindings.PushBack(sequence::SequenceBinding{String(u8"Speaker")});
    original.tracks.PushBack(track);

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        sequence::SerializeSequence(writer, original);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    sequence::Sequence read;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        sequence::SerializeSequence(reader, read);
    }
    REQUIRE(read.tracks.Size() == 1u);
    auto* sounds = Cast<SoundSequenceTrack>(read.tracks[0].Get());
    REQUIRE(sounds != nullptr);
    REQUIRE(sounds->sections.Size() == 1u);
    const SoundSection& back = sounds->sections[0];
    CHECK(back.clip.id == Guid{0x56, 0x78});
    CHECK(back.start == doctest::Approx(0.5f));
    CHECK(back.length == doctest::Approx(4.0f));
    CHECK(back.offset == doctest::Approx(0.25f));
    CHECK(back.volume == doctest::Approx(0.75f));
    CHECK(back.bus == AudioBus::Music);
    CHECK(back.loop);
    CHECK(sounds->Duration() == doctest::Approx(4.5f));
}
