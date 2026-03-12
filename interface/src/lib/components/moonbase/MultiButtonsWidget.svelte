<!--
   @title     MoonBase
   @file      MultiButtonsWidget.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2026 GitHub MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
-->

<script lang="ts">
	import { onDestroy } from 'svelte';

	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	export let property: any;
	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	export let value: any;
	export let onChange = (event: Event) => {
		console.log(event);
	};

	let dragSource: { row: number; col: number } | null = null;

	function handleDragStart(event: DragEvent, row: number, col: number) {
		dragSource = { row, col };
		event.dataTransfer?.setData('text/plain', `${row},${col}`);
	}

	function handleDrop(event: DragEvent, targetRow: number, targetCol: number) {
		if (dragSource && property.rows) {
			const temp = property.rows[dragSource.row][dragSource.col];
			property.rows[dragSource.row][dragSource.col] = property.rows[targetRow][targetCol];
			property.rows[targetRow][targetCol] = temp;
			dragSource = null;
		}
	}

	let hoverTimeout: ReturnType<typeof setTimeout> | null = null;
	let fileContent: { nodes?: { name: string; on: boolean }[] } | string | null;
	let popupCell: number | null = null;
	let popupX = 0;
	let popupY = 0;

	function handleMouseEnter(cell: number, event: MouseEvent, savedPreset: boolean) {
		hoverTimeout = setTimeout(async () => {
			if (!property.hoverToServer && savedPreset) {
				const response = await fetch(
					`/rest/file/.config/presets/preset${cell.toString().padStart(2, '0')}.json`
				);
				if (response.ok) {
					fileContent = await response.json();
					console.log(fileContent);
				} else {
					fileContent = 'Failed to load file.';
				}
				popupCell = cell;
			}
			// Offset the popup a bit to the right and down
			popupX = event.clientX + 16;
			popupY = event.clientY + 16;
		}, 1000);
	}

	function handleMouseLeave() {
		clearTimeout(hoverTimeout);
		popupCell = null;
		fileContent = null;
	}

	let clickTimeout: ReturnType<typeof setTimeout> | null = null;
	let preventClick = false;

	onDestroy(() => {
		if (hoverTimeout) clearTimeout(hoverTimeout);
		if (clickTimeout) clearTimeout(clickTimeout);
	});

	$: width = property.width || 8;
	$: count = value.count || 64;
	$: rows = Math.ceil(count / width);
</script>

{#snippet padButton(btnIdx: number, x: number, y: number)}
	<button
		class="btn {value.labels
			? 'flex-col !gap-0 overflow-hidden'
			: 'btn-square'} w-{property.size} h-{property.size} rounded-lg text-xl {value.selected ==
		btnIdx
			? `btn-error`
			: Array.isArray(value.list) && value.list.includes(btnIdx)
				? `btn-success`
				: 'btn-primary'}"
		type="button"
		draggable="true"
		ondragstart={(event) => handleDragStart(event, y, x)}
		ondragover={(event) => event.preventDefault()}
		ondrop={(event) => handleDrop(event, y, x)}
		onclick={(event: MouseEvent) => {
			preventClick = false;
			clickTimeout = setTimeout(() => {
				if (!preventClick) {
					value.select = btnIdx;
					console.log('click', y, x, value.select);
					if (value.selected == value.select) value.select = 255;
					value.selected = value.select;
					value.action = 'click';
					onChange(event);
				}
			}, 250);
		}}
		ondblclick={(event: MouseEvent) => {
			preventClick = true;
			clearTimeout(clickTimeout);
			value.select = btnIdx;
			console.log('dblclick', y, x, value.select);
			value.action = 'dblclick';
			onChange(event);
		}}
		onmouseenter={(event: MouseEvent) => {
			if (property.hoverToServer) {
				value.select = btnIdx;
				value.action = 'mouseenter';
				onChange(event);
			} else handleMouseEnter(btnIdx, event, value.list.includes(btnIdx));
		}}
		onmouseleave={(event: MouseEvent) => {
			if (property.hoverToServer) {
				value.select = btnIdx;
				value.action = 'mouseleave';
				onChange(event);
			} else handleMouseLeave();
		}}
	>
		{#if value.labels && Array.isArray(value.labels) && value.labels[value.list?.indexOf(btnIdx)]}
			<span class="block w-full truncate text-[9px] leading-tight opacity-80"
				>{value.labels[value.list.indexOf(btnIdx)]}</span
			>
		{:else}
			{btnIdx}
		{/if}
		{#if popupCell === btnIdx}
			<div
				class="fixed z-50 mt-2 inline-block min-h-0 min-w-0 rounded bg-neutral-100 p-6 text-left shadow-lg"
				style="left: {popupX}px; top: {popupY}px;"
			>
				{#if fileContent && fileContent.nodes}
					{#each fileContent.nodes as node, ni (ni)}
						{console.log('node.name', node.name)}
						<p>{node.name} {node.on ? 'on' : 'off'}</p>
					{/each}
				{/if}
			</div>
		{/if}
	</button>
{/snippet}

{#if property.wrap}
	<div class="flex flex-wrap gap-2">
		{#each Array(count) as _, i (i)}
			{@const x = i % width}
			{@const y = Math.floor(i / width)}
			{@render padButton(i + 1, x, y)}
		{/each}
	</div>
{:else}
	<div class="flex flex-col space-y-2">
		{#each Array(rows) as _, y (y)}
			<div class="flex flex-row space-x-2">
				{#each Array(width) as _, x (x)}
					{#if x + y * width < count}
						{@render padButton(x + y * width + 1, x, y)}
					{/if}
				{/each}
			</div>
		{/each}
	</div>
{/if}
<!-- btn btn-square btn-primary gives you square, colored buttons (DaisyUI).
flex flex-col and flex flex-row create the grid layout (TailwindCSS).
Use property.wrap = true for responsive wrapping instead of fixed rows.
Adjust space-x-2 and space-y-2 / gap-2 for spacing. -->
