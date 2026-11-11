#!/usr/bin/env python3
"""The APU test's sound (xbox-core --wav): 1 kHz at amplitude 0.5; 500 Hz, 2 kHz (ADPCM), 3 kHz
(streamed) and 4 kHz (an ADPCM stream from the program's own memory) at 0.25.
python3 check.py sound.wav"""
import sys
import wave

import numpy as np

w = wave.open(sys.argv[1])
pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2) / 32768.0
left = pcm[:, 0]
# a second from where the sound starts
start = int(np.argmax(np.abs(left) > 0.05))
part = left[start + 4800:start + 4800 + 48000]
spectrum = np.abs(np.fft.rfft(part * np.hanning(len(part)))) / (len(part) / 4)
def amplitude(hz):
    return spectrum[hz - 3:hz + 4].max()
failures = 0
for hz, want, tolerance in [(1000, 0.5, 0.05), (500, 0.25, 0.05), (2000, 0.25, 0.1), (3000, 0.25, 0.05), (4002, 0.25, 0.1)]:
    got = amplitude(hz)
    ok = abs(got - want) < want * tolerance
    print(f"{'PASS' if ok else 'FAIL'} {hz} Hz at {got:.3f} (expected {want})")
    failures += not ok
other = spectrum.copy()
for hz in (500, 1000, 2000, 3000, 4002):
    other[hz - 10:hz + 11] = 0
ok = other.max() < 0.02  # (ADPCM's own noise stays far below)
print(f"{'PASS' if ok else 'FAIL'} nothing else louder than 0.02 ({other.max():.4f} at {other.argmax()} Hz)")
failures += not ok
print(f"APU sound: {failures} failures")
sys.exit(1 if failures else 0)
