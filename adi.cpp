#include <cstdio>
#include <cstdlib>
#include "utility/common.h"
#include <unistd.h>
#include <time.h>
#include "math/qmath.h"

#include "bcm_host.h"

#include "GLES2/gl2.h"
#include "EGL/egl.h"
#include "EGL/eglext.h"

#include "mqtt.h"

#define STBI_ONLY_PNG
#include "nanovg/src/stb_image.h"
#include "nanovg/src/nanovg.h"
#define NANOVG_GLES2_IMPLEMENTATION
#include "nanovg/src/nanovg_gl.h"

extern "C" {
#include "revision.h"
}


struct AppState
{
	// dispmanx / EGL objects
	u32 		screenWidth;
	u32 		screenHeight;
	DISPMANX_DISPLAY_HANDLE_T dispmanDisplay;
	DISPMANX_ELEMENT_HANDLE_T dispmanElement;
	EGLDisplay 	display;
	EGLSurface 	surface;
	EGLContext 	context;
	// OpenGL|ES state
	GLuint 		vShader;
	GLuint 		fShader;
	GLuint 		program;
	GLuint 		fbo;
	GLuint 		renderTex;
	// shader attribs and uniforms
	GLint 		attrVertexPosition;
	GLint 		attrVertexNormal;
	GLint 		attrVertexUV;
	GLint 		unifModelView;
	GLint 		unifModelViewProj;
	GLint 		unifNormalMatrix;
	GLint 		unifCameraPos;
	GLint 		unifDiffuseColor;
	GLint 		unifDiffuseTex;
	// texture buffers
	GLuint 		texBackupADI;
	// NanoVG state
	NVGcontext*	vg;
	i32 		fontNormal;
	i32 		fontBold;
	i32 		fontIcons;
};


struct TimeState
{
	timespec	tsNow;
	u64			now_nsec;
	u64			prev_nsec;
	r32			dt_ms;
	r32			fps;
	u64			fpsPrev_nsec;
};


volatile bool running = true;
AppState state{};
TimeState timer{};
MQTTState mqttState{};


bool initOpenGL()
{
	static EGL_DISPMANX_WINDOW_T nativeWindow;

	static const EGLint attributeList[] =
	{
		EGL_RED_SIZE,		8,
		EGL_GREEN_SIZE,		8,
		EGL_BLUE_SIZE,		8,
		EGL_ALPHA_SIZE,		8,
		EGL_DEPTH_SIZE,		0,
		EGL_STENCIL_SIZE,	0,
		EGL_SURFACE_TYPE,	EGL_WINDOW_BIT,
		EGL_SAMPLE_BUFFERS,	1,
		EGL_SAMPLES,		4, // 4x MSAA
		EGL_MIN_SWAP_INTERVAL, 0,
		EGL_NONE
	};

	static const EGLint contextAttributes[] = 
	{
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	// get an EGL display connection
	state.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	assert(state.display != EGL_NO_DISPLAY);

	// initialize the EGL display connection
	EGLBoolean result = eglInitialize(state.display, nullptr, nullptr);
	assert(result != EGL_FALSE);

	// get an appropriate EGL frame buffer configuration
	EGLConfig config;
	EGLint numConfig;
	result = eglChooseConfig(
		state.display,
		attributeList,
		&config,
		1,
		&numConfig);
	assert(result != EGL_FALSE);

	// get an appropriate EGL frame buffer configuration
	result = eglBindAPI(EGL_OPENGL_ES_API);
	assert(result != EGL_FALSE);

	// create an EGL rendering context
	state.context = eglCreateContext(
		state.display,
		config,
		EGL_NO_CONTEXT,
		contextAttributes);
	assert(state.context != EGL_NO_CONTEXT);

	// create an EGL window surface
	i32 success = graphics_get_display_size(
		0, // LCD
		&state.screenWidth,
		&state.screenHeight);
	assert(success >= 0);

	VC_RECT_T dstRect{};
	dstRect.x = 0;
	dstRect.y = 0;
	dstRect.width = state.screenWidth;
	dstRect.height = state.screenHeight;
	
	VC_RECT_T srcRect{};
	srcRect.x = 0;
	srcRect.y = 0;
	srcRect.width = state.screenWidth << 16;
	srcRect.height = state.screenHeight << 16;        

	state.dispmanDisplay = vc_dispmanx_display_open(0); // LCD
	DISPMANX_UPDATE_HANDLE_T dispmanUpdate = vc_dispmanx_update_start(0);
			
	state.dispmanElement = vc_dispmanx_element_add(
		dispmanUpdate,
		state.dispmanDisplay,
		0, // layer
		&dstRect,
		0, // src
		&srcRect,
		DISPMANX_PROTECTION_NONE,
		nullptr, // alpha
		nullptr, // clamp
		(DISPMANX_TRANSFORM_T)0); // transform
		
	nativeWindow.element = state.dispmanElement;
	nativeWindow.width = state.screenWidth;
	nativeWindow.height = state.screenHeight;
	vc_dispmanx_update_submit_sync(dispmanUpdate);
	
	state.surface = eglCreateWindowSurface(
		state.display,
		config,
		&nativeWindow,
		nullptr);
	assert(state.surface != EGL_NO_SURFACE);

	// connect the context to the surface
	result = eglMakeCurrent(state.display, state.surface, state.surface, state.context);
	assert(result != EGL_FALSE);

	// Set background color and clear buffers
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// Enable back face culling.
	glEnable(GL_CULL_FACE);

	// create a texture
	glGenTextures(1, &state.renderTex);
	glBindTexture(GL_TEXTURE_2D,state.renderTex);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGB,
		state.screenWidth,
		state.screenHeight,
		0,
		GL_RGB,
		GL_UNSIGNED_SHORT_5_6_5,
		0);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	
	// create the FBO
	glGenFramebuffers(1, &state.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER,state.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state.renderTex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER,0);

	glViewport(0, 0, (GLsizei)state.screenWidth, (GLsizei)state.screenHeight);

	ASSERT_GL_ERROR;

	return true;
}


