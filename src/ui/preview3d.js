// 3D WebGL preview — renders the light pipeline's output as an orbit-able point
// cloud. Extracted from app.js as a self-contained module (same pattern as
// install-picker.js): app.js wires it at three points only —
//   preview.init()            once, after the canvas exists
//   preview.setupLayout()     once, for docked-split ↔ floating-PiP responsiveness
//   preview.onBinaryMessage(buf)  per WebSocket binary frame
// It owns its own GL context, camera, and geometry; it talks to the rest of the
// app only through the DOM (#preview canvas, --bg-0 theme color) and
// localStorage (mm_cam). No app.js state crosses the boundary.

import { nextPullState, initialPullState } from "./preview-adapt.js";

let gl = null;
let glProgram = null;
let glBuffer = null;
let glLocs = null;          // cached attrib/uniform locations
let glMaxPointSize = 64;    // driver's gl_PointSize cap (ALIASED_POINT_SIZE_RANGE max)
let glLoopRunning = false;  // continuous rAF render loop active
// Parse the persisted camera, tolerating a malformed/corrupt value so a bad
// localStorage entry can't throw during module init. Falls back to defaults.
const _cam = (() => {
    try {
        const c = JSON.parse(localStorage.getItem("mm_cam") || "null");
        if (c && typeof c.t === "number" && typeof c.p === "number" && typeof c.d === "number") return c;
    } catch { /* corrupt value — ignore, use defaults */ }
    return null;
})();
// Camera-distance clamp. The scene is normalized to ~[-0.5, 0.5] (box-centered), so CAM_MIN
// well below the scene radius lets you zoom DEEP into a dense grid — close enough that a
// single 128²-grid cell fills the view and its sequence number fits the bulb (the projection
// near plane is lowered to match, so the scene doesn't clip as you approach). CAM_MAX frames
// the whole volume with headroom.
const CAM_MIN = 0.03, CAM_MAX = 10;

// Beam length as a fraction of the scene. Shared: drawBeams builds the cones from it, and the
// camera auto-fit needs it to frame and PIVOT on the lit volume rather than on the fixtures
// alone (a rig of beams is mostly beam).
const BEAM_LEN = 0.42;
let camTheta    = _cam ? _cam.t : Math.PI;
let camPhi      = _cam ? _cam.p : 0.4;
let camDist     = _cam ? _cam.d : 2.5;
// The point the camera orbits + looks at. Origin by default (the scene is box-centered);
// cursor-anchored zoom pans it so the world point under the pointer stays put (Google-Maps
// style). Persisted with the angles/distance so a reload keeps the framing.
let camTgtX     = _cam ? (_cam.tx || 0) : 0;
let camTgtY     = _cam ? (_cam.ty || 0) : 0;
let camTgtZ     = _cam ? (_cam.tz || 0) : 0;
let camAutoFit  = !_cam;   // fit on first frame when no saved position
function saveCam() { localStorage.setItem("mm_cam", JSON.stringify({t: camTheta, p: camPhi, d: camDist, tx: camTgtX, ty: camTgtY, tz: camTgtZ})); }

// Vertical field of view, shared by the projection and by every gesture that converts screen
// pixels to world units (cursor-zoom, pan). One value, one home.
const fov = 0.8;

// Slide the orbit target across the view plane: `dR` world units along the camera's right axis,
// `dU` along its up axis. Both the cursor-zoom and the pan gesture move the target, so the basis
// derivation lives here once rather than in each handler.
function moveTarget(dR, dU) {
    // The same basis buildMVP derives: right = forward x worldUp, up = right x forward.
    const fx = -Math.cos(camPhi) * Math.sin(camTheta);
    const fy = -Math.sin(camPhi);
    const fz = -Math.cos(camPhi) * Math.cos(camTheta);
    let rx = fz, rz = -fx; const rl = Math.hypot(rx, 0, rz) || 1; rx /= rl; rz /= rl;   // ry = 0
    const ux = (-rz) * fy - 0, uy = rz * fx - rx * fz, uz = 0 - (-rx) * fy;
    camTgtX += dR * rx + dU * ux;
    camTgtY += dR * 0  + dU * uy;
    camTgtZ += dR * rz + dU * uz;

    // Keep the pivot on a leash around the scene. Moving the target is how BOTH gestures work
    // (pan slides it deliberately; cursor-zoom shifts it so the pointed-at world point stays
    // put), but only pan is aimed: an off-center scroll nudges the target a little each time,
    // and the result is SAVED, so it compounds across sessions until the camera orbits a point
    // clearly off the rig. Reported from the bench as a rotation center that sits far behind the
    // fixtures, after nothing more than some zooming. The scene is normalized to about
    // [-0.5, 0.5], so one
    // scene-width lets a pan frame any corner while the pivot can never wander into empty space.
    const LEASH = 1.0;
    camTgtX = Math.max(-LEASH, Math.min(LEASH, camTgtX));
    camTgtY = Math.max(-LEASH, Math.min(LEASH, camTgtY));
    camTgtZ = Math.max(-LEASH, Math.min(LEASH, camTgtZ));
}

// Put the pivot back on the rig, keeping the current viewing angle and zoom. Bound to a
// double-click, the gesture 3D tools use for "frame this": the cheapest possible answer to a
// pivot that has drifted, without resetting the dot size, numbers and layout the way ⌖ does.
function recenterPivot() {
    camTgtX = camTgtY = camTgtZ = 0;   // the coordinate table is already box-centered on the rig
    redrawCached();
    saveCam();
}
let lastVerts = null;        // cached vertex array for orbit-without-server-frame
let lastVertCount = 0;
let lastMaxDim = 1;
let vertsBuf = null;         // reused worst-case Float32Array; grows but never shrinks
// True-shape preview geometry, set from the 0x03 coordinate table and reused
// across 0x02 color frames (positions change only on a layout/LUT rebuild).
let previewCoords_ = null;   // Float32Array[count*3], normalized + box-centered positions
// Aim, from the 0x04 message: two bytes (pan, tilt) per light, in the coord table's order.
// Null for every rig whose fixtures carry no pan/tilt, which is most of them: the device does
// not send the message at all then, so nothing here allocates or draws.
let previewAim_ = null;
// Color frames seen since the last aim message. The device alternates the two, so anything above
// a couple means it has stopped sending aim and the beams must go (see renderPreviewFrame).
let framesSinceAim_ = 0;
// The (epoch, stride) the cached aim was gathered against; beams are drawn only while these still
// match the active coordinate table (see parsePreviewAim).
let previewAimEpoch_ = -1;
let previewAimStride_ = 0;
let sawAim_ = false;         // the auto-fit has already been re-armed for beams
const kAimStaleFrames = 4;
let beamVerts_ = null;       // reused Float32Array of beam vertices (x,y,z,intensity), grows only
let beamProgram = null, beamLocs = null, beamBuffer = null;
let previewRgb_ = null;      // last 0x02 frame's RGB, so beams take their fixture's color
let bgLuma_ = 0.07;          // background brightness, so a ghost beam stays visible per theme
let previewCoordCount_ = 0;
let previewMaxDim_ = 1;
let previewStride_ = 1;      // device's adaptive downscale factor (1 = full res); for the status line
let showSeqNumbers_ = false; // sequence-number overlay toggle (preview-numbers button)
// Dot-size multiplier on the auto-computed "filled-panel" base (1 = ¾-fill). A user knob
// because the ideal fill is subjective and layout-dependent — a 2D panel reads best solid,
// a 3D cube reads best with smaller dots so the back layers show through. Persisted.
const DOT_MIN = 0.25, DOT_MAX = 1.5;   // matches the slider range; clamp so a bad
const clampDot = (v) => Math.min(DOT_MAX, Math.max(DOT_MIN, Number.isFinite(v) ? v : 1));
// localStorage value (or a manual edit) can't push the dot size to a performance-killing extreme.
let dotScale_ = clampDot(parseFloat(localStorage.getItem("mm_preview_dot")));
let resetLayout_ = null;     // set by setupLayout(): restores docked/PiP state to defaults
let previewBox_ = null;      // {x,y,z} bounding-box extent for camera auto-fit
let previewPos_ = null;      // the table's raw (x,y,z) bytes: what the stride test reads
let previewTableStride_ = 1; // the stride the CACHED table was built at, the finest available
let previewEpochKey_ = 0;    // the epoch the cached table belongs to, keying the upsample map
let lineProgram = null;      // separate program for the wireframe bounding box
let lineLocs = null;
let lineBuffer = null;
let boxVerts = null;         // 12-edge wireframe (24 line endpoints) for the current box
let boxKey = "";             // cache key so the box buffer rebuilds only when extents change

