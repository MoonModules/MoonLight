<script lang="ts">
	import type { PageData } from './$types';
	import { onMount } from 'svelte';
	import { goto } from '$app/navigation';
	import { modals } from 'svelte-modals';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import { user } from '$lib/stores/user';
	import type { userProfile } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import InputPassword from '$lib/components/InputPassword.svelte';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import ConfirmDialog from '$lib/components/ConfirmDialog.svelte';
	import EditUser from './EditUser.svelte';
	import Spinner from '$lib/components/Spinner.svelte';
	import Help from '~icons/tabler/help';
	import Delete from '~icons/tabler/trash';
	import AddUser from '~icons/tabler/user-plus';
	import Edit from '~icons/tabler/pencil';
	import Admin from '~icons/tabler/key';
	import Users from '~icons/tabler/users';
	import Warning from '~icons/tabler/alert-triangle';
	import Cancel from '~icons/tabler/x';
	import Check from '~icons/tabler/check';

	interface Props {
		data: PageData;
	}

	let { data }: Props = $props();

	type userSetting = {
		username: string;
		password: string;
		admin: boolean;
	};

	type SecuritySettings = {
		jwt_secret: string;
		users: userSetting[];
	};

	let securitySettings: SecuritySettings = $state();

	async function getSecuritySettings() {
		try {
			const response = await fetch('/rest/securitySettings', {
				method: 'GET',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				}
			});
			securitySettings = await response.json();
		} catch (error) {
			console.error('Error:', error);
		}
		return;
	}

	async function postSecuritySettings(data: SecuritySettings) {
		try {
			const response = await fetch('/rest/securitySettings', {
				method: 'POST',
				headers: {
					Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(data)
			});

			securitySettings = await response.json();
			if (response.status == 200) {
				if (await validateUser($user)) {
					notifications.success('Security settings updated.', 3000);
				}
			} else {
				notifications.error('User not authorized.', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
		return;
	}

	async function validateUser(userdata: userProfile) {
		try {
			const response = await fetch('/rest/verifyAuthorization', {
				method: 'GET',
				headers: {
					Authorization: 'Bearer ' + userdata.bearer_token,
					'Content-Type': 'application/json'
				}
			});
			if (response.status !== 200) {
				user.invalidate();
				return false;
			}
		} catch (error) {
			console.error('Error:', error);
		}
		return true;
	}

	function confirmDelete(index: number) {
		modals.open(ConfirmDialog, {
			title: 'Confirm Delete User',
			message:
				'Are you sure you want to delete the user "' +
				securitySettings.users[index].username +
				'"?',
			labels: {
				cancel: { label: 'Abort', icon: Cancel },
				confirm: { label: 'Yes', icon: Check }
			},
			onConfirm: () => {
				securitySettings.users.splice(index, 1);
				securitySettings = securitySettings;
				modals.close();
				postSecuritySettings(securitySettings);
			}
		});
	}

	function handleEdit(index: number) {
		modals.open(EditUser, {
			title: 'Edit User',
			user: { ...securitySettings.users[index] }, // Shallow Copy
			onSaveUser: (editedUser: userSetting) => {
				securitySettings.users[index] = editedUser;
				modals.close();
				postSecuritySettings(securitySettings);
			}
		});
	}

	function handleNewUser() {
		modals.open(EditUser, {
			title: 'Add User',
			onSaveUser: (newUser: userSetting) => {
				securitySettings.users = [...securitySettings.users, newUser];
				modals.close();
				postSecuritySettings(securitySettings);
			}
		});
		//
	}
</script>

{#if $user.admin}
	<div
		class="mx-0 my-1 flex flex-col space-y-4
     sm:mx-8 sm:my-8"
	>
		<SettingsCard collapsible={false}>
			{#snippet icon()}
				<Users class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
			{/snippet}
			{#snippet title()}
				<span>Manage Users</span>
						<div class="absolute right-20"><a href="https://{page.data.github.split("/")[0]}.github.io/{page.data.github.split("/")[1]}{page.url.pathname}" target="_blank" title="Documentation"><Help  class="lex-shrink-0 mr-2 h-6 w-6 self-end" /></a></div> <!-- 🌙 link to docs -->
			{/snippet}
			{#await getSecuritySettings()}
				<Spinner />
			{:then nothing}
				<div class="relative w-full overflow-visible">
					<button
						class="btn btn-primary text-primary-content btn-md absolute -top-14 right-0"
						onclick={handleNewUser}
					>
						<AddUser class="h-6 w-6" /></button
					>

					<div class="overflow-x-auto" transition:slide|local={{ duration: 300, easing: cubicOut }}>
						<table class="table w-full table-auto">
							<thead>
								<tr class="font-bold">
									<th align="left">Username</th>
									<th align="center">Admin</th>
									<th align="right" class="pr-8">Edit</th>
								</tr>
							</thead>
							<tbody>
								{#each securitySettings.users as user, index}
									<tr>
										<td align="left">{user.username}</td>
										<td align="center">
											{#if user.admin}
												<Admin class="text-secondary" />
											{/if}
										</td>
										<td align="right">
											<span class="my-auto inline-flex flex-row space-x-2">
												<button
													class="btn btn-ghost btn-circle btn-xs"
													onclick={() => handleEdit(index)}
												>
													<Edit class="h-6 w-6" /></button
												>
												<button
													class="btn btn-ghost btn-circle btn-xs"
													onclick={() => confirmDelete(index)}
												>
													<Delete class="text-error h-6 w-6" />
												</button>
											</span>
										</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				</div>
				<div class="divider mb-0"></div>

				<span class="pb-2 text-xl font-medium">Security Settings</span>
				<div class="alert alert-warning shadow-lg">
					<Warning class="h-6 w-6 shrink-0" />
					<span
						>The JWT secret is used to sign authentication tokens. If you modify the JWT Secret, all
						users will be signed out.</span
					>
				</div>
				<label class="label" for="secret">JWT Secret</label>
				<InputPassword bind:value={securitySettings.jwt_secret} id="secret" />
				<div class="mt-6 flex justify-end">
					<button class="btn btn-primary" onclick={() => postSecuritySettings(securitySettings)}
						>Apply Settings</button
					>
				</div>
			{/await}
		</SettingsCard>
	</div>
{:else}
	{goto('/')}
{/if}