void showShaderLog(
	GLint shader)
{
	char log[1024];
	glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
	printf("%d:shader:\n%s\n", shader, log);
}


void showProgramLog(
	GLint shader)
{
	char log[1024];
	glGetProgramInfoLog(shader, sizeof(log), nullptr, log);
	printf("%d:program:\n%s\n", shader, log);
}


bool initShaders()
{
	const GLchar *vShaderSource =
		"#version 100\n"
		// Uniforms
		"uniform mat4 modelView;"
		"uniform mat4 modelViewProj;"
		"uniform mat4 normalMatrix;"		// inverse transpose of upper-left 3x3 of modelView"
		// Input Variables
		"attribute vec3 vertexPosition;"	// in modelspace
		"attribute vec3 vertexNormal;"
		"attribute vec2 vertexUV;"
		// Output Variables
		"varying vec4 positionViewspace;"
		"varying vec3 normalViewspace;"
		"varying vec2 uv;"

		"void main() {"
			"positionViewspace = modelView * vec4(vertexPosition, 1.0);"
			"normalViewspace = normalize(normalMatrix * vec4(vertexNormal, 0.0)).xyz;"
			"uv = vertexUV;"
			"gl_Position = modelViewProj * vec4(vertexPosition, 1.0);"
		"}";

	const GLchar *fShaderSource =
		"#version 100\n"
		// Uniforms
		"uniform sampler2D diffuseTex;"
		"uniform vec3 cameraPos;"
		// Input Variables
		"varying vec4 positionViewspace;"
		"varying vec3 normalViewspace;"
		"varying vec2 uv;"

		"void main() {"
			"float lightIntensity = dot(normalize(cameraPos - positionViewspace.xyz), normalViewspace);"
			"vec3 diffuse = texture2D(diffuseTex, uv).rgb;"
			"gl_FragColor = vec4(diffuse * lightIntensity, 1.0);"
		"}";

	GLint status = GL_FALSE;

	state.vShader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(state.vShader, 1, &vShaderSource, 0);
	glCompileShader(state.vShader);
	glGetShaderiv(state.vShader, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		showShaderLog(state.vShader);
		return false;
	}

	state.fShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(state.fShader, 1, &fShaderSource, 0);
	glCompileShader(state.fShader);
	glGetShaderiv(state.fShader, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		showShaderLog(state.fShader);
		return false;
	}

	state.program = glCreateProgram();
	glAttachShader(state.program, state.vShader);
	glAttachShader(state.program, state.fShader);
	glLinkProgram(state.program);
	glGetProgramiv(state.program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		showProgramLog(state.program);
		return false;
	}

	state.attrVertexPosition = glGetAttribLocation(state.program, "vertexPosition");
	state.attrVertexNormal   = glGetAttribLocation(state.program, "vertexNormal");
	state.attrVertexUV       = glGetAttribLocation(state.program, "vertexUV");

	state.unifModelView     = glGetUniformLocation(state.program, "modelView");
	state.unifModelViewProj = glGetUniformLocation(state.program, "modelViewProj");
	state.unifNormalMatrix  = glGetUniformLocation(state.program, "normalMatrix");
	state.unifCameraPos     = glGetUniformLocation(state.program, "cameraPos");
	state.unifDiffuseColor  = glGetUniformLocation(state.program, "diffuseColor");
	state.unifDiffuseTex    = glGetUniformLocation(state.program, "diffuseTex");

	ASSERT_GL_ERROR;

	return true;
}


