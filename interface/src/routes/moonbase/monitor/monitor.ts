/**
 * Pure utility functions extracted from Monitor.svelte for testability.
 */

/**
 * Maps pixel coordinates [0, dim-1] to WebGL clip space [-1, 1].
 * Y and Z are inverted so that (0,0) is top-left in screen space.
 * When a dimension is 1, the coordinate collapses to 0 (centre).
 */
export function normalizePosition(
	x: number,
	y: number,
	z: number,
	width: number,
	height: number,
	depth: number
): [number, number, number] {
	return [
		width === 1 ? 0 : (x / (width - 1)) * 2.0 - 1.0,
		height === 1 ? 0 : ((height - 1 - y) / (height - 1)) * 2.0 - 1.0,
		depth === 1 ? 0 : ((depth - 1 - z) / (depth - 1)) * 2.0 - 1.0
	];
}
