/*
 * pass_bench: what ONE render pass costs on the r1 (OpenGL ES 3 by Mesa's Zink over the PowerVR Vulkan driver, the
 * way the window server has it), by load and store setting, at the home screen's target size (375 x 500, RGBA8):
 *   load:  clear (glClear), load (nothing: what is there is kept), dontcare (glInvalidateFramebuffer first)
 *   store: store (as it is), discard (glInvalidateFramebuffer before the flush)
 *   with 1 and with 8 small draws, and the framebuffer-fetch cases: 2 draws, the second reading what is there
 *   (EXT_shader_framebuffer_fetch, what CoreAnimation's vibrant layers do), and 8 draws, every other one.
 * Each case is N passes (a pass: set up, draw, glFlush), ending with one glFinish. Per pass it says the CPU time of the
 * app thread, of the rest of the process (Zink's driver thread, the Vulkan driver's work: the process total less the
 * app thread) and the wall time to issue the passes and to have them all done (the GPU's time if it is the longer).
 * After each case the program sleeps 11 s so that the kernel's ten-second "pvr: stats:" lines (boot-arg pvr_stats=1)
 * hold that case alone: tools/swprof.py reads jobs and syncobj requests per pass from them.
 * Idea after passbench.m of Open Mapple's mapple_gpu_mali (cost of a pass by load/store action); written here for GLES.
 *
 * Runs only with `pass_bench ifarg passbench=1` (a boot-arg), so it can stay in rc.jobs. Context set up as src/pvr_gl_test.c.
 */
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "redwood_gl.h"

extern int redwood_gl_egl_error;

extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
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
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride, const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern void glFlush(void);
extern void glInvalidateFramebuffer(unsigned target, int count, const unsigned *attachments);
extern void glDisable(unsigned cap);

#define GL_TRIANGLE_STRIP       0x0005
#define GL_TEXTURE_2D           0x0de1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_FLOAT                0x1406
#define GL_RGBA                 0x1908
#define GL_RGBA8                0x8058
#define GL_NEAREST              0x2600
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_FRAGMENT_SHADER      0x8b30
#define GL_VERTEX_SHADER        0x8b31
#define GL_COMPILE_STATUS       0x8b81
#define GL_LINK_STATUS          0x8b82
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_FRAMEBUFFER          0x8d40
#define GL_COLOR_ATTACHMENT0    0x8ce0
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_EXTENSIONS           0x1f03
#define GL_BLEND                0x0be2
#define GL_DITHER               0x0bd0

#define WIDE    375
#define HIGH    500

static void
say(const char *format, ...)
{
	char line[200], with[216];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("pass_bench: %s\n", line);
	fflush(stdout);
	snprintf(with, sizeof(with), "!pass_bench: %s", line);
	sysctlbyname("kern.redwood.say", NULL, NULL, with, strlen(with));
}

static double
thread_cpu_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static double
process_cpu_ms(void)
{
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e3 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e3;
}

static double
wall_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return (double)mach_absolute_time() * base.numer / base.denom / 1e6;
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
		say("a shader does not compile: %s", log);
		return 0;
	}
	return s;
}

static unsigned
program(const char *fragment)
{
	static const char vertex[] =
	    "#version 300 es\n"
	    "in vec2 position;\n"
	    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
	unsigned v = shader(GL_VERTEX_SHADER, vertex), f = shader(GL_FRAGMENT_SHADER, fragment), p;
	int good = 0;

	if (v == 0 || f == 0) {
		return 0;
	}
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "position");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &good);
	return good ? p : 0;
}

enum { LOAD_CLEAR, LOAD_KEEP, LOAD_DONTCARE };

static struct {
	const char *name;
	int load, discard, draws, fetch;        /* fetch: 0 none, 1 the second draw, 2 every other draw */
} cases[] = {
	{ "clear/store   1 draw ", LOAD_CLEAR, 0, 1, 0 }, { "clear/discard 1 draw ", LOAD_CLEAR, 1, 1, 0 },
	{ "load/store    1 draw ", LOAD_KEEP, 0, 1, 0 }, { "load/discard  1 draw ", LOAD_KEEP, 1, 1, 0 },
	{ "dontcare/store   1 draw ", LOAD_DONTCARE, 0, 1, 0 }, { "dontcare/discard 1 draw ", LOAD_DONTCARE, 1, 1, 0 },
	{ "clear/store   8 draws", LOAD_CLEAR, 0, 8, 0 }, { "clear/discard 8 draws", LOAD_CLEAR, 1, 8, 0 },
	{ "load/store    8 draws", LOAD_KEEP, 0, 8, 0 }, { "load/discard  8 draws", LOAD_KEEP, 1, 8, 0 },
	{ "dontcare/store   8 draws", LOAD_DONTCARE, 0, 8, 0 }, { "dontcare/discard 8 draws", LOAD_DONTCARE, 1, 8, 0 },
	{ "load/store    2 draws plain", LOAD_KEEP, 0, 2, 0 }, { "load/store    2 draws, 2nd fetch", LOAD_KEEP, 0, 2, 1 },
	{ "load/store    8 draws plain", LOAD_KEEP, 0, 8, 0 }, { "load/store    8 draws, 4 fetch", LOAD_KEEP, 0, 8, 2 },
};