void cleanupOpenGL()
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	eglSwapBuffers(state.display, state.surface);

	eglDestroySurface(state.display, state.surface);

	DISPMANX_UPDATE_HANDLE_T dispmanUpdate = vc_dispmanx_update_start(0);
	int s = vc_dispmanx_element_remove(dispmanUpdate, state.dispmanElement);
	assert(s == 0);
	vc_dispmanx_update_submit_sync(dispmanUpdate);
	s = vc_dispmanx_display_close(state.dispmanDisplay);
	assert(s == 0);

	// Release OpenGL resources
	eglMakeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(state.display, state.context);
	eglTerminate(state.display);
}


bool initNanoVG()
{
	state.vg = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);

	return true;
}


void cleanupNanoVG()
{
	nvgDeleteGLES2(state.vg);
}


bool loadTextures()
{
	int w, h, n;
	const char* adiBackupFilename = "backup_adi.png";

	uint8_t* img = stbi_load(adiBackupFilename, &w, &h, &n, 3);
	if (img == nullptr) {
		fprintf(stderr, "Texture load failed: %s - %s\n", adiBackupFilename, stbi_failure_reason());
		return false;
	}

	glGenTextures(1, &state.texBackupADI);
	glBindTexture(GL_TEXTURE_2D, state.texBackupADI);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,					// level
		GL_RGB,				// internal format
		w, h,				// width, height
		0,					// border
		GL_RGB,				// format
		GL_UNSIGNED_BYTE,	// type
		img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);

	stbi_image_free(img);

	return true;
}


void freeTextures()
{
	glDeleteTextures(1, &state.texBackupADI);
}


bool loadFonts(
	NVGcontext* vg)
{
	/*for(int i = 0;
		i < 12;
		++i)
	{
		char file[128];
		snprintf(file, 128, "../example/images/image%d.jpg", i+1);
		data->images[i] = nvgCreateImage(vg, file, 0);
		if (data->images[i] == 0) {
			printf("Could not load %s.\n", file);
			return -1;
		}
	}*/

	state.fontIcons = nvgCreateFont(vg, "icons", "nanovg/example/entypo.ttf");
	if (state.fontIcons == -1) {
		fprintf(stderr, "Could not add font icons.\n");
		return false;
	}
	
	state.fontNormal = nvgCreateFont(vg, "sans", "nanovg/example/Roboto-Regular.ttf");
	if (state.fontNormal == -1) {
		fprintf(stderr, "Could not add font normal.\n");
		return false;
	}
	
	state.fontBold = nvgCreateFont(vg, "sans-bold", "nanovg/example/Roboto-Bold.ttf");
	if (state.fontBold == -1) {
		fprintf(stderr, "Could not add font bold.\n");
		return false;
	}

	return true;
}


