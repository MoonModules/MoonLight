# Audio sync format

One board analyzes the music and the others follow it over UDP. A MoonLight board sends and reads two packets, defined here so another program can send to MoonLight or listen to it: the MoonLight format, which carries the whole analysis, and WLED's, which WLED and WLED-MM devices speak.

## Transport

| | |
|---|---|
| Protocol | UDP, one datagram per packet |
| Port | 11988 by default, the Audio card's `syncPort` |
| Addressing | multicast to `239.0.0.1`, or unicast to the Audio card's `hosts` |
| Rate | at most one packet every 25 ms, about 40 a second |
| Byte order | little-endian for every multi-byte field |
| Size | at most 256 bytes, which a sender stays within and a receiver reads up to |

A sending board sends one format, the Audio card's `syncFormat`, never both. A receiver tells them apart by the first bytes: `"ML"` is the MoonLight format, `"00002"` WLED's. Anything else is ignored.

The hits of every analyzed frame between two packets ride in the next one: the strongest onset per range, and every band's and note's hit, so a receiver at 40 packets a second misses none of the 47 frames a second the analysis makes at 24 kHz.

## The MoonLight format

### Header

Eight bytes.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 2 | magic | `'M'`, `'L'` (0x4D 0x4C) |
| 2 | 1 | version | 1; a receiver refuses another version |
| 3 | 1 | flags | 0; none are defined |
| 4 | 2 | seq | the sender's frame number, uint16, wrapping |
| 6 | 2 | section mask | uint16, bit *n* set when section *n* follows |

### Sections

After the header come the sections whose mask bit is set, in bit order. Each is one length byte, the count of bytes that follow it, then those bytes.

| Bit | Section | Length | Sent when |
|---|---|---|---|
| 0 | core | 27 | always |
| 1 | smoothed bands | 16 | the sender smooths its bands |
| 2 | thirds | 1 + count + 4 | the sender measured third-octave bands |
| 3 | harmony | 16 | the sender has `harmony` on |
| 4 | notes | 1 + count + 10 | the sender has `harmony` on |

A section is there when the sender measured what it holds, so an absent section means "not measured", never zeros: a sender with no smoothing, no thirds or no harmony leaves that section out.

**Core**, 27 bytes:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | level | the level's place in its own dynamics window, 0..255, 0 while gated |
| 1 | 1 | levelSmoothed | `level` through a peak meter's ballistic, fast rise and slow fall |
| 2 | 1 | gate | where the noise gate opens on the bands' 0..255 scale, at least 1; 0 when none was measured |
| 3 | 1 | gateOpen | 1 while sound is over the gate, else 0 |
| 4 | 1 | onset | 0, or the strength 1..255 of a drum heard, that of the range heard first: the kick, the snare or a hat, counted once |
| 5 | 1 | onsetLow | 0, or the strength of a hit in the bass, 20 to 125 Hz: the kick |
| 6 | 1 | onsetMid | the same for the mids, 160 Hz to 2 kHz: the snare, the toms |
| 7 | 1 | onsetHigh | the same for the treble, 2.5 kHz up: the hats, the cymbals |
| 8 | 2 | peakHz | the dominant frequency in Hz, uint16, 0 for none |
| 10 | 1 | peakMag | that frequency's magnitude, 0..255 |
| 11 | 16 | bands | sixteen third-octave bands, 0..255, raw, at 40, 63, 80, 125, 160, 250, 400, 500, 800, 1000, 1600, 2000, 3150, 5000, 6300 and 10000 Hz |

A sender that measures no gate sends `gate` 0, and one that detects no drums sends the four onsets 0; a receiver then reads sound from `levelSmoothed` and sees no hits, which is what those values mean.

**Smoothed bands**, 16 bytes: `bandsSmoothed`, the sixteen bands under the peak meter's ballistic (IEC 60268-10), 0..255.

**Thirds**:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | count | how many bands follow, at most 31; 28 at a 24 kHz sample rate |
| 1 | count | thirds | the third-octave bands from 20 Hz up, on the ISO 266 centers (20, 25, 31.5 … 20000 Hz), 0..255 |
| 1 + count | 4 | thirdsHits | a bit per band, set on a band's hit |

**Harmony**, 16 bytes:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 12 | chroma | the twelve pitch classes, C first, 0..255 against the loudest over the last 200 ms |
| 12 | 1 | key | the key over the last 15 s: 0 none yet, 1..12 C to B major, 13..24 C to B minor |
| 13 | 1 | tuning | int8, how far the music is tuned from A440, in cents |
| 14 | 1 | harmonyChange | 1 on a frame whose harmony moved, a chord changing |
| 15 | 1 | bassNote | the bass line's note as a MIDI number (A4 = 69), 0 while none stands out |

