<script lang="ts">
	import { page } from '$app/state';
	import { telemetry } from '$lib/stores/telemetry';
	import { modals } from 'svelte-modals';
	import { user } from '$lib/stores/user';
	import ConfirmDialog from '$lib/components/ConfirmDialog.svelte';
	import WiFiOff from '~icons/tabler/wifi-off';
	import Hamburger from '~icons/tabler/menu-2';
	import Power from '~icons/tabler/power';
	import Cancel from '~icons/tabler/x';
	import RssiIndicator from '$lib/components/RSSIIndicator.svelte';
	import BatteryIndicator from '$lib/components/BatteryIndicator.svelte';
	import UpdateIndicator from '$lib/components/UpdateIndicator.svelte';

	async function postSleep() {
		const response = await fetch('/rest/sleep', {
			method: 'POST',
			headers: {
				Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic'
			}
		});
	}

	function confirmSleep() {
		modals.open(ConfirmDialog, {
			title: 'Confirm Power Down',
			message: 'Are you sure you want to switch off the device?',
			labels: {
				cancel: { label: 'Abort', icon: Cancel },
				confirm: { label: 'Switch Off', icon: Power }
			},
			onConfirm: () => {
				modals.close();
				postSleep();
			}
		});
	}

	// 🌙 for safeMode and restartNeeded
	async function postRestart() {
		const response = await fetch('/rest/restart', {
			method: 'POST',
			headers: {
				Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic'
			}
		});
	}

	function confirmRestart() {
		modals.open(ConfirmDialog, {
			title: 'Confirm Restart',
			message: 'Are you sure you want to restart the device?',
			labels: {
				cancel: { label: 'Abort', icon: Cancel },
				confirm: { label: 'Restart', icon: Power }
			},
			onConfirm: () => {
				modals.close();
				postRestart();
			}
		});
	}

</script>

<div class="navbar bg-base-300 sticky top-0 z-10 h-12 min-h-fit drop-shadow-lg lg:h-16">
	<div class="flex-1 flex items-center justify-left">
		<!-- Page Hamburger Icon here -->
		<label for="main-menu" class="btn btn-ghost btn-circle btn-sm drawer-button lg:hidden"
			><Hamburger class="h-6 w-auto" /></label
		>
		<span class="px-2 text-xl font-bold lg:text-2xl">{page.data.devices.deviceName}</span> <!-- 🌙 -->
	</div>
	<div class="indicator flex-none">
		<UpdateIndicator />
	</div>
	<!-- 🌙 safeMode -->
	{#if $telemetry.rssi.safeMode}
		<div class="flex-none">
			<button class="btn btn-square btn-ghost h-9 w-10" onclick={confirmRestart}>
				🛡️
			</button>
		</div>
	{/if}
	<!-- 🌙 restartNeeded -->
	{#if $telemetry.rssi.restartNeeded}
		<div class="flex-none">
			<button class="btn btn-square btn-ghost h-9 w-10" onclick={confirmRestart}>
				🔄
			</button>
		</div>
	{/if}
	<div class="flex-none">
		{#if $telemetry.rssi.disconnected}
			<WiFiOff class="inline-block h-7 w-7" />
		{:else}
			<RssiIndicator
				showDBm={false}
				rssi_dbm={$telemetry.rssi.rssi}
				ssid={$telemetry.rssi.ssid}
				class="inline-block h-7 w-7"
			/>
		{/if}
	</div>

	{#if page.data.features.battery}
		<div class="flex-none">
			<BatteryIndicator
				charging={$telemetry.battery.charging}
				soc={$telemetry.battery.soc}
				class="inline-block h-7 w-7"
			/>
		</div>
	{/if}

	{#if page.data.features.sleep}
		<div class="flex-none">
			<button class="btn btn-square btn-ghost h-9 w-10" onclick={confirmSleep}>
				<Power class="text-error h-9 w-9" />
			</button>
		</div>
	{/if}
</div>
