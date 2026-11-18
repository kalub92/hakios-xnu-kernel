/*
 * pvr_gl_test: OpenGL ES by the rabbit r1's PowerVR, as the window server would have it -- Mesa's Zink
 * (OpenGL ES drawn with Vulkan) over Mesa's PowerVR driver, through src/gl_egl.c, the same file that
 * is behind src/eagl.c. The sequel of src/pvr_vk_test.c, which tried the Vulkan driver alone.
 *
 * In steps, each said before and after, so that the last line of a log is where it stopped:
 *   a  a context of OpenGL ES 2 (EGL with no window system; Zink finds the Vulkan driver, that the GPU)
 *      and what it says it is
 *   b  a texture of 64 x 64 as a framebuffer, cleared, read back with glReadPixels
 *   c  a triangle with a colour at each corner (shaders in GLSL ES 1.00, compiled here by Mesa)
 *   d  a texture of 2 x 2 colours drawn over the whole picture, its pixels taken as they are and smoothed
 *   e  the triangle into an IOSurface that is the texture's own memory (redwood_gl_surface_texture():
 *      the kernel makes a buffer of the surface, request 0x7e), and the surface's memory read here
 *   f  thirty frames of a turning triangle at the panel's size, 480 x 640, each waited for (glFinish):
 *      how long a frame takes, and how long reading one back
 *   g  the window server's way with a picture for the display, at its size: an IOSurface of 1125 x 2436
 *      'BGRA'; a texture filled from it and drawn into (a clear and the triangle); a second texture
 *      that is the surface's memory, into which the GPU copies the first, turned over (src/eagl.c,
 *      -attachImage:toCoreSurface:invertedRender: and target_to_surface()); the surface's memory read
 *      here; ten such frames timed. In a run of its own too (pvr_gl_test display), with OpenGL ES 3.
 * A call that does not come back within sixty seconds ends the program (HUNG in ...).
 * Step e is made by a second run of this program (pvr_gl_test surface), last: what goes wrong in it --
 * Zink ends the program when Vulkan lacks something -- is then its alone, and is said here.
 *
 * Every line goes to the console. The kernel is given lines to keep (kern.redwood.say, "!" first), and
 * keeps the last hundred or so of everybody's: so while it runs only one for each step as it begins,
 * for telling where a run stopped, and at the very end what there is to say of every step, some
 * twenty lines, which are then the last in the log.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "redwood_gl.h"

extern int redwood_gl_egl_error, redwood_gl_surface_stage, redwood_gl_surface_error;      /* src/gl_egl.c */

/* GLES2/gl2.h of Khronos, as far as it is used here */
extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
extern void glDeleteTextures(int n, const unsigned *names);
extern void glBindTexture(unsigned target, unsigned texture);
extern void glTexImage2D(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
    unsigned type, const void *pixels);
extern void glTexParameteri(unsigned target, unsigned name, int value);
extern void glGenFramebuffers(int n, unsigned *names);
extern void glBindFramebuffer(unsigned target, unsigned framebuffer);
extern void glFramebufferTexture2D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level);
extern unsigned glCheckFramebufferStatus(unsigned target);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float red, float green, float blue, float alpha);
extern void glClear(unsigned mask);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern void glGetShaderInfoLog(unsigned shader, int size, int *length, char *log);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glBindAttribLocation(unsigned program, unsigned index, const char *name);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned name, int *value);
extern void glGetProgramInfoLog(unsigned program, int size, int *length, char *log);
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDisableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern int glGetUniformLocation(unsigned program, const char *name);
extern void glUniform1i(int location, int value);
extern void glUniform1f(int location, float value);
extern void glActiveTexture(unsigned unit);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);
extern void glPixelStorei(unsigned name, int value);
extern void glBlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter);

#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_NO_ERROR                     0
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_EXTENSIONS                   0x1f03
#define GL_NEAREST                      0x2600
#define GL_LINEAR                       0x2601
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_CLAMP_TO_EDGE                0x812f
#define GL_TEXTURE0                     0x84c0
#define GL_BGRA                         0x80e1
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_FRAMEBUFFER                  0x8d40
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_READ_FRAMEBUFFER             0x8ca8
#define GL_DRAW_FRAMEBUFFER             0x8ca9
#define GL_UNPACK_ROW_LENGTH            0x0cf2
#define GL_UNPACK_ALIGNMENT             0x0cf5

