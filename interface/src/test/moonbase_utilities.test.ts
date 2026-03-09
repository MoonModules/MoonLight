import { describe, expect, it } from 'vitest';
import { getTimeAgo, initCap } from '../lib/stores/moonbase_utilities';

describe('initCap', () => {
	it('capitalises first letter', () => {
		expect(initCap('hello')).toBe('Hello');
	});

	it('inserts space before uppercase transition', () => {
		expect(initCap('sensorStatus')).toBe('Sensor Status');
	});

	it('converts underscore to space (no re-capitalisation after separator)', () => {
		expect(initCap('pin_number')).toBe('Pin number');
	});

	it('converts hyphen to space (no re-capitalisation after separator)', () => {
		expect(initCap('my-label')).toBe('My label');
	});

	it('returns empty string for non-string input', () => {
		// eslint-disable-next-line @typescript-eslint/no-explicit-any
		expect(initCap(42 as any)).toBe('');
	});
});

describe('getTimeAgo', () => {
	const now = 1_000_000_000_000; // fixed "current" ms timestamp

	it('shows seconds when under a minute', () => {
		expect(getTimeAgo(now / 1000 - 42, now)).toBe('42s');
	});

	it('shows minutes and seconds', () => {
		expect(getTimeAgo(now / 1000 - 125, now)).toBe('2m5s');
	});

	it('shows hours and minutes', () => {
		expect(getTimeAgo(now / 1000 - 3661, now)).toBe('1h1m');
	});

	it('shows days and hours', () => {
		expect(getTimeAgo(now / 1000 - 90000, now)).toBe('1d1h');
	});
});
