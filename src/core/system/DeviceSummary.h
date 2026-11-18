#pragma once

/// The facts about what this device runs that both its `/api/system` summary and its own row on the network map carry, written in one place so the two cannot differ.

namespace mm {

class JsonSink;

/// Append `,"mode":N`, and `,"moonbaseMismatch":"…"` and `,"coprocessorMismatch":"…"` while each image is behind the app: the reader's level, developer following `latest`, and what a peer's map marks.
void writeDeviceFacts(JsonSink& sink);

}  // namespace mm