#define DISPLAY_WIDE    1125                    /* what CoreAnimation draws for the display, whatever the panel has */
#define DISPLAY_HIGH    2436

#define SIDE            64
#define HUNG_SECONDS    60
#define PANEL_WIDE      480
#define PANEL_HIGH      640
#define FRAMES          30

static void
kept(const char *line)
{
	char with[216];

	snprintf(with, sizeof(with), "!pvr_gl_test: %s", line);
	sysctlbyname("kern.redwood.say", NULL, NULL, with, strlen(with));
}

/* What is to be said of a step: to the console now, and to the kernel at the end (told()). */
static char said[40][200];
static unsigned said_count;

static void
say(const char *format, ...)
{
	char overflow[200];
	char *line = said_count < 40 ? said[said_count++] : overflow;
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, 200, format, ap);
	va_end(ap);
	printf("pvr_gl_test: %s\n", line);
	fflush(stdout);
	kept(line);             /* (now, should the next step be the program's last; and again at the end) */
}

static void
told(void)
{
	for (unsigned i = 0; i < said_count; i++) {
		kept(said[i]);
	}
	said_count = 0;
}

/* ... and what only the console is told */
static void
note(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	printf("pvr_gl_test: ");
	vprintf(format, ap);
	printf("\n");
	va_end(ap);
	fflush(stdout);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

/* The step being made, for whoever watches that it ends. */
static const char *volatile step_name;
static volatile uint64_t step_began;

static void *
watch(void *unused)
{
	(void)unused;
	for (;;) {
		const char *name = step_name;

		sleep(1);
		if (name != NULL && name == step_name && now_us() - step_began > HUNG_SECONDS * 1000000ull) {
			say("HUNG in \"%s\": no answer in %d s, giving up", name, HUNG_SECONDS);
			told();
			_exit(2);
		}
	}
	return NULL;
}

static void
begin(const char *what)
{
	char line[200];

	snprintf(line, sizeof(line), "%s ...", what);
	printf("pvr_gl_test: %s\n", line);
	fflush(stdout);
	kept(line);
	step_began = now_us();
	step_name = what;
}

/* ... and its end: OpenGL ES's error since the step began, and how long it took. True without an error. */
static int
end(const char *what)
{
	unsigned error = glGetError();

	step_name = NULL;
	(error == GL_NO_ERROR ? note : say)("%s: %s%#x (%llu ms)", what, error == GL_NO_ERROR ? "done, error " : "GL ERROR ", error,
	    (unsigned long long)((now_us() - step_began) / 1000));
	return error == GL_NO_ERROR;
}

static unsigned
shader(unsigned type, const char *source)
{
	unsigned s = glCreateShader(type);
	int good = 0;
	char log[150] = "";

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		say("a %s shader does not compile: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		return 0;
	}
	return s;
}

/* A program of two shaders, its attributes `position` at 0 and `second` at 1; 0 when it cannot be made. */
static unsigned
program(const char *vertex, const char *fragment, const char *second)
{
	unsigned v = shader(GL_VERTEX_SHADER, vertex), f = shader(GL_FRAGMENT_SHADER, fragment), p;
	int good = 0;
	char log[150] = "";

	if (v == 0 || f == 0) {
		return 0;
	}
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "position");
	glBindAttribLocation(p, 1, second);
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &good);
	if (!good) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		say("the program does not link: %s", log);
		return 0;
	}
	return p;
}

/* A texture of this size (with these pixels, or none yet) and a framebuffer that draws into it. */
static unsigned
target(int width, int height, unsigned *texture)
{
	unsigned framebuffer = 0;

	glGenTextures(1, texture);
	glBindTexture(GL_TEXTURE_2D, *texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (width != 0) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	glGenFramebuffers(1, &framebuffer);
	return framebuffer;
}

static int
target_bind(unsigned framebuffer, unsigned texture, int width, int height)
{
	unsigned status;

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		say("the framebuffer is not complete: %#x", status);
		return 0;
	}
	glViewport(0, 0, width, height);
	return 1;
}

