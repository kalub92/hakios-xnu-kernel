/*
 * The OpenGL ES library behind src/eagl.c when that library is Mesa as tools/mk-mesa-pvr.sh builds
 * it (ZINK=1) for the rabbit r1's PowerVR: Zink, which draws with Vulkan, over Mesa's PowerVR driver
 * (lib/libvulkan_powervr_mesa.dylib, which Zink opens itself: there is no Vulkan loader), reached
 * through EGL with no window system ("surfaceless"). The sibling of src/gl_osmesa.c, and like it
 * all that knows which library draws.
 *
 * Boot-arg pvr_gl_import=0: no IOSurface is made a texture's memory (redwood_gl_surface_texture()
 * says no every time, and eagl.c copies), for telling what the import does.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#include <IOSurface/IOSurfaceRef.h>

#include "redwood_gl.h"

/* EGL/egl.h, EGL/eglext.h and GLES2/gl2.h of Khronos, as far as they are used here */
typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLConfig, *EGLImageKHR, *EGLClientBuffer;
typedef int EGLint;
typedef unsigned EGLBoolean, EGLenum;
extern EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const intptr_t *attributes);
extern EGLBoolean eglInitialize(EGLDisplay display, EGLint *major, EGLint *minor);
extern EGLBoolean eglBindAPI(EGLenum api);
extern EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share, const EGLint *attributes);
extern EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context);
extern EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context);
extern EGLint eglGetError(void);
extern EGLContext eglGetCurrentContext(void);
extern const char *eglQueryString(EGLDisplay display, EGLint name);
extern void (*eglGetProcAddress(const char *name))(void);
extern unsigned glGetError(void);

#define EGL_PLATFORM_SURFACELESS_MESA   0x31dd
#define EGL_OPENGL_ES_API               0x30a0
#define EGL_CONTEXT_MAJOR_VERSION       0x3098
#define EGL_NONE                        0x3038
#define EGL_EXTENSIONS                  0x3055
#define EGL_WIDTH                       0x3057
#define EGL_HEIGHT                      0x3056
#define EGL_LINUX_DMA_BUF_EXT           0x3270
#define EGL_LINUX_DRM_FOURCC_EXT        0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT       0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT   0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT    0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define DRM_FORMAT_ARGB8888             0x34325241      /* "AR24": blue, green, red, alpha in memory -- an IOSurface's 'BGRA' */
#define DRM_FORMAT_R8                   0x20203852      /* "R8  ": a '420f' surface's plane 0, luma */
#define DRM_FORMAT_GR88                 0x38385247      /* "GR88": its plane 1, Cb then Cr in memory */
#define GL_TEXTURE_2D                   0x0de1

struct redwood_gl {
	EGLContext context;
};

static EGLDisplay display;
static int failed;
int redwood_gl_egl_error;               /* why there is no context, for whoever asks (src/pvr_gl_test.c): EGL's error, or -1 */
/*
 * ... and why the last IOSurface was not made a texture's memory (redwood_gl_surface_texture() said 0):
 * how far it came -- 1 told not to (pvr_gl_import=0), 2 EGL or OpenGL ES has not the functions, 3 EGL
 * does not offer EGL_EXT_image_dma_buf_import, 4 no such surface, 5 the surface is not one plane of
 * 'BGRA', 6 eglCreateImageKHR refused (EGL's error in redwood_gl_surface_error), 7
 * glEGLImageTargetTexture2DOES refused (OpenGL ES's error there); 0: it was made one.
 */
int redwood_gl_surface_stage, redwood_gl_surface_error;

