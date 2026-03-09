import { describe, expect, it } from 'vitest';
import { updateRecursive } from '../routes/moonbase/module/module';

describe('updateRecursive', () => {
	it('updates a primitive value when changed', () => {
		const old = { a: 1 };
		updateRecursive(old, { a: 2 });
		expect(old.a).toBe(2);
	});

	it('does not reassign a primitive when unchanged', () => {
		const old = { a: 1 };
		const ref = old;
		updateRecursive(old, { a: 1 });
		expect(old).toBe(ref);
		expect(old.a).toBe(1);
	});

	it('adds a new key', () => {
		const old: Record<string, unknown> = { a: 1 };
		updateRecursive(old, { b: 2 });
		expect(old.b).toBe(2);
	});

	it('does not delete keys absent from newData by default (pruneMissing=false)', () => {
		const old = { a: 1, b: 2 };
		updateRecursive(old, { a: 10 });
		expect(old.b).toBe(2);
	});

	it('deletes keys absent from newData when pruneMissing=true', () => {
		const old: Record<string, unknown> = { a: 1, b: 2 };
		updateRecursive(old, { a: 10 }, true);
		expect(old.a).toBe(10);
		expect('b' in old).toBe(false);
	});

	it('merges nested objects recursively', () => {
		const old = { nested: { x: 1, y: 2 } };
		updateRecursive(old, { nested: { x: 99 } });
		expect((old.nested as Record<string, unknown>).x).toBe(99);
		expect((old.nested as Record<string, unknown>).y).toBe(2); // sibling preserved
	});

	it('normalizes non-object oldData[key] to {} before recursive merge', () => {
		const old: Record<string, unknown> = { nested: 'not-an-object' };
		updateRecursive(old, { nested: { x: 5 } });
		expect((old.nested as Record<string, unknown>).x).toBe(5);
	});

	it('normalizes non-array oldData[key] to [] before array merge', () => {
		const old: Record<string, unknown> = { arr: 'not-an-array' };
		updateRecursive(old, { arr: [1, 2] });
		expect(old.arr).toEqual([1, 2]);
	});

	it('appends to array when newArr is longer', () => {
		const old: Record<string, unknown> = { arr: [1] };
		updateRecursive(old, { arr: [1, 2, 3] });
		expect(old.arr).toEqual([1, 2, 3]);
	});

	it('truncates array when newArr is shorter', () => {
		const old: Record<string, unknown> = { arr: [1, 2, 3] };
		updateRecursive(old, { arr: [1] });
		expect(old.arr).toEqual([1]);
	});

	it('patches object items inside an array recursively', () => {
		const old: Record<string, unknown> = { arr: [{ name: 'fx', v: 1, extra: 9 }] };
		updateRecursive(old, { arr: [{ name: 'fx', v: 2 }] });
		const item = (old.arr as Record<string, unknown>[])[0];
		expect(item.v).toBe(2);
		expect(item.extra).toBe(9); // partial patch keeps siblings
	});

	it('fully replaces array item when "p" changes', () => {
		const old: Record<string, unknown> = { arr: [{ p: 'A', extra: 9 }] };
		updateRecursive(old, { arr: [{ p: 'B' }] });
		const item = (old.arr as Record<string, unknown>[])[0];
		expect(item.p).toBe('B');
		expect('extra' in item).toBe(false); // full replacement — no stale keys
	});

	it('fully replaces array item when "name" changes', () => {
		const old: Record<string, unknown> = { arr: [{ name: 'OldName', extra: 7 }] };
		updateRecursive(old, { arr: [{ name: 'NewName' }] });
		const item = (old.arr as Record<string, unknown>[])[0];
		expect(item.name).toBe('NewName');
		expect('extra' in item).toBe(false);
	});

	it('replaces a primitive array item when value differs', () => {
		const old: Record<string, unknown> = { arr: [1, 2, 3] };
		updateRecursive(old, { arr: [1, 99, 3] });
		expect(old.arr).toEqual([1, 99, 3]);
	});

	it('prunes nested object keys when pruneMissing=true', () => {
		const old: Record<string, unknown> = { nested: { keep: 1, drop: 2 } };
		updateRecursive(old, { nested: { keep: 10 } }, true);
		const n = old.nested as Record<string, unknown>;
		expect(n.keep).toBe(10);
		expect('drop' in n).toBe(false);
	});

	it('handles deeply nested structures', () => {
		const old = { a: { b: { c: { d: 1 } } } };
		updateRecursive(old, { a: { b: { c: { d: 99 } } } });
		expect(((old.a as Record<string, unknown>).b as Record<string, unknown>).c).toEqual({ d: 99 });
	});

	it('handles null in newData as a primitive (no recursive merge)', () => {
		const old: Record<string, unknown> = { a: { x: 1 } };
		updateRecursive(old, { a: null });
		expect(old.a).toBeNull();
	});
});
