import { writable } from 'svelte/store';
import type { RSSI } from '../types/models';
import type { SystemStatus } from '../types/models'; // 🌙
import type { Battery } from '../types/models';
import type { OTAStatus } from '../types/models';
import type { Ethernet } from '../types/models';

let telemetry_data = {
	rssi: {
		rssi: 0,
		ssid: '',
		disconnected: true
	},
	// 🌙 System status — separate from rssi so it works on all boards (including ethernet-only)
	status: {
		safeMode: false,
		restartNeeded: false,
		saveNeeded: false,
		hostName: localStorage.getItem('hostName') || 'MoonLight' // 🌙 persist across page loads
	},
	battery: {
		soc: -1,
		charging: false,
		voltage: -1, // 🌙
		current: -1, // 🌙
	},
	ota_status: {
		status: 'none',
		progress: 0,
		bytes_written: 0,
		total_bytes: 0,
		error: ''
	},
	ethernet: {
		connected: false
	}
};

function createTelemetry() {
	const { subscribe, set, update } = writable(telemetry_data);

	return {
		subscribe,
		setRSSI: (data: RSSI) => {
			if (!isNaN(Number(data.rssi))) {
				update((telemetry_data) => ({
					...telemetry_data,
					rssi: { rssi: Number(data.rssi), ssid: data.ssid, disconnected: false }
				}));
			} else {
				update((telemetry_data) => ({
					...telemetry_data,
					rssi: { rssi: 0, ssid: data.ssid, disconnected: true }
				}));
			}
		},
		// 🌙 System status flags (saveNeeded, restartNeeded, safeMode, hostName)
		setStatus: (data: SystemStatus) => {
			if (data.hostName) localStorage.setItem('hostName', data.hostName); // 🌙 persist for next page load
			update((telemetry_data) => ({
				...telemetry_data,
				status: {
					safeMode: data.safeMode,
					restartNeeded: data.restartNeeded,
					saveNeeded: data.saveNeeded,
					hostName: data.hostName
				}
			}));
		},
		setBattery: (data: Battery) => {
			update((telemetry_data) => ({
				...telemetry_data,
				battery: { soc: data.soc, charging: data.charging, voltage: data.voltage, current: data.current }
			}));
		},
		setOTAStatus: (data: OTAStatus) => {
			update((telemetry_data) => ({
				...telemetry_data,
				ota_status: {
					status: data.status,
					progress: data.progress,
					bytes_written: data.bytes_written ?? 0,
					total_bytes: data.total_bytes ?? 0,
					error: data.error
				}
			}));
		},
		setEthernet: (data: Ethernet) => {
			update((telemetry_data) => ({
				...telemetry_data,
				ethernet: { connected: data.connected }
			}));
		}
	};
}

export const telemetry = createTelemetry();
