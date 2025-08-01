<script lang="ts">
	import type { LayoutData } from './$types';
	import { onDestroy, onMount } from 'svelte';
	import { user } from '$lib/stores/user';
	import { telemetry } from '$lib/stores/telemetry';
	import { analytics } from '$lib/stores/analytics';
	import { batteryHistory } from '$lib/stores/battery';
	import { socket } from '$lib/stores/socket';
	import type { userProfile } from '$lib/stores/user';
	import { page } from '$app/state';
	import { Modals, modals } from 'svelte-modals';
	import Toast from '$lib/components/toasts/Toast.svelte';
	import { notifications } from '$lib/components/toasts/notifications';
	import { fade } from 'svelte/transition';
	import '../app.css';
	import Menu from './menu.svelte';
	import Statusbar from './statusbar.svelte';
	import Login from './login.svelte';
	import type { Analytics } from '$lib/types/models';
	import type { RSSI } from '$lib/types/models';
	import type { Battery } from '$lib/types/models';
	import type { DownloadOTA } from '$lib/types/models';
	import Monitor from './moonbase/monitor/Monitor.svelte'; // 🌙

	interface Props {
		data: LayoutData;
		children?: import('svelte').Snippet;
	}

	let { data, children }: Props = $props();

	onMount(async () => {
		if ($user.bearer_token !== '') {
			await validateUser($user);
		}
		if (!(page.data.features.security && $user.bearer_token === '')) {
			initSocket();
		}
	});

	const initSocket = () => {
		const ws_token = page.data.features.security ? '?access_token=' + $user.bearer_token : '';
		socket.init(
			`ws://${window.location.host}/ws/events${ws_token}`,
			page.data.features.event_use_json
		);
		addEventListeners();
	};

	onDestroy(() => {
		removeEventListeners();
	});

	const addEventListeners = () => {
		socket.on('open', handleOpen);
		socket.on('close', handleClose);
		socket.on('error', handleError);
		socket.on('rssi', handleNetworkStatus);
		socket.on('notification', handleNotification);
		if (page.data.features.analytics) socket.on('analytics', handleAnalytics);
		if (page.data.features.battery) socket.on('battery', handleBattery);
		if (page.data.features.download_firmware) socket.on('otastatus', handleOAT);
	};

	const removeEventListeners = () => {
		socket.off('analytics', handleAnalytics);
		socket.off('open', handleOpen);
		socket.off('close', handleClose);
		socket.off('rssi', handleNetworkStatus);
		socket.off('notification', handleNotification);
		socket.off('battery', handleBattery);
		socket.off('otastatus', handleOAT);
	};

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
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	const handleOpen = () => {
		notifications.success('Connection to device established', 5000);
	};

	const handleClose = () => {
		if (!location.host.includes("captive.apple.com")) // 🌙 dirty workaround to not show this on macOS captive portal...
			notifications.error('Connection to device lost', 5000);
		telemetry.setRSSI({ rssi: 0, ssid: '', safeMode: false, restartNeeded: false });
	};

	const handleError = (data: any) => console.error(data);

	const handleNotification = (data: any) => {
		switch (data.type) {
			case 'info':
				notifications.info(data.message, 5000);
				break;
			case 'warning':
				notifications.warning(data.message, 5000);
				break;
			case 'error':
				notifications.error(data.message, 5000);
				break;
			case 'success':
				notifications.success(data.message, 5000);
				break;
			default:
				break;
		}
	};

	const handleAnalytics = (data: Analytics) => analytics.addData(data);

	const handleNetworkStatus = (data: RSSI) => telemetry.setRSSI(data);

	const handleBattery = (data: Battery) => {
		telemetry.setBattery(data);
		batteryHistory.addData(data);
	};

	const handleOAT = (data: DownloadOTA) => telemetry.setDownloadOTA(data);

	let menuOpen = $state(false);
</script>

<svelte:head>
	<title>{page.data.devices.deviceName}</title>
</svelte:head>

{#if page.data.features.security && $user.bearer_token === ''}
	<Login on:signIn={initSocket} />
{:else}
	<div class="drawer lg:drawer-open">
		<input id="main-menu" type="checkbox" class="drawer-toggle" bind:checked={menuOpen} />
		<div class="drawer-content flex flex-col">
			<!-- Status bar content here -->
			<Statusbar />

			<!-- 🌙 Show Monitor (only if moon screen) -->
			{#if (page.data.features.monitor && page.url.pathname.includes("moon"))}
				<br>
				<Monitor />
			{/if}
		
			<!-- Main page content here -->
			{@render children?.()}
		</div>
		<!-- Side Navigation -->
		<div class="drawer-side z-30 shadow-lg">
			<label for="main-menu" class="drawer-overlay"></label>
			<Menu
				closeMenu={() => {
					menuOpen = false;
				}}
			/>
		</div>
	</div>
{/if}

<Modals>
	<!-- svelte-ignore a11y_click_events_have_key_events -->
	{#snippet backdrop({ close })}
		<div
			class="fixed inset-0 z-40 max-h-full max-w-full bg-black/20 backdrop-blur-sm"
			transition:fade|global
			onclick={() => close()}
			role="button"
			tabindex="0"
		></div>
	{/snippet}
</Modals>

<Toast />
