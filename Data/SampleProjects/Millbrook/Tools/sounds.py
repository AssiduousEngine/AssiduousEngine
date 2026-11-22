#!/usr/bin/env python3
"""sounds.py: Millbrook's sounds and its tune, generated here (no third-party content, as
Masquerade's are) and imported through the editor's MCP into Audio/.

- Audio/Rumble: a cart's wheels on the cobbles, three seconds.
- Audio/Splash: water off the waterwheel.
- Audio/Knock: two knocks, a sign hung on its hook.
- Audio/Meow: a small cat.
- Audio/Chime: the fair opening, three bells.
- Audio/FairTune: the fair's music, a jig in G on a plucked fiddle and a whistle, looping.

The conversations name them through the dialogue assets (dialogue.py).
"""
import math, os, random, struct, sys, wave
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

RATE = 22050
OUT = os.path.join(HERE, "generated", "Sounds")
TWO_PI = 2.0 * math.pi


def write(path, samples, level=0.8):
    peak = max(1e-6, max(abs(v) for v in samples))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v / peak * level * 32767)) for v in samples))


def lowpass(samples, k):
    out, y = [], 0.0
    for v in samples:
        y += (v - y) * k
        out.append(y)
    return out


def freq(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def silence(seconds):
    return [0.0] * int(seconds * RATE)


def rumble():
    rnd = random.Random(1)
    n = int(3.0 * RATE)
    noise = lowpass([rnd.uniform(-1, 1) for _ in range(n)], 0.04)
    out = []
    for i, v in enumerate(noise):
        t = i / RATE
        clack = 1.0 + 0.8 * max(0.0, math.sin(TWO_PI * 3.2 * t)) ** 12  # the wheels over the stones
        env = min(1.0, t / 0.3) * min(1.0, (3.0 - t) / 0.5)
        out.append(v * clack * env)
    return out


def splash():
    rnd = random.Random(2)
    n = int(1.4 * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * 4.0) * min(1.0, t / 0.02)
        bubble = 0.3 * math.sin(TWO_PI * (500 + 900 * math.exp(-t * 6)) * t) * math.exp(-t * 9)
        out.append(rnd.uniform(-1, 1) * env + bubble)
    return lowpass(out, 0.5)


def knock():
    out = silence(0.7)
    for at in (0.05, 0.3):
        start = int(at * RATE)
        for i in range(int(0.12 * RATE)):
            t = i / RATE
            out[start + i] += math.sin(TWO_PI * 180 * t) * math.exp(-t * 40) + 0.4 * math.sin(TWO_PI * 410 * t) * math.exp(-t * 60)
    return out


def meow():
    n = int(0.7 * RATE)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        u = t / 0.7
        f = 520 + 380 * math.sin(math.pi * min(1.0, u * 1.2)) - 120 * u  # up, then down
        phase += TWO_PI * f / RATE
        env = min(1.0, t / 0.05) * min(1.0, (0.7 - t) / 0.2)
        voice = math.sin(phase) + 0.5 * math.sin(2 * phase) + 0.25 * math.sin(3 * phase)
        out.append(voice * env * (0.6 + 0.4 * math.sin(TWO_PI * 7 * t)))
    return lowpass(out, 0.35)


def chime():
    out = silence(2.6)
    for k, note in enumerate((79, 83, 86)):
        start = int(k * 0.25 * RATE)
        f = freq(note)
        for i in range(len(out) - start):
            t = i / RATE
            out[start + i] += (math.sin(TWO_PI * f * t) + 0.3 * math.sin(TWO_PI * 2.76 * f * t)) * math.exp(-t * 2.2) * 0.6
    return out


def pluck(out, at, midi, seconds, amp):
    start = int(at * RATE)
    f = freq(midi)
    for i in range(int(seconds * RATE)):
        if start + i >= len(out):
            break
        t = i / RATE
        out[start + i] += amp * math.exp(-t * 5.0) * (math.sin(TWO_PI * f * t) + 0.4 * math.sin(TWO_PI * 2 * f * t)
                                                       + 0.2 * math.sin(TWO_PI * 3 * f * t))


def whistle(out, at, midi, seconds, amp):
    start = int(at * RATE)
    f = freq(midi)
    for i in range(int(seconds * RATE)):
        if start + i >= len(out):
            break
        t = i / RATE
        env = min(1.0, t / 0.03) * min(1.0, (seconds - t) / 0.06)
        vib = 1.0 + 0.004 * math.sin(TWO_PI * 5.5 * t)
        out[start + i] += amp * env * math.sin(TWO_PI * f * vib * t)


def fair_tune():
    """A jig in G, 6/8 at 116 bars a minute's worth of eighths, eight bars, looping."""
    eighth = 60.0 / 116 / 2
    bars = 8
    out = silence(bars * 6 * eighth)
    tune = [67, 71, 74, 71, 67, 71, 72, 76, 79, 76, 72, 76, 74, 78, 81, 78, 74, 78, 79, 74, 71, 67, 71, 74,
            67, 71, 74, 71, 67, 71, 72, 76, 79, 76, 72, 76, 74, 72, 71, 69, 71, 72, 74, 71, 69, 67, 67, 67]
    roots = [55, 60, 62, 55, 55, 60, 62, 55]
    for i, note in enumerate(tune):
        whistle(out, i * eighth, note + 12, eighth * 0.92, 0.32)
    for b, root in enumerate(roots):
        for k in (0, 3):
            pluck(out, (b * 6 + k) * eighth, root, eighth * 3, 0.5)
            pluck(out, (b * 6 + k + 1) * eighth, root + 7, eighth * 2, 0.25)
            pluck(out, (b * 6 + k + 2) * eighth, root + 12, eighth * 2, 0.2)
    return out


SOUNDS = (("Rumble", rumble), ("Splash", splash), ("Knock", knock), ("Meow", meow), ("Chime", chime),
          ("FairTune", fair_tune))


def main():
    os.makedirs(OUT, exist_ok=True)
    only = set(sys.argv[1:])
    for name, make in SOUNDS:
        if only and name not in only:
            continue
        path = os.path.join(OUT, name + ".wav")
        write(path, make())
        r = mcp("asset_import", {"source": path, "group": "Audio"})
        print("Audio/" + name, r["guid"])
    print(mcp("asset_cook", {}))


main()
