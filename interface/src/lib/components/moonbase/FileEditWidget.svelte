<!--
   @title     MoonBase
   @file      FileEditWidget.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Doc       https://moonmodules.org/MoonLight/components/#FileEditWidget
   @Copyright © 2026 Github MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

   known issues
   - showEditor does collapse in Files module, but not in Virtual and Physical Module
-->

<script lang="ts">
	import type { FilesState } from '$lib/types/moonbase_models';
	import { tick } from 'svelte';
    import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import Collapsible from '$lib/components/Collapsible.svelte';
	import { onMount } from 'svelte';
	import FieldRenderer from '$lib/components/moonbase/FieldRenderer.svelte';

	let { path = "", showEditor = true, newItem = false, isFile = true } = $props();

	let folder:string = "";

	let formErrors = {
		name: false
	};

    let editableFile: FilesState = $state({
		name: '',
		path: path,
		isFile: isFile,
		size: 0,
		time: 0,
		contents: '',
		files: [],
		fs_total: 0,
		fs_used: 0,
		showHidden: false,
	});

	let changed: boolean = $state(false);

    async function postFilesState(data: any) { //export needed to call from other components
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

    function uploadFile(event: any) {
		let fileNode = event.target;
        let file = fileNode.files[0]; // the first file uploaded (multiple files not supported yet)
		// console.log("uploadFile", event, file)
		if (file) {
			// let fileContents: string | ArrayBuffer | null = null;

            const reader = new FileReader();
			reader.onload = async (e) => {
				const contents = e.target?.result;
				editableFile.name = file.name;
				editableFile.contents = typeof contents === 'string' ? contents : '';			

				showEditor = false; await tick(); showEditor = true; //Trigger reactivity (folderList = [...folderList]; is not doing it)
				console.log("uploadFileWithText", file, editableFile.contents)
			};
            reader.readAsText(file);
			changed = true;
        }
	}

    async function getFileContents() {
		// console.log("getFileContents", path, path[0])
		editableFile.isFile = isFile;
		editableFile.path = path;
		if (newItem) {
			editableFile.name = '';
			folder = path + "/";
			editableFile.contents = '';
		} else {
			const parts = path.split('/');
			editableFile.name = parts.pop() || '';
			folder = parts.join('/') + "/";
			if (path[0] === '/') {
				const response = await fetch('/rest/file/' + editableFile.path, {
					method: 'GET',
					headers: {'Content-Type': 'text/plain'}
				});
				editableFile.contents = await response.text();
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
		showEditor = false;
		changed = false;
	}

	function onSave() {
		console.log("onSave", editableFile.isFile)
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
			let response:any = {};
			if (newItem) {
				editableFile.path = folder + editableFile.name;
				// folderList.push(editableFile);
				//order by name ...

				//send newfile or folder to server

				// editableFile.path = "/" + breadCrumbs.join("/") + "/" + editableFile.name;
				response.news = [];
				response.news.push(editableFile);
				console.log("new item", response)
				//send the new itemstate to server
				
			} else {
				console.log("update item", editableFile)
				// folderList.splice(folderList.indexOf(editableFile), 1, editableFile);
				
				response.updates = [];
				response.updates.push(editableFile);
			}
			postFilesState(response);
			showEditor = false;
			changed = false;
		}
	}

</script>

{#if path[0] === '/'}
	<Collapsible open={showEditor} class="shadow-lg" opened={() => {}} closed={() => {}}>
		{#snippet title()}
			<span>{newItem ? 'Add ' + (isFile?"file":"folder") : 'Edit ' + editableFile.name}</span>
		{/snippet}
		<div class="divider my-0"></div>

		<div>
			<FieldRenderer property={{name:"Name", type:"text"}} bind:value={editableFile.name}
				onChange={() => {
					changed = true;
				}}>
			</FieldRenderer>
			<label class="label" for="name">
				<span class="text-error {formErrors.name ? '' : 'hidden'}"
					>Name must be between 3 and 32 characters long</span
				>
			</label>
		</div>
		{#if isFile}
			<div>
				<FieldRenderer property={{name:"Contents", type:"textarea"}} bind:value={editableFile.contents} 
					onChange={(event) => {
						editableFile.contents = event.target.value;
						changed = true;
					}}>
				</FieldRenderer>
			</div>
			<div>
				<FieldRenderer property={{name:"Upload", type:"file"}} bind:value={editableFile.contents} 
					onChange={(event) => {
						uploadFile(event);
						changed = true;
					}}>
				</FieldRenderer>
			</div>
		{/if}
		<div class="flex flex-wrap justify-end gap-2">
			<button
				class="btn btn-primary"
				onclick={() => {
					console.log("Save");
					onCancel();
				}}
				disabled={!changed}
			>
				Cancel</button
			>
			<button
				class="btn btn-primary"
				onclick={() => {
					console.log("Save");
					onSave();
				}}
				disabled={!changed}
			>
				Save</button
			>
		</div>
		<div class="divider mb-2 mt-0"></div>
	</Collapsible>
{/if}