/* The one display: Mesa's EGL told to take Zink for its driver, and the PowerVR driver to start on this core. */
static EGLDisplay
the_display(void)
{
	if (display == NULL && !failed) {
		setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
		setenv("PVR_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);    /* (not conformant on the GE8320, and says so instead of starting) */
		display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
		if (display == NULL || !eglInitialize(display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
			redwood_gl_egl_error = display == NULL ? -1 : eglGetError();
			display = NULL;
			failed = 1;
		}
	}
	return display;
}

struct redwood_gl *
redwood_gl_create(int version)
{
	const EGLint attributes[] = { EGL_CONTEXT_MAJOR_VERSION, version, EGL_NONE };
	struct redwood_gl *gl;

	if (the_display() == NULL || (gl = calloc(1, sizeof(*gl))) == NULL) {
		return NULL;
	}
	/* (no EGLConfig: EGL_KHR_no_config_context -- nothing here draws into a window or a pbuffer) */
	if ((gl->context = eglCreateContext(display, NULL, NULL, attributes)) == NULL) {
		redwood_gl_egl_error = eglGetError();
		free(gl);
		return NULL;
	}
	return gl;
}

int
redwood_gl_make_current(struct redwood_gl *gl)
{
	if (display == NULL) {
		return gl == NULL;
	}
	/* (no surface: EGL_KHR_surfaceless_context -- the window server draws into textures of its own, see eagl.c) */
	return (int)eglMakeCurrent(display, NULL, NULL, gl == NULL ? NULL : gl->context);
}

void
redwood_gl_destroy(struct redwood_gl *gl)
{
	eglDestroyContext(display, gl->context);
	free(gl);
}

void *
redwood_gl_current(void)
{
	return eglGetCurrentContext();
}

const char *
redwood_gl_library(void)
{
	return "Mesa, Zink over Vulkan (EGL)";
}

/*
 * OES_EGL_image: the IOSurface as what Linux calls a dma-buf, its ID where the file descriptor would
 * be. Mesa hands that down as it is -- EGL, Zink, Vulkan's VK_EXT_external_memory_dma_buf, the PowerVR
 * driver -- to tools/mesa/redwood-drm/redwood_pvr_drm.c, where the kernel's driver makes a buffer of
 * the surface's memory (request 0x7e). One plane of four bytes a pixel, 'BGRA'; anything else is
 * refused, and eagl.c copies.
 */
int
redwood_gl_surface_texture(struct redwood_gl *gl, uint32_t surface)
{
	return redwood_gl_surface_plane_texture(gl, surface, -1);
}

/*
 * ... or one plane of a planar one (glyuv=1, eagl.c: a camera's '420f' preview): plane 0 as one byte a
 * pixel (R8), plane 1 as two (GR88: Cb then Cr in memory), at the plane's offset and with its own row.
 */
int
redwood_gl_surface_plane_texture(struct redwood_gl *gl, uint32_t surface, int plane)
{
	static EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
	static EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR);
	static void (*image_target)(unsigned target, void *image);
	static int asked, refused, offered;
	IOSurfaceRef ref;
	EGLImageKHR image;
	int done;

	(void)gl;
	if (!asked) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		asked = 1;
		create_image = (EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))
		    eglGetProcAddress("eglCreateImageKHR");
		destroy_image = (EGLBoolean (*)(EGLDisplay, EGLImageKHR))eglGetProcAddress("eglDestroyImageKHR");
		image_target = (void (*)(unsigned, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
		refused = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
		          strstr(arguments, "pvr_gl_import=0") != NULL;
		offered = display != NULL && eglQueryString(display, EGL_EXTENSIONS) != NULL &&
		          strstr(eglQueryString(display, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
	}
	redwood_gl_surface_error = 0;
	if (refused || display == NULL || create_image == NULL || destroy_image == NULL || image_target == NULL) {
		redwood_gl_surface_stage = refused ? 1 : 2;
		return 0;
	}
	if (!offered) {
		redwood_gl_surface_stage = 3;
		return 0;
	}
	if ((ref = IOSurfaceLookup(surface)) == NULL) {
		redwood_gl_surface_stage = 4;
		return 0;
	}
	{
		const bool planar = plane >= 0;
		const EGLint attributes[] = {
			EGL_WIDTH, (EGLint)(planar ? IOSurfaceGetWidthOfPlane(ref, (size_t)plane) : IOSurfaceGetWidth(ref)),
			EGL_HEIGHT, (EGLint)(planar ? IOSurfaceGetHeightOfPlane(ref, (size_t)plane) : IOSurfaceGetHeight(ref)),
			EGL_LINUX_DRM_FOURCC_EXT, !planar ? DRM_FORMAT_ARGB8888 : plane == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88,
			EGL_DMA_BUF_PLANE0_FD_EXT, (EGLint)surface,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, planar ? (EGLint)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(ref, (size_t)plane) -
			    (uint8_t *)IOSurfaceGetBaseAddress(ref)) : 0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)(planar ? IOSurfaceGetBytesPerRowOfPlane(ref, (size_t)plane) :
			    IOSurfaceGetBytesPerRow(ref)),
			/* (the modifier said, and it is DRM_FORMAT_MOD_LINEAR, 0: rows of pixels as they are. Unsaid, Zink
			 * would not take the image from this Vulkan driver) */
			EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
			EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
			EGL_NONE
		};
		int fits = planar ? (IOSurfaceGetPixelFormat(ref) == '420f' || IOSurfaceGetPixelFormat(ref) == '420v') &&
		                    IOSurfaceGetPlaneCount(ref) == 2 && plane < 2 :
		           IOSurfaceGetPixelFormat(ref) == 'BGRA' && IOSurfaceGetPlaneCount(ref) == 0 &&
		           IOSurfaceGetBytesPerElement(ref) == 4;

		CFRelease(ref);
		if (!fits) {
			redwood_gl_surface_stage = 5;
			return 0;
		}
		if ((image = create_image(display, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, attributes)) == NULL) {
			redwood_gl_surface_stage = 6;
			redwood_gl_surface_error = eglGetError();
			return 0;
		}
	}
	while (glGetError() != 0) {
	}
	image_target(GL_TEXTURE_2D, image);
	redwood_gl_surface_error = (int)glGetError();
	done = redwood_gl_surface_error == 0;
	redwood_gl_surface_stage = done ? 0 : 7;
	destroy_image(display, image);          /* (the texture keeps what it was made of) */
	return done;
}
