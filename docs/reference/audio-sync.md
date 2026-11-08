# Audio sync format

One board analyzes the music and the others follow it over UDP. A MoonLight board sends and reads two packets, defined here so another program can send to MoonLight or listen to it: the MoonLight format, which carries the whole analysis, and WLED's, which WLED and WLED-MM devices speak.

## Transport

| | |
|---|---|
| Protocol | UDP, one datagram per packet |
| Port | 11988 by default, the Audio card's `syncPort` |
| Addressing | multicast, the MoonLight format to `239.0.0.2` and WLED's to `239.0.0.1`; or unicast to the Audio card's `hosts` |
| Rate | at most one packet every 25 ms, about 40 a second |
| Byte order | little-endian for every multi-byte field |
| Size | at most 256 bytes, which a sender stays within and a receiver reads up to |

A sending board sends the format the Audio card's `syncFormat` picks: MoonLight's, WLED's, or both for an installation that mixes WLED devices with MoonLight boards. Each format has its own multicast group, so a WLED device never receives a packet it cannot read. A receiver joins both groups and tells the formats apart by the first bytes: `"ML"` is the MoonLight format, `"00002"` WLED's. Anything else is ignored. A board sending both sends each frame twice, so a receiver ignores WLED's packet while MoonLight's keeps arriving, and takes WLED's after a second without it.

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
| 4 | notes | 1 + count + 30 | the sender has `harmony` on |

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
| 11 + count | 10 | notesKept | a bit per key the sender's core keeps: outlasting an attack, neither an overtone nor a neighbor's leakage |
| 21 + count | 10 | notesKeptHits | a bit per kept key, set on the frame its strike counts |

A hit bit field is read least significant bit first: band or key *i* is bit `i % 8` of byte `i / 8`.

The sender keeps the notes once, so every receiver shows the notes it kept rather than deciding again from fewer frames: a packet carries the latest frame's kept keys and every kept key's strike since the last packet.

A frame with every section is 218 bytes at a 48 kHz sample rate, 90 bytes with `harmony` off, and 36 bytes with the core alone. The bass, mid and treble levels and the spectral flux do not travel: a receiver computes them from the thirds, or from the sixteen bands when no thirds came, as the sender did.

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
| Size | 36 to 218 bytes | 44 bytes |
| Bands | the sixteen, the 31 third octaves, the 79 keys | the sixteen |
| Hits | which drum, each band's and each key's | one peak flag |
| Harmony | the pitch classes, the key, the tuning, the bass note | none |
| Smoothing | the sender's, shared by every receiver | each receiver its own |
| Order | `seq`, late and repeated frames dropped | `frameCounter` in WLED-MM |
| Growth | new sections, skipped by older receivers | no spare byte |

## A proposal: the beat section

Lights on the beat need the tempo and where the beat is now, which onsets alone do not give. A board that tracks the beat itself disagrees with its neighbor by a few milliseconds and, after a track change, by seconds; a board receiving WLED's packet has no beat at all. So one board decides the beat and every board follows it, in section 5 of the MoonLight format. The section is a proposal, to agree with WLED-MM before either side builds it.

### Where the beat comes from

A sending board takes its beat from the first source it has:

