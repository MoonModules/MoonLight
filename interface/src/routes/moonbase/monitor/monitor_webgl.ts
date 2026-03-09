//based on https://github.com/jasonsturges/threejs-sveltekit/blob/main/src/lib/scene.ts
// also check https://github.com/threlte/threlte

import { mat4 } from 'gl-matrix';

let uMVPLocation: WebGLUniformLocation | null = null;

let gl: WebGLRenderingContext | null = null;
let program: WebGLProgram;
let positionBuffer: WebGLBuffer;

export let vertices: number[] = [];
export let colors: number[] = [];

// Store LED matrix dimensions
let matrixWidth: number = 1;
let matrixHeight: number = 1;
let matrixDepth: number = 1;

let colorBuffer: WebGLBuffer; // Buffer for color data

export function createScene(el: HTMLCanvasElement) {
	// Initialize WebGL
	gl = el.getContext('webgl');
	if (!gl) {
		console.error('WebGL not supported');
		return;
	}

	clearVertices();

	// Set up shaders
	const vertexShaderSource = `
  precision mediump float;
  attribute vec3 aPosition;
  attribute vec4 aColor; // Color attribute
  uniform mat4 uMVP; // Model-View-Projection matrix
  varying vec4 vColor;   // Pass color to the fragment shader

  void main() {
    gl_PointSize = 10.0;
    gl_Position = uMVP * vec4(aPosition, 1.0);
    vColor = aColor; // Pass the color to the fragment shader
  }
  `;
	const fragmentShaderSource = `
  precision mediump float;
    varying vec4 vColor;

    void main() {
      gl_FragColor = vColor;
    }
      `;

	const vertexShader = createShader(gl, gl.VERTEX_SHADER, vertexShaderSource);
	const fragmentShader = createShader(gl, gl.FRAGMENT_SHADER, fragmentShaderSource);

	program = createProgram(gl, vertexShader, fragmentShader);
	gl.useProgram(program);

	uMVPLocation = gl.getUniformLocation(program, 'uMVP');

	// Set up position buffer
	positionBuffer = gl.createBuffer();
	const positionAttributeLocation = gl.getAttribLocation(program, 'aPosition');
	gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
	gl.enableVertexAttribArray(positionAttributeLocation);
	gl.vertexAttribPointer(positionAttributeLocation, 3, gl.FLOAT, false, 0, 0);

	// Set up color buffer
	colorBuffer = gl.createBuffer();
	const colorAttributeLocation = gl.getAttribLocation(program, 'aColor');
	gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
	gl.enableVertexAttribArray(colorAttributeLocation);
	gl.vertexAttribPointer(colorAttributeLocation, 4, gl.FLOAT, false, 0, 0);

	// Set up WebGL viewport
	gl.viewport(0, 0, gl.canvas.width, gl.canvas.height);
	gl.clearColor(0, 0, 0, 1);
	gl.enable(gl.DEPTH_TEST);
}

const createShader = (gl: WebGLRenderingContext, type: number, source: string): WebGLShader => {
	const shader = gl.createShader(type);
	if (shader) {
		gl.shaderSource(shader, source);
		gl.compileShader(shader);
		if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
			console.error(gl.getShaderInfoLog(shader));
			gl.deleteShader(shader);
			throw new Error('Shader compilation failed');
		}
		return shader;
	} else throw new Error('Unable to create shader');
};

const createProgram = (
	gl: WebGLRenderingContext,
	vertexShader: WebGLShader,
	fragmentShader: WebGLShader
): WebGLProgram => {
	const program = gl.createProgram();
	gl.attachShader(program, vertexShader);
	gl.attachShader(program, fragmentShader);
	gl.linkProgram(program);
	if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
		console.error(gl.getProgramInfoLog(program));
		gl.deleteProgram(program);
		throw new Error('Program linking failed');
	}
	return program;
};

export function clearColors() {
	colors = [];
}

export function clearVertices() {
	vertices = [];
}

export function setMatrixDimensions(width: number, height: number, depth: number = 1) {
	matrixWidth = width;
	matrixHeight = height;
	matrixDepth = depth;
}

export const updateScene = (vertices: number[], colors: number[]) => {
	if (!gl) return;

	// Set the MVP matrix
	const mvp = getMVPMatrix();
	gl.uniformMatrix4fv(uMVPLocation, false, mvp);

	// Bind the position buffer and upload the vertex data
	gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
	gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.STATIC_DRAW);

	// Bind the color buffer and upload the color data
	gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
	gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(colors), gl.STATIC_DRAW);

	// Clear the canvas
	gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

	// Draw the points
	gl.drawArrays(gl.POINTS, 0, vertices.length / 3);
};

function getMVPMatrix(): mat4 {
	const canvas = gl!.canvas as HTMLCanvasElement;
	const canvasAspect = canvas.width / canvas.height;

	const fov = Math.PI / 6; // 30 degrees
	const near = 0.1;
	const far = 100.0;

	const projection = mat4.create();
	mat4.perspective(projection, fov, canvasAspect, near, far);

	// Normalize dimensions
	const maxDim = Math.max(matrixWidth, matrixHeight, matrixDepth);
	const normalizedWidth = matrixWidth / maxDim;
	const normalizedHeight = matrixHeight / maxDim;
	const normalizedDepth = matrixDepth / maxDim;

	// Calculate required distance for vertical fit
	const verticalSize = normalizedHeight;
	const distanceForHeight = verticalSize / (2 * Math.tan(fov / 2));

	// Calculate required distance for horizontal fit
	const horizontalFov = 2 * Math.atan(Math.tan(fov / 2) * canvasAspect);
	const horizontalSize = normalizedWidth;
	const distanceForWidth = horizontalSize / (2 * Math.tan(horizontalFov / 2));

	// Use the larger distance to ensure both dimensions fit
	const cameraDistance = Math.max(distanceForHeight, distanceForWidth) * 2.5;

	const view = mat4.create();
	mat4.lookAt(view, [0, 0, cameraDistance], [0, 0, 0], [0, 1, 0]);

	const model = mat4.create();

	// Scale by ALL normalized dimensions  ✅
	mat4.scale(model, model, [normalizedWidth, normalizedHeight, normalizedDepth]);

	const mvp = mat4.create();
	mat4.multiply(mvp, projection, view);
	mat4.multiply(mvp, mvp, model);

	return mvp;
}
