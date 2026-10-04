#!/usr/bin/env python3
"""Makes Vexa's sample media (CC0, made here, nothing borrowed), with ffmpeg:

  rootfs/share/music/glass-morning.ogg   a little piece: soft bells over chords
  rootfs/share/videos/Vexa.mpg           a Mandelbrot zoom with that music (MPEG-1)
  tests/disk-content/media/              tones for the tests: 523 Hz MP3,
                                         659 Hz Ogg, 784 Hz FLAC, and a short
                                         MPEG-1 video with a 440 Hz tone

The files are kept in the repository; run this again only to change them."""
import math
import os
import struct
import subprocess
import tempfile
import wave

RATE = 44100


def write_wav(path, left, right):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for l, r in zip(left, right):
            frames += struct.pack("<hh", max(-32767, min(32767, int(l))), max(-32767, min(32767, int(r))))
        w.writeframes(bytes(frames))


def note(freq, start, length, volume, buffer, pan=0.5, bell=True):
    """A note added into buffer (a list of [l, r]): a few harmonics, a quick
    attack and a long fade (bell-like), or a slower swell (pads)."""
    first = int(start * RATE)
    count = int(length * RATE)
    for i in range(count):
        if first + i >= len(buffer):
            break
        t = i / RATE
        if bell:
            env = min(1, t / 0.01) * math.exp(-3.0 * t / length)
            v = (math.sin(2 * math.pi * freq * t) + 0.35 * math.sin(2 * math.pi * freq * 2 * t)
                 + 0.12 * math.sin(2 * math.pi * freq * 3.01 * t))
        else:
            env = min(1, t / 0.6) * min(1, (length - t) / 0.8)
            v = (math.sin(2 * math.pi * freq * t) + 0.5 * math.sin(2 * math.pi * freq * 1.002 * t)
                 + 0.2 * math.sin(2 * math.pi * freq * 2 * t))
        s = v * env * volume
        buffer[first + i][0] += s * (1 - pan)
        buffer[first + i][1] += s * pan


def freq(n):  # MIDI note number
    return 440 * 2 ** ((n - 69) / 12)


def piece():
    beat = 0.5
    chords = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]] * 2  # Am F C G
    melody = [76, 72, 74, 76, 77, 76, 72, 69, 72, 74, 71, 67, 69, 72, 76, 79,
              81, 79, 76, 74, 72, 74, 76, 72, 69, 72, 71, 74, 72, 69, 67, 69]
    seconds = len(chords) * 4 * beat + 3
    buffer = [[0.0, 0.0] for _ in range(int(seconds * RATE))]
    for c, chord in enumerate(chords):
        for k, n in enumerate(chord):
            note(freq(n), c * 4 * beat, 4 * beat + 0.6, 1800, buffer, pan=0.3 + 0.2 * k, bell=False)
        note(freq(chord[0] - 12), c * 4 * beat, 4 * beat, 2600, buffer, pan=0.5, bell=False)
    for i, n in enumerate(melody):
        note(freq(n), i * beat, beat * 3, 5200, buffer, pan=0.35 + 0.3 * ((i % 4) / 3))
        if i % 2 == 0:  # A quiet echo.
            note(freq(n), i * beat + 0.375, beat * 2, 1400, buffer, pan=0.75 - 0.3 * ((i % 4) / 3))
    return buffer


def tone(hz, seconds, path):
    count = int(seconds * RATE)
    s = [12000 * math.sin(2 * math.pi * hz * i / RATE) for i in range(count)]
    write_wav(path, s, s)


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *args], check=True)


root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
music = os.path.join(root, "rootfs/share/music/glass-morning.ogg")
video = os.path.join(root, "rootfs/share/videos/Vexa.mpg")
media = os.path.join(root, "tests/disk-content/media")
os.makedirs(os.path.dirname(music), exist_ok=True)
os.makedirs(os.path.dirname(video), exist_ok=True)
os.makedirs(media, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    wav = os.path.join(tmp, "piece.wav")
    buffer = piece()
    write_wav(wav, [b[0] for b in buffer], [b[1] for b in buffer])
    tags = ["-metadata", "title=Glass Morning", "-metadata", "artist=Vexa",
            "-metadata", "album=Sounds of Vexa"]
    ffmpeg("-i", wav, "-c:a", "libvorbis", "-q:a", "3", *tags, music)
    ffmpeg("-f", "lavfi", "-i", "mandelbrot=size=400x224:rate=24", "-i", wav, "-t", "12",
           "-vf", "format=yuv420p", "-c:v", "mpeg1video", "-q:v", "12", "-c:a", "mp2", "-b:a", "112k",
           "-ar", "44100", "-f", "mpeg", video)
    for hz, ext, codec in ((523, "mp3", ["-c:a", "libmp3lame", "-b:a", "96k"]),
                           (659, "ogg", ["-c:a", "libvorbis", "-q:a", "2"]),
                           (784, "flac", ["-c:a", "flac"])):
        t = os.path.join(tmp, f"tone{hz}.wav")
        tone(hz, 3, t)
        ffmpeg("-i", t, *codec, "-metadata", f"title=Tone {hz}", os.path.join(media, f"tone-{hz}.{ext}"))
    t = os.path.join(tmp, "tone440.wav")
    tone(440, 4, t)
    ffmpeg("-f", "lavfi", "-i", "testsrc=size=320x240:rate=25", "-i", t, "-t", "4",
           "-vf", "format=yuv420p", "-c:v", "mpeg1video", "-q:v", "8", "-c:a", "mp2", "-b:a", "96k",
           "-f", "mpeg", os.path.join(media, "test.mpg"))