function initWebGL() {
    const canvas = document.getElementById("preview");
    if (!canvas) return;
    gl = canvas.getContext("webgl", {alpha: false});
    if (!gl) return;

    const vsrc = `
        attribute vec3 aPos;
        attribute vec3 aCol;
        varying vec3 vCol;
        varying float vSize;
        uniform mat4 uMVP;
        uniform float uPointSize;
        void main() {
            vCol = aCol;
            gl_Position = uMVP * vec4(aPos, 1.0);
            // Depth-corrected point size — closer LEDs render larger
            gl_PointSize = uPointSize / gl_Position.w;
            vSize = gl_PointSize;   // px size, so the fragment can keep the AA edge ~1px wide
        }
    `;
    const fsrc = `
        precision mediump float;
        varying vec3 vCol;
        varying float vSize;
        uniform float uRingFade;   // 0..1: off-LED placeholder opacity. 1 at small/zoomed
                                   // grids (placeholders show the layout); →0 when points get
                                   // dense (they'd be noise, so the lit pattern reads cleanly).
        uniform float uLitPass;    // two-pass draw: 0 = placeholders only, 1 = lit LEDs only.
                                   // Lit are drawn second with depth-test off so they always
                                   // layer ABOVE the grey placeholders at the same spot,
                                   // regardless of pan/tilt draw order (no z-fighting).
        void main() {
            float d = length(gl_PointCoord - vec2(0.5));   // 0 at center .. 0.5 at rim
            // Anti-alias band ~1px wide regardless of sprite size: crisp disc at 8x8
            // (huge sprites) AND smooth at large grids (tiny sprites).
            float aa = clamp(1.0 / max(vSize, 1.0), 0.004, 0.12);
            float disc = 1.0 - smoothstep(0.5 - aa, 0.5, d);   // filled circle, thin soft rim
            // Gamma 0.7 lifts mid-greys so dim effects stay readable; not sRGB-correct.
            vec3 bright = pow(vCol, vec3(0.7));
            float lum = max(max(vCol.r, vCol.g), vCol.b);
            // How "lit" the LED is, ramped over the bottom of the range so a near-off LED is
            // a placeholder but a genuinely lit one is solid.
            float lit = smoothstep(0.02, 0.10, lum);

            if (uLitPass < 0.5) {
                // Pass 1 — placeholders for OFF LEDs only. A faint filled grey CIRCLE (a disc,
                // not a hollow ring or square) so irregular layouts (a wheel, a sphere) read
                // cleanly; fades out as the grid gets dense (uRingFade→0).
                if (lit > 0.5) discard;                       // lit LEDs belong to pass 2
                float a = disc * 0.22 * uRingFade;
                if (a < 0.01) discard;
                gl_FragColor = vec4(vec3(0.32), a);
            } else {
                // Pass 2 — lit LEDs only, solid disc in the real color, on top.
                if (lit < 0.5) discard;                       // off LEDs were pass 1
                if (disc < 0.01) discard;
                gl_FragColor = vec4(bright, disc);
            }
        }
    `;

    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, vsrc); gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, fsrc); gl.compileShader(fs);
    glProgram = gl.createProgram();
    gl.attachShader(glProgram, vs); gl.attachShader(glProgram, fs);
    gl.linkProgram(glProgram); gl.useProgram(glProgram);

    // WebGL clamps gl_PointSize to the driver's range — bulbs stop growing past this even
    // as you zoom in, so the label fit-check must clamp to the same cap (else it thinks a
    // bulb is big enough for a number when the drawn sprite is actually capped smaller).
    glMaxPointSize = (gl.getParameter(gl.ALIASED_POINT_SIZE_RANGE) || [1, 64])[1] || 64;

    glBuffer = gl.createBuffer();
    glLocs = {
        aPos:      gl.getAttribLocation(glProgram,  "aPos"),
        aCol:      gl.getAttribLocation(glProgram,  "aCol"),
        uMVP:      gl.getUniformLocation(glProgram, "uMVP"),
        uPointSize:gl.getUniformLocation(glProgram, "uPointSize"),
        uRingFade: gl.getUniformLocation(glProgram, "uRingFade"),
        uLitPass:  gl.getUniformLocation(glProgram, "uLitPass"),
    };
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    // A second, minimal program for the wireframe bounding box (a faint cuboid around
    // the light volume — gives the scene bounds + 3D orientation while orbiting, and a
    // frame even when every LED is off). Flat color, no per-vertex attributes beyond pos.
    const lvs = `attribute vec3 aPos; uniform mat4 uMVP; void main(){ gl_Position = uMVP * vec4(aPos,1.0); }`;
    const lfs = `precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor = uColor; }`;
    const lv = gl.createShader(gl.VERTEX_SHADER); gl.shaderSource(lv, lvs); gl.compileShader(lv);
    const lf = gl.createShader(gl.FRAGMENT_SHADER); gl.shaderSource(lf, lfs); gl.compileShader(lf);
    lineProgram = gl.createProgram();
    gl.attachShader(lineProgram, lv); gl.attachShader(lineProgram, lf);
    gl.linkProgram(lineProgram);
    lineLocs = {
        aPos:   gl.getAttribLocation(lineProgram, "aPos"),
        uMVP:   gl.getUniformLocation(lineProgram, "uMVP"),
        uColor: gl.getUniformLocation(lineProgram, "uColor"),
    };
    lineBuffer = gl.createBuffer();

    // A third program for moving-head BEAMS. Separate from the box lines because a beam carries a
    // per-vertex intensity: it is bright at the fixture and fades along its length and toward its
    // edges, which is what makes a solid cone read as light in the air rather than a plastic cone.
    // The technique is the one stage visualizers use for a cheap beam: solid tapered geometry,
    // additive blending, no depth WRITE, and a very low base alpha.
    const bvs = `attribute vec3 aPos; attribute float aI; attribute vec3 aColor;
                 uniform mat4 uMVP; varying float vI; varying vec3 vColor;
                 void main(){ vI = aI; vColor = aColor; gl_Position = uMVP * vec4(aPos,1.0); }`;
    // The core desaturates toward white as it gets hot, which is what a real beam does: the
    // saturated color reads in the cone's body while the center blows out, so a deep blue beam
    // still shows a bright core instead of going muddy.
    const bfs = `precision mediump float; varying float vI; varying vec3 vColor;
                 void main(){ vec3 c = mix(vColor, vec3(1.0), vI * vI * 0.5);
                              gl_FragColor = vec4(c * vI, vI); }`;
    const bv = gl.createShader(gl.VERTEX_SHADER); gl.shaderSource(bv, bvs); gl.compileShader(bv);
    const bf = gl.createShader(gl.FRAGMENT_SHADER); gl.shaderSource(bf, bfs); gl.compileShader(bf);
    beamProgram = gl.createProgram();
    gl.attachShader(beamProgram, bv); gl.attachShader(beamProgram, bf);
    gl.linkProgram(beamProgram);
    beamLocs = {
        aPos:   gl.getAttribLocation(beamProgram, "aPos"),
        aI:     gl.getAttribLocation(beamProgram, "aI"),
        aColor: gl.getAttribLocation(beamProgram, "aColor"),
        uMVP:   gl.getUniformLocation(beamProgram, "uMVP"),
    };
    beamBuffer = gl.createBuffer();

    // Orbit controls (mouse + touch).
    //
    // The gesture set every 3D tool shares (Blender, Maya, CAD, three.js OrbitControls): DRAG
    // orbits, SHIFT-drag or a right/middle drag PANS, and the wheel zooms. Orbit alone cannot
    // reach an off-center rig, which is what "I can tilt it but not rotate" reports: the camera
    // was already turning, but with the target pinned to the origin it never felt like moving
    // AROUND something. Pan is the missing half, and it needs no new state (camTgt already
    // exists for cursor-zoom).
    //
    // Deliberately NOT a roll control: an orbit camera keeps world-up on screen, and every tool
    // above does the same. Roll is a flying camera's gesture, and it mostly leaves users lost.
    let dragging = false, panning = false, lastX = 0, lastY = 0;
    canvas.addEventListener("mousedown", (e) => {
        // button 0 = left (orbit, or pan with shift), 1 = middle, 2 = right (pan).
        panning = e.button === 1 || e.button === 2 || e.shiftKey;
        dragging = true;
        lastX = e.clientX; lastY = e.clientY;
        if (panning) e.preventDefault();   // a middle-drag would otherwise autoscroll
    });
    // Right-drag is a pan, so the context menu must not eat it.
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    // Double-click recenters the pivot on the rig (see recenterPivot).
    canvas.addEventListener("dblclick", recenterPivot);
    canvas.addEventListener("mousemove", (e) => {
        if (!dragging) return;
        const dx = e.clientX - lastX, dy = e.clientY - lastY;
        if (panning) {
            // Screen pixels to world units at the target plane, so the scene tracks the cursor
            // 1:1 whatever the zoom: the same extent the cursor-zoom math uses.
            const halfH = camDist * Math.tan(fov / 2);
            const perPx = (2 * halfH) / Math.max(1, canvas.height);
            moveTarget(-dx * perPx, dy * perPx);
        } else {
            camTheta += dx * 0.01;
            camPhi = Math.max(-1.5, Math.min(1.5, camPhi - dy * 0.01));
        }
        lastX = e.clientX; lastY = e.clientY;
        redrawCached();
    });
    canvas.addEventListener("mouseup",    () => { dragging = false; panning = false; saveCam(); });
    canvas.addEventListener("mouseleave", () => { dragging = false; panning = false; saveCam(); });
    canvas.addEventListener("wheel", (e) => {
        e.preventDefault();
        // Cursor-anchored zoom (Google-Maps style): keep the world point under the pointer
        // fixed on screen while zooming. The orbit camera looks at camTgt from camDist; the
        // view half-extent at the target plane is camDist*tan(fov/2). The cursor's offset
        // from canvas center, in that world scale along the camera's right/up axes, is where
        // the pointer is in the target plane. Scaling camDist by k scales that plane's extent
        // by k, so shifting camTgt by (1-k)*cursorOffset keeps the pointed-at point put.
        const r = canvas.getBoundingClientRect();
        const ndcX = ((e.clientX - r.left) / r.width) * 2 - 1;
        const ndcY = 1 - ((e.clientY - r.top) / r.height) * 2;   // y-up
        const aspect = r.width / Math.max(1, r.height);
        const halfH = camDist * Math.tan(fov / 2);
        const offU = ndcY * halfH;             // world units along camera up at target plane
        const offR = ndcX * halfH * aspect;    // along camera right

        const oldDist = camDist;
        camDist = Math.max(CAM_MIN, Math.min(CAM_MAX, camDist * Math.exp(e.deltaY * 0.0015)));
        const k = camDist / oldDist;           // <1 zooming in, >1 zooming out

        // Shift the target so the cursor-pointed world point stays fixed as the extent scales.
        moveTarget(offR * (1 - k), offU * (1 - k));

        redrawCached();
        saveCam();
    }, {passive: false});

    // Touch: single-finger orbit, two-finger pinch zoom. touch-action: none on
    // #preview keeps the browser's own scroll/zoom from firing first.
    let pinchDist = 0;
    const touchDistance = (a, b) => Math.hypot(a.clientX - b.clientX, a.clientY - b.clientY);

    canvas.addEventListener("touchstart", (e) => {
        if (e.touches.length === 1) {
            dragging = true;
            lastX = e.touches[0].clientX; lastY = e.touches[0].clientY;
        } else if (e.touches.length >= 2) {
            dragging = false;  // hand off from orbit to pinch
            pinchDist = touchDistance(e.touches[0], e.touches[1]);
            e.preventDefault();
        }
    }, {passive: false});
    canvas.addEventListener("touchmove", (e) => {
        if (e.touches.length === 1 && dragging) {
            const t = e.touches[0];
            camTheta += (t.clientX - lastX) * 0.01;
            camPhi = Math.max(-1.5, Math.min(1.5, camPhi - (t.clientY - lastY) * 0.01));
            lastX = t.clientX; lastY = t.clientY;
            redrawCached();
            e.preventDefault();
        } else if (e.touches.length >= 2 && pinchDist > 0) {
            // Pinch zoom: ratio of finger-distance change scales camDist
            // (fingers apart → zoom in / camDist down). Guard against the
            // degenerate case where both fingers report identical coords
            // (d === 0): division would produce Infinity and snap camDist
            // to its clamp boundary. Skip the update and let the next move
            // produce a sane d.
            const d = touchDistance(e.touches[0], e.touches[1]);
            if (d > 0) {
                const ratio = pinchDist / d;
                camDist = Math.max(CAM_MIN, Math.min(CAM_MAX, camDist * ratio));
                pinchDist = d;
                redrawCached();
            }
            e.preventDefault();
        }
    }, {passive: false});
    canvas.addEventListener("touchend", (e) => {
        if (e.touches.length === 0) { dragging = false; pinchDist = 0; saveCam(); }
        // 2→1 touches: stay in pinch (let user finish lifting); pinchDist stays
        // valid for the remaining finger? No — drop pinch, but don't start
        // orbit either, to avoid a jump when one finger lifts.
        else if (e.touches.length === 1) { pinchDist = 0; dragging = false; saveCam(); }
    });

}

