<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { addLed, colorLed, createScene } from './monitor';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import { socket } from '$lib/stores/socket';
	import type { FixtureState } from '$lib/types/models';
	import ControlIcon from '~icons/tabler/adjustments';

	let el:HTMLCanvasElement;

	let fixtureState: FixtureState = { name: "test", lightsOn:true, brightness: 50, fixture: -1, width: 16, height:16, depth:16, driverOn: true, monitorOn: true, pin: 16 };
	let fixture: number = -1;
	let width = -1;
	let height = -1;
	let depth = -1;

	let done = false; //temp to show one instance of monitor data receiced

	const loadFixtureDefinition = async () => {
		// try {
			const response = await fetch('/rest/starAPI?{map:true}', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				}
			});
			const arrayBuffer = await response.json();
			console.log("Monitor.starAPI map", arrayBuffer);
			// handleFixtureDefinition(new Uint8Array(arrayBuffer));
		// } catch (error) {
		// 	console.error('Error:', error);
		// }
	}
	loadFixtureDefinition();

	const handleFixtureState = (data: FixtureState) => {
		console.log("Monitor.handleFixtureState", data.fixture, fixtureState.fixture);

		fixtureState = data;
	};

	const handleMonitor = (ledsPExtended: Uint8Array) => {
        const headerLength = 3; // Define the length of the header
        const header = ledsPExtended.slice(0, headerLength);
        const ledsP = ledsPExtended.slice(headerLength);

		
		let type:number = header[0];
		
		//fixChange
		if (type == 1) {
			// console.log("Monitor.handleMonitor", ledsPExtended);
			handleFixtureDefinition(header, ledsP);
		} else {
			if (!done)
				console.log("Monitor.handleMonitor", ledsP);
			for (let index = 0; index < ledsP.length; index +=3) {
				colorLed(index/3, ledsP[index]/255, ledsP[index+1]/255, ledsP[index+2]/255);
			}
			done = true;
		}
	};

	const handleFixtureDefinition = (header: Uint8Array, ledsP: Uint8Array) => {
		console.log("Monitor.handleFixtureDefinition", header, ledsP);
		// data.forEach((value, index) => {
    	//     console.log(`Index ${index}: ${value}`);
	    // });

		//rebuild scene
		createScene(el);

		width = 0;
		height = 0;
		depth = 0;
		let ledFactor: number = header[1];
		let ledSize: number = header[2];
		// data[3]=0; data[4]=0; data[5]=0;

		//parse 1
		for (let index = 0; index < ledsP.length; index +=3) {
			// console.log(data[index], data[index+1], data[index+2]);
			let x = ledsP[index] / ledFactor;
			let y = ledsP[index+1] / ledFactor;
			let z = ledsP[index+2] / ledFactor;

			if (x > width) width = x;
			if (y > height) height = y;
			if (z > depth) depth = z;
		}

		width = Math.ceil(width) + 1;
		height = Math.ceil(height) + 1;
		depth = Math.ceil(depth) + 1;
		console.log("dimensions", width, height, depth);
		//parse 2
		for (let index = 0; index < ledsP.length; index +=3) {
			let x = ledsP[index] / ledFactor;
			let y = ledsP[index+1] / ledFactor;
			let z = ledsP[index+2] / ledFactor;
			addLed(0.3, x - width/2, y - height/2, z - depth/2);
		}
	}

	onMount(() => {
		console.log("onMount Monitor", el)
		
		socket.on("fixture", handleFixtureState);
		socket.on('monitor', handleMonitor);
	});

	onDestroy(() => {
		console.log("onDestroy Monitor");
		socket.off("fixture", handleFixtureState);
		socket.off("monitor", handleMonitor);
	});

</script>

<SettingsCard collapsible={false}>

	<ControlIcon slot="icon" class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
	<span slot="title">Monitor</span>
	<!-- <div class="w-full overflow-x-auto"> -->
	 <div>
		<canvas bind:this={el}/>
	</div>

</SettingsCard>