1. **A Pioneer DJ deck**, over [Pro DJ Link](https://djl-analysis.deepsymmetry.org/djl-analysis/beats.html). The master deck knows the tempo and the beat in the bar exactly, since its track was analyzed beforehand.
2. **A MIDI clock or tap tempo**, as the [audio plan's step 6](../work/Plan-20261028%20-%20Audio%20that%20answers%20the%20music%20(4%20of%206).md#6-the-beat-lands-on-the-beat) lists them.
3. **The audio tracker**, step 6 of the same plan, which hears the beat but not which beat starts the bar.

A board that hears the decks itself reads them directly, since Pro DJ Link is broadcast already. The section carries the beat to every board that cannot hear the decks: WLED devices on the MoonLight format, boards on another subnet, and boards without a Pro DJ Link listener. It costs 8 bytes in a packet that goes out every frame anyway.

### The section

Bit 5, 8 bytes:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | bpm | the tempo × 100, uint16, the unit Pro DJ Link uses: 12800 is 128 BPM |
| 2 | 2 | beatCount | counts the beats since the source locked, uint16, wrapping; a change is a beat |
| 4 | 1 | phase | how far the current beat has run when the packet leaves, 0..255 |
| 5 | 1 | beatInBar | 0 for a beat whose place in the bar is unknown, else 1..4 |
| 6 | 1 | source | 0 the audio tracker, 1 Pro DJ Link, 2 a MIDI clock, 3 tap tempo |
| 7 | 1 | confidence | 0..255: the tracker's lock score, 255 for a deck or a clock |

A sender without a tempo leaves the section out, so an absent section means "no beat", and a sender never sends a bpm of 0.

`beatInBar` 0 for a beat and 1 to 4 for a deck's position in the bar is WLED-MM's convention. The audio tracker sends 0 until it finds the downbeat.

### What a receiver does

- A change of `beatCount` is a beat: a receiver takes it once, by the number, the way it takes a frame's hits, and a jump of more than one counts the beats it missed.
- Between packets a receiver runs its own beat clock: the phase advances by `bpm` and the time since the packet arrived, and each packet pulls it back toward the phase the packet carries, as a phase-locked loop does, which smooths WiFi's 5 to 20 ms of jitter.
- `phase` holds when the packet leaves, so the sender compensates its source's delay. A Pro DJ Link beat packet reaches a board about 40 ms after the deck's audio plays (troyhacks measured it on a CDJ at 118 BPM), so a sender reading the decks moves the phase forward by that delay.
- The section's length byte allows fields appended later, such as the beat in the phrase, which a receiver reading the first 8 bytes ignores.

### Reading Pro DJ Link

What a sender reads, from the packet analysis above and troyhacks's implementation in WLED-MM:

| Port | Packet | What it gives |
|---|---|---|
| 50000 | keep-alive, type `0x06` | the devices on the network; a listener announces itself here every 1.5 s, as a device number of its own, so the decks send it their status |
| 50001 | beat, type `0x28`, broadcast on each beat | the player at `0x21`; the track's tempo × 100 at `0x5A`, big-endian; the pitch as a speed factor at `0x54`, a big-endian uint32 where `0x100000` is 1.0; the beat in the bar, 1..4, at `0x5C` |
| 50002 | status, type `0x0A` | the player at `0x21`; the master flag, bit `0x20` of `0x89` |

The tempo is the track's tempo times the speed. Only the master deck's beats count; a beat packet's arrival time anchors the beat clock, which runs free between packets and stops after 4 beats without one.

## A proposal for WLED

WLED's packet has served a large installed base for years, and a WLED device listening to a MoonLight board, or the other way round, already works. The proposal below adds what a modern analysis knows without changing a byte an existing device reads.

### Why it is worth doing

- **A dropped or doubled packet double-fires a peak.** Multicast over WiFi is unacknowledged, so packets arrive late, twice or not at all. Upstream WLED has no frame counter to tell; WLED-MM added one at offset 17 and drops out-of-order frames with it, which proves the need and the fix.
- **There is no room left to grow.** Every byte of the 44 is taken once WLED-MM's fields count, so each new field means a new version and a flag day. A receiver that skips what it does not know ends that: a field is added without breaking anyone.
- **One flag says nothing about which drum.** An effect that answers the kick and ignores the hi-hat needs the range of the hit.
- **No beat travels.** A device receiving v2 has no tempo, so every device tracks the beat on its own or not at all. The beat section gives every device the beat one board decided, a Pioneer deck's among them.
- **Every receiver smooths on its own.** `sampleSmth` is shared, but the bands' smoothing is each receiver's, so two devices on one sender drift apart visually. Sending the sender's smoothed bands makes every device show the same frame.
- **Floats carry bytes.** `sampleRaw` and `sampleSmth` hold 0..255 values in 4-byte floats. In bytes, everything v2 carries plus the gate and which drum fits a 36-byte packet, against v2's 44, and reads the same on every platform.
- **Sixteen bands is a ceiling.** Third octaves (31 bands) and semitones (79 keys) open note-accurate effects; the sixteen stay for every effect written against them.
- **It stays small.** A full frame is under 256 bytes, far below one Ethernet frame, so it never fragments.
- **One format for one community.** WLED, WLED-MM and MoonLight all come from the same sound-reactive lineage. One packet lets any of them drive any other with nothing lost.

### The proposal: v2 as it is, and the MoonLight format beside it

1. **v2 stays exactly as it is**, on `239.0.0.1`. Every WLED device reads what it reads now, and a sender keeps sending it while such devices listen.
2. **A sender adds the MoonLight format on `239.0.0.2`**, the same port: a header with a version, a 16-bit frame number and a section mask, then length-prefixed sections a receiver skips when it does not know them. A WLED sender sends the core, 36 bytes, every field of it one WLED fills or defines as unmeasured, plus the sections it has data for. WLED-MM's 31 bands are the thirds section as it is, its tempo tracker and Pro DJ Link reader fill the beat section, and its `pressure` and `zeroCrossingCount` become a section of their own.
3. **A receiver that knows the format joins both groups** and prefers the MoonLight packet while it arrives, as MoonLight does. An old device never joins `239.0.0.2`, so it never sees the new packet.

What it brings over v2: a frame number that rolls over every 27 minutes instead of every 6 seconds, the hits of each range, band and key, the third octaves and the keys, the harmony, the beat, the sender's smoothing, bytes instead of floats, and room for the next field without a new version.

What it costs: a sender sends two packets a frame while old devices listen, under 10 KB/s at 40 frames a second, and only updated receivers read the new one.
