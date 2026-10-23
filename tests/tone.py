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
    while abs(left[i]) < 300:   # the beep's first loud sample
        i += 1
    seg = left[i + rate // 500:i + rate // 500 + win]   # past the onset fade
    rms = math.sqrt(sum(x * x for x in seg) / win)
    # Crossings with hysteresis, so low-level noise around zero adds none.
    zc, sign = 0, 0
    for x in seg:
        s = 1 if x > 200 else -1 if x < -200 else sign
        zc += sign != 0 and s != sign
        sign = s
    print("%.2f %.0f %.1f" % (i / rate, zc / 2 / (win / rate), 20 * math.log10(rms / 32768 + 1e-12)))
    i += rate // 2
