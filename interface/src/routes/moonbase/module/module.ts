/**
 * Pure utility functions extracted from Module.svelte for testability.
 */

/**
 * Recursively merges newData into oldData in-place, preserving Svelte reactivity.
 *
 * - Arrays: patched element by element; shrunk if newData is shorter.
 * - Array items: fully replaced when 'p' or 'name' changes (different control in same slot).
 * - Objects: merged recursively (partial patch — siblings not in newData are kept).
 * - Primitives: assigned only when the value actually changed.
 * - pruneMissing: when true, keys present in oldData but absent from newData are deleted.
 */
export function updateRecursive(
	oldData: Record<string, unknown>,
	newData: Record<string, unknown>,
	pruneMissing = false
) {
	for (let key in newData) {
		if (Array.isArray(newData[key])) {
			if (!Array.isArray(oldData[key])) oldData[key] = [];
			const oldArr = oldData[key] as unknown[];
			const newArr = newData[key] as unknown[];
			for (let i = 0; i < newArr.length; i++) {
				if (oldArr[i] == undefined) {
					oldArr[i] = newArr[i];
				} else {
					const oldItem = oldArr[i];
					const newItem = newArr[i];
					const bothObjects =
						oldItem !== null &&
						typeof oldItem === 'object' &&
						newItem !== null &&
						typeof newItem === 'object';
					if (bothObjects) {
						const oldObj = oldItem as Record<string, unknown>;
						const newObj = newItem as Record<string, unknown>;
						if (
							(newObj.p !== undefined && oldObj.p !== newObj.p) ||
							(newObj.name !== undefined && oldObj.name !== newObj.name)
						) {
							oldArr[i] = newItem;
						} else {
							updateRecursive(oldObj, newObj, pruneMissing);
						}
					} else if (oldItem !== newItem) {
						oldArr[i] = newItem;
					}
				}
			}
			if (oldArr.length > newArr.length) {
				oldArr.splice(newArr.length);
			}
		} else if (newData[key] !== null && typeof newData[key] === 'object') {
			if (
				oldData[key] === null ||
				typeof oldData[key] !== 'object' ||
				Array.isArray(oldData[key])
			) {
				oldData[key] = {};
			}
			updateRecursive(
				oldData[key] as Record<string, unknown>,
				newData[key] as Record<string, unknown>,
				pruneMissing
			);
		} else if (newData[key] !== oldData[key]) {
			oldData[key] = newData[key];
		}
	}
	if (pruneMissing) {
		for (let key in oldData) {
			if (!(key in newData)) delete oldData[key];
		}
	}
}
