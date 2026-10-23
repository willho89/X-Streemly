#!/usr/bin/env python3
# X-Streemly — tests: the beeps in a harness WAV (HARNESS_WAV).
#
#   tests/tone.py <wav>
#
# Prints one line per beep in the left channel: when it starts (s), its
# frequency (Hz, from zero crossings) and its level (dBFS).
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1])
n, rate = w.getnframes(), w.getframerate()
left = struct.unpack("<%dh" % (n * w.getnchannels()), w.readframes(n))[0::w.getnchannels()]
hop, win = rate // 100, int(rate * 0.06)
i = 0
while i + hop + win <= n:
    if max(abs(x) for x in left[i:i + hop]) < 300:
        i += hop
        continue
    seg = left[i + hop // 2:i + hop // 2 + win]   # past the onset fade
    rms = math.sqrt(sum(x * x for x in seg) / win)
    zc = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
    print("%.2f %.0f %.1f" % (i / rate, zc / 2 / (win / rate), 20 * math.log10(rms / 32768 + 1e-12)))
    i += rate // 2
