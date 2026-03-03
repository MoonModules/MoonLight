import { defineConfig } from 'vitest/config';

// Minimal config for unit tests — intentionally excludes vite plugins (sveltekit,
// tailwindcss, icons, littlefs) so tests have no native-binary or platform-specific
// dependencies (e.g. lightningcss). Tests are pure TypeScript only.
export default defineConfig({
	test: {
		include: ['src/**/*.test.ts']
	}
});