static const char triangle_vertex[] =
	"attribute vec2 position;\n"
	"attribute vec3 colour;\n"
	"uniform float turn;\n"
	"varying vec3 shade;\n"
	"void main() {\n"
	"	float c = cos(turn), s = sin(turn);\n"
	"	gl_Position = vec4(position.x * c - position.y * s, position.x * s + position.y * c, 0.0, 1.0);\n"
	"	shade = colour;\n"
	"}\n";
static const char triangle_fragment[] =
	"precision mediump float;\n"
	"varying vec3 shade;\n"
	"void main() { gl_FragColor = vec4(shade, 1.0); }\n";
static const char quad_vertex[] =
	"attribute vec2 position;\n"
	"attribute vec2 where;\n"
	"varying vec2 at;\n"
	"void main() { gl_Position = vec4(position, 0.0, 1.0); at = where; }\n";
static const char quad_fragment[] =
	"precision mediump float;\n"
	"uniform sampler2D picture;\n"
	"varying vec2 at;\n"
	"void main() { gl_FragColor = texture2D(picture, at); }\n";

/* red at the top in the middle, green at the bottom right, blue at the bottom left (OpenGL's y is up) */
static const float corners[3][5] = {
	{ 0.0f, 0.8f, 1, 0, 0 },
	{ 0.8f, -0.8f, 0, 1, 0 },
	{ -0.8f, -0.8f, 0, 0, 1 },
};

static void
triangle_draw(unsigned triangle, float turn)
{
	glUseProgram(triangle);
	glUniform1f(glGetUniformLocation(triangle, "turn"), turn);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(corners[0]), corners[0]);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, sizeof(corners[0]), corners[0] + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

/*
 * A picture of 64 x 64 said in two lines of 16 x 16 letters, each the middle of a square of 4 x 4 pixels:
 * . black, R G B the strongest of red, green and blue, ? neither drawn nor cleared (its alpha is not 255).
 * `pixels` are rows of `row` bytes, `red` the byte of a pixel that is red (0: RGBA; 2: an IOSurface's
 * BGRA), `top` the row that is the top (63: OpenGL's rows, the bottom one first; 0: the top one first).
 * Also said: how many pixels are drawn (the triangle is 1311), and three of them.
 */
static int
triangle_look(const char *what, const uint8_t *pixels, size_t row, unsigned red, int top)
{
	unsigned drawn = 0;
	const uint8_t *apex, *right, *left;
	int good;

#define PIXEL(x, y) (pixels + (size_t)(top == 0 ? (y) : SIDE - 1 - (y)) * row + (x) * 4)
	for (unsigned half = 0; half < 2; half++) {
		char rows[8 * 17 + 1];

		for (unsigned r = 0; r < 8; r++) {
			for (unsigned column = 0; column < 16; column++) {
				const uint8_t *p = PIXEL(column * 4 + 2, (half * 8 + r) * 4 + 2);
				unsigned rd = p[red], gr = p[1], bl = p[2 - red];

				rows[r * 17 + column] = p[3] != 0xff ? '?' : rd + gr + bl < 48 ? '.' : rd >= gr && rd >= bl ? 'R' :
				    gr >= bl ? 'G' : 'B';
			}
			rows[r * 17 + 16] = r == 7 ? '\0' : '/';
		}
		say("%s: rows %2u-%2u: %s", what, half * 8, half * 8 + 7, rows);
	}
	for (unsigned i = 0; i < SIDE * SIDE; i++) {
		const uint8_t *p = PIXEL(i % SIDE, i / SIDE);

		drawn += p[3] == 0xff && p[0] + p[1] + p[2] >= 48;
	}
	apex = PIXEL(32, 12);
	right = PIXEL(52, 55);
	left = PIXEL(12, 55);
#undef PIXEL
	good = drawn > 1200 && drawn < 1420 && apex[red] > 150 && right[1] > 150 && left[2 - red] > 150;
	say("%s: %s: %u pixels drawn (about 1311 expected); bytes near the top %02x%02x%02x%02x, the bottom right "
	    "%02x%02x%02x%02x, the bottom left %02x%02x%02x%02x", what, good ? "RIGHT" : "WRONG", drawn, apex[0], apex[1],
	    apex[2], apex[3], right[0], right[1], right[2], right[3], left[0], left[1], left[2], left[3]);
	return good;
}

