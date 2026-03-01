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