void drawLabel(
	const char* text,
	r32 x, r32 y,
	r32 w, r32 h)
{
	nvgFontSize(state.vg, 18.0f);
	nvgFontFace(state.vg, "sans");
	nvgFillColor(state.vg, nvgRGBA(255,255,255,255));

	nvgTextAlign(state.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgText(state.vg, x, y+h*0.5f, text, nullptr);
}

void drawPitchLadder()
{
	nvgBeginPath(state.vg);
	nvgMoveTo(state.vg, -0.5f, 0.5f);
	nvgLineTo(state.vg, 0.6f, 0.5f);
	nvgStrokeColor(state.vg, nvgRGBA(255,255,255,255));
	nvgStroke(state.vg);
}


struct ARU2BA
{
	void*		buffer;		// single malloc'd buffer
	
	vec3*		verts;		// pointers into buffer
	vec3*		normals;
	vec2*		texCoords;
	u16*		indexes;
	
	GLuint		glVertexBuffer;
	GLuint		glIndexBuffer;
	u32			numVerts;
	u32			numIndexes;
	uintptr_t	vertsOffset;
	uintptr_t	normalsOffset;
	uintptr_t	texCoordsOffset;

	// current state
	r32			pitch;
	r32			bank;
	r32			turn;
	
	// target state for interpolation
	r32			targetPitch;
	r32			targetBank;
	r32			targetTurn;

	mat4		orthoProjMat;
};


const vec3 cameraPos{ 0, 20, 100 };
const vec3 xAxis{ 1, 0, 0 };
const vec3 yAxis{ 0, 1, 0 };
const vec3 zAxis{ 0, 0, 1 };


ARU2BA makeARU2BA()
{
	ARU2BA scene{};
	
	scene.pitch = scene.targetPitch = PIf; // start pitch and bank level
	scene.bank  = scene.targetBank  = PIf;

	r32 radius = 1.75f; // dimension in inches
	
	u32 numCols = 12;
	u32 numColVerts = numCols + 1;
	r32 radsPerColumn = 90.0f * DEG_TO_RADf / numCols;
	r32 startColumnRads = -45.0f * DEG_TO_RADf;

	u32 numRows = 48;
	u32 numRowVerts = numRows + 1;
	r32 radsPerRow = 360.0f * DEG_TO_RADf / numRows;

	scene.numVerts = numColVerts * numRowVerts;
	u32 vertexSize = sizeof(vec3) + sizeof(vec3) + sizeof(vec2); // position + normal + texCoord
	size_t vBufferSize = vertexSize * scene.numVerts;
	
	u32 indexesPerCol = (numRowVerts * 2) + 2;		// number of triangles in strip + first 2 points (numRowVerts*2) + 2 points for degenerate
	scene.numIndexes = indexesPerCol * numCols - 1;	// take one away for no degenerate on first column
	size_t iBufferSize = sizeof(u16) * scene.numIndexes;

	scene.buffer = malloc(vBufferSize + iBufferSize);
	scene.vertsOffset = 0;
	scene.normalsOffset = scene.vertsOffset + (sizeof(vec3) * scene.numVerts);
	scene.texCoordsOffset = scene.normalsOffset + (sizeof(vec3) * scene.numVerts);
	scene.verts     = (vec3*)((uintptr_t)scene.buffer + scene.vertsOffset);
	scene.normals   = (vec3*)((uintptr_t)scene.buffer + scene.normalsOffset);
	scene.texCoords = (vec2*)((uintptr_t)scene.buffer + scene.texCoordsOffset);
	scene.indexes   = (u16*)((uintptr_t)scene.buffer + scene.texCoordsOffset + (sizeof(vec2) * scene.numVerts));

	// create vertices
	u32 v = 0;
	r32 colRad = startColumnRads;

	for(u32 c = 0;
		c < numColVerts;
		++c)
	{
		r32 colX = sinf(colRad);
		r32 colUnitRadius = cosf(colRad);
		r32 rowRad = 0;

		for(u32 r = 0;
			r < numRowVerts;
			++r)
		{
			vec3& vert = scene.verts[v];
			vec3& norm = scene.normals[v];
			vec2& tex  = scene.texCoords[v];

			r32 rowY = (r != numRows
				? sinf(rowRad)	// for all rows but the last
				: sinf(0));		// ensure last row exactly equals first
			r32 rowZ = (r != numRows
				? cosf(rowRad)
				: cosf(0));
			
			vert.x = colX;
			vert.y = rowY * colUnitRadius;
			vert.z = rowZ * colUnitRadius;
			vert = normalize(vert);
			norm = vert;
			vert *= radius;
			tex.s = (r32)r / (r32)numRows;
			tex.t = (r32)c / (r32)numCols;

			rowRad += radsPerRow;
			++v;
		}

		colRad += radsPerColumn;
	}
	assert(v == scene.numVerts);

	// create indexes for triangle strips
	u32 i = 0;

	for(u32 c = 0;
		c < numCols;
		++c)
	{
		u32 colBaseV = c * numRowVerts;

		// add point for degenerate triangle from end of row to start of next row
		if (c > 0) {
			scene.indexes[i++] = colBaseV;
		}

		for(u32 r = 0;
			r < numRowVerts;
			++r)
		{
			scene.indexes[i++] = colBaseV + r;
			scene.indexes[i++] = colBaseV + r + numRowVerts;
		}
		// add point for degenerate triangle from end of row to start of next row
		scene.indexes[i] = scene.indexes[i-1];
		++i;
	}
	assert(i == scene.numIndexes);

	// create vertex buffer
	glGenBuffers(1, &scene.glVertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, scene.glVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, vBufferSize, scene.buffer, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// create index buffer
	glGenBuffers(1, &scene.glIndexBuffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene.glIndexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, iBufferSize, scene.indexes, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	// set up projection matrices
	/**
	 * ortho extents are taken from actual screen dimensions in inches
	 *  3.36"
	 * -------
	 * |     |
	 * |     | 4.48"
	 * |     |
	 * -------
	 * with origin at center, each coordinate is half of its dimension
	 */
	scene.orthoProjMat = orthoRH(
		-1.68f,		// left
		 1.68f,		// right
		-2.24f,		// bottom
		 2.24f,		// top
		 0,			// near
		 200.0f);	// far

	return scene;
}


void freeARU2BA(
	ARU2BA& scene)
{
	glDeleteBuffers(1, &scene.glVertexBuffer);
	glDeleteBuffers(1, &scene.glIndexBuffer);
	free(scene.buffer);
}


void drawARU2BA(
	ARU2BA& scene)
{
	glEnable(GL_CULL_FACE);
	
	glUseProgram(state.program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, state.texBackupADI);//scene.glBallTex);
	glUniform1i(state.unifDiffuseTex, 0);

	// roll
	mat4 modelToWorld = rotate(
		mat4{},			// identity
		scene.bank,
		zAxis);
	// pitch
	modelToWorld = rotate(
		modelToWorld,
		scene.pitch,
		xAxis);

	mat4 viewMat = lookAtRH(
		cameraPos,		// eye
		vec3{0, 0, 0},	// target
		yAxis);			// up
	
	mat4 modelView(viewMat * modelToWorld);
	mat4 mvp(scene.orthoProjMat * modelView);
	mat4 normalMat = make_mat4(transpose(inverse(make_mat3(modelView))));

	glUniformMatrix4fv(
		state.unifModelView,
		1, GL_FALSE,
		modelView.E);

	glUniformMatrix4fv(
		state.unifModelViewProj,
		1, GL_FALSE,
		mvp.E);

	glUniformMatrix4fv(
		state.unifNormalMatrix,
		1, GL_FALSE,
		normalMat.E);
	
	glUniform3fv(state.unifCameraPos, 1, cameraPos.E);


	glBindBuffer(GL_ARRAY_BUFFER, scene.glVertexBuffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene.glIndexBuffer);

	glEnableVertexAttribArray(state.attrVertexPosition);
	glEnableVertexAttribArray(state.attrVertexNormal);
	glEnableVertexAttribArray(state.attrVertexUV);
	
	// vertex position
	glVertexAttribPointer(
		state.attrVertexPosition,
		3,			// size
		GL_FLOAT,	// type
		GL_FALSE,	// normalized, fixed point?
		0,			// stride, 0 = tightly packed
		(const GLvoid*)scene.vertsOffset); // offset of position data in buffer

	// normal
	glVertexAttribPointer(
		state.attrVertexNormal,
		3, GL_FLOAT, GL_FALSE, 0,
		(const GLvoid*)scene.normalsOffset);
	
	// texture coords
	glVertexAttribPointer(
		state.attrVertexUV,
		2, GL_FLOAT, GL_FALSE, 0,
		(const GLvoid*)scene.texCoordsOffset);

	glDrawElements(
		GL_TRIANGLE_STRIP,		// mode
		scene.numIndexes,		// element count
		GL_UNSIGNED_SHORT,		// type
		(const GLvoid*)0);

	glDisableVertexAttribArray(state.attrVertexPosition);
	glDisableVertexAttribArray(state.attrVertexNormal);
	glDisableVertexAttribArray(state.attrVertexUV);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	ASSERT_GL_ERROR;
}


/**
 * interpolate val to target, smooths out "choppiness" from network updates
 */
void interpolateValue(
	r32& val,
	const r32& target,
	r32 easingFactor = 0.25f,
	bool wrap = true,
	r32 wrapVal = PIf * 2.0f,
	r32 equalEpsilon = 0.0001f)
{
	r32 delta = target - val;
	
	if (wrap) {
		// |-----------V=>----T---|W
		// |--<=V-------------T---|W
		if (target > val) {
			r32 deltaWrap = wrapVal - target + val;
			if (deltaWrap < delta) {
				delta = -deltaWrap;
			}
		}
		// |-----------T----<=V---|W
		// |----T-------------V=>-|W
		else if (target < val) {
			r32 deltaWrap = wrapVal - val + target;
			if (deltaWrap < -delta) {
				delta = deltaWrap;
			}
		}
	}

	if (fabs(delta) < equalEpsilon) {
		val = target;
	}
	else {
		// ease val toward target
		val += delta * easingFactor;
	}
}


void updateARU2BA(
	ARU2BA& scene)
{
	if (mqttState.pitch_nsec > 0) {
		scene.targetPitch = (r32)mqttState.rawPitch / 65535.0f * PIf + (PIf * 0.5f);
	}
	else {
		scene.targetPitch = PIf;
	}

	if (mqttState.bank_nsec > 0) {
		scene.targetBank = (r32)mqttState.rawBank / 65535.0f * PIf * 2.0f;
	}
	else {
		scene.targetBank = PIf;
	}

	interpolateValue(scene.pitch, scene.targetPitch);
	interpolateValue(scene.bank,  scene.targetBank);
}


void drawFPS()
{
	char fpsText[16] = {};
	snprintf(fpsText, 16, "FPS: %.2f", timer.fps);

	drawLabel(
		fpsText,
		10.0f, 10.0f,
		0, 18.0f);
}


void drawScene(
	ARU2BA& scene)
{
	// render to the main frame buffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
	
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// Draw first (front) face:
	// Bind texture surface to current vertices
/*	glBindTexture(GL_TEXTURE_2D, state.tex[0]);
*/

	//glEnable(GL_BLEND);
	//glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	//glEnable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);

	nvgBeginFrame(state.vg,
		state.screenWidth,
		state.screenHeight,
		1.0f); // pixel ratio

	drawPitchLadder();

	drawFPS();

	nvgEndFrame(state.vg);

	glEnable(GL_DEPTH_TEST);

	drawARU2BA(scene);

	glFlush();
	glFinish();

	eglSwapBuffers(state.display, state.surface);
	ASSERT_GL_ERROR;
}


void handleSignal(int s)
{
	printf("Exiting...\n");
	running = false;
}


void updateTime(
	u32 frame)
{
	// get current time
	timer.prev_nsec = timer.now_nsec;
	clock_gettime(CLOCK_MONOTONIC, &timer.tsNow);
	timer.now_nsec = timer.tsNow.tv_sec * 1000000000 + timer.tsNow.tv_nsec;
	// calc delta time
	timer.dt_ms = (r32)(timer.now_nsec - timer.prev_nsec) * 0.000001f;
	
	// calc fps
	if (frame > 0 && (frame % 60) == 0) {
		timer.fps = 60.0f / ((r32)(timer.now_nsec - timer.fpsPrev_nsec) * 0.000000001f);
		timer.fpsPrev_nsec = timer.now_nsec;
	}
}


int main()
{
	signal(SIGINT, handleSignal);
	signal(SIGTERM, handleSignal);

	bcm_host_init();

	if (get_processor_id() == PROCESSOR_BCM2838)
	{
		fprintf(stderr, "This application is not available on the Pi4\n");
		exit(0);
	}

	if (initMQTT(&mqttState)
		&& initOpenGL()
		&& initShaders()
		&& initNanoVG()
		&& loadTextures()
		&& loadFonts(state.vg))
	{
		ARU2BA scene = makeARU2BA();
		
		u32 frame = 0;
		updateTime(frame);

		while (running)
		{
			updateTime(frame);
			updateARU2BA(scene);
			drawScene(scene);
			++frame;
		}
		freeARU2BA(scene);
	}

	freeTextures();
	cleanupNanoVG();
	cleanupOpenGL();
	cleanupMQTT();

	return 0;
}


#include "mqtt.cpp"
#include "nanovg/src/nanovg.c"