// Responsive layout: docked split-pane on wide screens, a draggable floating
// picture-in-picture on narrow screens (or when the user pops the preview out).
// One canvas throughout — only the wrapper's class/position change, so the WebGL
// context is never lost. Width drives the default mode; a manual toggle overrides.
const PIP_BELOW = 960;           // px: auto-PiP under this width
const LS_KEY = "MoonLight.preview.v1";   // {corner, dismissed, forcePip}

// Hostile-storage guards (a 3-line idiom shared with the rest of the UI; localStorage
// throws in private mode / when disabled, and may hold a hand-edited non-JSON value).
function loadPrefs() {
    try { return JSON.parse(localStorage.getItem(LS_KEY) || "{}") || {}; }
    catch (_) { return {}; }
}
function savePrefs(p) {
    try { localStorage.setItem(LS_KEY, JSON.stringify(p)); } catch (_) { /* ignore */ }
}

function setupLayout() {
    const ws = document.querySelector(".workspace");
    const pane = document.querySelector(".preview-pane");
    const bar = document.querySelector(".preview-bar");
    const canvas = document.getElementById("preview");
    if (!ws || !pane || !bar || !canvas) return;

    const prefs = loadPrefs();
    let forcePip = !!prefs.forcePip;          // user popped the preview out on a wide screen
    let dismissed = !!prefs.dismissed;        // user hid the PiP entirely
    let corner = prefs.corner || "br";        // tl | tr | bl | br

    const refit = () => { if (lastVerts) redrawCached(); };

    // Place the PiP at its snapped corner (left/top so dragging can move it freely).
    function placeCorner() {
        if (!ws.classList.contains("mode-pip")) { pane.style.left = pane.style.top = ""; return; }
        const m = 12, w = pane.offsetWidth, h = pane.offsetHeight;
        const x = corner.includes("l") ? m : window.innerWidth - w - m;
        const y = corner.includes("t") ? 56 : window.innerHeight - h - m;
        pane.style.left = x + "px";
        pane.style.top = y + "px";
        pane.style.right = "auto";
        pane.style.bottom = "auto";
    }

    // Pick the mode from width + the manual override, apply classes, re-fit the canvas.
    function applyMode() {
        const pip = forcePip || window.innerWidth < PIP_BELOW;
        ws.classList.toggle("mode-pip", pip);
        ws.classList.toggle("mode-docked", !pip);
        const hidden = pip && dismissed;
        ws.classList.toggle("preview-hidden", hidden);
        // Tell the app whether frames are wanted. The device streams the preview only to clients on
        // its `/wsp` channel, so a dismissed pane closing that socket is what actually stops the
        // traffic at the source, not just hiding pixels the device already paid to send.
        if (wantsFrames_) wantsFrames_(!hidden);
        const showBtn = document.getElementById("preview-show");
        if (showBtn) showBtn.hidden = !(pip && dismissed);
        // The dock button means "pop out" when docked, "re-dock" when floating.
        const dockBtn = document.getElementById("preview-dock");
        if (dockBtn) dockBtn.textContent = pip ? "⤡" : "⤢";
        requestAnimationFrame(() => { placeCorner(); refit(); });
    }

    // Let the preview "reset" button restore the docked/PiP layout to defaults too (back to
    // auto docked-vs-PiP, not dismissed, default corner) — these vars are closure-local here.
    resetLayout_ = () => {
        forcePip = false; dismissed = false; corner = "br";
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    };

    // matchMedia catches only the breakpoint crossing; this resize listener also keeps
    // the PiP pinned to its corner as the window changes. rAF-throttled.
    let ticking = false;
    window.addEventListener("resize", () => {
        if (ticking) return;
        ticking = true;
        requestAnimationFrame(() => { ticking = false; applyMode(); });
    });

    // Dock / pop-out toggle.
    document.getElementById("preview-dock")?.addEventListener("click", () => {
        forcePip = !ws.classList.contains("mode-pip") ? true : false;
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    // Hide the preview; reveal the re-show pill. Dismissal takes visible effect only in
    // PiP mode (the pill replaces the floating preview); closing from docked mode also
    // pops it out (forcePip) so the result is immediate — closing a docked preview hides
    // it now rather than only when narrow auto-PiP later kicks in.
    document.getElementById("preview-close")?.addEventListener("click", () => {
        dismissed = true;
        if (!ws.classList.contains("mode-pip")) forcePip = true;
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    document.getElementById("preview-show")?.addEventListener("click", () => {
        dismissed = false;
        forcePip = false;   // bring it back in the width-appropriate mode (docked on wide)
        savePrefs({ corner, dismissed, forcePip });
        applyMode();
    });
    // Sequence-number overlay toggle. The active state reflects the flag; the labels
    // themselves only appear when also legible (few enough on-screen — see drawSeqLabels).
    const numBtn = document.getElementById("preview-numbers");
    numBtn?.addEventListener("click", () => {
        showSeqNumbers_ = !showSeqNumbers_;
        numBtn.classList.toggle("active", showSeqNumbers_);
        if (lastVerts) redrawCached();   // repaint so labels appear/clear immediately
    });

    // Dot-size knob: scales the auto "filled-panel" base. Persisted so the preference sticks.
    const dotSlider = document.getElementById("preview-dot");
    if (dotSlider) {
        dotSlider.value = String(dotScale_);
        dotSlider.addEventListener("input", () => {
            dotScale_ = clampDot(parseFloat(dotSlider.value));
            localStorage.setItem("mm_preview_dot", String(dotScale_));
            if (lastVerts) redrawCached();
        });
    }

    // Drag the PiP by its bar; snap to the nearest corner on release. Pointer events
    // on the BAR only (the canvas keeps its own orbit handler, untouched).
    let drag = null;
    bar.addEventListener("pointerdown", (e) => {
        if (!ws.classList.contains("mode-pip")) return;          // bar inert when docked
        if (e.target.closest(".preview-bar-btn")) return;        // let buttons click
        const r = pane.getBoundingClientRect();
        drag = { dx: e.clientX - r.left, dy: e.clientY - r.top };
        pane.classList.add("dragging");
        bar.setPointerCapture(e.pointerId);
        e.preventDefault();
    });
    bar.addEventListener("pointermove", (e) => {
        if (!drag) return;
        const w = pane.offsetWidth, h = pane.offsetHeight;
        let x = e.clientX - drag.dx, y = e.clientY - drag.dy;
        x = Math.max(0, Math.min(window.innerWidth - w, x));     // clamp to viewport
        y = Math.max(44, Math.min(window.innerHeight - h, y));
        pane.style.left = x + "px";
        pane.style.top = y + "px";
        pane.style.right = pane.style.bottom = "auto";
    });
    bar.addEventListener("pointerup", (e) => {
        if (!drag) return;
        drag = null;
        pane.classList.remove("dragging");
        try { bar.releasePointerCapture(e.pointerId); } catch (_) { /* ignore */ }
        // Snap to nearest corner by the pane's center.
        const r = pane.getBoundingClientRect();
        const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
        corner = (cy < window.innerHeight / 2 ? "t" : "b") + (cx < window.innerWidth / 2 ? "l" : "r");
        savePrefs({ corner, dismissed, forcePip });
        placeCorner();
    });

    applyMode();
}

// True-shape preview: two binary message types on the preview WebSocket.
//   0x03 coordinate table (answered on a [0x52] request; cached per (epoch, stride)):
//        [0x03][count:u32][bx:u8][by:u8][bz:u8][stride:u16][epoch:u8][(x,y,z):u8×3 × count]
//        Stores the real lights' normalized positions in previewCoords_ (the
//        geometry); per-frame 0x02 messages then just recolor those points.
//   0x02 per-frame channels: [0x02][count:u32][stride:u16][epoch:u8][drops:u8][(r,g,b) × count]
//        Color for light i sits at position previewCoords_[i].
// count is u32 so a >65535-light panel (HUB75 walls) isn't capped by the wire format.
// Light index i in the 0x02 stream matches coordinate-table entry i (both are
// every stride-th driver light, in the same order) — no dense grid, no decompress.
// Show the device's adaptive-downscale factor in the preview bar — only while it's active
// (factor > 1), so the bar stays clean at full resolution. previewStride_ is the per-axis
// downscale: factor f means ~1/f² of the lights are shown (f per axis on a 2D grid).
function updatePreviewStatus() {
    const el = document.getElementById("preview-status");
    if (!el) return;
    const parts = [];
    // Name the RIGHT cause: a stride we asked for is the link adapting; a stride above our own
    // request was imposed elsewhere (the device's memory cap, or a coarser co-viewer's request),
    // and targetFps cannot make that finer.
    if (previewStride_ > 1)
        parts.push(`1/${previewStride_}` + (previewStride_ > adaptState_.stride ? " · capped" : " · link limited"));
    if (effectiveFps_ > 0)  parts.push(`${Math.round(effectiveFps_)} fps`);      // adaptive rate
    if (parts.length) {
        el.textContent = "preview " + parts.join(" · ");
        el.hidden = false;
    } else {
        el.hidden = true;
    }
}

function renderPreviewBinary(buf) {
    if (buf.byteLength < 1) return;
    const view = new DataView(buf);
    const type = view.getUint8(0);
    if (type === 0x03) { parsePreviewCoords(view, buf); return; }
    if (type === 0x02) { renderPreviewFrame(view, buf); return; }
    if (type === 0x04) { parsePreviewAim(view, buf); return; }
}

// Parse the per-frame AIM message: [0x04][count:u32][stride:u16][epoch][reserved][(pan,tilt) x n].
// Kept raw (0..255 per axis) and turned into a direction at draw time, so the browser owns how a
// moving head is DRAWN and the wire only says where it points. A richer visual later (a cone with
// falloff rather than a line) is then a change here alone.
function parsePreviewAim(view, buf) {
    if (buf.byteLength < 9) return;
    const count = view.getUint32(1, true);
    const bytes = buf.byteLength - 9;
    if (count === 0 || bytes < count * 2) { previewAim_ = null; return; }
    // A COPY, not a view: previewAim_ outlives this message (drawBeams reads it on every orbit
    // redraw), and a view onto a recycled receive buffer would render beams from whatever arrived
    // next. parsePreviewCoords copies for the same reason; 2 bytes/light on a cold path.
    previewAim_ = new Uint8Array(new Uint8Array(buf, 9, count * 2));
    // Tag the aim with the table it was gathered against. aim[k] means "the k-th light of THAT
    // table", so after a geometry change (new epoch) or a downscale change (new stride) the same
    // index names a different fixture and the beams would point from the wrong heads until the
    // next aim frame. Cheap to carry, and drawBeams simply ignores a mismatch.
    previewAimEpoch_ = view.getUint8(7);
    previewAimStride_ = view.getUint16(5, true) || 1;
    framesSinceAim_ = 0;
}

// Parse + cache the coordinate table: normalized (x,y,z) per point, centered on
// the bounding box so the cloud sits around the origin like the old grid did.
function parsePreviewCoords(view, buf) {
    // Header: [0x03][count:u32][bx][by][bz][stride:u16][epoch] = 11 bytes.
    if (buf.byteLength < 11) return;
    const count = view.getUint32(1, true);
    const bx = view.getUint8(5), by = view.getUint8(6), bz = view.getUint8(7);
    // Validate the full payload BEFORE mutating any parser state — a truncated buffer must leave
    // the cache / status line untouched (else they'd describe coords we never stored).
    if (buf.byteLength < 11 + count * 3) return;
    const stride = view.getUint16(8, true) || 1;
    const epoch = view.getUint8(10);
    // A geometry change (a new epoch) is a NEW canvas: verdicts measured on the old one do not
    // apply. Restart the controller and re-announce, exactly as a fresh connect does. A
    // stride-only table keeps the epoch and resets nothing.
    if (lastEpoch_ !== null && lastEpoch_ !== epoch) {
        adaptState_ = initialPullState();
        adaptFrames_ = 0;
        announceRequest();
    }
    lastEpoch_ = epoch;
    const pos = new Uint8Array(buf, 11);
    const maxDim = Math.max(1, bx, by, bz);
    const coords = new Float32Array(count * 3);
    for (let i = 0; i < count; i++) {
        coords[i * 3 + 0] = (pos[i * 3 + 0] / maxDim) - 0.5 * bx / maxDim;
        coords[i * 3 + 1] = (pos[i * 3 + 1] / maxDim) - 0.5 * by / maxDim;
        coords[i * 3 + 2] = (pos[i * 3 + 2] / maxDim) - 0.5 * bz / maxDim;
    }
    // ONE table per epoch, the finest the device will serve. Every coarser stride is derived from
    // it here rather than fetched, so a stride change costs no table traffic at any rung and the
    // preview keeps the layout's full extent at every detail level. Positions are static for an
    // epoch, so this is also less traffic than a table per rung: one fetch, not three or four.
    for (const k of tableCache_.keys()) if (!k.startsWith(epoch + ":")) tableCache_.delete(k);
    tableCache_.set(epoch + ":" + stride, { coords, count, maxDim, bx, by, bz, stride, pos: new Uint8Array(pos) });
    activateTable(epoch, stride);
    // Draw the geometry NOW, dark, so a fresh page shows the layout the instant the table arrives,
    // not only once the first color frame lands. Color frames then light it.
    drawLights(null);
}

// Make a cached table the rendering one. Returns false when the cache misses (the caller then
// asks the device for it: the pull model).
function activateTable(epoch, stride) {
    // Any cached table for this epoch serves every stride: the colors are mapped onto it below.
    // The FINEST one when several are cached, since a coarser base cannot express a finer frame:
    // insertion order would have handed back whichever arrived first.
    const t = tableCache_.get(epoch + ":" + stride)
           || [...tableCache_.keys()].filter(k => k.startsWith(epoch + ":"))
                .map(k => tableCache_.get(k))
                .sort((a, b) => (a.stride ?? 1) - (b.stride ?? 1))[0];
    if (!t) return false;
    previewCoords_ = t.coords;
    previewCoordCount_ = t.count;
    previewMaxDim_ = t.maxDim;
    previewBox_ = { x: t.bx, y: t.by, z: t.bz };
    previewPos_ = t.pos;
    previewTableStride_ = t.stride ?? 1;
    previewEpochKey_ = epoch;
    previewStride_ = stride;
    updatePreviewStatus();
    return true;
}

// Which table point each color in a stride-`s` frame belongs to. The device keeps a light when
// `x % s == 0 && y % s == 0 && z % s == 0` (PreviewDriver::buildCoordTable), walking z, then y,
// then x, so running the same test over the table's own positions reproduces its order exactly.
// Every point then takes the color of the kept light leading its block: the layout stays whole
// and only the detail coarsens. Cached per (table, stride), since it changes only when one does.
let upsampleCache_ = new Map();
function upsampleMap(stride) {
    const base = previewTableStride_ || 1;
    const n = previewCoordCount_;
    if (!previewPos_ || stride <= base) return null;   // already the finest the device serves
    const key = previewEpochKey_ + ":" + base + ":" + stride;
    const hit = upsampleCache_.get(key);
    if (hit && hit.length === n) return hit;
    // The kept lights, in the device's emit order.
    const kept = [];
    for (let i = 0; i < n; i++) {
        const x = previewPos_[i * 3], y = previewPos_[i * 3 + 1], z = previewPos_[i * 3 + 2];
        if (x % stride === 0 && y % stride === 0 && z % stride === 0) kept.push(i);
    }
    // Each point to the nearest kept light at or before it on each axis: its block's leader.
    const map = new Uint32Array(n);
    const slot = new Map();
    for (let k = 0; k < kept.length; k++) {
        const i = kept[k];
        slot.set(previewPos_[i * 3] + "," + previewPos_[i * 3 + 1] + "," + previewPos_[i * 3 + 2], k);
    }
    // A block with no kept light is normal on a sparse layout: the lattice point its leader would
    // occupy simply holds no light. Such a point draws DARK rather than borrowing colour 0, which
    // is a real light elsewhere in the frame and would paint the wrong pixel.
    const UNMAPPED = 0xffffffff;
    for (let i = 0; i < n; i++) {
        const bxq = previewPos_[i * 3] - (previewPos_[i * 3] % stride);
        const byq = previewPos_[i * 3 + 1] - (previewPos_[i * 3 + 1] % stride);
        const bzq = previewPos_[i * 3 + 2] - (previewPos_[i * 3 + 2] % stride);
        const k = slot.get(bxq + "," + byq + "," + bzq);
        map[i] = k === undefined ? UNMAPPED : k;
    }
    upsampleCache_ = new Map([[key, map]]);   // one epoch, one stride in flight: keep the last
    return map;
}

// Ask the device for the coordinate table ([0x52][stride]), at most once per half second: the
// answer is paced by the drain, and re-asking faster only queues duplicate work.
function requestTable(stride) {
    const now = performance.now();
    if (now - lastTableReq_ < 500) return;
    lastTableReq_ = now;
    if (sendRequest_) sendRequest_([0x52, stride]);
}

function renderPreviewFrame(view, buf) {
    if (!gl) initWebGL();
    if (!gl) return;
    // Header: [0x02][count:u32][stride:u16][epoch][drops] = 9 bytes. Validate the WHOLE frame
    // before feeding the adaptation counters: a truncated frame is not a delivered frame, and
    // counting its drops byte would steer the controller on garbage.
    if (buf.byteLength < 9) return;
    const count = view.getUint32(1, true);
    const stride = view.getUint16(5, true) || 1;
    const epoch = view.getUint8(7);
    if (buf.byteLength < 9 + count * 3) return;
    adaptFrames_++;                     // the controller's measurement: frames that actually arrived
    windowDrops_ += view.getUint8(8);   // sum the device's drop reports over the controller window
    // (epoch, stride) is the table-cache key. A hit renders immediately (a stride flip costs zero
    // table traffic); a miss asks the device for the positions and skips this frame, the pull
    // model's whole geometry story.
    if ((epoch !== lastEpoch_ || stride !== previewStride_) && !activateTable(epoch, stride)) {
        // The FINEST table, once per epoch: every coarser stride is derived from it here.
        requestTable(1);
        return;
    }
    previewStride_ = stride;
    const up = upsampleMap(stride);
    // A frame carries one color per kept light; without a map the two counts must agree.
    if (!up && count !== previewCoordCount_) return;   // mid-rebuild mismatch: the next table realigns
    const rgb = new Uint8Array(buf, 9);
    // Kept for drawBeams: a moving head's beam is the color the fixture is EMITTING, so the beam
    // pass needs the same frame the dots were drawn from. A COPY, because this outlives the
    // message: drawBeams reads it on every orbit redraw, and a view onto a recycled receive
    // buffer would color beams from whatever arrived next.
    // UPSAMPLE: one color per kept light becomes one per table point, each taking its block's.
    // The layout keeps its full extent and every light stays on screen; only the detail coarsens.
    if (up) {
        const full = new Uint8Array(previewCoordCount_ * 3);
        for (let i = 0; i < previewCoordCount_; i++) {
            const k = up[i];
            // Out of range means the frame and the map disagree (a mid-rebuild frame): draw dark
            // rather than read past the body, which would repeat a stale colour.
            if (k >= count) continue;
            const src = k * 3;
            full[i * 3] = rgb[src]; full[i * 3 + 1] = rgb[src + 1]; full[i * 3 + 2] = rgb[src + 2];
        }
        previewRgb_ = full;
    } else {
        previewRgb_ = new Uint8Array(rgb);
    }
    // The first aim frame changes what the scene CONTAINS: beams extend well past the fixtures,
    // and a fit measured before they existed frames only the heads. Re-arm the auto-fit once so
    // the next one accounts for them; `sawAim_` keeps it to once, not once per aim frame.
    if (previewAim_ && !sawAim_) { sawAim_ = true; camAutoFit = true; }
    // Beams are only real while the device is still SENDING aim. It stops the moment the rig
    // stops carrying motion channels (PreviewDriver gates on fixtureChannels().movable()), but a
    // cached previewAim_ would go on drawing the last aim forever: selecting a plain effect then
    // showed beams on every light. The device alternates aim and color frames, so a couple of
    // color frames with no aim between them means the aim stream has ended.
    if (previewAim_ && ++framesSinceAim_ > kAimStaleFrames) { previewAim_ = null; sawAim_ = false; }
    drawLights(previewRgb_);
    measureFrameRate();
}

// Effective preview frame rate, measured browser-side as a SLIDING-WINDOW COUNT: how many frames
// arrived in the last second of wall time. The device drains each resumable frame across transport
// ticks, so frames arrive BURSTY (several back-to-back, then a gap) — an instantaneous 1000/Δt
// reading would spike to absurd values on a 0 ms gap (the "200 fps" artifact). Counting over a
// fixed window is immune to that: it's the true delivered rate. performance.now() is the standard
// high-resolution browser clock.
const frameStamps_ = [];        // arrival times (ms) within the trailing window
let effectiveFps_ = 0;
function measureFrameRate() {
    const now = performance.now();
    frameStamps_.push(now);
    const cutoff = now - 1000;                                  // 1-second trailing window
    while (frameStamps_.length && frameStamps_[0] < cutoff) frameStamps_.shift();
    effectiveFps_ = frameStamps_.length;                        // frames in the last second = fps
    updatePreviewStatus();
}

// Build the vertex buffer from previewCoords_ + per-light color and (re)start the render loop.
// rgb may be null — then every light is drawn off (the shader's placeholder ring), so the grid
// LAYOUT shows the instant the coordinate table arrives (a fresh page / UI refresh), before any
// color frame. A color frame then calls this again with its rgb to light the scene.
function drawLights(rgb) {
    if (!gl) initWebGL();
    if (!gl) return;
    if (!previewCoords_ || previewCoordCount_ === 0) return;
    const n = previewCoordCount_;

    if (!vertsBuf || vertsBuf.length < n * 6) vertsBuf = new Float32Array(n * 6);
    let vi = 0;
    for (let i = 0; i < n; i++) {
        // Include EVERY light, dark ones too: the shader draws an off LED as a faint
        // placeholder ring (a lit one as a solid disc), so the grid shape stays visible
        // and an all-off scene shows the layout instead of a black screen. (The count is
        // already bounded by the table's stride downsampling for large grids.)
        vertsBuf[vi++] = previewCoords_[i * 3 + 0];
        vertsBuf[vi++] = previewCoords_[i * 3 + 1];
        vertsBuf[vi++] = previewCoords_[i * 3 + 2];
        vertsBuf[vi++] = rgb ? rgb[i * 3] / 255 : 0;
        vertsBuf[vi++] = rgb ? rgb[i * 3 + 1] / 255 : 0;
        vertsBuf[vi++] = rgb ? rgb[i * 3 + 2] / 255 : 0;
    }
    const vertCount = vi / 6;

    if (vi === 0) return;  // no coords at all — keep the last geometry, let rAF idle
    lastVerts = vertsBuf.subarray(0, vi);
    lastVertCount = vertCount;
    lastMaxDim = previewMaxDim_;

    if (camAutoFit && previewBox_) {
        camAutoFit = false;
        const canvas = document.getElementById("preview");
        const aspect = canvas ? canvas.clientWidth / Math.max(1, canvas.clientHeight) : 1;
        const bx = previewBox_.x, by = previewBox_.y, bz = previewBox_.z;
        let halfExtent = 0.5 * Math.sqrt(bx * bx + by * by + bz * bz) / previewMaxDim_;
        // With moving heads the BEAMS are most of what is on screen, and they all start at the
        // fixtures and run outward, so the lit volume sits well to one side of the fixture
        // centroid. Orbiting around that centroid then feels like orbiting a point behind the
        // rig. Pivot on the middle of the beams instead, and widen the fit to include them.
        // The pivot stays on the FIXTURES (the coordinate table is already box-centered on them,
        // so that is the origin). Deliberately not the beams' midpoint: beams swing as the rig
        // moves, and a pivot that drifted with them would make the scene wander under a drag.
        // The fit still widens for the beams so they stay in frame.
        if (previewAim_) halfExtent += BEAM_LEN / 2;
        const fitDist = halfExtent / Math.tan(fov / 2) * (aspect < 1 ? 1 / aspect : 1) * 1.1;
        camDist = Math.max(0.5, Math.min(10, fitDist));
    }

    if (!glLoopRunning) startRenderLoop();
}

function redrawCached() {
    if (!lastVerts) return;
    if (!glLoopRunning) startRenderLoop();
}

// The preview "reset" (⌖) button: restore the WHOLE preview to defaults — camera, dot-size
// slider, sequence-number toggle, and docked/PiP layout. One button clears every preview
// preference (all browser-local; nothing touches the device). Lives here since it owns the
// camera + dot/number state; the layout part defers to setupLayout's resetLayout_ hook.
function resetCamera() {
    // Camera: forget the saved orbit + re-fit on the next frame.
    localStorage.removeItem("mm_cam");
    camTheta = Math.PI;
    camPhi = 0.4;
    camTgtX = camTgtY = camTgtZ = 0;   // recenter the pan target (cursor-zoom resets too)
    camAutoFit = true;

    // Dot size: back to the auto "filled-panel" base (1×); sync the slider control.
    dotScale_ = 1;
    localStorage.removeItem("mm_preview_dot");
    const dotSlider = document.getElementById("preview-dot");
    if (dotSlider) dotSlider.value = "1";

    // Sequence numbers: off; clear the № button's active state.
    showSeqNumbers_ = false;
    document.getElementById("preview-numbers")?.classList.remove("active");

    // Layout: back to auto docked/PiP, not dismissed, default corner.
    if (resetLayout_) resetLayout_();

    if (lastVerts) redrawCached();
}

function startRenderLoop() {
    if (glLoopRunning) return;
    glLoopRunning = true;
    function loop() {
        if (!lastVerts) { glLoopRunning = false; return; }
        drawVerts();
        requestAnimationFrame(loop);
    }
    requestAnimationFrame(loop);
}

function drawVerts() {
    if (!gl || !lastVerts || !glLocs) return;
    const canvas = document.getElementById("preview");
    const cw = Math.round(canvas.clientWidth), ch = Math.round(canvas.clientHeight);
    if (canvas.width !== cw || canvas.height !== ch) {
        canvas.width = cw;
        canvas.height = ch;
    }
    gl.viewport(0, 0, canvas.width, canvas.height);

    // Eye orbits the target at camDist; the view looks AT the target (not the origin), so
    // cursor-anchored zoom can pan the target without changing the orbit angles.
    const ex = camTgtX + camDist * Math.cos(camPhi) * Math.sin(camTheta);
    const ey = camTgtY + camDist * Math.sin(camPhi);
    const ez = camTgtZ + camDist * Math.cos(camPhi) * Math.cos(camTheta);
    const mvp = buildMVP(ex, ey, ez, camTgtX, camTgtY, camTgtZ, canvas.width / Math.max(1, canvas.height));

    // alpha:false context — clear to page background color so the canvas
    // blends seamlessly in both light and dark themes. Read from <body>, not
    // <html>: the theme override is `body[data-theme="light"]`, so --bg-0 is
    // redefined on the body; getComputedStyle(documentElement) would only ever
    // see the dark :root default and never the light override.
    const bg = getComputedStyle(document.body).getPropertyValue("--bg-0").trim();
    const m = bg.match(/^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i);
    if (m) {
        const cr = parseInt(m[1],16)/255, cg = parseInt(m[2],16)/255, cb = parseInt(m[3],16)/255;
        gl.clearColor(cr, cg, cb, 1.0);
        // Kept for drawBeams: an ADDITIVE ghost beam can only brighten, so on a light theme it
        // has to be drawn a different way to be seen at all (see drawBeams).
        bgLuma_ = 0.2126 * cr + 0.7152 * cg + 0.0722 * cb;
    }
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    gl.bindBuffer(gl.ARRAY_BUFFER, glBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, lastVerts, gl.DYNAMIC_DRAW);

    gl.enableVertexAttribArray(glLocs.aPos);
    gl.enableVertexAttribArray(glLocs.aCol);
    gl.vertexAttribPointer(glLocs.aPos, 3, gl.FLOAT, false, 24, 0);
    gl.vertexAttribPointer(glLocs.aCol, 3, gl.FLOAT, false, 24, 12);

    gl.uniformMatrix4fv(glLocs.uMVP, false, mvp);
    // Size each dot to the spacing between the SAMPLED points so the layout reads as a filled
    // panel (¾ light, ¼ gap) at any size — a big grid is spatially downsampled (the device
    // sends ~1800 lattice points), so sizing by the full dimension left each dot a fraction of
    // its cell with big gaps. The sampled points fill the bounding box uniformly, so the pitch
    // between neighbors (in grid units) is (boxVolume / count)^(1/activeDims): the square root
    // for a flat grid, the CUBE root for a 3D volume (a cube's points spread over depth, so a
    // flat √ undercounts the pitch and the dots come out too small — the 3D-gap bug). Convert
    // that grid pitch to on-screen pixels (canvas px per grid unit) and take 75% of it. The
    // shader depth-corrects per point, so zooming still enlarges the dots beyond this base.
    const bX = previewBox_ ? Math.max(1, previewBox_.x) : 1;
    const bY = previewBox_ ? Math.max(1, previewBox_.y) : 1;
    const bZ = previewBox_ ? Math.max(1, previewBox_.z) : 1;
    const dims = (previewBox_ ? [bX, bY, bZ].filter(d => d > 1).length : 2) || 1;  // active axes
    const volume = (bX > 1 ? bX : 1) * (bY > 1 ? bY : 1) * (bZ > 1 ? bZ : 1);
    const gridPitch = Math.pow(volume / Math.max(1, lastVertCount), 1 / dims);     // grid units
    const pxPerGridUnit = canvas.width / lastMaxDim;
    const pointSize = Math.max(2, gridPitch * pxPerGridUnit * 0.75 * dotScale_);
    gl.uniform1f(glLocs.uPointSize, pointSize);
    // LOD: the off-LED placeholder rings are useful when sprites are big enough to show a
    // hollow rim, but become visual mud once sprites are tiny (a dense grid zoomed out).
    // Fade them by base sprite size — full rings ≥8px, gone ≤4px — so the layout shows on
    // small/zoomed grids and the lit pattern reads cleanly when dense. Lit dots are never
    // faded (their alpha ignores uRingFade in the shader).
    let ringFade = Math.max(0, Math.min(1, (pointSize - 4) / 4));
    // A VOLUME needs the opposite of a flat grid. On a panel the placeholders sit in one plane and
    // an opaque one costs nothing; in a cube every dark LED is in front of some other LED, so at a
    // large dot size the placeholders stack into a solid grey wall and the lit pattern inside it
    // cannot be seen at all. Fade them by the depth they have to be seen through, and further as
    // the dots grow: the layout still reads, and the effect shows through it.
    if (dims > 2) {
        // A volume stacks its placeholders: seen through N slices they compose as 1-(1-a)^N, so
        // the same alpha that is a light tint on a panel is a wall in a cube. Solve for the per-LED
        // value that holds the TOTAL at a quarter whatever the depth, so the layout stays readable
        // without hiding the effect inside it. (The occlusion itself is fixed above, by not writing
        // depth; this is what keeps the remaining tint from adding up.)
        const kVolumeHaze = 0.25;
        const perLed = 1 - Math.pow(1 - kVolumeHaze, 1 / Math.max(1, bZ));
        ringFade = Math.min(ringFade, perLed / 0.22);   // 0.22 is the shader's base alpha
    }
    gl.uniform1f(glLocs.uRingFade, ringFade);

    // Two passes so lit LEDs always sit ABOVE the grey placeholders. On a flat grid all LEDs share
    // a z-plane, so a single pass let draw order + z-fighting clip a lit dot behind a neighbor's
    // placeholder. Pass 1 draws the off-LED placeholders, pass 2 the lit ones with depthFunc LEQUAL
    // so they land on top.
    // Placeholders do NOT write depth. They are decoration, not geometry: an unlit LED that
    // occupies the depth buffer HIDES every lit LED behind it, however transparent it looks, since
    // the depth test rejects the later fragment before its alpha is ever considered. That is what
    // made a cube a solid wall at large dot sizes: not the grey, the depth. With the write off, a
    // dark LED tints what is behind it and nothing more, so a volume is seen through.
    // Pass 1, placeholders: depth TEST on (they hide behind lit LEDs in front of them) but depth
    // WRITE off, so an unlit LED never occupies the buffer.
    gl.depthMask(false);
    gl.uniform1f(glLocs.uLitPass, 0.0);
    gl.drawArrays(gl.POINTS, 0, lastVertCount);
    // Pass 2, lit LEDs: depth write back ON, so they depth-sort against EACH OTHER in a volume.
    // Leaving it off here was the bug's other half: a lit LED at the back of a cube then drew over
    // one at the front, because nothing recorded which was nearer.
    gl.depthMask(true);
    gl.depthFunc(gl.LEQUAL);                            // beats a co-located placeholder
    gl.uniform1f(glLocs.uLitPass, 1.0);
    gl.drawArrays(gl.POINTS, 0, lastVertCount);
    gl.depthFunc(gl.LESS);

    drawBoundingBox(mvp);
    drawBeams(mvp);
    drawSeqLabels(mvp, canvas, pointSize);
}

// Faint wireframe cuboid around the light volume. Rebuilt only when the box extent
// changes (cached by boxKey). Half-extents are box/2/maxDim — matching the same
// normalisation the point coords use (pos/maxDim - 0.5*box/maxDim), so the cuboid's
// faces pass through the outermost LED centers.
// Draw each moving head's beam: a short 3D ray from the fixture in the direction it is aimed.
//
// Returns immediately when the rig has no aim data, which is every LED strip and panel: the device
// does not send the 0x04 message for those, so previewAim_ stays null and this is one test.
//
// Pan/tilt arrive as the fixture's own 0..255 bytes, and are turned into a direction HERE rather
// than on the device: the wire says where the head points, the browser decides what that looks
// like. Drawing it as a line rather than a cone is deliberate for now, since beam width and throw
// distance are fixture attributes the fixture model does not carry yet, and a cone would mean
// inventing them.
// The unit direction head `i` points, from its raw (pan, tilt) bytes.
//
// A fixture's pan sweeps horizontally and its tilt vertically, both centered at 128, mapped onto
// the fixture's real travel: 540 degrees of pan, 180 of tilt on a typical head.
//
// A rest beam points along -Z, OUT of the layout toward the viewer. Z is the scene's depth axis
// (architecture.md: 2D is the (x,y) face and 3D adds slices across Z), so X and Y are where the
// fixtures are ARRANGED and Z is the only axis free to shine along. Aiming down -Y instead would
// send each head along the axis its neighbors occupy, which is what a 1 x N chain of heads made
// obvious: every beam ran through the next fixture.
//
// Pan then sweeps in the (x,z) plane and tilt lifts toward +Y, so a centered head points straight
// out and the controls read the way they do on a real fixture. Written as sin/cos of the same
// angle so the direction is UNIT length: the cone's cross-axes are only unit (and the cone only
// round) when it is.
function beamDirection(i) {
    const pan  = ((previewAim_[i * 2 + 0] - 128) / 128) * Math.PI * 1.5;   // +-270 degrees
    const tilt = ((previewAim_[i * 2 + 1] - 128) / 128) * (Math.PI / 2);   // +-90 degrees
    const st = Math.sin(tilt), ctd = Math.cos(tilt);
    return {dx: Math.sin(pan) * ctd, dy: st, dz: -Math.cos(pan) * ctd};
}

function drawBeams(mvp) {
    if (!previewAim_ || !previewCoords_ || !beamProgram) return;
    // Stale aim: gathered against a table that is no longer active, so its indices name other
    // fixtures now. Skip until the next aim frame rather than draw beams from the wrong heads.
    // Against the TABLE's stride, which is what the aim frame is gathered at: the color frame's
    // own stride is the link's and moves independently, so comparing it dropped every beam on a
    // slow link.
    if (previewAimEpoch_ !== lastEpoch_ || previewAimStride_ !== previewTableStride_) return;
    const n = Math.min(previewAim_.length >> 1, previewCoords_.length / 3);
    if (n === 0) return;

    // The technique stage visualizers use for a cheap beam, and the reason each part is here:
    //   SOLID tapered geometry, radius 0 at the fixture widening to the far end, so it reads as a
    //     beam rather than an outline;
    //   ADDITIVE blending, so two beams crossing brighten where they overlap, which is what light
    //     in air actually does and is the cue that sells it;
    //   depth WRITE off, so beams never occlude each other or the lights behind them (they are
    //     participating media, not surfaces), while still being depth-TESTED against the scene;
    //   a very low alpha, faded along the length and toward the edge, so it glows rather than
    //     looking like a plastic cone.
    const SEG  = 12;           // radial segments: round enough at this size, cheap enough for a rig
    const LEN  = BEAM_LEN;     // beam length as a fraction of the scene, so it reads at any rig size
    const RAD  = 0.10;         // end-face radius: a stage beam, not a floodlight
    const HEAD_I = 0.85;       // intensity at the fixture
    const GHOST_I = 0.10;      // a dark head's aim indicator: visible, never mistakable for output
    // Two triangles per segment (a quad from the apex ring to the end ring), 3 verts each, 7
    // floats per vert (x, y, z, intensity, r, g, b).
    const FPV = 7;
    const FLOATS_PER_BEAM = SEG * 6 * FPV;
    const need = n * FLOATS_PER_BEAM;
    if (!beamVerts_ || beamVerts_.length < need) beamVerts_ = new Float32Array(need);

    // Lit beams and ghosts need DIFFERENT blending (see the draw calls below), so they are built
    // into one buffer in two runs: lit first, ghosts after, and each is drawn with its own mode.
    // Two passes over the heads, no second allocation.
    let k = 0;
    let ghostStart = 0;
    for (let pass = 0; pass < 2; pass++) {
    const wantGhost = pass === 1;
    if (wantGhost) ghostStart = k;
    for (let i = 0; i < n; i++) {
        const x = previewCoords_[i * 3 + 0];
        const y = previewCoords_[i * 3 + 1];
        const z = previewCoords_[i * 3 + 2];

        const {dx, dy, dz} = beamDirection(i);

        // Two axes across the beam. u = d x world, picking the world axis the beam is LEAST
        // aligned to so the cross never collapses: a head at rest points straight along -Z,
        // exactly where a fixed up-vector aligned to Z would degenerate to zero.
        const wy = Math.abs(dy) < 0.9 ? 1 : 0;
        const wz = wy ? 0 : 1;
        let ux = dy * wz - dz * wy;
        let uy = dz * 0  - dx * wz;
        let uz = dx * wy - dy * 0;
        const ul = Math.hypot(ux, uy, uz) || 1;
        ux /= ul; uy /= ul; uz /= ul;
        const vx = dy * uz - dz * uy, vy = dz * ux - dx * uz, vz = dx * uy - dy * ux;

        const cx = x + dx * LEN, cy = y + dy * LEN, cz = z + dz * LEN;   // end-face center

        // The beam takes the color the FIXTURE is emitting, which is how a visualizer reads as
        // a lighting plot rather than a diagram: a rig of blue beams with one amber head shows
        // the amber head at a glance. Normalized to the brightest channel so a dim fixture still
        // shows its HUE (a beam at 10% is a dim beam of the same color, not a grey one); the
        // per-vertex intensity above already carries the brightness falloff.
        //
        // Whole cone, not just a core: an air beam is one volume of scattered light, so tinting
        // only the center would read as two separate objects. The shader instead desaturates the
        // hot core toward white, which is the real effect that "colored edges, white middle"
        // describes.
        let br = 1.0, bg = 0.93, bb = 0.75;   // warm white until a color frame arrives
        let headI = HEAD_I;
        const haveColor = previewRgb_ && i * 3 + 2 < previewRgb_.length;
        if (!haveColor && wantGhost) continue;   // no frame yet: a normal warm-white beam, pass 0
        if (haveColor) {
            const r8 = previewRgb_[i * 3], g8 = previewRgb_[i * 3 + 1], b8 = previewRgb_[i * 3 + 2];
            const peak = Math.max(r8, g8, b8);
            if ((peak === 0) !== wantGhost) continue;   // this head belongs to the other pass
            if (peak > 0) {
                br = r8 / peak; bg = g8 / peak; bb = b8 / peak;
            } else {
                // A blacked-out head keeps a GHOST beam: it still has an aim, and where a dark
                // head points is exactly what someone watches while programming a move. Drawn
                // faint and neutral so it never reads as emitted light (the rig's real state
                // stays honest), but bright enough to show the direction, the same trick
                // drawLights uses when it draws an off light as a placeholder ring.
                br = bg = bb = 1.0;
                headI = GHOST_I;
            }
        }

        for (let sgi = 0; sgi < SEG; sgi++) {
            const a0 = (sgi / SEG) * Math.PI * 2, a1 = ((sgi + 1) / SEG) * Math.PI * 2;
            const c0 = Math.cos(a0) * RAD, s0 = Math.sin(a0) * RAD;
            const c1 = Math.cos(a1) * RAD, s1 = Math.sin(a1) * RAD;
            const e0x = cx + ux * c0 + vx * s0, e0y = cy + uy * c0 + vy * s0, e0z = cz + uz * c0 + vz * s0;
            const e1x = cx + ux * c1 + vx * s1, e1y = cy + uy * c1 + vy * s1, e1z = cz + uz * c1 + vz * s1;

            // Apex is a point, so both triangles share it; the far ring fades to nothing, which is
            // the length falloff. Edge falloff comes free: the cone is thinnest (least overlapping
            // geometry) at its silhouette.
            const vert = (vx_, vy_, vz_, vi_) => {
                beamVerts_[k++] = vx_; beamVerts_[k++] = vy_; beamVerts_[k++] = vz_;
                beamVerts_[k++] = vi_;
                beamVerts_[k++] = br;  beamVerts_[k++] = bg;  beamVerts_[k++] = bb;
            };
            vert(x, y, z, headI);
            vert(e0x, e0y, e0z, 0);
            vert(e1x, e1y, e1z, 0);

            // A second, narrower inner triangle brightens the core, so the beam has a hot center
            // and soft edges rather than a flat wash.
            vert(x, y, z, headI);
            const coreI = 0.25 * (headI / HEAD_I);   // the core scales with the head, so a ghost stays faint
            vert(cx + (e0x - cx) * 0.35, cy + (e0y - cy) * 0.35, cz + (e0z - cz) * 0.35, coreI);
            vert(cx + (e1x - cx) * 0.35, cy + (e1y - cy) * 0.35, cz + (e1z - cz) * 0.35, coreI);
        }
    }
    }

    if (k === 0) return;
    const verts = k / FPV;
    const litVerts = ghostStart / FPV;
    gl.useProgram(beamProgram);
    gl.bindBuffer(gl.ARRAY_BUFFER, beamBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, beamVerts_.subarray(0, k), gl.DYNAMIC_DRAW);
    const STRIDE = FPV * 4;
    gl.enableVertexAttribArray(beamLocs.aPos);
    gl.vertexAttribPointer(beamLocs.aPos, 3, gl.FLOAT, false, STRIDE, 0);
    gl.enableVertexAttribArray(beamLocs.aI);
    gl.vertexAttribPointer(beamLocs.aI, 1, gl.FLOAT, false, STRIDE, 12);
    gl.enableVertexAttribArray(beamLocs.aColor);
    gl.vertexAttribPointer(beamLocs.aColor, 3, gl.FLOAT, false, STRIDE, 16);
    gl.uniformMatrix4fv(beamLocs.uMVP, false, mvp);

    gl.depthMask(false);                  // media, not surfaces: never occlude what is behind

    // Lit beams: ADDITIVE, so crossing beams brighten where they overlap, which is what light in
    // air does and is the cue that sells it.
    if (litVerts > 0) {
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
        gl.drawArrays(gl.TRIANGLES, 0, litVerts);
    }

    // Ghost beams (a dark head's aim) cannot use the same blend on a LIGHT theme: additive only
    // ever brightens, and nothing brightens visibly against a near-white background. So a ghost
    // darkens there instead, which is the only direction with contrast to spare. On a dark theme
    // it stays additive like everything else.
    if (verts > litVerts) {
        if (bgLuma_ > 0.5) gl.blendFunc(gl.ZERO, gl.ONE_MINUS_SRC_ALPHA);   // darken toward the bg
        else               gl.blendFunc(gl.SRC_ALPHA, gl.ONE);              // brighten as usual
        gl.drawArrays(gl.TRIANGLES, litVerts, verts - litVerts);
    }

    gl.depthMask(true);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);   // restore for the rest of the scene

    gl.disableVertexAttribArray(beamLocs.aI);
    gl.disableVertexAttribArray(beamLocs.aColor);
    gl.useProgram(glProgram);   // restore the points program, as drawBoundingBox does
}

function drawBoundingBox(mvp) {
    if (!lineProgram || !previewBox_ || !previewMaxDim_) return;
    const md = previewMaxDim_;
    const key = previewBox_.x + "x" + previewBox_.y + "x" + previewBox_.z + "@" + md;
    if (key !== boxKey) {
        const hx = (previewBox_.x) / 2 / md, hy = (previewBox_.y) / 2 / md, hz = (previewBox_.z) / 2 / md;
        // 8 corners → 12 edges → 24 endpoints.
        const c = [
            [-hx,-hy,-hz],[ hx,-hy,-hz],[ hx, hy,-hz],[-hx, hy,-hz],
            [-hx,-hy, hz],[ hx,-hy, hz],[ hx, hy, hz],[-hx, hy, hz],
        ];
        const E = [[0,1],[1,2],[2,3],[3,0],[4,5],[5,6],[6,7],[7,4],[0,4],[1,5],[2,6],[3,7]];
        boxVerts = new Float32Array(E.length * 6);
        let k = 0;
        for (const [a, b] of E) { boxVerts.set(c[a], k); k += 3; boxVerts.set(c[b], k); k += 3; }
        boxKey = key;
    }
    gl.useProgram(lineProgram);
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, boxVerts, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(lineLocs.aPos);
    gl.vertexAttribPointer(lineLocs.aPos, 3, gl.FLOAT, false, 0, 0);
    gl.uniformMatrix4fv(lineLocs.uMVP, false, mvp);
    // Faint, theme-neutral grey — visible on both dark and light backgrounds.
    gl.uniform4f(lineLocs.uColor, 0.5, 0.5, 0.55, 0.25);
    gl.drawArrays(gl.LINES, 0, boxVerts.length / 3);
    gl.useProgram(glProgram);   // restore the points program for the next frame
}

// Sequence-number overlay. WebGL point sprites can't draw text, so each light's index is
// rendered onto a 2D canvas laid over #preview: project the light's position through the
// SAME mvp the GL render uses (so labels track LEDs in 2D AND 3D layouts), to a screen
// pixel, and draw its number. Legibility LOD: a number is drawn only if it FITS INSIDE its
// light bulb (the on-screen sprite), so it never overflows onto neighbors. The font is
// sized to the sprite, so as you zoom in (sprites grow, depth-corrected) more numbers fit
// and appear; zoomed out on a dense grid they don't fit and stay hidden. Behind-camera
// points (w ≤ 0) are skipped — essential for 3D.
function drawSeqLabels(mvp, glCanvas, pointSize) {
    const lc = document.getElementById("preview-labels");
    if (!lc) return;
    // Match the overlay to the GL canvas's on-screen box (the pane also holds the bar
    // above the canvas, so anchor to #preview's rect, not the pane's).
    const gr = glCanvas.getBoundingClientRect();
    const pr = lc.parentElement.getBoundingClientRect();
    lc.style.left = (gr.left - pr.left) + "px";
    lc.style.top = (gr.top - pr.top) + "px";
    lc.style.width = gr.width + "px";
    lc.style.height = gr.height + "px";
    const W = Math.max(1, Math.round(gr.width)), H = Math.max(1, Math.round(gr.height));
    if (lc.width !== W || lc.height !== H) { lc.width = W; lc.height = H; }
    const ctx = lc.getContext("2d");
    ctx.clearRect(0, 0, W, H);

    if (!showSeqNumbers_ || !previewCoords_ || previewCoordCount_ === 0) return;

    // Project every sent point; keep those in front of the camera + inside the viewport.
    // mvp is column-major: clip[r] = Σ_c mvp[c*4+r] * v[c].
    const proj = [];
    for (let i = 0; i < previewCoordCount_; i++) {
        const x = previewCoords_[i*3], y = previewCoords_[i*3+1], z = previewCoords_[i*3+2];
        const cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        if (cw <= 0) continue;                                   // behind the camera (3D)
        const cx = mvp[0] * x + mvp[4] * y + mvp[8]  * z + mvp[12];
        const cy = mvp[1] * x + mvp[5] * y + mvp[9]  * z + mvp[13];
        const sx = (cx / cw * 0.5 + 0.5) * W;
        const sy = (1 - (cy / cw * 0.5 + 0.5)) * H;              // GL y-up → canvas y-down
        if (sx < 0 || sx > W || sy < 0 || sy > H) continue;      // off-screen
        // The bulb's on-screen diameter, in CSS px. The shader's gl_PointSize = uPointSize/w
        // (same depth correction) is clamped to glMaxPointSize by the driver, so the DRAWN
        // bulb can't exceed that — clamp here too (in backing px) before converting to CSS
        // px (sprite px are backing px; the overlay is CSS px), or the fit-check would
        // believe a bulb is bigger than it renders and labels would never appear at the cap.
        const cssPerBacking = W / glCanvas.width;
        const diam = Math.min(pointSize / cw, glMaxPointSize) * cssPerBacking;
        // Label = the sent-point index. At full resolution (previewStride_==1) that IS the
        // driver light index. When downsampled (>1) the device sends a spatial LATTICE, not
        // every Nth flat light, so the true driver index isn't i*stride — we show the sent
        // index i (monotonic, still useful for orientation) rather than a wrong number.
        proj.push({ n: i, sx, sy, depth: cw, diam });
    }
    if (proj.length === 0) return;

    // Nearest first so a label drawn later (farther) doesn't overwrite a closer one's slot.
    proj.sort((a, b) => a.depth - b.depth);
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    for (const p of proj) {
        const t = String(p.n);
        // "Fits in the bulb or hide": size the font so the number's WIDTH fills ~85% of the
        // bulb (monospace digit ≈ 0.6em wide, so width ≈ digits*0.6*fontPx), capped so a
        // 1-digit number isn't comically tall. Draw only if the resulting font is readable
        // (≥7px). A dense grid zoomed out → tiny bulbs → fontPx<7 → hidden; zoom in → bulbs
        // grow → the same numbers cross 7px and appear. (A 3-digit number needs a ~3× bigger
        // bulb than a 1-digit one to show — exactly right: more digits need more room.)
        const widthLimited = (p.diam * 0.85) / (t.length * 0.6);
        const fontPx = Math.min(widthLimited, p.diam * 0.8);
        if (fontPx < 7) continue;                       // too small to read at this zoom
        ctx.font = `${fontPx}px ui-monospace, monospace`;
        // A dark halo so the number reads over both lit LEDs and the dark background.
        ctx.lineWidth = Math.max(2, fontPx * 0.18);
        ctx.strokeStyle = "rgba(0,0,0,0.85)";
        ctx.strokeText(t, p.sx, p.sy);
        ctx.fillStyle = "#fff";
        ctx.fillText(t, p.sx, p.sy);
    }
}

function buildMVP(ex, ey, ez, tx, ty, tz, aspect) {
    // forward = normalize(target - eye)
    const dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const fLen = Math.sqrt(dx*dx + dy*dy + dz*dz) || 1;
    const fx = dx/fLen, fy = dy/fLen, fz = dz/fLen;
    // Right = cross(forward, (0,1,0))
    let rx = fz, ry = 0, rz = -fx;
    const rLen = Math.sqrt(rx*rx + ry*ry + rz*rz) || 1;
    rx /= rLen; ry /= rLen; rz /= rLen;
    // Up = cross(right, forward)
    const ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;

    const view = [
        rx, ux, -fx, 0,
        ry, uy, -fy, 0,
        rz, uz, -fz, 0,
        -(rx*ex+ry*ey+rz*ez), -(ux*ex+uy*ey+uz*ez), (fx*ex+fy*ey+fz*ez), 1
    ];

    // near is small so a deep zoom-in (camDist down to CAM_MIN) doesn't clip the LEDs in
    // front of the camera away before their sequence numbers get big enough to read.
    const near = 0.01, far = 50, fov = 0.8;
    const f = 1 / Math.tan(fov / 2);
    const proj = [
        f/aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (far+near)/(near-far), -1,
        0, 0, 2*far*near/(near-far), 0
    ];

    const m = new Float32Array(16);
    for (let i = 0; i < 4; i++) {
        for (let j = 0; j < 4; j++) {
            m[j*4+i] = 0;
            for (let k = 0; k < 4; k++) {
                m[j*4+i] += proj[k*4+i] * view[j*4+k];
            }
        }
    }
    return m;
}

// Public surface — the only entry points app.js touches.
// Set by app.js: called with true when the preview wants frames, false when it is dismissed.
let wantsFrames_ = null;

// --- client-side adaptation (see preview-adapt.js for the controller itself) -------------------
// The loop runs only while the preview socket is open (app.js calls adaptStart/adaptStop with the
// socket lifecycle), so a hidden or dismissed pane costs nothing and sends nothing.
let adaptState_ = initialPullState();
let lastEpoch_ = null;          // geometry epoch of the active table; a change means a new canvas
const tableCache_ = new Map();  // "epoch:stride" -> {coords, count, maxDim, bx, by, bz}
let lastTableReq_ = 0;          // requestTable throttle stamp
let windowDrops_ = 0;           // drops reported by the device over the current window
let lastFrameAt_ = 0;           // self-repair: silence past ~2 s re-announces the standing request
let adaptFrames_ = 0;
let adaptTimer_ = null;
let adaptTargetFps_ = 24;   // fed from the device state by app.js (the user's targetFps control)
let sendRequest_ = null;    // installed by app.js: (bytes) => send them up the /wsp socket

// The standing frame request, the pull model's one recurring message:
// [0x51][stride][fps]. Sent on connect, on every controller decision, and as self-repair.
function announceRequest() {
    if (sendRequest_) sendRequest_([0x51, adaptState_.stride, adaptTargetFps_ & 0xff]);
}

function adaptTick() {
    // SELF-REPAIR: a device reboot loses every standing request, and the client cannot tell a
    // silent link from a forgotten one, so silence past one whole window re-announces. One tiny
    // message; a healthy stream renders it a no-op.
    if (adaptFrames_ === 0 && performance.now() - lastFrameAt_ > 2000) announceRequest();
    else if (adaptFrames_ > 0) lastFrameAt_ = performance.now();

    const delivered = adaptFrames_;
    const dropped = windowDrops_;
    adaptFrames_ = 0;
    windowDrops_ = 0;
    adaptState_ = nextPullState(adaptState_, delivered, dropped);
    if (adaptState_.request) announceRequest();
}

export const preview = {
    init: initWebGL,
    /// The adaptation loop follows the preview socket's lifecycle (app.js owns the socket).
    adaptStart() {
        adaptState_ = initialPullState();
        adaptFrames_ = 0;
        windowDrops_ = 0;
        lastFrameAt_ = performance.now();
        // ANNOUNCE the standing request immediately: under the pull model an unannounced client
        // receives NOTHING, the device serves only what is asked.
        announceRequest();
        if (!adaptTimer_) adaptTimer_ = setInterval(adaptTick, 2000);
    },
    adaptStop() {
        if (adaptTimer_) { clearInterval(adaptTimer_); adaptTimer_ = null; }
    },
    // A NEW target invalidates every conclusion measured against the old one (a source-limited
    // hold, a remembered failed stride), so the controller restarts clean and re-announces.
    setTargetFps(v) {
        if (!(v > 0) || v === adaptTargetFps_) return;
        adaptTargetFps_ = Math.min(25, v);
        adaptState_ = initialPullState();
        adaptFrames_ = 0;   // the frames counted so far belong to the OLD target's window
        windowDrops_ = 0;
        announceRequest();
    },
    onSendRequest(cb) { sendRequest_ = cb; },
    /// Install the frames-wanted callback and report the current state immediately, so the caller
    /// can open or close the preview socket without waiting for the next visibility change.
    onWantsFrames(cb) { wantsFrames_ = cb; if (cb) cb(!document.querySelector(".preview-hidden")); },
    setupLayout: setupLayout,
    onBinaryMessage: renderPreviewBinary,
    resetCamera: resetCamera,
    // Redraw the last frame with the current theme's background — call on a theme
    // toggle so an idle preview (no live frames) repaints to the new --bg-0
    // instead of keeping the previous theme's clear color until the next frame.
    redraw: redrawCached,
};
