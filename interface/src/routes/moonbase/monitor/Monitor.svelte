<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import {
		clearColors,
		colors,
		vertices,
		createScene,
		updateScene,
		setMatrixDimensions
	} from './monitor';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import { socket } from '$lib/stores/socket';
	import ControlIcon from '~icons/tabler/adjustments';

	let el: HTMLCanvasElement;

	let width = -1;
	let height = -1;
	let depth = -1;

	let done = false; //temp to show one instance of monitor data receiced

	//ask the server to run the mapping, the resulting positions are sent by websocket monitor
	const requestLayout = async () => {
		// try {
		const response = await fetch('/rest/monitorLayout', {
			method: 'GET',
			headers: {
				'Content-Type': 'application/json'
			}
		});
		console.log('requestLayout submitted');
		const arrayBuffer = await response.json();
		console.log('requestLayout received', arrayBuffer);
		// } catch (error) {
		// 	console.error('Error:', error);
		// }
	};

	const handleMonitor = (data: Uint8Array) => {
		if (data.length == 41)
			//see ModuleLightsControl.h:243
			handleHeader(data);
		else {
			if (isPositions) {
				handlePositions(data);
				isPositions = false;
			} else handleChannels(data);
		}
	};

	let nrOfLights: number;
	let channelsPerLight: number;
	let offsetRGB: number;
	let offsetWhite: number;
	let isPositions: boolean = false;
	let lightPreset: number;
	let nrOfChannels: number = 0;
	// let offsetRed:number;
	// let offsetGreen:number;
	// let offsetBlue:number;
	const lightPreset_RGB2040 = 9;

	const handleHeader = (header: Uint8Array) => {
		console.log('Monitor.handleHeader', header);

		let view = new DataView(header.buffer);

		// let isPositions:number = header[6];
		isPositions = true; //(header[6] >> 0) & 0x3; // bits 0-1
		// offsetRed     = (header[27] >> 2) & 0x3; // bits 2-3
		// offsetGreen   = (header[27] >> 4) & 0x3; // bits 4-5
		// offsetBlue    = (header[27] >> 6) & 0x3; // bits 6-7
		// offsetWhite   = view.getUint8(28);

		nrOfLights = view.getUint32(12, true);
		nrOfChannels = view.getUint32(16, true);
		lightPreset = view.getUint8(20);
		channelsPerLight = view.getUint8(21);
		offsetRGB = view.getUint8(26);
		offsetWhite = view.getUint8(28);

		//rebuild scene
		createScene(el);

		// let ledFactor: number = 1;//header[1];
		// let ledSize: number = header[23];
		width = view.getInt32(0, true);
		height = view.getInt32(4, true);
		depth = view.getInt32(8, true);

		setMatrixDimensions(width, height);

		// let nrOfLights = header[4] + 256 * header[5];

		console.log(
			'Monitor.handleHeader',
			width,
			height,
			depth,
			nrOfLights,
			channelsPerLight,
			offsetRGB,
			nrOfChannels
		);
	};

	const handlePositions = (positions: Uint8Array) => {
		console.log('Monitor.handlePositions', positions);

		for (let indexP = 0; indexP < nrOfLights; indexP++) {
			let x = positions[indexP * 3];
			let y = positions[indexP * 3 + 1];
			let z = positions[indexP * 3 + 2];

			//set to -1,1 coordinate system of webGL
			//width -1 etc as 0,0 should be top left, not bottom right
			x = width == 1 ? 0 : (x / (width - 1)) * 2.0 - 1.0;
			y = height == 1 ? 0 : ((height - 1 - y) / (height - 1)) * 2.0 - 1.0;
			z = depth == 1 ? 0 : ((depth - 1 - z) / (depth - 1)) * 2.0 - 1.0;

			vertices.push(x, y, z);
		}
	};

	const handleChannels = (channels: Uint8Array) => {
		if (!done) {
			requestLayout(); //ask for positions
			console.log('Monitor.handleChannels', channels);
			done = true;
		}
		clearColors();
		const groupSize = 20 * channelsPerLight; // RGB2040 groups: 20 lights per physical group (will be 3 channelsPerLight)
		//max size supported is 255x255x255 (index < width * height * depth) ... todo: only any of the component < 255
		for (let index = 0; index < nrOfChannels; index += channelsPerLight) {
			if (lightPreset != lightPreset_RGB2040 || Math.floor(index / groupSize) % 2 == 0) {
				// Math.floor: RGB2040 Skip the empty channels
				// && index < width * height * depth
				const r = channels[index + offsetRGB + 0] / 255;
				const g = channels[index + offsetRGB + 1] / 255;
				const b = channels[index + offsetRGB + 2] / 255;
				let w = 0;
				if (offsetWhite != 255) w = channels[index + offsetRGB + 3] / 255; //add white channel if present
				const a = 1.0; // Full opacity
				colors.push(r + w, g + w, b + w, a);
			}
		}

		updateScene(vertices, colors);
	};

	onMount(() => {
		console.log('onMount Monitor');
		socket.on('monitor', handleMonitor);
	});

	onDestroy(() => {
		console.log('onDestroy Monitor');
		socket.off('monitor', handleMonitor);
	});
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<ControlIcon class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Monitor</span>
	{/snippet}

	<div class="w-full overflow-x-auto">
		<canvas bind:this={el} width="720" height="360"></canvas>
	</div>
</SettingsCard>
