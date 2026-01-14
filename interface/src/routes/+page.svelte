<script lang="ts">
	import type { PageData } from './$types';
	import logo from '$lib/assets/logo.png';
	import { notifications } from '$lib/components/toasts/notifications';

	interface Props {
		data: PageData;
	}

	let { data }: Props = $props();
</script>

<div class="hero bg-base-100 h-screen">
	<div class="card md:card-side bg-base-200 shadow-primary shadow-2xl">
		<figure class="bg-base-200"><img src={logo} alt="Logo" class="h-auto w-64" /></figure>
		<div class="card-body w-80">
			<h2 class="card-title text-center text-2xl">Welcome to MoonLight</h2>
			<p class="py-6 text-center">
				<b>Effect controller for direct and networked LEDs and lights. By MoonModules.</b>
			</p>
			<span>
				1 Setup <a class="btn btn-primary" href="/wifi/sta">WiFi</a>
			</span>
			<span>
				2 Select a board preset in
				<a class="btn btn-primary" href="/moonbase/module?group=moonlight&module=inputoutput">IO</a>
			</span>
			<span>
				3 Add Layout & Driver
				<a class="btn btn-primary" href="/moonbase/module?group=moonlight&module=drivers">Drivers</a
				>
			</span>
			<span>
				4 Add
				<a class="btn btn-primary" href="/moonbase/module?group=moonlight&module=effects">Effects</a
				>
			</span>
			<span>
				5
				<a class="btn btn-primary" href="/moonbase/module?group=moonlight&module=lightscontrol">Control</a
				>
			</span>
			<span>
				🛟
				<a
					href="https://moonmodules.github.io/MoonLight/gettingstarted/installer/#setup-moonlight"
					class="link"
					target="_blank"
					rel="noopener noreferrer">Help</a
				>
			</span>
			<span>
				👀
				<a
					href="https://discord.gg/TC8NSUSCdV"
					class="link"
					target="_blank"
					rel="noopener noreferrer">Discord</a
				>
				<a
					href="https://www.youtube.com/@MoonModulesLighting"
					class="link"
					target="_blank"
					rel="noopener noreferrer">YouTube</a
				>
				<a
					href="https://reddit.com/r/MoonModules"
					class="link"
					target="_blank"
					rel="noopener noreferrer">Reddit</a
				>
				<a href="https://moonmodules.org" class="link" target="_blank" rel="noopener noreferrer"
					>MoonModules.org</a
				>
			</span>
		</div>
	</div>
</div>
