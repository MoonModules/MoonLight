<script lang="ts">
	import { onMount } from 'svelte';
	import { page } from '$app/state';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import { Chart, registerables } from 'chart.js';
	import Help from '~icons/tabler/help';
	import Metrics from '~icons/tabler/report-analytics';
	import { daisyColor } from '$lib/DaisyUiHelper';
	import { analytics } from '$lib/stores/analytics';
	
	Chart.register(...registerables);

	// 🌙
	let lpsChartElement: HTMLCanvasElement | undefined = $state();
	let lpsChart: Chart;

	let heapChartElement: HTMLCanvasElement | undefined = $state();
	let heapChart: Chart;

	let psramChartElement: HTMLCanvasElement | undefined = $state();
	let psramChart: Chart;

	let filesystemChartElement: HTMLCanvasElement | undefined = $state();
	let filesystemChart: Chart;

	let temperatureChartElement: HTMLCanvasElement | undefined = $state();
	let temperatureChart: Chart;

	// Check if PSRAM data is available
	let hasPsramData = $derived(Math.max(...$analytics.psram_size) > 0);

	$effect(() => {
		if (hasPsramData) initPsramChart();
	});

	function initPsramChart() {
		if (!psramChartElement || psramChart) return;
		psramChart = new Chart(psramChartElement, {
			type: 'line',
			data: {
				labels: $analytics.uptime,
				datasets: [
					{
						label: 'Used',
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $analytics.used_psram,
						yAxisID: 'y'
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
						grid: {
							color: daisyColor('--color-base-content', 10)
						},
						ticks: {
							color: daisyColor('--color-base-content')
						},
						display: false
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: 'PSRAM [KB]',
							color: daisyColor('--color-base-content'),
							font: {
								size: 16,
								weight: 'bold'
							}
						},
						position: 'left',
						min: 0,
						max: Math.round(Math.max(...$analytics.psram_size)),
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: {
							color: daisyColor('--color-base-content')
						},
						border: { color: daisyColor('--color-base-content', 10) }
					}
				}
			}
		});
	}

	onMount(() => {
		// 🌙
		lpsChart = new Chart(lpsChartElement, {
			type: 'line',
			data: {
				labels: $analytics.uptime,
				datasets: [
					{
						label: 'Loops/s',
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $analytics.lps,
						yAxisID: 'y'
					},
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
						grid: {
							color: daisyColor('--color-base-content', 10)
						},
						ticks: {
							color: daisyColor('--color-base-content')
						},
						display: false
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: 'Performance',
							color: daisyColor('--color-base-content'),
							font: {
								size: 16,
								weight: 'bold'
							}
						},
						position: 'left',
						min: 0,
						max: Math.round(Math.max(...$analytics.lps)),
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: {
							color: daisyColor('--color-base-content')
						},
						border: { color: daisyColor('--color-base-content', 10) }
					}
				}
			}
		}); //lpsChart
		heapChart = new Chart(heapChartElement, {
			type: 'line',
			data: {
				labels: $analytics.uptime,
				datasets: [
					{
						label: 'Used',
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $analytics.used_heap,
						yAxisID: 'y'
					},
					{
						label: 'Max Alloc',
						borderColor: daisyColor('--color-secondary'),
						backgroundColor: daisyColor('--color-secondary', 50),
						borderWidth: 2,
						data: $analytics.max_alloc_heap,
						yAxisID: 'y'
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
						grid: {
							color: daisyColor('--color-base-content', 10)
						},
						ticks: {
							color: daisyColor('--color-base-content')
						},
						display: false
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: 'Memory [KB]',
							color: daisyColor('--color-base-content'),
							font: {
								size: 16,
								weight: 'bold'
							}
						},
						position: 'left',
						min: 0,
						max: Math.round(Math.max(...$analytics.total_heap)),
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: {
							color: daisyColor('--color-base-content')
						},
						border: { color: daisyColor('--color-base-content', 10) }
					}
				}
			}
		});
		
		// Only create PSRAM chart if PSRAM data is available
		if (hasPsramData) initPsramChart();
		
		filesystemChart = new Chart(filesystemChartElement, {
			type: 'line',
			data: {
				labels: $analytics.uptime,
				datasets: [
					{
						label: 'Used',
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $analytics.fs_used,
						yAxisID: 'y'
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
						grid: {
							color: daisyColor('--color-base-content', 10)
						},
						ticks: {
							color: daisyColor('--color-base-content')
						},
						display: false
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: 'File System [KB]',
							color: daisyColor('--color-base-content'),
							font: {
								size: 16,
								weight: 'bold'
							}
						},
						position: 'left',
						min: 0,
						max: Math.round(Math.max(...$analytics.fs_total)),
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: {
							color: daisyColor('--color-base-content')
						},
						border: { color: daisyColor('--color-base-content', 10) }
					}
				}
			}
		});
		temperatureChart = new Chart(temperatureChartElement, {
			type: 'line',
			data: {
				labels: $analytics.uptime,
				datasets: [
					{
						label: 'Core Temperature',
						borderColor: daisyColor('--color-primary'),
						backgroundColor: daisyColor('--color-primary', 50),
						borderWidth: 2,
						data: $analytics.core_temp,
						yAxisID: 'y'
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
						grid: {
							color: daisyColor('--color-base-content', 10)
						},
						ticks: {
							color: daisyColor('--color-base-content')
						},
						display: false
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: 'Core Temperature [°C]',
							color: daisyColor('--color-base-content'),
							font: {
								size: 16,
								weight: 'bold'
							}
						},
						position: 'left',
						suggestedMin: 20,
						suggestedMax: 100,
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: {
							color: daisyColor('--color-base-content')
						},
						border: { color: daisyColor('--color-base-content', 10) }
					}
				}
			}
		});

		const poller = setInterval(updateData, 2000);
		return () => {
			clearInterval(poller);
			lpsChart?.destroy();
			heapChart?.destroy();
			psramChart?.destroy();
			filesystemChart?.destroy();
			temperatureChart?.destroy();
		};
	});

	function updateData() {
		// 🌙
		lpsChart.data.labels = $analytics.uptime;
		lpsChart.data.datasets[0].data = $analytics.lps;
		lpsChart.update('none');
		if (lpsChart.options.scales?.y) {
			lpsChart.options.scales.y.max = Math.round(Math.max(...$analytics.lps));
		}

		heapChart.data.labels = $analytics.uptime;
		heapChart.data.datasets[0].data = $analytics.used_heap;
		heapChart.data.datasets[1].data = $analytics.max_alloc_heap;
		heapChart.update('none');
		if (heapChart.options.scales?.y) {
			heapChart.options.scales.y.max = Math.round(Math.max(...$analytics.total_heap));
		}

		if (hasPsramData && psramChart) {
			psramChart.data.labels = $analytics.uptime;
			psramChart.data.datasets[0].data = $analytics.used_psram;
			psramChart.update('none');
			if (psramChart.options.scales?.y) {
				psramChart.options.scales.y.max = Math.round(Math.max(...$analytics.psram_size));
			}
		}

		filesystemChart.data.labels = $analytics.uptime;
		filesystemChart.data.datasets[0].data = $analytics.fs_used;
		filesystemChart.update('none');
		if (filesystemChart.options.scales?.y) {
			filesystemChart.options.scales.y.max = Math.round(Math.max(...$analytics.fs_total));
		}

		temperatureChart.data.labels = $analytics.uptime;
		temperatureChart.data.datasets[0].data = $analytics.core_temp;
		temperatureChart.update('none');
	}

	function convertSeconds(seconds: number) {
		// Calculate the number of seconds, minutes, hours, and days
		let minutes = Math.floor(seconds / 60);
		let hours = Math.floor(minutes / 60);
		let days = Math.floor(hours / 24);

		// Calculate the remaining hours, minutes, and seconds
		hours = hours % 24;
		minutes = minutes % 60;
		seconds = seconds % 60;

		// Create the formatted string
		let result = '';
		if (days > 0) {
			result += days + ' day' + (days > 1 ? 's' : '') + ' ';
		}
		if (hours > 0) {
			result += hours + ' hour' + (hours > 1 ? 's' : '') + ' ';
		}
		if (minutes > 0) {
			result += minutes + ' minute' + (minutes > 1 ? 's' : '') + ' ';
		}
		result += seconds + ' second' + (seconds > 1 ? 's' : '');

		return result;
	}
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Metrics class="shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>System Metrics</span>
		<div class="absolute right-5"><a href="https://{page.data.github.split("/")[0]}.github.io/{page.data.github.split("/")[1]}{page.url.pathname}" target="_blank" rel="noopener noreferrer" title="Documentation"><Help  class="shrink-0 mr-2 h-6 w-6 self-end" /></a></div> <!-- 🌙 link to docs -->
	{/snippet}

	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-60"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas bind:this={lpsChartElement}></canvas> <!-- 🌙 -->
		</div>
	</div>
	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-60"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas bind:this={heapChartElement}></canvas>
		</div>
	</div>
	{#if hasPsramData}
		<div class="w-full overflow-x-auto">
			<div
				class="flex w-full flex-col space-y-1 h-60"
				transition:slide|local={{ duration: 300, easing: cubicOut }}
			>
				<canvas bind:this={psramChartElement}></canvas>
			</div>
		</div>
	{/if}
	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-52"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas bind:this={filesystemChartElement}></canvas>
		</div>
	</div>
	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-52"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas bind:this={temperatureChartElement}></canvas>
		</div>
	</div>
</SettingsCard>
