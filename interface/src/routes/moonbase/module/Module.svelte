<script lang="ts">
	import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Spinner from '$lib/components/Spinner.svelte';
	import Router from '~icons/tabler/router';
	import Help from '~icons/tabler/help';
	// import Cancel from '~icons/tabler/x';
	import FieldRenderer from '$lib/components/moonbase/FieldRenderer.svelte';
	import { socket } from '$lib/stores/socket';
	import RowRenderer from '$src/lib/components/moonbase/RowRenderer.svelte';
	import { initCap } from '$lib/stores/moonbase_utilities';

	let definition: any = $state([]);
	let data: any = $state({});

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

	function inputChanged() {
		if (modeWS) {
			let moduleName = page.url.searchParams.get('module') || '';
			// console.log("inputChanged", moduleName, data);
			socket.sendEvent(moduleName, data);
		} else {
			changed = true;
		}
	}

	function updateRecursive(oldData: any, newData: any) {
		//loop over properties
		for (let key in newData) {
			// if (typeof newData[key] != 'object') {
			// 	if (newData[key] != oldData[key]) {
			// 		// console.log("updateRecursive", key, newData[key], oldData[key]);
			// 		oldData[key] = newData[key]; //trigger reactiveness
			// 	}
			// } else {
			if (Array.isArray(newData[key])) {
				//loop over array
				if (!oldData[key]) oldData[key] = []; //create an empty array
				for (let i = 0; i < Math.max(oldData[key].length, newData[key].length); i++) {
					if (oldData[key][i] == undefined) {
						// console.log("add row", key, i, newData[key][i]);
						oldData[key][i] = newData[key][i]; //create new row if not existed, trigger reactiveness
					} else if (newData[key][i] == undefined) {
						// console.log("remove remaining rows", key, i, oldData[key][i]);
						oldData[key].splice(i);
					} else {
						// console.log("change row", key, i, oldData[key][i], newData[key][i]);
						updateRecursive(oldData[key][i], newData[key][i]);
					}
				}
			} else {
				if (newData[key] != oldData[key]) {
					// console.log("updateRecursive", key, newData[key], oldData[key]);
					oldData[key] = newData[key]; //trigger reactiveness
				}
			}
			// }
		}
		//remove properties that are not in newData (e.g. control min and max)
		for (let key in oldData) {
			if (newData[key] == null) {
				delete oldData[key]; //remove property if not in newData
			}
		}
	}

	const handleState = (state: any) => {
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
		<Router class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>{initCap(page.url.searchParams.get('module') || '')}</span>
		<div class="absolute right-5">
			<a
				href="https://{page.data.github.split('/')[0]}.github.io/{page.data.github.split(
					'/'
				)[1]}/{page.url.searchParams.get('group') + '/' + page.url.searchParams.get('module')}"
				target="_blank"
				title="Documentation"><Help class="lex-shrink-0 mr-2 h-6 w-6 self-end" /></a
			>
		</div>
		<!-- 🌙 link to docs -->
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
									onChange={inputChanged}
									changeOnInput={!modeWS}
								></FieldRenderer>
							</div>
						{:else if property.type == 'rows'}
							<!-- e.g. definition: [name:"nodes", n: [name: ,,, name:"on", name:"controls", n:[]]]] -->
							<RowRenderer
								{property}
								bind:data
								{definition}
								onChange={inputChanged}
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
</SettingsCard>