**Notes**:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | count | how many keys follow, at most 79 |
| 1 | count | notes | one band a semitone from A0 (MIDI 21) up, 0..255 on the bands' scale |
| 1 + count | 10 | notesHits | a bit per key, set on a key's attack |

A hit bit field is read least significant bit first: band or key *i* is bit `i % 8` of byte `i / 8`.

A frame with every section is 198 bytes at a 48 kHz sample rate, 90 bytes with `harmony` off, and 36 bytes with the core alone. The bass, mid and treble levels and the spectral flux do not travel: a receiver computes them from the thirds, or from the sixteen bands when no thirds came, as the sender did.

### What a receiver does

- **Refuses** a packet shorter than the header, with another magic or version, without the core section, with a section running past the datagram, or with a count past its array.
- **Skips** a section whose bit it does not know, by its length byte, and reads the known start of a section longer than it expects. A later version adds a field as a new section or at the end of one, so an older receiver keeps working.
- **Drops** a frame whose `seq` is the last one taken or fewer than 64 behind it: a repeated or duplicated packet, since a sender with no new frame repeats its last, or a late one, whose hits ride in the next frame taken since they went in no other packet. A `seq` further behind is a sender that restarted, and is taken. After a second without packets, any `seq` is taken.
- **Keeps** the sender's `levelSmoothed`, and its `bandsSmoothed` when the section came, so every board shows the same frame; without the section it smooths the bands itself, as it does for a WLED packet.

### An example

A frame with `level` 100, `levelSmoothed` 50, `gate` 40 and open, a kick of strength 7, `peakHz` 1000, `peakMag` 25, the bands 0 to 15, nothing smoothed yet, `seq` 0x0102, and no thirds or harmony: 53 bytes.

```text
4D 4C 01 00 02 01 03 00                      header: "ML", v1, flags, seq 0x0102, mask 0x0003
1B                                           core length 27
64 32 28 01                                  level, levelSmoothed, gate, gateOpen
07 07 00 00                                  onset, onsetLow, onsetMid, onsetHigh
E8 03 19                                     peakHz 1000, peakMag 25
00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F   bands
10                                           smoothed bands length 16
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   bandsSmoothed
```

