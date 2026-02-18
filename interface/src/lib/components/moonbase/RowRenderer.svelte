<!--
   @title     MoonBase
   @file      RowRenderer.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2026 Github MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

   Not w-full!
   https://tabler.io/icons
-->

<script lang="ts">
	import DraggableList from '$lib/components/DraggableList.svelte';
	import Add from '~icons/tabler/circle-plus';
	import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import SettingsIcon from '~icons/tabler/settings';
	import Delete from '~icons/tabler/trash';
	import { initCap } from '$lib/stores/moonbase_utilities';

	import EditRowWidget from './EditRowWidget.svelte';
	import { modals } from 'svelte-modals';
	import Grip from '~icons/tabler/grip-vertical';
	import FieldRenderer from './FieldRenderer.svelte';
	import { isNumber } from 'chart.js/helpers';

	let { property, data = $bindable(), definition, onChange, changeOnInput } = $props();

	let dataEditable: any = $state({});

	//if no records added yet, add an empty array
	if (data[property.name] == undefined) {
		data[property.name] = [];
	}

	let localDefinition: any = $state([]);

	// console.log("Array property", property, data, definition, changeOnInput, data[property.name], value1, value2);
	for (let i = 0; i < definition.length; i++) {
		// console.log("addItem def", propertyName, property)
		if (property.name == definition[i].name) {
			localDefinition = definition[i].n;
			// console.log("localDefinition", property.name, definition[i].n)
		}
	}

	function handleReorder(reorderedItems: { item: any; originalIndex: number }[]) {
		console.log('handleReorder', property.name, reorderedItems);
		const full = [...data[property.name]];
		// Capture the current filteredItems mapping before any potential state changes
		const indexMap = filteredItems.map((f: any) => f.originalIndex);
		reorderedItems.forEach(({ item }, pos) => {
			const originalIndex = indexMap[pos];
			full[originalIndex] = item;
		});
		data[property.name] = full;
		onChange();
	}

	function addItem(propertyName: string) {
		// propertyEditable = propertyName;
		//set the default values from the definition...
		dataEditable = {};

		//set properties with their defaults
		for (let i = 0; i < definition.length; i++) {
			let property = definition[i];
			if (property.name == propertyName) {
				console.log('addItem def', propertyName);
				for (let i = 0; i < property.n.length; i++) {
					let propertyN = property.n[i];
					// console.log("propertyN", propertyN)
					dataEditable[propertyN.name] = propertyN.default;
				}
			}
		}
	}

	function handleEdit(propertyName: string, itemToEdit: any) {
		console.log('handleEdit', propertyName);
		modals.open(EditRowWidget as any, {
			property,
			localDefinition,
			title: initCap(propertyName),
			dataEditable: itemToEdit, // direct reference
			onChange,
			changeOnInput
		});
	}

	function deleteItem(propertyName: string, itemToDelete: any) {
		console.log('deleteItem', propertyName);
		data[propertyName] = data[propertyName].filter((item: any) => item !== itemToDelete);
		onChange();
	}

	// Filter items based on filter query - returns array of {item, originalIndex}
	let filteredItems = $derived.by(() => {
		const filterValue = data[property.name + '_filter']?.trim() || '';
		const isNegated = filterValue.startsWith('!');
		const query = (isNegated ? filterValue.slice(1) : filterValue).toLowerCase();

		// No filter or empty query → return items directly
		if (!query)
			return data[property.name].map((item: any, index: number) => ({
				item,
				originalIndex: index
			}));

		// Filtered items
		return data[property.name]
			.map((item: any, index: number) => ({ item, originalIndex: index }))
			.filter(({ item }: { item: any }) => {
				const matchFound = property.n
					.filter((propertyN: any, index: number) => {
						return index < 3 || propertyN.show === true;
					})
					.some((propertyN: any) => {
						let valueStr;

						if (
							propertyN.values &&
							Array.isArray(propertyN.values) &&
							isNumber(item[propertyN.name])
						) {
							valueStr = propertyN.values[item[propertyN.name]];
						} else {
							valueStr = item[propertyN.name];
						}

						return String(valueStr).toLowerCase().includes(query);
					});

				return isNegated ? !matchFound : matchFound;
			});
	});

	const findItemInDefinition = $derived(definition.find((obj: any) => obj.name === property.name));

	let propertyFilter = $state({
		name: property.name + '_filter',
		type: 'text',
		label: 'Filter ' + initCap(property.name),
		default: ''
	});

	// Update the default value reactively so we don't capture the initial derived value only once
	$effect(() => {
		propertyFilter.default = findItemInDefinition?.filter ?? '!Unused';
		// assign default filter if not defined yet
		if (data[property.name + '_filter'] == null)
			data[property.name + '_filter'] = propertyFilter.default;
	});