/* A surface of this size, 'BGRA', its memory filled with a5. */
static IOSurfaceRef
surface_make(int width, int height)
{
	const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
	int numbers[] = { width, height, 4, 'BGRA' };
	const void *values[4];
	IOSurfaceRef surface;

	for (unsigned i = 0; i < 4; i++) {
		values[i] = CFNumberCreate(NULL, kCFNumberIntType, &numbers[i]);
	}
	surface = IOSurfaceCreate(CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks));
	if (surface != NULL) {
		IOSurfaceLock(surface, 0, NULL);
		memset(IOSurfaceGetBaseAddress(surface), 0xa5, IOSurfaceGetBytesPerRow(surface) * (size_t)height);
		IOSurfaceUnlock(surface, 0, NULL);
	}
	return surface;
}

/* g: see the top. 0: the picture is in the surface and right. */
static int
display_test(struct redwood_gl *gl, unsigned triangle)
{
	IOSurfaceRef surface = surface_make(DISPLAY_WIDE, DISPLAY_HIGH);
	unsigned texture = 0, mirror = 0, drawn_into = 0, reader = 0, writer = 0, status, error;
	uint64_t began, total = 0, least = ~0ull, most = 0;
	size_t row, untouched = 0, cleared = 0, other = 0;
	const uint8_t *base, *corner, *middle;
	int mirrored;

	begin("g: the display's picture, 1125 x 2436, the window server's way");
	if (surface == NULL) {
		step_name = NULL;
		say("g: DISPLAY: NOT DONE: no IOSurface of 1125 x 2436");
		return 3;
	}
	row = IOSurfaceGetBytesPerRow(surface);
	/* the texture CoreAnimation draws into: filled from the surface as it is (eagl.c, surface_to_texture()) */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (int)(row / 4));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, DISPLAY_WIDE, DISPLAY_HIGH, 0, GL_BGRA, GL_UNSIGNED_BYTE, IOSurfaceGetBaseAddress(surface));
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	error = glGetError();
	glGenFramebuffers(1, &drawn_into);
	glBindFramebuffer(GL_FRAMEBUFFER, drawn_into);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	say("g: the texture of 1125 x 2436 BGRA (%zu bytes a row in the surface): filled, error %#x; as a framebuffer %#x (8cd5: "
	    "complete)", row, error, status);
	/* the second texture, the surface's memory (surface_mirrored()) */
	glGenTextures(1, &mirror);
	glBindTexture(GL_TEXTURE_2D, mirror);
	mirrored = redwood_gl_surface_texture(gl, IOSurfaceGetID(surface));
	glGenFramebuffers(1, &reader);
	glGenFramebuffers(1, &writer);
	if (mirrored) {
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, writer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mirror, 0);
		say("g: the surface is a second texture's memory; as a framebuffer %#x", glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
	} else {
		say("g: the surface is NOT taken for a texture's memory: stage %d, error %#x (stages: src/gl_egl.c); the window "
		    "server would read every frame back", redwood_gl_surface_stage, redwood_gl_surface_error);
	}
	for (unsigned frame = 0; frame < 10 && status == GL_FRAMEBUFFER_COMPLETE && mirrored; frame++) {
		uint64_t took;

		began = now_us();
		glBindFramebuffer(GL_FRAMEBUFFER, drawn_into);
		glViewport(0, 0, DISPLAY_WIDE, DISPLAY_HIGH);
		triangle_draw(triangle, 0);             /* (clears to black first) */
		/* target_to_surface(): the frame into the surface's memory by the GPU, upside down as the display wants it */
		glBindFramebuffer(GL_READ_FRAMEBUFFER, reader);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, writer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mirror, 0);
		glBlitFramebuffer(0, 0, DISPLAY_WIDE, DISPLAY_HIGH, 0, DISPLAY_HIGH, DISPLAY_WIDE, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glFinish();
		took = now_us() - began;
		total += took;
		least = took < least ? took : least;
		most = took > most ? took : most;
		step_began = now_us();
	}
	error = glGetError();
	/* what is in the surface: black (000000ff) around a triangle whose top, red, is at the bottom (turned over) */
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	base = IOSurfaceGetBaseAddress(surface);
	for (unsigned y = 0; y < DISPLAY_HIGH; y += 12) {
		for (unsigned x = 0; x < DISPLAY_WIDE; x += 5) {
			uint32_t word;

			memcpy(&word, base + row * y + x * 4, 4);
			untouched += word == 0xa5a5a5a5u;
			cleared += word == 0xff000000u;
			other += word != 0xa5a5a5a5u && word != 0xff000000u;
		}
	}
	corner = base;
	middle = base + row * (DISPLAY_HIGH / 2) + DISPLAY_WIDE / 2 * 4;
	say("g: the surface's memory, a pixel in 60: %zu never drawn (a5), %zu black, %zu of the triangle (about 31000 and 14600 "
	    "expected); its first pixel %02x%02x%02x%02x, its middle %02x%02x%02x%02x (blue, green, red, alpha); error %#x",
	    untouched, cleared, other, corner[0], corner[1], corner[2], corner[3], middle[0], middle[1], middle[2], middle[3], error);
	mirrored = mirrored && untouched == 0 && cleared > 10000 && other > 5000 && middle[3] == 0xff && middle[0] + middle[1] + middle[2] > 48;
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	end("g: display");
	say("g: DISPLAY: %s; a frame (drawn, copied to the surface by the GPU, waited for) %llu.%llu ms, the shortest %llu.%llu, "
	    "the longest %llu.%llu", mirrored ? "RIGHT" : "WRONG", (unsigned long long)(total / 10 / 1000),
	    (unsigned long long)(total / 10 % 1000 / 100), (unsigned long long)(least == ~0ull ? 0 : least / 1000),
	    (unsigned long long)(least == ~0ull ? 0 : least % 1000 / 100), (unsigned long long)(most / 1000),
	    (unsigned long long)(most % 1000 / 100));
	return mirrored ? 0 : 3;
}

