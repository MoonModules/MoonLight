<!--
   @title     MoonBase
   @file      FieldRenderer.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2026 GitHub MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

   Not w-full!
-->

<script lang="ts">
	import { onDestroy } from 'svelte';
	import { modals } from 'svelte-modals';
	import FileEditWidget from '$lib/components/moonbase/FileEditWidget.svelte';
	import MultiButtonsWidget from '$lib/components/moonbase/MultiButtonsWidget.svelte';
	import SearchableDropdown from '$lib/components/moonbase/SearchableDropdown.svelte';
	import { initCap, getTimeAgo } from '$lib/stores/moonbase_utilities';

	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	export let property: any;
	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	export let value: any;
	export let onChange = (event: Event) => {
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

	// Find currently selected node object by matching value string
	$: selectedNode =
		property.type === 'selectFile' && Array.isArray(property.values)
			? // eslint-disable-next-line @typescript-eslint/no-explicit-any
				property.values.find((v: any) => v.name === value)
			: undefined;

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

<div class="flex flex-row items-center space-x-2 {!noPrompts ? 'mb-1' : ''}">
	{#if !noPrompts}
		<label class="label min-w-24 cursor-pointer" for={property.name}>
			<span class="mr-4">{initCap(property.name)}</span>
		</label>
	{/if}

	{#if property.ro}
		{#if property.type == 'ip'}
			<a href="http://{value}">{value}</a>
		{:else if property.type == 'mDNSName'}
			<a href="http://{value}.local">{value}</a>
		{:else if property.type == 'time'}
			<span>{getTimeAgo(value, currentTime)}</span>
		{:else if property.type == 'coord3D' && value != null}
			<!-- value not null otherwise value.x etc can cause errors-->
			<span>{value.x}, {value.y}, {value.z}</span>
		{:else}
			<span>{value}</span>
		{/if}
	{:else if property.type == 'select'}
		<select bind:value onchange={onChange} class="select">
			{#each property.values as optionLabel, index (index)}
				<option value={index}>
					{optionLabel}
				</option>
			{/each}
		</select>
	{:else if property.type == 'selectFile'}
		<SearchableDropdown
			values={property.values ?? []}
			isSelected={(val) => value === val.name}
			onSelect={(val, _idx, event) => {
				value = val.name;
				onChange(event);
			}}
			{disabled}
			showTags={true}
			minWidth="min-w-60"
		>
			<span slot="trigger" class="flex-1 truncate text-left text-sm"
				>{selectedNode?.name ?? value ?? ''}</span
			>
		</SearchableDropdown>
		<button
			class="btn btn-ghost btn-sm"
			onclick={() => {
				modals.open(FileEditWidget as any, { path: value });
			}}
			title="Edit file"
		>
			&#9998;
		</button>
	{:else if property.type == 'palette'}
		<SearchableDropdown
			values={property.values ?? []}
			isSelected={(_val, idx) => value === idx}
			onSelect={(_val, idx, event) => {
				value = idx;
				onChange(event);
			}}
			{disabled}
			minWidth="min-w-122"
		>
			<span slot="trigger" class="flex items-center gap-2">
				<span
					style="{genPalPrev(
						property.values[value]?.colors
					)} width:240px; height:30px; border-radius:2px; flex-shrink:0; display:inline-block; border:1px solid rgba(128,128,128,0.25);"
				></span>
				<span class="flex-1 truncate text-left text-sm">{property.values[value]?.name ?? ''}</span>
			</span>
			<span slot="item" let:val class="flex items-center gap-2">
				<span
					style="{genPalPrev(
						val.colors
					)} width:240px; height:20px; border-radius:2px; flex-shrink:0; display:inline-block; border:1px solid rgba(128,128,128,0.25);"
				></span>
				<span class="truncate text-sm">{val.name}</span>
			</span>
		</SearchableDropdown>
	{:else if property.type == 'checkbox'}
		<input type="checkbox" class="toggle toggle-primary" bind:checked={value} onchange={onChange} />
	{:else if property.type == 'slider'}
		<!-- range colors: https://daisyui.com/components/range/ 
         oninput: throttled response to server for performance
         -->
		<input
			type="range"
			min={property.min ?? 0}
			max={property.max ?? 255}
			{step}
			class={'range flex-1 ' +
				(disabled == false
					? property.color == 'Red'
						? 'range-error'
						: property.color == 'Green'
							? 'range-success'
							: 'range-primary'
					: 'range-secondary')}
			{disabled}
			bind:value
			oninput={handleSliderInput}
		/>
		{#if hasNumber}
			<input
				type="number"
				min={property.min ?? 0}
				max={property.max ?? 255}
				{step}
				class="input"
				style="height: 2rem; width: 5rem"
				{disabled}
				bind:value
				onchange={onChange}
			/>
		{/if}
	{:else if property.type == 'textarea'}
		<textarea
			rows="10"
			cols="61"
			class="textarea w-full"
			onchange={onChange}
			oninput={(event: Event) => {
				if (changeOnInput) onChange(event);
			}}>{value}</textarea
		>
	{:else if property.type == 'file'}
		<input type="file" onchange={onChange} />
	{:else if property.type == 'number'}
		<input
			type="number"
			style="width: {String(property.max ?? 255).length + 5}ch"
			min={property.min ?? 0}
			max={property.max ?? 255}
			class="input invalid:border-error invalid:border-2"
			bind:value
			onchange={onChange}
			oninput={(event: Event) => {
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
			onchange={onChange}
			oninput={(event: Event) => {
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
			onchange={onChange}
			oninput={(event: Event) => {
				if (changeOnInput) onChange(event);
			}}
		/>
		<a href="http://{value}" target="_blank">Link</a>
	{:else if property.type == 'button'}
		<button
			class="btn btn-primary"
			type="button"
			onclick={(event: MouseEvent) => {
				if (value == null) value = 1;
				else value++;
				onChange(event);
			}}>{property.name}</button
		>
	{:else if property.type == 'coord3D'}
		<input
			type="number"
			style="width: {String(property.max ?? 255).length + 5}ch"
			class="input invalid:border-error invalid:border-2"
			min="0"
			max="65536"
			bind:value={value.x}
			onchange={onChange}
		/>
		<input
			type="number"
			style="width: {String(property.max ?? 255).length + 5}ch"
			class="input invalid:border-error invalid:border-2"
			min="0"
			max="65536"
			bind:value={value.y}
			onchange={onChange}
		/>
		<input
			type="number"
			style="width: {String(property.max ?? 255).length + 5}ch"
			class="input invalid:border-error invalid:border-2"
			min="0"
			max="65536"
			bind:value={value.z}
			onchange={onChange}
		/>
	{:else if property.type == 'preset' || property.type == 'channels'}
		<MultiButtonsWidget {property} {value} {onChange} />
	{:else}
		<input
			type={property.type}
			class="input invalid:border-error invalid:border-2"
			bind:value
			onchange={onChange}
		/>
	{/if}
	{#if !noPrompts}
		{#if !property.ro && property.default != null && property.type != 'preset' && property.type != 'channels'}
			<button
				type="button"
				class="btn btn-ghost btn-sm"
				disabled={disabled ||
					(property.type == 'coord3D'
						? property.default.x == value.x &&
							property.default.y == value.y &&
							property.default.z == value.z
						: property.default == value)}
				onclick={(event: MouseEvent) => {
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
