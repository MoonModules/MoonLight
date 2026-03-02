<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import { page } from '$app/state';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import { Chart, registerables } from 'chart.js';
	import * as LuxonAdapter from 'chartjs-adapter-luxon';
	import Battery from '~icons/tabler/battery-automotive';
	import { daisyColor } from '$lib/DaisyUiHelper';
	import { batteryHistory } from '$lib/stores/battery';

	Chart.register(...registerables);
	Chart.register(LuxonAdapter);

	let batteryChartElement: HTMLCanvasElement | undefined = $state();
	let batteryChart: Chart;

	onMount(() => {
		batteryChart = new Chart(batteryChartElement, {
			type: 'line',
			data: {
				labels: $batteryHistory.timestamp,
				datasets: [
					{
						label: 'SOC [%]',
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $batteryHistory.soc,
						yAxisID: 'y'
						// hidden: Math.max(...$batteryHistory.soc) < 0
					},
					{
						label: 'Charging',
						borderColor: daisyColor('--color-secondary', 25),
						backgroundColor: daisyColor('--color-secondary', 25),
						borderWidth: 0,
						data: $batteryHistory.charging,
						fill: true,
						stepped: true,
						yAxisID: 'y'
						// hidden: Math.max(...$batteryHistory.soc) < 0
					},
					{
						label: 'Voltage [V]', // 🌙
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $batteryHistory.voltage,
						yAxisID: 'y'
						// hidden: Math.max(...$batteryHistory.voltage) < 0
					},
					{
						label: 'Current [A]', // 🌙
						borderColor: daisyColor('--color-secondary'),
						backgroundColor: daisyColor('--color-secondary', 50),
						borderWidth: 2,
						data: $batteryHistory.current,
						yAxisID: 'y'
						// hidden: Math.max(...$batteryHistory.current) < 0
					}
				]
			},
			options: {
				maintainAspectRatio: false,
				responsive: true,
				plugins: {
					legend: {
						display: true
					},
					tooltip: {
						mode: 'index',
						intersect: false
					}
				},
				elements: {
					point: {
						radius: 1
					}
				},
				scales: {
					x: {
						type: 'time',
						grid: {
							color: daisyColor('--color-base-content', 10)
						},
						ticks: {
							color: daisyColor('--color-base-content')
						},
						display: true
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: 'Energy', // 🌙
							color: daisyColor('--color-base-content'),
							font: {
								size: 16,
								weight: 'bold'
							}
						},
						position: 'left',
						min: 0,
						max: Math.round(
							Math.max(Math.max(...$batteryHistory.voltage), Math.max(...$batteryHistory.current))
						),
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: {
							color: daisyColor('--color-base-content')
						},
						border: { color: daisyColor('--color-base-content', 10) }
					}
					// y2: {
					// 	type: 'linear',
					// 	position: 'right',
					// 	min: 0,
					// 	max: 1,
					// 	display: false
					// },
					// y3: { // 🌙
					// 	type: 'linear',
					// 	position: 'left',
					// 	min: 0,
					// 	max: Math.round(Math.max(...$batteryHistory.voltage)),
					// 	display: false
					// }, // 🌙
					// y4: {
					// 	type: 'linear',
					// 	position: 'left',
					// 	min: 0,
					// 	max: Math.round(Math.max(...$batteryHistory.current)),
					// 	display: false
					// }
				}
			}
		});

		const poller = setInterval(updateData, 5000);
		return () => {
			clearInterval(poller);
			batteryChart?.destroy();
		};
	});

	function updateData() {
		batteryChart.data.labels = $batteryHistory.timestamp;
		batteryChart.data.datasets[0].data = $batteryHistory.soc;
		batteryChart.data.datasets[1].data = $batteryHistory.charging;
		batteryChart.data.datasets[2].data = $batteryHistory.voltage; // 🌙
		batteryChart.data.datasets[3].data = $batteryHistory.current; // 🌙
		batteryChart.update('none');
		if (batteryChart.options?.scales?.y) {
			batteryChart.options.scales.y.max = Math.round(
				Math.max(Math.max(...$batteryHistory.voltage), Math.max(...$batteryHistory.current))
			);
		}
	}

</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Battery class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Energy History</span>
	{/snippet}

	<div class="w-full overflow-x-auto">
		<div
			class="flex h-60 w-full flex-col space-y-1"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas bind:this={batteryChartElement}></canvas>
		</div>
	</div>
</SettingsCard>