Its source of truth is [MoonLightAudioSyncPacket.h](https://github.com/MoonModules/MoonLight/blob/main/src/core/util/MoonLightAudioSyncPacket.h), and the unit test `unit_MoonLightAudioSyncPacket.cpp` pins these bytes.

## The WLED format

WLED's audio sync version 2, 44 bytes, defined by the `audioSyncPacket` struct in [WLED's audioreactive usermod](https://github.com/wled/WLED/blob/fffa05a765d41d7126b66ab5a3d1ce1f41e7d11e/usermods/audioreactive_v1/audio_reactive.cpp#L800-L813) and [WLED-MM's](https://github.com/MoonModules/WLED-AudioReactive-Usermod/blob/a4338f2ae26e303653c8fffbbb9af975401cefc5/audio_reactive.h#L1351-L1363), which fills WLED's reserved bytes.

| Offset | Size | WLED | WLED-MM | What MoonLight sends | What MoonLight reads |
|---|---|---|---|---|---|
| 0 | 6 | header `"00002\0"` | the same | the same | refuses another |
| 6 | 2 | reserved | `pressure`, 8.8 fixed point | 0 | ignored |
| 8 | 4 | `sampleRaw`, float | the same | `level` | `level` |
| 12 | 4 | `sampleSmth`, float | the same | `levelSmoothed` | `levelSmoothed` |
| 16 | 1 | `samplePeak`, nonzero on a peak | the same | bit 0 on a drum hit, with bits 1, 2, 3 for the kick, the snare and the hats beside it | any nonzero value as `onset`; bits 1 to 3 as the ranges |
| 17 | 1 | reserved | `frameCounter` | the low byte of `seq` | ignored |
| 18 | 16 | `fftResult`, the sixteen GEQ bands | the same | `bands`, clamped to 254 | `bands` |
| 34 | 2 | reserved | `zeroCrossingCount` | 0 | ignored |
| 36 | 4 | `FFT_Magnitude`, float, up to about 4096 | the same | `peakMag` × 16 | ÷ 16, clamped to 255 |
| 40 | 4 | `FFT_MajorPeak`, float, Hz | the same | `peakHz` | `peakHz` |

WLED reads any nonzero `samplePeak` as a peak, so the drum bits are sent only beside bit 0, and a WLED device sees the peak it always saw. The price is a range heard a block after its drum's first range: its bit does not go out, so a MoonLight board receiving the WLED format can miss a kick whose hat came first. The MoonLight format carries every range's hit. Both upstream sources reserve `samplePeak` for a peak magnitude in a later version; a sender that adopts it would read in MoonLight as drum bits.

## The two side by side

| | MoonLight | WLED |
|---|---|---|
| Size | 36 to 198 bytes | 44 bytes |
| Bands | the sixteen, the 31 third octaves, the 79 keys | the sixteen |
| Hits | which drum, each band's and each key's | one peak flag |
| Harmony | the pitch classes, the key, the tuning, the bass note | none |
| Smoothing | the sender's, shared by every receiver | each receiver its own |
| Order | `seq`, late and repeated frames dropped | `frameCounter` in WLED-MM |
| Growth | new sections, skipped by older receivers | no spare byte |

## A proposal for WLED

WLED's packet has served a large installed base for years, and a WLED device listening to a MoonLight board, or the other way round, already works. The proposal below is how the WLED format could carry what a modern analysis knows, in two stages, the first of which no existing device notices.

### Why it is worth doing

- **A dropped or doubled packet double-fires a peak.** Multicast over WiFi is unacknowledged, so packets arrive late, twice or not at all. Upstream WLED has no frame counter to tell; WLED-MM added one at offset 17 and drops out-of-order frames with it, which proves the need and the fix.
- **There is no room left to grow.** Every byte of the 44 is taken once WLED-MM's fields count, so each new field means a new version and a flag day. A receiver that skips what it does not know ends that: a field is added without breaking anyone.
- **One flag says nothing about which drum.** An effect that answers the kick and ignores the hi-hat needs the range of the hit. MoonLight's drum bits already ride in `samplePeak` without a WLED receiver noticing.
- **Every receiver smooths on its own.** `sampleSmth` is shared, but the bands' smoothing is each receiver's, so two devices on one sender drift apart visually. Sending the sender's smoothed bands makes every device show the same frame.
- **Floats carry bytes.** `sampleRaw` and `sampleSmth` hold 0..255 values in 4-byte floats. In bytes, everything v2 carries plus the gate and which drum fits a 36-byte packet, against v2's 44, and reads the same on every platform.
- **Sixteen bands is a ceiling.** Third octaves (31 bands) and semitones (79 keys) open note-accurate effects; the sixteen stay for every effect written against them.
- **It stays small.** A full frame is under 200 bytes, far below one Ethernet frame, so it never fragments, and 40 a second is 8 KB/s.
- **One format for one community.** WLED, WLED-MM and MoonLight all come from the same sound-reactive lineage. One packet lets any of them drive any other with nothing lost.

### Option 1: compatible, old devices keep working

Every step below is invisible to a WLED device that is never updated: an old receiver reads exactly what it reads now.

1. **Adopt WLED-MM's use of the reserved bytes upstream**: `pressure` at offset 6, `frameCounter` at 17, `zeroCrossingCount` at 34. An old receiver ignores them; an updated one drops a late or doubled packet by `frameCounter`, which WLED-MM already does.
2. **Define `samplePeak`'s bits** as MoonLight sends them: bit 0 any peak, bits 1, 2 and 3 the bass, the mids and the treble, set only beside bit 0. An old receiver still sees a nonzero peak, and an updated one knows the drum. This claims the byte both upstream sources reserve for a future peak magnitude, so the decision is now.
3. **Accept a v2 packet of 44 bytes or more**, reading the first 44. Both upstream receivers refuse any other length. Once accepting longer packets is common, a sender can append fields to the v2 packet without a new header.
4. **Read a second, richer packet when one arrives.** A sender that opts in sends a v3 packet (Option 2) in place of v2, chosen per sender as MoonLight's `syncFormat` does. Receivers that know v3 read either; an installation with old devices keeps its senders on v2.

### Option 2: the full frame, a flash on both ends

A version 3 with MoonLight's shape: a header with a version, a 16-bit frame number and a section mask, then length-prefixed sections a receiver skips when it does not know them. The MoonLight format above is a working implementation, and WLED could adopt it as is, or as v3 with its own magic. A WLED sender sends the core alone, 36 bytes, every field of it one WLED fills or defines as unmeasured, and leaves out the sections it has no data for; WLED-MM's `pressure` and `zeroCrossingCount` become a section of their own.

What it brings over v2: a frame number that rolls over every 27 minutes instead of every 6 seconds, the hits of each range, band and key, the third octaves and the keys, the harmony, the sender's smoothing, bytes instead of floats, and room for the next field without a new version. A beat section (tempo, phase, downbeat) is the next one MoonLight adds.

What it costs: both ends need the new firmware to use it, and a sender feeding old devices keeps sending v2, which is why Option 1's step 4 lets the two coexist during the move.
