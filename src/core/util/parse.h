#pragma once

/// @module parse
/// @also JsonUtil, MqttModule, HueDriver

/// @defgroup parse Reading a number from text
/// @{
/// The inverse of format.h: the integer a string starts with, from a JSON value, an MQTT payload or a query parameter alike.
///
/// @moreinfo
///
/// ## Overflow needs both checks
///
/// The conversion reports out of range rather than saturating, because a caller that narrows the result would otherwise store a different valid number.
/// Two checks are needed because they cover different targets.
/// On a desktop a huge value lands inside the wide type, so only the range compare rejects it.
/// On a device that compare is dead code, and the library's own saturation is the only signal.
/// Testing one alone passes on the desktop and silently returns the maximum on the target this exists to protect.
///
/// Trailing text is deliberately allowed, a value often being followed by a comma or a brace, so only the leading characters decide.
///
/// ## The conversion is out of line
///
/// As an inline its three checks were duplicated into every caller and cost 1712 bytes of flash on one chip, measured per symbol.
/// One call instead is free in practice, every user being off the hot path.

namespace mm {

/// The integer a string starts with, decimal or 0x-prefixed hex, or the fallback: @xref{overflow-needs-both-checks|both range checks} and @xref{the-conversion-is-out-of-line|why not inline}.
int parseIntStr(const char* s, int fallback = 0);

}  // namespace mm

/// @}