</script>

<div class="divider mb-2 mt-0"></div>
<div class="h-16 flex w-full items-center justify-between space-x-3 p-0 text-xl font-medium">
	{initCap(property.name)}
</div>
{#if findItemInDefinition?.crud == null || findItemInDefinition?.crud?.includes('c')}
	<div class="relative w-full overflow-visible">
		<!-- <div class="mx-4 mb-4 flex flex-wrap justify-end gap-2"> -->
		<button
			class="btn btn-primary text-primary-content btn-md absolute -top-14 right-0"
			onclick={() => {
				addItem(property.name);

				//add the new item to the data
				data[property.name].push(dataEditable);
				onChange();
			}}
		>
			<Add class="h-6 w-6" /></button
		>
	</div>
{/if}

<!-- Search Filter -->
{#if findItemInDefinition?.filter != null}
	<FieldRenderer
		property={propertyFilter}
		bind:value={data[property.name + '_filter']}
		noPrompts={false}
		onChange={(event) => {
			onChange(event);
		}}
	></FieldRenderer>
	{#if data[property.name + '_filter']}
		<div class="text-sm text-base-content/60 mt-1 ml-1">
			{filteredItems.length} of {data[property.name].length} items
			{data[property.name + '_filter'].startsWith('!') ? '(excluding matches)' : ''}
		</div>
	{/if}
{/if}

<div class="overflow-x-auto space-y-1" transition:slide|local={{ duration: 300, easing: cubicOut }}>
	<DraggableList
		items={filteredItems}
		onReorder={handleReorder}
		class="space-y-2"
		dragDisabled={!(
			findItemInDefinition?.crud == null || findItemInDefinition?.crud?.includes('s')
		)}
	>
		{#snippet children({ item: itemWrapper }: { item: any })}
			<!-- svelte-ignore a11y_click_events_have_key_events -->
			<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
				{#if findItemInDefinition?.crud == null || findItemInDefinition?.crud?.includes('s')}
					<Grip class="h-6 w-6 text-base-content/30 cursor-grab flex-shrink-0" />
				{/if}
				<!-- Show the first 3 fields -->
				{#each property.n.filter((propertyN: any, index: any) => {
					return index < 3 || propertyN.show === true;
				}) as propertyN}
					{#if propertyN.type != 'array' && propertyN.type != 'controls' && propertyN.type != 'password'}
						<FieldRenderer
							property={propertyN}
							bind:value={itemWrapper.item[propertyN.name]}
							noPrompts={true}
							onChange={(event) => {
								onChange(event);
							}}
						></FieldRenderer>
					{/if}
				{/each}
				<!-- Show nr of controls -->
				{#each property.n as propertyN}
					{#if propertyN.type == 'array' || propertyN.type == 'controls'}
						<div>
							<div class="font-bold">
								↓{itemWrapper.item[propertyN.name] ? itemWrapper.item[propertyN.name].length : ''}
							</div>
						</div>
					{/if}
				{/each}
				{#if !page.data.features.security || $user.admin}
					<div class="grow"></div>
					<div class="space-x-0 px-0 mx-0">
						<button
							class="btn btn-ghost btn-sm"
							onclick={() => {
								handleEdit(property.name, itemWrapper.item);
							}}
						>
							<SettingsIcon class="h-6 w-6" /></button
						>
						{#if findItemInDefinition?.crud == null || findItemInDefinition?.crud?.includes('d')}
							<button
								class="btn btn-ghost btn-sm"
								onclick={() => {
									deleteItem(property.name, itemWrapper.item);
								}}
							>
								<Delete class="text-error h-6 w-6" />
							</button>
						{/if}
					</div>
				{/if}
			</div>
		{/snippet}
	</DraggableList>
</div>
<div class="divider mb-2 mt-0"></div>