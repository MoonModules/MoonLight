<!--
   @title     MoonBase
   @file      SearchableDropdown.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2026 GitHub MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

   Reusable searchable dropdown with category tabs and tag cloud filtering.
   Used by FieldRenderer for both node selector (selectFile) and palette selector.
-->

<script lang="ts">
	import { onMount, onDestroy, tick } from 'svelte';
	import { positionDropdown, extractEmojis } from '$lib/stores/moonbase_utilities';

	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	export let values: any[] = [];
	export let isSelected: (val: any, index: number) => boolean; // eslint-disable-line @typescript-eslint/no-explicit-any
	export let onSelect: (val: any, index: number, event: Event) => void; // eslint-disable-line @typescript-eslint/no-explicit-any
	export let disabled = false;
	export let showTags = false;
	export let minWidth = 'min-w-72';

	let open = false;
	let dropdownEl: HTMLElement | undefined;
	let listEl: HTMLElement | undefined;
	let search = '';
	let categoryFilter = '';
	let tagFilter = '';

	$: categories = [
		// eslint-disable-next-line @typescript-eslint/no-explicit-any
		...new Set(values.map((v: any) => v.category).filter((c: string) => c))
	] as string[];

	$: tags = showTags
		? (() => {
				const t = new Set<string>();
				for (const v of values) {
					for (const e of extractEmojis(v.name || '')) t.add(e);
				}
				return [...t];
			})()
		: [];

	$: filtered = values
		// eslint-disable-next-line @typescript-eslint/no-explicit-any
		.map((v: any, i: number) => ({ ...v, _idx: i }))
		// eslint-disable-next-line @typescript-eslint/no-explicit-any
		.filter((v: any) => {
			const name: string = v.name || '';
			if (categoryFilter && v.category !== categoryFilter) return false;
			if (tagFilter && !extractEmojis(name).includes(tagFilter)) return false;
			if (search && !name.toLowerCase().includes(search.toLowerCase())) return false;
			return true;
		});

	async function openDropdown() {
		open = true;
		search = '';
		await tick();
		if (!listEl || !dropdownEl) return;
		const triggerEl = dropdownEl.querySelector('button') as HTMLElement;
		positionDropdown(triggerEl, listEl);
		// Scroll selected item to center
		const selectedEl = listEl.querySelector('[aria-selected="true"]') as HTMLElement | null;
		if (selectedEl) {
			const listHeight = listEl.clientHeight;
			listEl.scrollTop = selectedEl.offsetTop - listHeight / 2 + selectedEl.offsetHeight / 2;
		}
		// Focus search input
		const searchInput = listEl.querySelector('input[type="text"]') as HTMLInputElement;
		if (searchInput) searchInput.focus();
	}

	export function toggle() {
		if (!disabled) {
			if (!open) openDropdown();
			else open = false;
		}
	}

	function closeOnOutsideClick(e: MouseEvent) {
		if (open && dropdownEl && !dropdownEl.contains(e.target as Node)) {
			open = false;
		}
	}

	onMount(() => {
		window.addEventListener('mousedown', closeOnOutsideClick);
	});

	onDestroy(() => {
		window.removeEventListener('mousedown', closeOnOutsideClick);
	});

	// Sticky top offset depends on how many header rows are visible
	$: hasCategoryRow = categories.length > 1;
	$: hasTagRow = tags.length > 0;
	// search bar is ~2.5rem, category row is ~2.25rem
	$: tagTop = hasCategoryRow ? 'top-[4.75rem]' : 'top-10';
</script>

<div class="relative" bind:this={dropdownEl}>
	<button
		type="button"
		class="select flex {minWidth} cursor-pointer items-center gap-2"
		{disabled}
		aria-haspopup="listbox"
		aria-expanded={open}
		onclick={() => toggle()}
	>
		<slot name="trigger">
			<span class="flex-1 truncate text-left text-sm">Select...</span>
		</slot>
		<span class="ml-1 text-xs opacity-60">&#9662;</span>
	</button>
	{#if open}
		<div
			bind:this={listEl}
			role="listbox"
			class="border-base-300 bg-base-100 z-50 {minWidth} max-h-96 overflow-y-auto rounded border shadow-xl"
		>
			<!-- Search input -->
			<div class="border-base-300 sticky top-0 z-10 border-b p-2">
				<input
					type="text"
					class="input input-sm w-full"
					placeholder="Search..."
					bind:value={search}
				/>
			</div>
			<!-- Category tabs -->
			{#if hasCategoryRow}
				<div class="border-base-300 sticky top-10 z-10 flex flex-wrap gap-1 border-b p-1">
					<button
						type="button"
						class="btn btn-xs {categoryFilter === '' ? 'btn-primary' : 'btn-ghost'}"
						onclick={() => {
							categoryFilter = '';
						}}>All</button
					>
					{#each categories as cat (cat)}
						<button
							type="button"
							class="btn btn-xs {categoryFilter === cat ? 'btn-primary' : 'btn-ghost'}"
							onclick={() => {
								categoryFilter = categoryFilter === cat ? '' : cat;
							}}>{cat}</button
						>
					{/each}
				</div>
			{/if}
			<!-- Tag cloud -->
			{#if hasTagRow}
				<div class="border-base-300 sticky {tagTop} z-10 flex flex-wrap gap-1 border-b p-1">
					{#each tags as tag (tag)}
						<button
							type="button"
							class="btn btn-xs btn-circle {tagFilter === tag ? 'btn-accent' : 'btn-ghost'}"
							onclick={() => {
								tagFilter = tagFilter === tag ? '' : tag;
							}}
							title={tag}>{tag}</button
						>
					{/each}
				</div>
			{/if}
			<!-- Items -->
			{#each filtered as val (val._idx)}
				<button
					type="button"
					role="option"
					aria-selected={isSelected(val, val._idx)}
					class="hover:bg-base-200 flex w-full cursor-pointer items-center gap-2 px-2 py-1.5 {isSelected(
						val,
						val._idx
					)
						? 'bg-base-300'
						: ''}"
					onclick={(event) => {
						open = false;
						onSelect(val, val._idx, event);
					}}
				>
					<slot name="item" {val} index={val._idx}>
						<span class="truncate text-sm">{val.name}</span>
					</slot>
				</button>
			{/each}
			{#if filtered.length === 0}
				<div class="p-2 text-sm opacity-50">No matches</div>
			{/if}
		</div>
	{/if}
</div>
