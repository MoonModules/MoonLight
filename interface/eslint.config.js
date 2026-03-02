import js from '@eslint/js';
import svelte from 'eslint-plugin-svelte';
import tsPlugin from '@typescript-eslint/eslint-plugin';
import tsParser from '@typescript-eslint/parser';
import prettier from 'eslint-config-prettier';
import globals from 'globals';

export default [
	// Global ignores (replaces .eslintignore)
	// Upstream ESP32-sveltekit files are excluded so that merging upstream changes
	// doesn't produce spurious conflicts. Only MoonLight-specific files are linted.
	{
		ignores: [
			'build/**',
			'.svelte-kit/**',
			'node_modules/**',
			'package/**',
			'**/*.timestamp-*.mjs',

			// Upstream ESP32-sveltekit files – do not modify to keep merges clean
			'src/app.css',
			'src/app.html',
			// lib: everything except the moonbase subdirectory
			'src/lib/components/*.svelte',
			'src/lib/components/*.ts',
			'src/lib/components/toasts/**',
			'src/lib/stores/analytics.ts',
			'src/lib/stores/battery.ts',
			'src/lib/stores/socket.ts',
			'src/lib/stores/telemetry.ts',
			'src/lib/stores/user.ts',
			'src/lib/types/models.ts',
			// routes: everything except moonbase
			'src/routes/+error.svelte',
			'src/routes/+layout.svelte',
			'src/routes/+layout.ts',
			'src/routes/+page.svelte',
			'src/routes/connections/**',
			'src/routes/ethernet/**',
			'src/routes/login.svelte',
			'src/routes/menu.svelte',
			'src/routes/statusbar.svelte',
			'src/routes/system/**',
			'src/routes/user/**',
			'src/routes/wifi/**'
		]
	},

	// Base JavaScript recommended rules
	js.configs.recommended,

	// TypeScript plugin + parser (applies to all files; svelte overrides parser for .svelte below)
	...tsPlugin.configs['flat/recommended'],

	// Browser + Node globals for all source files
	{
		languageOptions: {
			globals: {
				...globals.browser,
				...globals.node,
				...globals.es2020
			},
			parserOptions: {
				sourceType: 'module',
				ecmaVersion: 2020
			}
		}
	},

	// Svelte: sets svelte-eslint-parser for .svelte files and includes a11y rules (as warnings)
	...svelte.configs['flat/recommended'],

	// Tell svelte-eslint-parser to use @typescript-eslint/parser for <script> blocks
	{
		files: ['**/*.svelte'],
		languageOptions: {
			parserOptions: {
				parser: tsParser
			}
		}
	},

	// Project-level rule overrides
	{
		rules: {
			// Dynamic JSON data from the backend makes `any` unavoidable in many places;
			// flag as a warning to remain visible without blocking the build.
			'@typescript-eslint/no-explicit-any': 'warn',
			// Writable $derived can't always replace $state+$effect when state is also mutated externally.
			'svelte/prefer-writable-derived': 'warn',
			// Allow _-prefixed identifiers as intentionally unused (common convention).
			'@typescript-eslint/no-unused-vars': [
				'error',
				{
					argsIgnorePattern: '^_',
					varsIgnorePattern: '^_',
					caughtErrorsIgnorePattern: '^_'
				}
			]
		}
	},

	// Prettier: disables all formatting-related ESLint rules (must be last)
	prettier
];
