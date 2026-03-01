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
