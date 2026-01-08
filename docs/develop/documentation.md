## Overview

A software project includes its functional aspects and the knowledge surrounding it. Document your experience to make it easier to have fun making things and to get engaged in improving the project.

Documentation of MoonLight is also included in the MoonLight repository, see the [docs folder](https://github.com/MoonModules/MoonLight/tree/main/docs). Fomat of documentation is mkdocs, see below.
Part of submitting a change via a pull request is updated documentation. Functionality and documentation should be in one Pull Request
After the pull request is accepted, the documentation shows up in this MoonLight website.

Store technical documentation directly in the code. Functional documentation as follows:

### Adding or changing Nodes

If you create or change a [node](https://moonmodules.org/MoonLight/develop/nodes), create or change the node in the overview tables of the node type (Effects, Modifiers, Layers, Drivers). Each row in the table looks as follows: 

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Name of the node 🆕🚧 | [Image](https://moonmodules.org/MoonLight/develop/documentation#images) | Screenshot of the controls | Short description |

Add the 🆕 or 🚧 emoji if applicable

Location of the source doc files and the resulting webpages:

* [Nodes docs](https://github.com/MoonModules/MoonLight/tree/main/docs/moonlight)
* Nodes webpages: [Effects](https://moonmodules.org/MoonLight/moonlight/effects), [Modifiers](https://moonmodules.org/MoonLight/moonlight/modifiers), [Layouts](https://moonmodules.org/MoonLight/moonlight/layouts) and [Drivers](https://moonmodules.org/MoonLight/moonlight/drivers)

### Adding or changing Modules

If you create or change a [module](https://moonmodules.org/MoonLight/develop/modules), update the according module docs.

* Image: see [Image](https://moonmodules.org/MoonLight/develop/documentation#images)

Location of the source doc files and the resulting webpages:

* [MoonBase docs](https://github.com/MoonModules/MoonLight/tree/main/docs/moonbase)
* [MoonBase Webpages](https://moonmodules.org/MoonLight/moonbase/overview)

* [MoonLight docs](https://github.com/MoonModules/MoonLight/tree/main/docs/moonlight)
* [MoonLight Webpages](https://moonmodules.org/MoonLight/moonlight/overview)

## Images

* Image
	* For drivers an image of what is driven, for Effects, Modifiers and layouts image showing the result on the monitor. 
	* Images should be max 40-100KB, 320 px wide for nodes, 640 px wide, ~100KB for larger images, use [iloveimg resize](https://www.iloveimg.com/resize-image) or similar to resize the image. Otherwise page download will be too slow.
	* Store the image in the [media folder](https://github.com/MoonModules/MoonLight/tree/main/docs/media) in /moonbase or /moonlight and refer to it using relative paths: ../../media/moonlight/<image>
	* For WLED effects, copy the image link already existing on [kno.WLED.ge](https://kno.wled.ge/features/effects) or [MM.kno.WLED.ge](https://mm.kno.wled.ge/features/effects).
	* Movie to gif: use the [gifmaker.sh](https://github.com/MoonModules/MoonLight/blob/main/misc/gifmaker.sh) - ffmpeg must be installed.

## MKDocs

### MKDocs Preview in VS Code - Installation

This method can be used to see a live preview of changes to the docs while editing in VS Code.

Download the ZIP: [https://github.com/libukai/mkdocs-preview](https://github.com/libukai/mkdocs-preview)

Place the following in workspace settings (F1, if working in a workspace) or create .vscode/settings.json:

	{
		"mkdocsPreview.port": 8000,
		//"mkdocsPreview.port": 4001,
		
		// remove the '/docs' prefix from every URL
		// rewrite "/docs/XYZ" → "/XYZ"
		"mkdocsPreview.baseDir": "",
		"mkdocsPreview.urlReplace": "/docs/||/",

		"simpleBrowser.defaultUrl": "http://localhost:8000/"
		//"simpleBrowser.defaultUrl": "http://127.0.0.1:8000/",
	}

Open Powershell in your working directory in VS Code.

Run:

	python --version
	python -m pip --version
	python -m pip install --upgrade pip
	python -m pip install mkdocs-material

Run either:

	mkdocs serve -a localhost:8000
	mkdocs serve -a 127.0.0.1:8000

It should now be visible 🐉
	

### Troubleshooting:
1. Try using these commands to manually test addresses after starting the service in VS Code:

	Ctl + Shft + P -> MKDocs Preview: Toggle MkDocs Preview

	Ctl+ Shft + P -> Simple Browser: Show

2. Check the address in a browser:

	[https://localhost:8000](https://localhost:8000)

	[https://127.0.0.1:8000/](https://127.0.0.1:8000)

3. Ensure mkdocs serve is running and reboot the service.