<!--
   @title     MoonLight
   @file      File.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Copyright © 2025 Github MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
-->

<script lang="ts">
    export let label: string;
    export let onChange = (event:any) => {console.log(event)};
</script>

<label class="label cursor-pointer" for={label}>
    <span class="mr-4">{label}</span>
</label>
<input 
    type="file"
    id={label}
    on:change={onChange}
/>