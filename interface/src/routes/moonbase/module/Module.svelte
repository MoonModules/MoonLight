<script lang="ts">
	import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Spinner from '$lib/components/Spinner.svelte';
	import Router from '~icons/tabler/router';
	import Help from '~icons/tabler/help';
	import Api from '~icons/tabler/api';
	// import Cancel from '~icons/tabler/x';
	import FieldRenderer from '$lib/components/moonbase/FieldRenderer.svelte';
	import { socket } from '$lib/stores/socket';
	import RowRenderer from '$src/lib/components/moonbase/RowRenderer.svelte';
	import { initCap } from '$lib/stores/moonbase_utilities';
	import type { ModuleProperty, ModuleData } from '$lib/types/moonbase_models';
	import { updateRecursive } from './module';

	let definition: ModuleProperty[] = $state([]);
	let data: ModuleData = $state({});

	let changed: boolean = $state(false);

	const modeWS: boolean = true; //todo: make this an argument

	// https://github.com/sveltejs/svelte/issues/14091
	// https://www.reddit.com/r/sveltejs/comments/1atm5xw/detect_url_params_changes_in_sveltekit_2/
	// let params = $state(page.url.searchParams)
	// let mName = $derived(params.get('module'));
	// didn't work
	let oldName: string = ''; //workaround for let params = $state(page.url.searchParams)

	async function getState() {
		let moduleName = page.url.searchParams.get('module') || '';

		//workaround for let params = $state(page.url.searchParams)
		if (moduleName != oldName) {
			console.log('getState new module loaded', moduleName);
			if (oldName != '') {
				socketOff(oldName);
			}
			oldName = moduleName;
			socketOn(moduleName);
		}

		console.log('getState', '/rest/' + moduleName);

		//load definition
		try {
			const response = await fetch('/rest/' + moduleName + 'Def', {
				method: 'GET',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				}
			});
			definition = await response.json();
			// console.log("definition", definition)
		} catch (error) {
			console.error('Error:', error);
		}

		console.log('get data', '/rest/' + moduleName);
		//load data
		try {
			const response = await fetch('/rest/' + moduleName, {
				method: 'GET',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				}
			});
			data = {}; //clear the data of the old module
			handleState(await response.json());
			// console.log("data", data)
		} catch (error) {
			console.error('Error:', error);
		}
		return data;
	}

	async function postState() {
		//validation (if needed) here?
		//optional checks if the whole state is correct

		let moduleName = page.url.searchParams.get('module') || '';

		try {
			const response = await fetch('/rest/' + moduleName, {
				method: 'POST',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(data)
			});
			if (response.status == 200) {
				notifications.success('Module settings updated.', 3000);
				data = await response.json();
				changed = false;
			} else {
				notifications.error('User not authorized.', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	function cancelState() {
		getState();
		changed = false;
	}

	function inputChanged(_event: Event, propertyName?: string) {
		if (modeWS) {
			let moduleName = page.url.searchParams.get('module') || '';
			if (propertyName) {
				// Send only the changed property to avoid overwriting unrelated server-side state changes with stale values
				const partial: ModuleData = {};
				partial[propertyName] = data[propertyName];
				socket.sendEvent(moduleName, partial);
			} else {
				socket.sendEvent(moduleName, data);
			}
		} else {
			changed = true;
		}
	}

	const handleState = (state: ModuleData) => {
		// console.log("handleState", state);
		updateRecursive(data, state);
		// data = state;
	};

	//workaround for let params = $state(page.url.searchParams)
	const socketOn = (name: string) => {
		if (modeWS) {
			console.log('socketOn', name);
			socket.on(name, handleState);
		}
	};
	const socketOff = (name: string) => {
		if (modeWS) {
			console.log('socketOff', name);
			socket.off(name, handleState);
		}
	};
</script>

<SettingsCard collapsible={false} bind:data>
	{#snippet icon()}
		<Router class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>{initCap(page.url.searchParams.get('module') || '')}</span>
		<!-- 🌙 link to docs -->
		<div class="absolute right-5">
			<a
				href="https://{page.data.github.split('/')[0]}.github.io/{page.data.github.split(
					'/'
				)[1]}/{page.url.searchParams.get('group') + '/' + page.url.searchParams.get('module')}"
				target="_blank"
				rel="noopener noreferrer"
				title="Documentation"><Help class="mr-2 h-6 w-6 shrink-0 self-end" /></a
			>
		</div>
	{/snippet}

	{#if !page.data.features.security || $user.admin}
		<div class="bg-base-200 relative grid w-full max-w-2xl self-center overflow-hidden shadow-lg">
			{#await getState()}
				<Spinner />
			{:then}
				<!-- <div class="grid w-full grid-cols-1 content-center gap-x-4 px-4 sm:grid-cols-2"> -->
				<div class="relative w-full overflow-visible">
					{#each definition as property (property.name)}
						{#if property.type != 'rows'}
							<div>
								<FieldRenderer
									{property}
									bind:value={data[property.name]}
									onChange={(event: Event) => inputChanged(event, property.name)}
									changeOnInput={!modeWS}
								></FieldRenderer>
							</div>
						{:else if property.type == 'rows'}
							<!-- e.g. definition: [name:"nodes", n: [name: ,,, name:"on", name:"controls", n:[]]]] -->
							<RowRenderer
								{property}
								bind:data
								{definition}
								onChange={(event: Event) => inputChanged(event, property.name)}
								onFilterChange={(event: Event) => inputChanged(event, property.name + '_filter')}
								changeOnInput={!modeWS}
							></RowRenderer>
						{/if}
					{/each}
				</div>

				{#if !modeWS}
					<div class="divider mt-0 mb-2"></div>
					<div class="mx-4 mb-4 flex flex-wrap justify-end gap-2">
						<button class="btn btn-primary" type="button" onclick={cancelState} disabled={!changed}
							>Cancel</button
						>
						<button class="btn btn-primary" type="button" onclick={postState} disabled={!changed}
							>Save</button
						>
					</div>
				{/if}
			{/await}
		</div>
	{/if}
	<!-- 🌙 link to api -->
	<div class="flex justify-end px-4 pb-3">
		<a
			href="http://{page.url.host}/rest/{page.url.searchParams.get('module')}"
			target="_blank"
			rel="noopener noreferrer"
			title="API: http://{page.url.host}/rest/{page.url.searchParams.get('module')}"
			><Api class="mr-2 h-6 w-6 shrink-0 self-end" /></a
		>
	</div>
</SettingsCard>