extern char **environ;

int
main(int argc, char **argv)
{
	int only_display = argc > 1 && strcmp(argv[1], "display") == 0;
	int only_surface = only_display || (argc > 1 && strcmp(argv[1], "surface") == 0);
	static uint8_t pixels[SIDE * SIDE * 4];
	static uint8_t panel[PANEL_WIDE * PANEL_HIGH * 4];
	static const uint8_t four[2 * 2 * 4] = {        /* the bottom row first: red, green; then blue, white */
		255, 0, 0, 255, 0, 255, 0, 255,
		0, 0, 255, 255, 255, 255, 255, 255,
	};
	static const float quad[4][4] = {       /* x, y; where in the texture */
		{ -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { -1, 1, 0, 1 }, { 1, 1, 1, 1 },
	};
	struct redwood_gl *gl;
	unsigned framebuffer, texture, triangle = 0, quad_program = 0, small = 0;
	int cleared = 0, drawn = 0, textured = 0, shared = -1, timed = 0, displayed = 0;
	const char *extensions;
	unsigned count = 0, wrong = 0;
	pthread_t watcher;

	pthread_create(&watcher, NULL, watch, NULL);

	begin("a: a context of OpenGL ES 2 (EGL, Zink, the Vulkan driver, the kernel's RedwoodPVR)");
	gl = redwood_gl_create(only_display ? 3 : 2);
	if (gl == NULL || !redwood_gl_make_current(gl)) {
		step_name = NULL;
		say("a: NOT DONE: %s (EGL's error %#x; -1: no display)", gl == NULL ? "no context" : "the context cannot be made current",
		    redwood_gl_egl_error);
		told();
		return 1;
	}
	end("a: a context");
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	for (const char *e = extensions; e != NULL && *e != 0; e++) {
		count += *e == ' ';
	}
	(only_surface ? note : say)("a: %s; %s", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
	(only_surface ? note : say)("a: %s; %s; %u extensions, GL_OES_EGL_image %s", (const char *)glGetString(GL_VERSION),
	    (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION), count,
	    extensions != NULL && strstr(extensions, "GL_OES_EGL_image") != NULL ? "among them" : "NOT among them");

	if (!only_surface) {
	begin("b: a picture of 64 x 64 cleared to red 0.25, green 0.5, blue 0.75 and read back");
	}
	framebuffer = target(SIDE, SIDE, &texture);
	if (only_surface) {
	} else if (target_bind(framebuffer, texture, SIDE, SIDE)) {
		glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		memset(pixels, 0x5a, sizeof(pixels));
		glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		for (unsigned i = 0; i < SIDE * SIDE; i++) {
			const uint8_t *p = pixels + i * 4;

			wrong += p[0] < 0x3f || p[0] > 0x41 || p[1] < 0x7f || p[1] > 0x81 || p[2] < 0xbe || p[2] > 0xc0 || p[3] != 0xff;
		}
		cleared = end("b: clear") && wrong == 0;
		say("b: CLEAR: %s: %u of %u pixels wrong; the first %02x%02x%02x%02x, the last %02x%02x%02x%02x (expected 4080bfff; "
		    "5a5a5a5a: nothing was read)", cleared ? "RIGHT" : "WRONG", wrong, SIDE * SIDE, pixels[0], pixels[1], pixels[2],
		    pixels[3], pixels[sizeof(pixels) - 4], pixels[sizeof(pixels) - 3], pixels[sizeof(pixels) - 2],
		    pixels[sizeof(pixels) - 1]);
	} else {
		end("b: clear");
	}

	if (!only_surface) {
	begin("c: a triangle; its two shaders compiled (GLSL ES to SPIR-V by Zink, that to the GPU's code by the driver)");
	}
	triangle = program(triangle_vertex, triangle_fragment, "colour");
	if (only_surface) {
	} else if (triangle != 0 && target_bind(framebuffer, texture, SIDE, SIDE)) {
		triangle_draw(triangle, 0);
		memset(pixels, 0x5a, sizeof(pixels));
		glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		if (end("c: triangle")) {
			drawn = triangle_look("c", pixels, SIDE * 4, 0, SIDE - 1);
		}
	} else {
		end("c: triangle");
	}

	if (!only_surface) {
	begin("d: a texture of 2 x 2 (red, green / blue, white) over the whole picture");
	quad_program = program(quad_vertex, quad_fragment, "where");
	}
	if (only_surface) {
	} else if (quad_program != 0 && target_bind(framebuffer, texture, SIDE, SIDE)) {
		const uint8_t *corner[4], *middle;
		uint8_t smooth[4];

		glGenTextures(1, &small);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, small);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, four);
		glUseProgram(quad_program);
		glUniform1i(glGetUniformLocation(quad_program, "picture"), 0);
		glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(quad[0]), quad[0]);
		glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(quad[0]), quad[0] + 2);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		memset(pixels, 0x5a, sizeof(pixels));
		glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		/* the same smoothed: the middle of the picture is the middle of the four colours */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		memset(smooth, 0x5a, sizeof(smooth));
		glReadPixels(SIDE / 2, SIDE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, smooth);
		corner[0] = pixels + (8 * SIDE + 8) * 4;
		corner[1] = pixels + (8 * SIDE + 56) * 4;
		corner[2] = pixels + (56 * SIDE + 8) * 4;
		corner[3] = pixels + (56 * SIDE + 56) * 4;
		middle = smooth;
		textured = end("d: texture") && memcmp(corner[0], four, 4) == 0 && memcmp(corner[1], four + 4, 4) == 0 &&
		           memcmp(corner[2], four + 8, 4) == 0 && memcmp(corner[3], four + 12, 4) == 0 && middle[0] > 0x60 &&
		           middle[0] < 0xa0 && middle[1] > 0x60 && middle[1] < 0xa0 && middle[2] > 0x60 && middle[2] < 0xa0;
		say("d: TEXTURE: %s: the quarters %02x%02x%02x %02x%02x%02x / %02x%02x%02x %02x%02x%02x (expected ff0000 00ff00 / "
		    "0000ff ffffff); smoothed, the middle %02x%02x%02x (expected about 808080)", textured ? "RIGHT" : "WRONG",
		    corner[0][0], corner[0][1], corner[0][2], corner[1][0], corner[1][1], corner[1][2], corner[2][0], corner[2][1],
		    corner[2][2], corner[3][0], corner[3][1], corner[3][2], middle[0], middle[1], middle[2]);
		glDisableVertexAttribArray(1);
	} else {
		end("d: texture");
	}

	if (only_display) {
		_exit(triangle != 0 ? display_test(gl, triangle) : 1);
	}
	if (triangle != 0 && only_surface) {
		const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
		int numbers[] = { SIDE, SIDE, 4, 'BGRA' };
		const void *values[4];
		CFDictionaryRef properties;
		IOSurfaceRef surface;
		unsigned surface_texture = 0, surface_framebuffer;

		begin("e: the triangle into an IOSurface that is the texture's own memory");
		for (unsigned i = 0; i < 4; i++) {
			values[i] = CFNumberCreate(NULL, kCFNumberIntType, &numbers[i]);
		}
		properties = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		surface = IOSurfaceCreate(properties);
		if (surface == NULL) {
			step_name = NULL;
			say("e: NOT DONE: no IOSurface");
		} else {
			size_t row = IOSurfaceGetBytesPerRow(surface);

			IOSurfaceLock(surface, 0, NULL);
			memset(IOSurfaceGetBaseAddress(surface), 0xa5, row * SIDE);     /* (what is read where the GPU drew nothing) */
			IOSurfaceUnlock(surface, 0, NULL);
			surface_framebuffer = target(0, 0, &surface_texture);
			shared = redwood_gl_surface_texture(gl, IOSurfaceGetID(surface));
			if (!shared) {
				static const char *const stages[8] = {
					"", "told not to (pvr_gl_import=0)", "EGL or OpenGL ES has not the functions",
					"EGL does not offer EGL_EXT_image_dma_buf_import_modifiers", "IOSurfaceLookup finds no such surface",
					"the surface is not one plane of BGRA", "eglCreateImageKHR refused, EGL's error",
					"glEGLImageTargetTexture2DOES refused, OpenGL ES's error",
				};

				end("e: IOSurface");
				say("e: IOSURFACE: NOT SHARED: surface %u (64 x 64 BGRA, %zu bytes a row): stage %d: %s %#x (a request "
				    "0x7e that failed is a line of rwpvr's)", IOSurfaceGetID(surface), row, redwood_gl_surface_stage,
				    stages[redwood_gl_surface_stage & 7], redwood_gl_surface_error);
			} else if (target_bind(surface_framebuffer, surface_texture, SIDE, SIDE)) {
				triangle_draw(triangle, 0);
				glFinish();
				say("e: the surface is the texture's memory and the framebuffer is complete; drawn, error %#x", glGetError());
				if (end("e: IOSurface")) {
					IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
					shared = triangle_look("e (the surface's memory)", IOSurfaceGetBaseAddress(surface), row, 2, SIDE - 1) ? 1 : 0;
					IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
					say("e: IOSURFACE: %s (read as OpenGL keeps a texture, the bottom row first; ? is a5, memory the GPU "
					    "never drew into)", shared ? "SHARED AND RIGHT" : "shared, but what is in it is not the triangle");
				} else {
					shared = 0;
				}
			} else {
				say("e: the surface is the texture's memory, but no framebuffer can be made of that texture (above)");
				end("e: IOSurface");
				shared = 0;
			}
		}
	}

	if (only_surface) {
		/* (this run was step e alone, and its lines are with the kernel already: 0 shared and right, 3 not) */
		_exit(shared == 1 ? 0 : 3);
	}
	if (triangle != 0) {
		unsigned big_texture = 0, big;
		uint64_t least = ~0ull, most = 0, total = 0, began, read_us, one_us, bgra_us;
		uint8_t one[4];

		begin("f: 30 frames of a turning triangle at 480 x 640, each waited for");
		big = target(PANEL_WIDE, PANEL_HIGH, &big_texture);
		if (target_bind(big, big_texture, PANEL_WIDE, PANEL_HIGH)) {
			triangle_draw(triangle, 0);
			glFinish();             /* (the first frame makes the framebuffer and is not counted) */
			for (unsigned frame = 0; frame < FRAMES; frame++) {
				uint64_t took;

				began = now_us();
				triangle_draw(triangle, (float)frame * 0.1f);
				glFinish();
				took = now_us() - began;
				least = took < least ? took : least;
				most = took > most ? took : most;
				total += took;
				step_began = now_us();  /* (still moving) */
			}
			/* reading back: one pixel (what any read costs), a whole frame as it is kept, and one as an IOSurface wants it */
			began = now_us();
			glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
			one_us = now_us() - began;
			began = now_us();
			glReadPixels(0, 0, PANEL_WIDE, PANEL_HIGH, GL_RGBA, GL_UNSIGNED_BYTE, panel);
			read_us = now_us() - began;
			began = now_us();
			glReadPixels(0, 0, PANEL_WIDE, PANEL_HIGH, GL_BGRA, GL_UNSIGNED_BYTE, panel);
			bgra_us = now_us() - began;
			say("f: reading back: one pixel %llu.%llu ms, a frame as RGBA %llu.%llu ms, as BGRA %llu.%llu ms (error %#x)",
			    (unsigned long long)(one_us / 1000), (unsigned long long)(one_us % 1000 / 100),
			    (unsigned long long)(read_us / 1000), (unsigned long long)(read_us % 1000 / 100),
			    (unsigned long long)(bgra_us / 1000), (unsigned long long)(bgra_us % 1000 / 100), glGetError());
			glReadPixels(0, 0, PANEL_WIDE, PANEL_HIGH, GL_RGBA, GL_UNSIGNED_BYTE, panel);
			timed = end("f: frames");
			say("f: FRAMES: %u of 480 x 640: %llu.%llu ms each (the shortest %llu.%llu, the longest %llu.%llu); reading one "
			    "back %llu.%llu ms; the middle pixel %02x%02x%02x%02x", FRAMES, (unsigned long long)(total / FRAMES / 1000),
			    (unsigned long long)(total / FRAMES % 1000 / 100), (unsigned long long)(least / 1000),
			    (unsigned long long)(least % 1000 / 100), (unsigned long long)(most / 1000),
			    (unsigned long long)(most % 1000 / 100), (unsigned long long)(read_us / 1000),
			    (unsigned long long)(read_us % 1000 / 100), panel[(PANEL_HIGH / 2 * PANEL_WIDE + PANEL_WIDE / 2) * 4],
			    panel[(PANEL_HIGH / 2 * PANEL_WIDE + PANEL_WIDE / 2) * 4 + 1],
			    panel[(PANEL_HIGH / 2 * PANEL_WIDE + PANEL_WIDE / 2) * 4 + 2],
			    panel[(PANEL_HIGH / 2 * PANEL_WIDE + PANEL_WIDE / 2) * 4 + 3]);
		} else {
			end("f: frames");
		}
	}

	/* e and g, each by a run of this program of its own: whatever becomes of them, the steps above are said */
	for (unsigned run = 0; run < 2; run++) {
		char *arguments[] = { argv[0], run == 0 ? "surface" : "display", NULL };
		const char *step = run == 0 ? "e: IOSURFACE" : "g: DISPLAY";
		pid_t child = 0;
		int status = 0, good = 0;

		begin(run == 0 ? "e: the IOSurface, in a run of its own (pvr_gl_test surface)" :
		    "g: the display's picture, in a run of its own (pvr_gl_test display)");
		step_name = NULL;       /* (the other run has its own watch) */
		if (posix_spawn(&child, argv[0], NULL, NULL, arguments, environ) != 0 || waitpid(child, &status, 0) != child) {
			say("%s: NOT DONE: the run cannot be started", step);
		} else if (WIFSIGNALED(status)) {
			say("%s: THE RUN DIED of signal %d (6: abort -- Mesa's last line says why)", step, WTERMSIG(status));
		} else {
			good = WEXITSTATUS(status) == 0;
			say("%s: the run ended with %d (0: right; 3: not; 2: hung; 1: no context)", step, WEXITSTATUS(status));
		}
		if (run == 0) {
			shared = good;
		} else {
			displayed = good;
		}
	}
	say("RESULT: clear %s, triangle %s, texture %s, IOSurface %s, frames %s, the display's picture %s",
	    cleared ? "RIGHT" : "not right", drawn ? "RIGHT" : "not right", textured ? "RIGHT" : "not right",
	    shared < 0 ? "not tried" : shared ? "SHARED AND RIGHT" : "not shared or not right", timed ? "timed" : "not timed",
	    displayed ? "RIGHT" : "not right");
	told();
	begin("the end: the context taken down");
	redwood_gl_make_current(NULL);
	redwood_gl_destroy(gl);
	step_name = NULL;
	kept("the end: done");
	return cleared && drawn && textured ? 0 : 1;
}
