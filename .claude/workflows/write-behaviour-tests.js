export const meta = {
  name: 'write-behaviour-tests',
  description: 'Write behaviour-specific unit tests for the 24 untested effects/modifiers',
  phases: [
    { title: 'Study + write', detail: 'one agent per module: read the .h, write a faithful behaviour test' },
  ],
}

// The 24 modules with no unit test (the broken effects.md/modifiers.md [Tests] links).
// Each entry: class name, header path, test-file destination, kind (effect|modifier).
const MODULES = [
  ['BlurzEffect',          'src/light/effects/BlurzEffect.h',          'effect'],
  ['BouncingBallsEffect',  'src/light/effects/BouncingBallsEffect.h',  'effect'],
  ['FixedRectangleEffect', 'src/light/effects/FixedRectangleEffect.h', 'effect'],
  ['FreqMatrixEffect',     'src/light/effects/FreqMatrixEffect.h',     'effect'],
  ['FreqSawsEffect',       'src/light/effects/FreqSawsEffect.h',        'effect'],
  ['GEQ3DEffect',          'src/light/effects/GEQ3DEffect.h',          'effect'],
  ['GEQEffect',            'src/light/effects/GEQEffect.h',            'effect'],
  ['LissajousEffect',      'src/light/effects/LissajousEffect.h',      'effect'],
  ['Noise2DEffect',        'src/light/effects/Noise2DEffect.h',        'effect'],
  ['NoiseMeterEffect',     'src/light/effects/NoiseMeterEffect.h',     'effect'],
  ['PaintBrushEffect',     'src/light/effects/PaintBrushEffect.h',     'effect'],
  ['PraxisEffect',         'src/light/effects/PraxisEffect.h',         'effect'],
  ['RandomEffect',         'src/light/effects/RandomEffect.h',         'effect'],
  ['RubiksCubeEffect',     'src/light/effects/RubiksCubeEffect.h',     'effect'],
  ['SolidEffect',          'src/light/effects/SolidEffect.h',          'effect'],
  ['SphereMoveEffect',     'src/light/effects/SphereMoveEffect.h',     'effect'],
  ['StarFieldEffect',      'src/light/effects/StarFieldEffect.h',      'effect'],
  ['StarSkyEffect',        'src/light/effects/StarSkyEffect.h',        'effect'],
  ['TetrixEffect',         'src/light/effects/TetrixEffect.h',         'effect'],
  ['BlockModifier',        'src/light/modifiers/BlockModifier.h',      'modifier'],
  ['CircleModifier',       'src/light/modifiers/CircleModifier.h',     'modifier'],
  ['MirrorModifier',       'src/light/modifiers/MirrorModifier.h',     'modifier'],
  ['RippleXZModifier',     'src/light/modifiers/RippleXZModifier.h',   'modifier'],
  ['TransposeModifier',    'src/light/modifiers/TransposeModifier.h',  'modifier'],
]

const RESULT = {
  type: 'object',
  properties: {
    module: { type: 'string' },
    test_file: { type: 'string' },
    behaviours_pinned: { type: 'array', items: { type: 'string' },
      description: 'one line per TEST_CASE: what real behaviour it asserts' },
    wrote: { type: 'boolean', description: 'true if the test file was written' },
    notes: { type: 'string', description: 'anything the caller should know (e.g. audio-driven, needs a fed AudioFrame; or a behaviour that could not be pinned)' },
  },
  required: ['module', 'test_file', 'wrote'],
}

phase('Study + write')

const results = await parallel(MODULES.map(([cls, header, kind]) => () =>
  agent(
`Write a behaviour-specific doctest unit test for the MoonLight module **${cls}** (${kind}).

Repo: the current workspace root (the MoonLight checkout you're running in) — all paths below are relative to it.

## Study first (do NOT guess behaviour)
1. Read the module header: ${header} — understand what it ACTUALLY does: its controls, its render/modify logic, what it writes to the buffer or how it transforms coordinates. Behaviour is the spec.
2. Read the module's spec entry if useful: docs/moonmodules/light/${kind === 'effect' ? 'effects.md' : 'modifiers.md'} (find the ${cls.replace(/Effect$|Modifier$/, '')} section).
3. Read TWO existing tests as your pattern templates — match their idiom EXACTLY (includes, harness, naming, comment style):
   - For an EFFECT: test/unit/light/unit_RainbowEffect.cpp (Layouts→GridLayout→Layer→addChild(effect)→prepare()→tick()→assert on layer.buffer()).
   - For a MODIFIER: test/unit/light/unit_RegionModifier.cpp (call modifyLogical / modifyLogicalSize directly; assert coordinate folding / size).
   Pick the one matching this module's kind (${kind}).

## Write the test
- Destination: test/unit/light/unit_${cls}.cpp
- First line MUST be: \`// @module ${cls}\`  (this is what the doc generator + MoonDeck read — the whole point is that this module becomes a documented, tested module). Add \`// @also X, Y\` only if the test genuinely also exercises another module.
- Each TEST_CASE gets a single \`//\` comment line ABOVE it describing the behaviour it pins (the generator turns that into the doc description). Write real, present-tense descriptions.
- Assert REAL BEHAVIOUR, not just "renders non-zero". Examples of the bar:
  - SolidEffect → the whole buffer is ONE uniform color (every light equals the configured color).
  - FixedRectangleEffect → only lights inside the configured rect are lit; outside is black; defaults (0,0,0)+(15,15,15) light the origin corner.
  - MirrorModifier → a coord and its mirror map to the same logical position; modifyLogicalSize halves the mirrored axis (study the .h for which axis/percentage).
  - TransposeModifier → swaps axes (x↔y etc. per the .h); modifyLogicalSize swaps the corresponding size fields.
  - FreqMatrix/GEQ/FreqSaws/NoiseMeter are AUDIO effects → they read an AudioFrame. Study how an existing audio path is tested or how the effect gets its data (look for AudioModule / an audio-frame accessor). If the effect needs a fed audio frame to produce output, set it up; if you truly cannot feed audio in a unit test, pin what you CAN (runs at multiple grid sizes incl 0×0 without crashing — the "Effects must run at every grid size" hard rule — and any non-audio behaviour) and say so in notes.
- Respect the hard rule: include a case that runs the effect at a DEGENERATE grid (0×0×0 or 1×1) and asserts no crash, where sensible.
- Keep it to 2–4 focused TEST_CASEs. Match the exact include style and mm:: namespace usage of the template.
- Use doctest macros (TEST_CASE / CHECK / REQUIRE) exactly as the templates do. Do NOT add the file to any CMakeLists — the test build globs test/unit/**.

## Do NOT
- Do NOT run the build or ctest (the caller compiles everything once at the end — per-agent builds would thrash).
- Do NOT edit any file other than creating test/unit/light/unit_${cls}.cpp.
- Do NOT invent controls or behaviour the .h doesn't have.

Return the structured result: the behaviours you pinned (one line each) and any notes (especially if audio-driven or a behaviour you couldn't pin).`,
    { label: `test:${cls}`, phase: 'Study + write', schema: RESULT, agentType: 'general-purpose' }
  )
))

const wrote = results.filter(r => r && r.wrote)
const failed = results.filter(r => !r || !r.wrote)
return {
  written: wrote.length,
  failed: failed.map(r => r ? r.module : 'unknown'),
  audio_or_caveats: results.filter(r => r && r.notes).map(r => ({ module: r.module, notes: r.notes })),
  summary: results.filter(Boolean).map(r => ({ module: r.module, file: r.test_file, behaviours: r.behaviours_pinned })),
}
