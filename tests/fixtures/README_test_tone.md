# test_tone.wav

A short (0.5s), self-generated 440Hz sine wave, mono 16-bit PCM at
22050Hz (matching the real sample rate found in Double Dragon's actual
audio assets -- see TASKS.md Phase 6) with a linear fade-in/fade-out to
avoid clicks. Entirely our own synthesized content (no copyright
concern), used to exercise the WAV parser / `IMedia` HLE / `Mixer` /
real SDL2 audio output path end to end without needing any real game
data.

Regenerate with:

```python
import struct, math
sample_rate = 22050
freq = 440.0
duration = 0.5
n = int(sample_rate * duration)
samples = []
for i in range(n):
    t = i / sample_rate
    envelope = min(1.0, i / 200.0, (n - i) / 200.0)
    samples.append(int(12000 * envelope * math.sin(2 * math.pi * freq * t)))
pcm = b''.join(struct.pack('<h', s) for s in samples)
fmt_chunk = struct.pack('<HHIIHH', 1, 1, sample_rate, sample_rate * 2, 2, 16)
body = (b'fmt ' + struct.pack('<I', len(fmt_chunk)) + fmt_chunk +
        b'data' + struct.pack('<I', len(pcm)) + pcm)
riff = b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body
open('test_tone.wav', 'wb').write(riff)
```
