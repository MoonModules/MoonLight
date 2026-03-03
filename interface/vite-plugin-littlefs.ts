import type { Plugin } from 'vite';

export default function viteLittleFS(): Plugin[] {
	return [
		{
			name: 'vite-plugin-littlefs',
			enforce: 'post',
			apply: 'build',

			async config(config, _configEnv) {
				// 🌙 Initialise rollupOptions.output if absent so the plugin is
				// self-sufficient and doesn't require it to be pre-set in vite.config.ts
				if (!config.build) config.build = {};
				if (!config.build.rollupOptions) config.build.rollupOptions = {};
				const output = config.build.rollupOptions.output ?? {};

				const outputs = Array.isArray(output) ? output : [output];
				const normalized = outputs.map((o) => {
					const next = { ...o };

					if (typeof next.assetFileNames === 'string') {
						next.assetFileNames = next.assetFileNames.replace('.[hash]', '');
					}

					if (typeof next.chunkFileNames === 'string' && next.chunkFileNames.includes('[hash]')) {
						next.chunkFileNames = next.chunkFileNames.replace('.[hash]', '');
					}

					if (typeof next.entryFileNames === 'string' && next.entryFileNames.includes('[hash]')) {
						next.entryFileNames = next.entryFileNames.replace('.[hash]', '');
					}

					return next;
				});

				config.build.rollupOptions.output = Array.isArray(output) ? normalized : normalized[0];
			}
		}
	];
}
