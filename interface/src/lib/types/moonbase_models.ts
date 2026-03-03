/** 3-axis coordinate used in coord3D controls */
export type Coord3D = { x: number; y: number; z: number };

/** Value for a pad (button-grid) control */
export type PadValue = {
	count: number;
	width?: number;
	selected?: number;
	select?: number;
	list: number[];
	action?: string;
	rows?: unknown[][];
};

/** A single row item within a 'rows'-type module property */
export type ModuleRow = Record<string, ModuleValue>;

/** Any value that a module control can hold */
export type ModuleValue = string | number | boolean | Coord3D | PadValue | ModuleRow[] | null;

/** Top-level module state — a flat map of property name → value */
export type ModuleData = Record<string, ModuleValue>;

/** A module property / control definition received from the backend */
export type ModuleProperty = {
	name: string;
	type: string;
	min?: number;
	max?: number;
	values?: string[] | { name: string; colors?: string }[];
	default?: ModuleValue;
	ro?: boolean;
	n?: ModuleProperty[];
	color?: string;
	desc?: string;
	hoverToServer?: boolean;
	width?: number;
	size?: number;
	show?: boolean;
	crud?: string;
	filter?: string;
	rows?: unknown[][];
	p?: number;
	id?: string | number;
	value?: ModuleValue;
};

export type FilesState = {
	name: string;
	path: string;
	isFile: boolean;
	size: number;
	time: number;
	contents: string;
	files: FilesState[];
	fs_total: number;
	fs_used: number;
	showHidden: boolean;
};
