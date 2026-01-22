<!--
   @title     MoonBase
   @file      FieldRenderer.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2026 Github MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

   Not w-full!
-->

<script lang="ts">
	import { onDestroy } from 'svelte';
	import FileEditWidget from '$lib/components/moonbase/FileEditWidget.svelte';
	import { initCap, getTimeAgo } from '$lib/stores/moonbase_utilities';

	export let property: any;
	export let value: any;
	export let onChange = (event: any) => {
		console.log(event);
	};
	export let hasNumber = true;
	export let disabled = false;
	export let step = 1;
	export let changeOnInput: boolean = true;
	export let noPrompts: boolean = false;

	//make getTimeAgo reactive
	export let currentTime = Date.now();
	// let currentTime = $state(Date.now());
	// Update the dummy variable every second
	const interval = setInterval(() => {
		currentTime = Date.now();
	}, 1000);

	// Throttling for slider control updates
	let throttleTimer: ReturnType<typeof setTimeout> | null = null;
	let pendingSliderEvent: Event | null = null;

	function handleSliderInput(event: Event) {
		// Update local value immediately for visual responsiveness
		// (bind:value already handles this)

		// Store the event for throttled sending
		pendingSliderEvent = event;

		// Only send to server once per 50ms
		if (!throttleTimer) {
			// Send immediately on first interaction
			onChange(event);
			pendingSliderEvent = null;

			throttleTimer = setTimeout(() => {
				if (pendingSliderEvent) {
					onChange(pendingSliderEvent);
					pendingSliderEvent = null;
				}
				throttleTimer = null;
			}, 50); // Max 20 updates/second
		}
	}

	// Clean up throttle timer on component destroy
	onDestroy(() => {
		clearInterval(interval);
		if (throttleTimer) {
			clearTimeout(throttleTimer);
			// Send final pending update if any
			if (pendingSliderEvent) {
				onChange(pendingSliderEvent);
			}
		}
	});

	let dragSource: { row: number; col: number } | null = null;

	function handleDragStart(event: DragEvent, row: number, col: number) {
		dragSource = { row, col };
		// Optionally, set drag data for external DnD
		event.dataTransfer?.setData('text/plain', `${row},${col}`);
	}

	function handleDrop(event: DragEvent, targetRow: number, targetCol: number) {
		if (dragSource && property.rows) {
			// Swap the cells
			const temp = property.rows[dragSource.row][dragSource.col];
			property.rows[dragSource.row][dragSource.col] = property.rows[targetRow][targetCol];
			property.rows[targetRow][targetCol] = temp;
			dragSource = null;
		}
	}

	let hoverTimeout: any = null;
	let fileContent: any;
	let popupCell: number | null = null;
	let popupX = 0;
	let popupY = 0;

	function handleMouseEnter(cell: number, event: MouseEvent, savedPreset: boolean) {
		hoverTimeout = setTimeout(async () => {
			if (!property.hoverToServer && savedPreset) {
				//open the file with cell
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
		// 2 seconds
	}

	function handleMouseLeave() {
		clearTimeout(hoverTimeout);
		popupCell = null;
		fileContent = null;
	}

	//precent onClick when dblClick
	let clickTimeout: any = null;
	let preventClick = false;

	// inspired by WLED
	function genPalPrev(hexString: string) {
		if (!hexString) return '';

		// Convert hex string to byte array
		const paletteData = [];
		for (let i = 0; i < hexString.length; i += 2) {
			paletteData.push(parseInt(hexString.substr(i, 2), 16));
		}

		// Parse palette data: groups of 4 bytes [index, r, g, b]
		const gradient = [];
		for (let i = 0; i < paletteData.length; i += 4) {
			const index = paletteData[i];
			const r = paletteData[i + 1];
			const g = paletteData[i + 2];
			const b = paletteData[i + 3];

			// Convert index from 0-255 to percentage
			const percent = Math.round((index / 255) * 100);

			gradient.push(`rgb(${r},${g},${b}) ${percent}%`);
		}

		return `background: linear-gradient(to right,${gradient.join(',')});`;
	}
</script>

<div class="flex-row flex items-center space-x-2 {!noPrompts ? 'mb-1' : ''}">
	{#if !noPrompts}
		<label class="label cursor-pointer min-w-24" for={property.name}>
			<span class="mr-4">{initCap(property.name)}</span>
		</label>
	{/if}

	{#if property.ro}
		{#if property.type == 'ip' || property.type == 'mDNSName'}
			<a href="http://{value}">{value}</a>
		{:else if property.type == 'time'}
			<span>{getTimeAgo(value, currentTime)}</span>
		{:else if property.type == 'coord3D' && value != null}
			<!-- value not null otherwise value.x etc can cause errors-->
			<span>{value.x}, {value.y}, {value.z}</span>
		{:else}
			<span>{value}</span>
		{/if}
	{:else if property.type == 'select' || property.type == 'selectFile'}
		<select bind:value on:change={onChange} class="select">
			{#each property.values as value, index}
				<option value={property.type == 'selectFile' ? value : index}>
					{value}
				</option>
			{/each}
		</select>
		{#if property.type == 'selectFile'}
			<FileEditWidget path={value} showEditor={false} />
		{/if}
	{:else if property.type == 'palette'}
		<div style="display: flex; gap: 8px; align-items: center;">
			<select bind:value on:change={onChange} class="select">
				{#each property.values as val, index}
					<option value={index}>{val.name}</option>
				{/each}
			</select>
			<div class="palette-preview" style={genPalPrev(property.values[value]?.colors)}></div>
		</div>

		<style>
			.palette-preview {
				width: 250px;
				height: 40px;
				border: 1px solid #ccc;
				border-radius: 3px;
			}
		</style>
	{:else if property.type == 'checkbox'}
		<input
			type="checkbox"
			class="toggle toggle-primary"
			bind:checked={value}
			on:change={onChange}
		/>
	{:else if property.type == 'slider'}
		<!-- range colors: https://daisyui.com/components/range/ 
         on:input: throttled response to server for performance
         -->
		<input
			type="range"
			min={property.min ? property.min : 0}
			max={property.max ? property.max : 255}
			{step}
			class={'flex-1 range ' +
				(disabled == false
					? property.color == 'Red'
						? 'range-error'
						: property.color == 'Green'
							? 'range-success'
							: 'range-primary'
					: 'range-secondary')}
			{disabled}
			bind:value
			on:input={handleSliderInput}
		/>
		{#if hasNumber}
			<input
				type="number"
				min={property.min ? property.min : 0}
				max={property.max ? property.max : 255}
				{step}
				class="input"
				style="height: 2rem; width: 5rem"
				{disabled}
				bind:value
				on:change={onChange}
			/>
		{/if}
	{:else if property.type == 'textarea'}
		<textarea
			rows="10"
			cols="61"
			class="w-full textarea"
			on:change={onChange}
			on:input={(event: any) => {
				if (changeOnInput) onChange(event);
			}}>{value}</textarea
		>
	{:else if property.type == 'file'}
		<input type="file" on:change={onChange} />
	{:else if property.type == 'number'}
		<input
			type="number"
			style="width: {String(property.max || 255).length + 5}ch"
			min={property.min ? property.min : 0}
			max={property.max ? property.max : 255}
			class="input invalid:border-error invalid:border-2"
			bind:value
			on:change={onChange}
			on:input={(event: any) => {
				if (changeOnInput) onChange(event);
			}}
		/>
	{:else if property.type == 'text'}
		<input
			type={property.type}
			class="input invalid:border-error invalid:border-2"
			minlength={property.min ? property.min : 0}
			maxlength={property.max ? property.max : 255}
			bind:value
			on:change={onChange}
			on:input={(event: any) => {
				if (changeOnInput) onChange(event);
			}}
		/>
	{:else if property.type == 'time'}
		<span>{getTimeAgo(value, currentTime)}</span>
	{:else if property.type == 'ip'}
		<input
			type={property.type}
			class="input invalid:border-error invalid:border-2"
			minlength="3"
			maxlength="15"
			bind:value
			on:change={onChange}
			on:input={(event: any) => {
				if (changeOnInput) onChange(event);
			}}
		/>
		<a href="http://{value}" target="_blank">Link</a>
	{:else if property.type == 'button'}
		<button
			class="btn btn-primary"
			type="button"
			on:click={(event: any) => {
				if (value == null) value = 1;
				else value++;
				onChange(event);
			}}>{property.name}</button
		>
	{:else if property.type == 'coord3D'}
		<input
			type="number"
			style="width: {String(property.max || 255).length + 5}ch"
			class="input invalid:border-error invalid:border-2"
			min="0"
			max="65536"
			bind:value={value.x}
			on:change={onChange}
		/>
		<input
			type="number"
			style="width: {String(property.max || 255).length + 5}ch"
			class="input invalid:border-error invalid:border-2"
			min="0"
			max="65536"
			bind:value={value.y}
			on:change={onChange}
		/>
		<input
			type="number"
			style="width: {String(property.max || 255).length + 5}ch"
			class="input invalid:border-error invalid:border-2"
			min="0"
			max="65536"
			bind:value={value.z}
			on:change={onChange}
		/>
	{:else if property.type == 'pad'}
		<div class="flex flex-col space-y-2">
			{#each Array(Math.ceil((value.count || 64) / (property.width || 8))) as _, y}
				<div class="flex flex-row space-x-2">
					{#each Array(property.width) as _, x}
						{#if x + y * property.width < value.count}
							<button
								class="btn btn-square w-{property.size} h-{property.size} text-xl rounded-lg {value.selected ==
								x + y * property.width + 1
									? `btn-error`
									: Array.isArray(value.list) && value.list.includes(x + y * property.width + 1)
										? `btn-success`
										: 'btn-primary'}"
								type="button"
								draggable="true"
								on:dragstart={(event) => handleDragStart(event, y, x)}
								on:dragover|preventDefault
								on:drop={(event) => handleDrop(event, y, x)}
								on:click={(event: any) => {
									preventClick = false;
									clickTimeout = setTimeout(() => {
										if (!preventClick) {
											value.select = x + y * property.width + 1;
											console.log('click', y, x, value.select);
											value.selected = value.select;
											value.action = 'click';
											onChange(event);
										}
									}, 250);
									// 250ms is a typical double-click threshold
								}}
								on:dblclick={(event: any) => {
									preventClick = true;
									clearTimeout(clickTimeout);
									value.select = x + y * property.width + 1;
									console.log('dblclick', y, x, value.select);
									value.action = 'dblclick';
									onChange(event);
								}}
								on:mouseenter={(event: any) => {
									// console.log("mousenter", rowIndex, colIndex, cell, value);
									if (property.hoverToServer) {
										value.select = x + y * property.width + 1;
										value.action = 'mouseenter';
										onChange(event);
									} else
										handleMouseEnter(
											x + y * property.width + 1,
											event,
											value.list.includes(x + y * property.width + 1)
										);
								}}
								on:mouseleave={(event: any) => {
									// console.log("mouseleave", rowIndex, colIndex, cell, value);
									if (property.hoverToServer) {
										value.select = x + y * property.width + 1;
										value.action = 'mouseleave';
										onChange(event);
									} else handleMouseLeave();
								}}
							>
								{x + y * property.width + 1}
								{#if popupCell === x + y * property.width + 1}
									<div
										class="fixed z-50 bg-neutral-100 p-6 rounded shadow-lg mt-2 min-h-0 text-left inline-block min-w-0"
										style="left: {popupX}px; top: {popupY}px;"
									>
										<!-- Popup for {cell} -->
										{#if fileContent && fileContent.nodes}
											{#each fileContent.nodes as node}
												{console.log('node.name', node.name)}
												<p>{node.name} {node.on ? 'on' : 'off'}</p>
											{/each}
										{/if}
									</div>
								{/if}
							</button>
						{/if}
					{/each}
				</div>
			{/each}
		</div>
		<!-- btn btn-square btn-primary gives you square, colored buttons (DaisyUI).
flex flex-col and flex flex-row create the grid layout (TailwindCSS).
Adjust space-x-2 and space-y-2 for spacing. -->
	{:else}
		<input
			type={property.type}
			class="input invalid:border-error invalid:border-2"
			bind:value
			on:change={onChange}
		/>
	{/if}
	{#if !noPrompts}
		{#if !property.ro && property.default != null && property.type != 'pad'}
			<button
				type="button"
				class="btn btn-ghost btn-sm"
				disabled={disabled ||
					(property.type == 'coord3D'
						? property.default.x == value.x &&
							property.default.y == value.y &&
							property.default.z == value.z
						: property.default == value)}
				on:click={(event: any) => {
					if (property.type == 'coord3D') {
						value.x = property.default.x;
						value.y = property.default.y;
						value.z = property.default.z;
					} else {
						value = property.default;
					}
					onChange(event);
				}}
				title={'Reset to default (' +
					(property.type == 'coord3D'
						? property.default.x + ',' + property.default.y + ',' + property.default.z
						: property.default) +
					')'}>↻</button
			>
		{/if}
		{#if property.desc}
			<label class="label cursor-pointer" for={property.desc}>
				<!-- <span class="text-md">{initCap(property.name)}</span> -->
				<span class="mr-4">{initCap(property.desc)}</span>
			</label>
		{/if}
	{/if}
</div>
