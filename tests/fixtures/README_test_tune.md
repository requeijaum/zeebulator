# test_tune.mid

A tiny (62-byte), self-composed 4-note ascending arpeggio (C4-E4-G4-C5,
format 0, 480 ticks/quarter, default 120 BPM tempo) used to exercise the
`ParseMidi`/`RenderMidiToPcm` synth path / `IMedia` HLE / `Mixer` / real
SDL2 audio output end to end without needing any real game data. Entirely
our own original content (no copyright concern).

Regenerate with:

```python
import struct

def vlq(value):
    bytes_out = [value & 0x7F]
    value >>= 7
    while value > 0:
        bytes_out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(bytes_out)

division = 480
track = b''
notes = [60, 64, 67, 72]  # C4 E4 G4 C5
for n in notes:
    track += vlq(0) + bytes([0x90, n, 100])
    track += vlq(240) + bytes([0x80, n, 0])
track += vlq(0) + bytes([0xFF, 0x2F, 0x00])

mthd = b'MThd' + struct.pack('>I', 6) + struct.pack('>HHH', 0, 1, division)
mtrk = b'MTrk' + struct.pack('>I', len(track)) + track
open('test_tune.mid', 'wb').write(mthd + mtrk)
```
