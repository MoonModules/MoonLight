<script lang="ts">
	let { vin = 0, class: className = '' } = $props();
</script>

<div class="indicator">
	<div class="tooltip tooltip-left" data-tip={vin}>
		<span class="indicator-item indicator-start badge badge-accent badge-outline badge-xs">
			{vin}V
		</span>
	</div>
</div>
