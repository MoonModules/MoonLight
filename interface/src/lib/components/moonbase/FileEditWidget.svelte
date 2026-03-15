<!--
   @title     MoonBase
   @file      FileEditWidget.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Doc       https://moonmodules.org/MoonLight/components/#FileEditWidget
   @Copyright © 2026 GitHub MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
-->

<script lang="ts">
	import type { FilesState } from '$lib/types/moonbase_models';
	import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import { modals } from 'svelte-modals';
	import { fly } from 'svelte/transition';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import { onMount } from 'svelte';
	import FieldRenderer from '$lib/components/moonbase/FieldRenderer.svelte';
	import Cancel from '~icons/tabler/x';

	let { path = '', newItem = false, isFile = true } = $props();

	let folder: string = '';

	let formErrors = {
		name: false
	};

	let editableFile: FilesState = $state({
		name: '',
		path: '',
		isFile: true,
		size: 0,
		time: 0,
		contents: '',
		files: [],
		fs_total: 0,
		fs_used: 0,
		showHidden: false
	});

	let changed: boolean = $state(false);

	async function postFilesState(data: Record<string, unknown>) {
		//export needed to call from other components
		try {
			const response = await fetch('/rest/FileManager', {
				method: 'POST',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(data)
			});
			if (response.status == 200) {
				notifications.success('Settings updated.', 3000);
				return await response.json();
			} else {
				notifications.error('User not authorized.', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
		return null; //no need to return anything!
	}

	function uploadFile(event: Event) {
		const fileNode = event.target as HTMLInputElement;
		const file = fileNode.files?.[0]; // the first file uploaded (multiple files not supported yet)
		if (file) {
			const reader = new FileReader();
			reader.onload = async (e) => {
				const contents = e.target?.result;
				editableFile.name = file.name;
				editableFile.contents = typeof contents === 'string' ? contents : '';
				console.log('uploadFileWithText', file, editableFile.contents);
				changed = true;
			};
			reader.readAsText(file);
		}
	}

	async function getFileContents() {
		// console.log("getFileContents", path, path[0])
		const requestedPath = path;
		editableFile.isFile = isFile;
		editableFile.path = requestedPath;
		if (newItem) {
			editableFile.name = '';
			folder = requestedPath.endsWith('/') ? requestedPath : `${requestedPath}/`;
			editableFile.contents = '';
		} else {
			const parts = requestedPath.split('/');
			editableFile.name = parts.pop() || '';
			const base = parts.join('/');
			folder = base === '' ? '/' : `${base}/`;
			if (requestedPath[0] === '/') {
				const response = await fetch('/rest/file/' + requestedPath, {
					method: 'GET',
					headers: { 'Content-Type': 'text/plain' }
				});
				const text = await response.text();
				if (path !== requestedPath) return;
				editableFile.contents = text;
				// console.log("getFileContents", editableFile.contents)
			}
		}
	}

	//reactive response as soon as path changes, load new file contents
	$effect(() => {
		if (editableFile.path != path) {
			getFileContents();
		}
	});

	onMount(() => {
		getFileContents();
	});

	function onCancel() {
		getFileContents();
		changed = false;
		modals.close(1);
	}

	async function onSave() {
		console.log('onSave', editableFile.isFile);
		let valid = true;

		// Validate Name
		if (editableFile.name.length < 3 || editableFile.name.length > 32) {
			valid = false;
			formErrors.name = true;
		} else {
			formErrors.name = false;
		}

		// Submit JSON to REST API
		if (valid) {
			let response: { news?: FilesState[]; updates?: FilesState[] } = {};
			if (newItem) {
				editableFile.path = folder + editableFile.name;
				// folderList.push(editableFile);
				//order by name ...

				//send newfile or folder to server

				// editableFile.path = "/" + breadCrumbs.join("/") + "/" + editableFile.name;
				response.news = [];
				response.news.push(editableFile);
				console.log('new item', response);
				//send the new itemstate to server
			} else {
				console.log('update item', editableFile);
				// folderList.splice(folderList.indexOf(editableFile), 1, editableFile);

				response.updates = [];
				response.updates.push(editableFile);
			}
			const saved = await postFilesState(response);
			if (saved !== null) {
				changed = false;
				modals.close(1);
			}
		}
	}
</script>

<div
	role="dialog"
	class="pointer-events-none fixed inset-0 z-50 flex items-center justify-center overflow-y-auto"
	transition:fly={{ y: 50 }}
>
	<div
		class="rounded-box bg-base-100 shadow-secondary/30 pointer-events-auto flex max-w-lg min-w-fit flex-col justify-between p-4 shadow-lg md:w-[32rem]"
	>
		<h2 class="text-base-content text-start text-2xl font-bold">
			{newItem ? 'Add ' + (isFile ? 'file' : 'folder') : 'Edit ' + editableFile.name}
		</h2>
		<div class="divider my-2"></div>
		<div
			class="w-full content-center gap-4 px-4"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<div>
				<FieldRenderer
					property={{ name: 'Name', type: 'text' }}
					bind:value={editableFile.name}
					onChange={() => {
						changed = true;
					}}
				></FieldRenderer>
				<label class="label" for="name">
					<span class="text-error {formErrors.name ? '' : 'hidden'}"
						>Name must be between 3 and 32 characters long</span
					>
				</label>
			</div>
			{#if isFile}
				<div>
					<FieldRenderer
						property={{ name: 'Contents', type: 'textarea' }}
						bind:value={editableFile.contents}
						onChange={(event) => {
							editableFile.contents = event.target.value;
							changed = true;
						}}
					></FieldRenderer>
				</div>
				<div>
					<FieldRenderer
						property={{ name: 'Upload', type: 'file' }}
						bind:value={editableFile.contents}
						onChange={(event) => {
							uploadFile(event);
						}}
					></FieldRenderer>
				</div>
			{/if}
		</div>

		<div class="divider my-2"></div>

		<div class="flex justify-end gap-2">
			<button
				class="btn btn-primary"
				onclick={() => {
					console.log('Cancel');
					onCancel();
				}}
				disabled={!changed}
			>
				Cancel
			</button>
			<button
				class="btn btn-primary"
				onclick={() => {
					console.log('Save');
					onSave();
				}}
				disabled={!changed}
			>
				Save
			</button>
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
