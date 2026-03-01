<!--
   @title     MoonBase
   @file      FileManager.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Doc       https://moonmodules.org/MoonLight/moonbase/FileManager/
   @Copyright © 2026 Github MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
-->

<script lang="ts">
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import { modals } from 'svelte-modals';
	import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import ConfirmDialog from '$lib/components/ConfirmDialog.svelte';
	import Spinner from '$lib/components/Spinner.svelte';
	import FilesIcon from '~icons/tabler/files';
	import FileIcon from '~icons/tabler/file';
	import FolderIcon from '~icons/tabler/folder';
	import Add from '~icons/tabler/circle-plus';
	import Edit from '~icons/tabler/pencil';
	import Delete from '~icons/tabler/trash';
	import Cancel from '~icons/tabler/x';
	import type { FilesState } from '$lib/types/moonbase_models';
	import { onMount, onDestroy } from 'svelte';
	import { socket } from '$lib/stores/socket';
	import { tick } from 'svelte';
	import FileEditWidget from '$lib/components/moonbase/FileEditWidget.svelte';
	import Help from '~icons/tabler/help';
	import FieldRenderer from '$lib/components/moonbase/FieldRenderer.svelte';

	let filesState: any = $state({});
	let folderList: FilesState[] = $state([]); //all files in a folder
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
	let breadCrumbs: string[] = $state([]);

	let newItem: boolean = $state(true);
	let showEditor: boolean = $state(false);
	let path: string = $state('');

	async function getState() {
		try {
			const response = await fetch('/rest/FileManager', {
				method: 'GET',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				}
			});
			filesState = await response.json();
			// console.log("filesState", filesState);
		} catch (error) {
			console.error('Error:', error);
		}
		folderListFromBreadCrumbs();
		return filesState;
	}

	async function postFilesState(data: any) {
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
				filesState = await response.json();
			} else {
				notifications.error('User not authorized.', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	function addFile() {
		console.log('addFile');
		newItem = true;
		path = '/' + breadCrumbs.join('/');
		editableFile = {
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
		};
	}
	function addFolder() {
		console.log('addFolder');
		newItem = true;
		path = '/' + breadCrumbs.join('/');
		editableFile = {
			name: '',
			path: '',
			isFile: false,
			size: 0,
			time: 0,
			contents: '',
			files: [],
			fs_total: 0,
			fs_used: 0,
			showHidden: false
		};
	}

	function folderListFromBreadCrumbs() {
		folderList = filesState.files;
		for (let indexF = 0; indexF < breadCrumbs.length; indexF++) {
			//find the parent folder
			let found = false;
			for (let indexI = 0; indexI < folderList.length; indexI++) {
				if (folderList[indexI].name === breadCrumbs[indexF]) {
					// console.log("handleEdit parent", folderList[indexI], breadCrumbs[indexF])
					folderList = [folderList[indexI], ...folderList[indexI].files];
					found = true;
				}
			}
			if (!found) {
				//e.g. old coookie, reset
				breadCrumbs = [];
				folderList = filesState.files;
				return;
			}
		}
		// console.log("folderListFromBreadCrumbs", filesState, breadCrumbs, folderList)
	}

	async function handleEdit(index: number) {
		newItem = false;
		editableFile = folderList[index];
		path = editableFile.path;

		if (breadCrumbs.length > 0 && editableFile.name === breadCrumbs[breadCrumbs.length - 1]) {
			//if parent folder
			breadCrumbs.pop(); //remove last folder
			folderListFromBreadCrumbs();

			localStorage.setItem('breadCrumbs', JSON.stringify(breadCrumbs));
			showEditor = false;
			console.log('handleEdit parent', folderList, breadCrumbs);
		} else if (editableFile.isFile) {
			//if file
			console.log('handleEdit file', editableFile, path);
			showEditor = false;
			await tick();
			showEditor = true; //Trigger reactivity
		} else {
			//if folder, go to folder
			breadCrumbs.push(editableFile.name);
			localStorage.setItem('breadCrumbs', JSON.stringify(breadCrumbs));
			// folderList = [folderList[index], ...editableFile.files];
			folderListFromBreadCrumbs();
			// showEditor = true; await tick(); //wait for reactivity, not needed here
			showEditor = false;

			console.log('handleEdit go to folder', folderList, breadCrumbs);
		}
	}

	function confirmDelete(index: number) {
		modals.open(ConfirmDialog, {
			title: 'Delete item',
			message: 'Are you sure you want to delete ' + folderList[index].name + '?',
			labels: {
				cancel: { label: 'Cancel', icon: Cancel },
				confirm: { label: 'Delete', icon: Delete }
			},
			onConfirm: () => {
				// Check if item is currently been edited and delete as well
				// if (folderList[index].name === editableFile.name) {
				// 	addFile();
				// }

				//update filesState
				let response: any = {};
				response.deletes = [];
				response.deletes.push(folderList[index]);
				console.log('confirmDelete', response);
				//send the new itemstate to server
				response.showHidden = filesState.showHidden; //otherwise set to false
				postFilesState(response);

				showEditor = false;
				modals.close();
			}
		});
	}

	const handleFilesState = (data: FilesState) => {
		console.log('socket update received');
		filesState = data;
		folderListFromBreadCrumbs();
	};

	onMount(() => {
		let bc = localStorage.getItem('breadCrumbs');
		if (bc) {
			try {
				breadCrumbs = JSON.parse(bc);
			} catch {
				breadCrumbs = [];
				localStorage.removeItem('breadCrumbs');
			}
		}

		socket.on('FileManager', handleFilesState);
		// getState(); //done in settingscard
	});

	onDestroy(() => socket.off('FileManager', handleFilesState));
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<FilesIcon class="shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Files</span>
		<div class="absolute right-5">
			<a
				href="https://{page.data.github.split('/')[0]}.github.io/{page.data.github.split(
					'/'
				)[1]}{page.url.pathname}"
				target="_blank"
				rel="noopener noreferrer"
				title="Documentation"><Help class="shrink-0 mr-2 h-6 w-6 self-end" /></a
			>
		</div>
		<!-- 🌙 link to docs -->
	{/snippet}

	{#if !page.data.features.security || $user.admin}
		<div class="bg-base-200 relative grid w-full max-w-2xl self-center overflow-hidden shadow-lg">
			<div class="flex h-16 w-full items-center justify-between space-x-3 p-0 text-xl font-medium">
				Files /{breadCrumbs.join('/')}
			</div>
			{#await getState()}
				<Spinner />
			{:then}
				<div class="relative w-full overflow-visible">
					<button
						class="btn btn-primary text-primary-content btn-md absolute -top-14 right-16"
						onclick={() => {
							addFile();
							showEditor = true;
						}}
					>
						<Add class="h-6 w-6" /></button
					>
					<button
						class="btn btn-primary text-primary-content btn-md absolute -top-14 right-1"
						onclick={() => {
							addFolder();
							showEditor = true;
						}}
					>
						<Add class="h-6 w-6" /></button
					>
				</div>

				<div
					class="flex flex-col gap-2 p-0"
					transition:slide|local={{ duration: 300, easing: cubicOut }}
				>
					{#if showEditor}
						<FileEditWidget {newItem} {path} isFile={editableFile.isFile} />
					{/if}
				</div>

				<div
					class="space-y-1 overflow-x-auto"
					transition:slide|local={{ duration: 300, easing: cubicOut }}
				>
					{#each folderList as item, index (item.path || item.name)}
						<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
							<div class="mask mask-hexagon bg-primary h-auto w-10 shrink-0">
								{#if item.isFile}
									<FileIcon class="text-primary-content h-auto w-full scale-75" />
								{:else}
									<FolderIcon class="text-primary-content h-auto w-full scale-75" />
								{/if}
							</div>
							<div>
								{#if breadCrumbs.length > 0 && item.name === breadCrumbs[breadCrumbs.length - 1]}
									<div>..</div>
								{:else}
									<div class="font-bold">{item.name}</div>
									{#if item.isFile}
										<div>
											{item.size / 1000} KB {new Intl.DateTimeFormat('en-GB', {
												dateStyle: 'short',
												timeStyle: 'short',
												timeZone: 'UTC'
											}).format(item.time * 1000)}
										</div>
									{:else}
										<div>{item.files.length} files/folders</div>
									{/if}
								{/if}
							</div>

							{#if !page.data.features.security || $user.admin}
								<div class="grow"></div>
								<div class="mx-0 space-x-0 px-0">
									<button
										class="btn btn-ghost btn-sm"
										onclick={() => {
											handleEdit(index);
										}}
									>
										<Edit class="h-6 w-6" /></button
									>
									{#if !(breadCrumbs.length > 0 && item.name === breadCrumbs[breadCrumbs.length - 1])}
										<button
											class="btn btn-ghost btn-sm"
											onclick={() => {
												confirmDelete(index);
											}}
											disabled={item.files && item.files.length > 0}
										>
											<Delete class="text-error h-6 w-6" />
										</button>
									{:else}
										&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
									{/if}
								</div>
							{/if}
						</div>
					{/each}
				</div>
				<br />
				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div class="mask mask-hexagon bg-primary h-auto w-10 flex-none">
						<FolderIcon class="text-primary-content h-auto w-full scale-75" />
					</div>
					<div>
						<div class="font-bold">File System Size</div>
						<div class="flex flex-wrap justify-start gap-1 text-sm opacity-75">
							<span
								>{((filesState.fs_used / filesState.fs_total) * 100).toFixed(1)} % of {Math.round(
									filesState.fs_total / 1000
								).toLocaleString('en-US')} KB</span
							>

							<span
								>({Math.round((filesState.fs_total - filesState.fs_used) / 1000).toLocaleString(
									'en-US'
								)}
								KB free)</span
							>
						</div>
					</div>
				</div>
				<FieldRenderer
					property={{ name: 'showHidden', type: 'checkbox' }}
					bind:value={filesState.showHidden}
					onChange={() => {
						postFilesState({ showHidden: filesState.showHidden });
					}}
				></FieldRenderer>
			{/await}
		</div>
	{/if}
</SettingsCard>
