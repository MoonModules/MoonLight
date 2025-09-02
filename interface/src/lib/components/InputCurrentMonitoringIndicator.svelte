<script lang="ts">
	let { iin = 0, class: className = '' } = $props();
</script>

<div class="indicator">
	<div class="tooltip tooltip-left" data-tip={iin}>
		<span class="indicator-item indicator-start badge badge-accent badge-outline badge-xs">
			{iin}A
		</span>
	</div>
</div>