int
main(int argc, char **argv)
{
	static const char plain[] =
	    "#version 300 es\nprecision mediump float;\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(0.2, 0.4, 0.6, 1.0); }\n";
	static const char fetch[] =
	    "#version 300 es\n#extension GL_EXT_shader_framebuffer_fetch : require\nprecision mediump float;\n"
	    "layout(location = 0) inout vec4 c;\nvoid main() { c = c * 0.5 + vec4(0.25); }\n";
	static const unsigned attachment = GL_COLOR_ATTACHMENT0;
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	struct redwood_gl *gl;
	unsigned texture, framebuffer, p_plain, p_fetch;
	unsigned passes = 200;
	const char *extensions;
	float quads[8][8];
	int have_fetch;

	if (argc > 2 && strcmp(argv[1], "ifarg") == 0 &&
	    (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || strstr(args, argv[2]) == NULL)) {
		return 0;
	}
	if (getenv("PASSES") != NULL) {
		passes = (unsigned)atoi(getenv("PASSES"));
	}
	sleep(100);     /* (the start-up's background work is over by then) */
	gl = redwood_gl_create(3);
	if (gl == NULL || !redwood_gl_make_current(gl)) {
		say("NOT DONE: no OpenGL ES 3 context (EGL error %#x)", redwood_gl_egl_error);
		return 1;
	}
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	have_fetch = extensions != NULL && strstr(extensions, "GL_EXT_shader_framebuffer_fetch") != NULL;
	{
		/* (whatever the library calls it: the words with "fetch" in them) */
		char names[300] = "";

		for (const char *w = extensions; w != NULL && *w != 0;) {
			size_t n = strcspn(w, " ");

			if (memmem(w, n, "fetch", 5) != NULL && strlen(names) + n + 2 < sizeof(names)) {
				strncat(names, w, n);
				strcat(names, " ");
			}
			w += n + (w[n] == ' ');
		}
		say("extensions with fetch: %s (glGetString %s)", names, extensions == NULL ? "gave NULL" : "gave a list");
	}
	say("%s; %s; %dx%d, %u passes a case; framebuffer fetch %s", (const char *)glGetString(0x1f01), (const char *)glGetString(0x1f02), WIDE, HIGH,
	    passes, have_fetch ? "offered" : "NOT in the extension list");
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WIDE, HIGH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		say("NOT DONE: the framebuffer is not complete");
		return 1;
	}
	glViewport(0, 0, WIDE, HIGH);
	glDisable(GL_BLEND);
	glDisable(GL_DITHER);
	p_plain = program(plain);
	p_fetch = program(fetch);                       /* (tried whatever the list says) */
	say("the framebuffer-fetch program %s", p_fetch != 0 ? "is made" : "is NOT made");
	for (unsigned q = 0; q < 8; q++) {      /* eight small squares (a tenth of the width) down the target */
		float x = -0.9f + 0.2f * (q % 4), y = -0.9f + 0.4f * (q / 4);

		float v[8] = { x, y, x + 0.1f, y, x, y + 0.1f, x + 0.1f, y + 0.1f };
		memcpy(quads[q], v, sizeof(v));
	}
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	say("case: label | passes | app-thread CPU ms/pass | rest-of-process CPU ms/pass | issue wall ms/pass | all done wall ms/pass");
	for (unsigned c = strstr(args, "passbench=2") != NULL ? 12 : 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		double t0, t1, w0, w1, w2, p0, p1;

		if (cases[c].fetch != 0 && p_fetch == 0) {
			continue;
		}
		/* one pass first, so that the compile and the first buffers are not in the numbers */
		for (unsigned warm = 0; warm < 2; warm++) {
			glFlush();
		}
		glFinish();
		sleep(1);
		t0 = thread_cpu_ms();
		p0 = process_cpu_ms();
		w0 = wall_ms();
		for (unsigned n = 0; n < passes; n++) {
			if (cases[c].load == LOAD_CLEAR) {
				glClear(GL_COLOR_BUFFER_BIT);
			} else if (cases[c].load == LOAD_DONTCARE) {
				glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
			}
			for (int d = 0; d < cases[c].draws; d++) {
				int use_fetch = cases[c].fetch == 1 ? d == 1 : cases[c].fetch == 2 ? (d & 1) : 0;

				glUseProgram(use_fetch ? p_fetch : p_plain);
				glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quads[d]);
				glEnableVertexAttribArray(0);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			if (cases[c].discard) {
				glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
			}
			glFlush();
		}
		w1 = wall_ms();
		t1 = thread_cpu_ms();
		glFinish();
		w2 = wall_ms();
		p1 = process_cpu_ms();
		say("case: %-26s | %u | %.2f | %.2f | %.2f | %.2f | error %#x", cases[c].name, passes, (t1 - t0) / passes,
		    (p1 - p0 - (t1 - t0)) / passes, (w1 - w0) / passes, (w2 - w0) / passes, glGetError());
		sleep(11);
	}
	say("done");
	return 0;
}
