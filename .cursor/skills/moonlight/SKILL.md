---
name: moonlight
description: >-
  MoonLight control help. Every slider, switch and other control that
  defineControls publishes needs a /// comment on its member, which is the
  Help line under the control. Use when adding or changing MoonLight controls,
  sliders, toggles, header comments, or the effect card.
---

# MoonLight controls and Help

The page has no hover tooltip on a control. **Help** shows a line under the field. That line is the first `///` on the member the `addControl` (or `addSelect`, `addTextArea`) call names. `gen_help.py` reads that comment. A control with no `///` shows its name alone.

Put the comment on the member, not only above the `add` call. A `/// @guide` line under it is the longer help, and it opens from that line.

The line says what moving the control does. Leave out the default and the range. The reset button and the slider show those. On an effect, modifier or layout the whole `` `name`: line `` is at most 80 characters. The rule is in `docs/contributing/documentation-standards.md`.
