<script lang="ts">
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import Down from '~icons/tabler/chevron-down';

	let { icon, title, children, opened, closed, open = false, class: className = '' } = $props();

	function openCollapsible() {
		open = !open;
		if (open) {
			if (opened) opened();
		} else {
			if (closed) closed();
		}
	}
</script>

<div class="{className} relative grid w-full max-w-2xl self-center overflow-hidden">
	<div class="min-h-16 flex w-full items-center justify-between space-x-3 p-4 text-xl font-medium">
		<span class="inline-flex items-baseline">
			{@render icon?.()}
			{@render title?.()}
		</span>
		<button class="btn btn-circle btn-ghost btn-sm" onclick={() => openCollapsible()}>
			<Down
				class="text-base-content h-auto w-6 transition-transform duration-300 ease-in-out {open
					? 'rotate-180'
					: ''}"
			/>
		</button>
	</div>
	{#if open}
		<div
			class="flex flex-col gap-2 p-4 pt-0"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			{@render children?.()}
		</div>
	{/if}
</div>
