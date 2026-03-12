import { describe, expect, it } from 'vitest';
import { normalizePosition } from '../routes/moonbase/monitor/monitor';

describe('normalizePosition', () => {
	it('maps origin (0,0,0) to (-1, 1, 1) for a 3D grid', () => {
		// x: 0/(w-1)*2-1 = -1; y: (h-1-0)/(h-1)*2-1 = 1 (inverted); z: same as y = 1
		expect(normalizePosition(0, 0, 0, 10, 10, 10)).toEqual([-1, 1, 1]);
	});

	it('maps far corner (w-1, h-1, d-1) to (1, -1, -1)', () => {
		expect(normalizePosition(9, 9, 9, 10, 10, 10)).toEqual([1, -1, -1]);
	});

	it('maps centre pixel to (0, 0, 0) for odd-sized grid', () => {
		// 5x5x5, centre = (2,2,2): x=2/4*2-1=0; y=(4-2)/4*2-1=0; z=0
		const [x, y, z] = normalizePosition(2, 2, 2, 5, 5, 5);
		expect(x).toBeCloseTo(0);
		expect(y).toBeCloseTo(0);
		expect(z).toBeCloseTo(0);
	});

	it('collapses to 0 when dimension is 1 (single-pixel axis)', () => {
		expect(normalizePosition(0, 0, 0, 1, 1, 1)).toEqual([0, 0, 0]);
	});

	it('handles width=1 while height and depth are normal', () => {
		const [x, y, z] = normalizePosition(0, 0, 0, 1, 10, 10);
		expect(x).toBe(0); // collapsed
		expect(y).toBeCloseTo(1); // top row → +1
		expect(z).toBeCloseTo(1);
	});

	it('y is inverted: row 0 maps to +1, last row maps to -1', () => {
		const [, y0] = normalizePosition(0, 0, 0, 5, 5, 1);
		const [, yLast] = normalizePosition(0, 4, 0, 5, 5, 1);
		expect(y0).toBeCloseTo(1);
		expect(yLast).toBeCloseTo(-1);
	});

	it('x is not inverted: column 0 maps to -1, last column maps to +1', () => {
		const [x0] = normalizePosition(0, 0, 0, 5, 1, 1);
		const [xLast] = normalizePosition(4, 0, 0, 5, 1, 1);
		expect(x0).toBeCloseTo(-1);
		expect(xLast).toBeCloseTo(1);
	});

	it('z is inverted: front (0) maps to +1, back (d-1) maps to -1', () => {
		const [, , z0] = normalizePosition(0, 0, 0, 1, 1, 5);
		const [, , zLast] = normalizePosition(0, 0, 4, 1, 1, 5);
		expect(z0).toBeCloseTo(1);
		expect(zLast).toBeCloseTo(-1);
	});

	it('handles a 2D strip (depth=1)', () => {
		const [x, y, z] = normalizePosition(3, 1, 0, 8, 4, 1);
		expect(x).toBeCloseTo((3 / 7) * 2 - 1);
		expect(y).toBeCloseTo((2 / 3) * 2 - 1);
		expect(z).toBe(0); // depth=1 collapses
	});
});
