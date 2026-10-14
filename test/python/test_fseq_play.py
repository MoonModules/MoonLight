# /// script
# dependencies = ["pytest"]
# ///
"""FSEQ reference sender tests: the file format FPP documents, and the DDP framing Network Receive parses."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "run"))

import fseq_play  # noqa: E402


def test_a_written_file_reads_back_as_uncompressed_v2(tmp_path):
    """The header carries the version, the channel count, the frame count and the step time a player needs."""
    path = tmp_path / "show.fseq"
    fseq_play.write_fseq(path, [bytes(12), bytes(12)], 50)
    info = fseq_play.read_header(path)
    assert info["version"] == "2.0"
    assert info["channels"] == 12 and info["frames"] == 2 and info["step_ms"] == 50
    assert info["compression"] == 0 and info["sparse_ranges"] == 0
    assert info["data_offset"] >= info["header_len"] and info["data_offset"] % 4 == 0


def test_frames_follow_the_header_at_the_data_offset(tmp_path):
    """A player seeks to the data offset and reads one channel count per frame."""
    path = tmp_path / "show.fseq"
    frames = [bytes([1, 2, 3]), bytes([4, 5, 6])]
    fseq_play.write_fseq(path, frames, 25)
    info = fseq_play.read_header(path)
    raw = path.read_bytes()[info["data_offset"]:]
    assert raw == b"".join(frames)


def test_a_file_that_is_not_fseq_is_refused(tmp_path):
    """Anything without the PSEQ identifier stops the tool rather than playing noise."""
    path = tmp_path / "not.fseq"
    path.write_bytes(b"GIF89a" + bytes(40))
    try:
        fseq_play.read_header(path)
    except SystemExit:
        return
    raise AssertionError("a non-FSEQ file was accepted")


def test_serpentine_wiring_reverses_every_other_row():
    """Wiring order is what xLights writes: row 1 of a serpentine panel runs right to left."""
    assert fseq_play.wiring_index(0, 0, 4, True) == 0
    assert fseq_play.wiring_index(3, 0, 4, True) == 3
    assert fseq_play.wiring_index(0, 1, 4, True) == 7
    assert fseq_play.wiring_index(3, 1, 4, True) == 4
    assert fseq_play.wiring_index(0, 1, 4, False) == 4


def test_the_sweep_marks_the_logical_corner_in_wiring_order():
    """The white light sits at logical (0, 0) wherever the wiring puts it, so a wrong mapping shows."""
    frame = fseq_play.sweep_frame(4, 2, 0, True)
    assert frame[0:3] == b"\xff\xff\xff"


def test_a_frame_becomes_ddp_packets_with_the_push_on_the_last():
    """1440-byte chunks, byte offsets, the version bits, and the push flag only on the final packet."""
    packets = fseq_play.ddp_packets(bytes(3000))
    assert [len(p) - fseq_play.DDP_HEADER for p in packets] == [1440, 1440, 120]
    assert [int.from_bytes(p[4:8], "big") for p in packets] == [0, 1440, 2880]
    assert all(p[0] & 0xC0 == 0x40 for p in packets)
    assert [p[0] & 0x01 for p in packets] == [0, 0, 1]
