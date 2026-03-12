function isLowerCase(s: string) {
	return s.toLowerCase() == s;
}

export function initCap(s: string) {
	if (typeof s !== 'string') return '';
	let result = '';
	for (let i = 0; i < s.length; i++) {
		if (i == 0)
			//first uppercase
			result += s.charAt(i).toUpperCase();
		else if (!isLowerCase(s.charAt(i)) && isLowerCase(s.charAt(i - 1)))
			//uppercase (previous not uppercase) => add space
			// else if (!isLowerCase(s.charAt(i)) && isLowerCase(s.charAt(i-1)) && (i+1 >= s.length || isLowerCase(s.charAt(i+1))))
			result += ' ' + s.charAt(i);
		// else if (!isLowerCase(s.charAt(i)) && !isLowerCase(s.charAt(i-1)) && (i+1 >= s.length || isLowerCase(s.charAt(i+1))))
		//   result += " " + s.charAt(i);
		else if (s.charAt(i) == '-' || s.charAt(i) == '_')
			//- and _ is space
			result += ' ';
		else result += s.charAt(i);
	}
	return result;
}

/**
 * Positions a fixed dropdown list relative to a trigger button, scrolling to the selected item.
 * Reused by palette dropdown and selectFile (node selector) dropdown.
 */
export function positionDropdown(
	triggerEl: HTMLElement,
	listEl: HTMLElement,
	selectedSelector: string = '[aria-selected="true"]'
) {
	const triggerRect = triggerEl.getBoundingClientRect();
	const vh = window.innerHeight;

	listEl.style.position = 'fixed';
	listEl.style.top = '-9999px';
	listEl.style.left = `${triggerRect.left}px`;
	listEl.style.maxHeight = `${vh}px`;
	void listEl.offsetHeight; // force reflow

	const selectedEl = listEl.querySelector(selectedSelector) as HTMLElement | null;

	if (selectedEl) {
		const triggerCenterY = triggerRect.top + triggerRect.height / 2;
		const idealTop = triggerCenterY - selectedEl.offsetTop - selectedEl.offsetHeight / 2;
		const clampedTop = Math.max(4, idealTop);
		const naturalBottom = idealTop + listEl.scrollHeight;
		const clampedBottom = Math.min(vh - 4, naturalBottom);
		listEl.style.top = `${clampedTop}px`;
		listEl.style.maxHeight = `${clampedBottom - clampedTop}px`;
		listEl.scrollTop = Math.max(
			0,
			clampedTop + selectedEl.offsetTop + selectedEl.offsetHeight / 2 - triggerCenterY
		);
	} else {
		listEl.style.top = `${triggerRect.bottom + 2}px`;
		listEl.style.maxHeight = `${vh - triggerRect.bottom - 8}px`;
	}
}

/**
 * Extracts unique emoji characters from a string. Used for tag cloud in node selector.
 */
export function extractEmojis(text: string): string[] {
	const emojiRegex =
		/[\p{Emoji_Presentation}\p{Extended_Pictographic}]\uFE0F?(\u200D[\p{Emoji_Presentation}\p{Extended_Pictographic}]\uFE0F?)*/gu;
	const matches = text.match(emojiRegex);
	return matches ? [...new Set(matches)] : [];
}

export function getTimeAgo(timestamp: number, currentTime: number = Date.now()): string {
	const diff = Math.abs(currentTime - timestamp * 1000); // Convert seconds to milliseconds (abs to avoid -1)

	const seconds = Math.floor(diff / 1000);
	const minutes = Math.floor(seconds / 60);
	const hours = Math.floor(minutes / 60);
	const days = Math.floor(hours / 24);

	if (days > 0) return `${days}d${hours - days * 24}h`;
	if (hours > 0) return `${hours}h${minutes - hours * 60}m`;
	if (minutes > 0) return `${minutes}m${seconds - minutes * 60}s`;
	return `${seconds}s`;
}
