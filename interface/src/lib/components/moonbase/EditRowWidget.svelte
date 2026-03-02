<!--
   @title     MoonBase
   @file      EditRowWidget.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2026 Github MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
-->

<script lang="ts">
	import { modals } from 'svelte-modals';
	import { fly } from 'svelte/transition';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import Cancel from '~icons/tabler/x';

	import FieldRenderer from '$lib/components/moonbase/FieldRenderer.svelte';
	import RowRenderer from '$src/lib/components/moonbase/RowRenderer.svelte';

	interface Props {
		property: any;
		localDefinition: any[];
		title: string;
		dataEditable: any;
		onChange: any;
		changeOnInput: any;
	}

	let {
		property,
		localDefinition,
		title,
		dataEditable = $bindable(),
		onChange,
		changeOnInput
	}: Props = $props();

	console.log(property, localDefinition, dataEditable);

	// Make passed object reactive to prevent Svelte warning 'binding_property_non_reactive'
	// https://github.com/sveltejs/svelte/issues/12320
	// let dataEditable = $state(_dataEditable);
</script>

<div
	role="dialog"
	class="pointer-events-none fixed inset-0 z-50 flex items-center justify-center overflow-y-auto"
	transition:fly={{ y: 50 }}
>
	<div
		class="rounded-box bg-base-100 shadow-secondary/30 pointer-events-auto flex max-w-md min-w-fit flex-col justify-between p-4 shadow-lg md:w-[28rem]"
	>
		<h2 class="text-base-content text-start text-2xl font-bold">{title}</h2>
		<div class="divider my-2"></div>
		<div
			class="w-full content-center gap-4 px-4"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<!-- <div class="divider my-0"></div> -->
			{#each property.n as propertyN (propertyN.name)}
				{#if propertyN.type == 'array'}
					<label for={propertyN.name}>{propertyN.name}</label>
					<RowRenderer
						property={propertyN}
						bind:data={dataEditable}
						definition={localDefinition}
						{onChange}
						{changeOnInput}
					></RowRenderer>
				{:else if propertyN.type == 'controls'}
					{#each dataEditable[propertyN.name] as control (control.p ?? control.name ?? control.id)}
						<!-- e.g. dE["controls"] -> {"name":"xFrequency","type":"slider","default":64,"p":1070417419,"value":64} -->
						<FieldRenderer property={control} bind:value={control.value} {onChange} {changeOnInput}
						></FieldRenderer>
					{/each}
				{:else}
					<div>
						<FieldRenderer
							property={propertyN}
							bind:value={dataEditable[propertyN.name]}
							{onChange}
							{changeOnInput}
						></FieldRenderer>
					</div>
				{/if}
			{/each}
		</div>

		<div class="divider my-2"></div>

		<div class="flex justify-end gap-2">
			<button
				class="btn btn-neutral text-neutral-content inline-flex items-center"
				onclick={() => {
					modals.close(1);
				}}
				type="button"
			>
				<Cancel class="mr-2 h-5 w-5" />
				<span>Close</span>
			</button>
		</div>
	</div>
</div>
