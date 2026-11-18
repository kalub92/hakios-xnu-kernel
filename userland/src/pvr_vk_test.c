/*
 * pvr_vk_test: the first work the PowerVR does for a program (the rabbit r1's GE8320). Vulkan by
 * Mesa's driver (lib/libvulkan_powervr_mesa.dylib, tools/mk-mesa-pvr.sh), which talks to the
 * kernel's service RedwoodPVR; there is no Vulkan loader, the driver is opened as a library and
 * asked for its functions itself.
 *
 * In steps, and of every step one line before it and one after with what Vulkan answered, so that
 * the last line of a log is the step that failed or never came back:
 *   a  the instance, the GPU as the driver describes it, its memory and its queues
 *   b  a device and a queue
 *   c  a picture of 64 x 64 cleared to one colour, copied into memory and looked at: which of its
 *      tiles of 16 x 16 have the colour, what else is in it, and what the picture's own memory holds
 *   d  a triangle with a colour at each corner into the same picture: 16 x 16 letters of it
 *   c' the clear again at 16 x 16, 32 x 32, 48 x 48, 128 x 128, 100 x 36 and 256 x 64
 *   e  a compute shader that fills a buffer with numbers, where the driver has compute
 * Nothing waits for the GPU longer than fifteen seconds (TIMEOUT) -- the kernel gives a job ten, then
 * calls its context faulty and signals its fences, so a job that hangs shows here as a wait that
 * ends after ten seconds and pixels that are WRONG -- and a call into the driver that does not come
 * back within forty ends the program (HUNG in ...): then nothing is taken down, which could hang
 * as well.
 *
 * Every line goes to the console ("pvr_vk_test: ..."); those of the steps that reach the GPU, and every
 * failure and result, also to the kernel as lines to keep (kern.redwood.say, "!" first), which the
 * kernel says again at the end of its log.
 *
 *   pvr_vk_test [LIBRARY]      (default /redwood/lib/libvulkan_powervr_mesa.dylib)
 *
 * The shaders, as SPIR-V at the end of this file (tools/mesa/pvr/shaders has the assembly they
 * were made from, with spirv-as). In GLSL they would be:
 *
 *   vertex:    layout(location = 0) in vec2 pos; layout(location = 1) in vec3 col;
 *              layout(location = 0) out vec3 colour;
 *              void main() { gl_Position = vec4(pos, 0.0, 1.0); colour = col; }
 *   fragment:  layout(location = 0) in vec3 colour; layout(location = 0) out vec4 pixel;
 *              void main() { pixel = vec4(colour, 1.0); }
 *   compute:   layout(local_size_x = 64) in;
 *              layout(set = 0, binding = 0) buffer Numbers { uint number[]; };
 *              void main() { uint i = gl_GlobalInvocationID.x; number[i] = i * 3u + 7u; }
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "pvr_vk_test_spv.h"

#define SIDE            64                      /* the picture's width and height */
#define WAIT_NS         15000000000ull          /* for the GPU: longer than the kernel gives a job (10 s, pvr_timeout) */
#define HUNG_SECONDS    40                      /* for a call into the driver */
#define NUMBERS         256                     /* what the compute shader fills */

/* A line for the console, and with `keep` for the kernel to say again at the end of its log. */
static void
line_out(int keep, const char *format, va_list ap)
{
	char line[200];
	char kept[216];

	vsnprintf(line, sizeof(line), format, ap);
	printf("pvr_vk_test: %s\n", line);
	fflush(stdout);
	if (keep) {
		snprintf(kept, sizeof(kept), "!pvr_vk_test: %s", line);
		sysctlbyname("kern.redwood.say", NULL, NULL, kept, strlen(kept));
	}
}

static void
say(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	line_out(1, format, ap);
	va_end(ap);
}

/* (the kernel keeps some hundred lines: the small steps are the console's alone, unless they fail) */
static void
note(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	line_out(0, format, ap);
	va_end(ap);
}

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
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
		if (name != NULL && name == step_name && now_ms() - step_began > HUNG_SECONDS * 1000) {
			say("HUNG in \"%s\": no answer in %d s, giving up (nothing is taken down)", name, HUNG_SECONDS);
			_exit(2);
		}
	}
	return NULL;
}

static const char *
result_name(VkResult result)
{
	static char number[32];

	switch (result) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "TIMEOUT";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
	case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
	default:
		snprintf(number, sizeof(number), "VkResult %d", (int)result);
		return number;
	}
}

/* One step: said before, made, said after with its answer. True when it went well. */
#define STEP(what, call) (step_begin(what, 0), step_end(what, 0, (call)))
/* ... of the steps that reach the kernel and the GPU: these lines the kernel keeps */
#define BIG_STEP(what, call) (step_begin(what, 1), step_end(what, 1, (call)))
/* ... of the calls that answer nothing */
#define DO(what, call) (step_begin(what, 0), (call), step_end(what, 0, VK_SUCCESS))

static int quiet;       /* the kernel is given only what fails (the clears of other sizes: it keeps some hundred lines) */

static void
step_begin(const char *what, int keep)
{
	keep = keep && !quiet;
	(keep ? say : note)("%s ...", what);
	step_began = now_ms();
	step_name = what;
}

static int
step_end(const char *what, int keep, VkResult result)
{
	step_name = NULL;
	((keep && !quiet) || result != VK_SUCCESS ? say : note)("%s: %s (%llu ms)", what, result_name(result),
	    (unsigned long long)(now_ms() - step_began));
	return result == VK_SUCCESS;
}

/* The driver's functions. */
#define INSTANCE_FUNCTIONS(F) \
	F(DestroyInstance) F(EnumeratePhysicalDevices) F(GetPhysicalDeviceProperties) F(GetPhysicalDeviceProperties2) \
	F(GetPhysicalDeviceMemoryProperties) F(GetPhysicalDeviceQueueFamilyProperties) F(CreateDevice) F(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(F) \
	F(DestroyDevice) F(GetDeviceQueue) F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements) F(AllocateMemory) \
	F(FreeMemory) F(BindImageMemory) F(CreateImageView) F(DestroyImageView) F(CreateRenderPass) F(DestroyRenderPass) \
	F(CreateFramebuffer) F(DestroyFramebuffer) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) \
	F(BindBufferMemory) F(MapMemory) F(UnmapMemory) F(CreateCommandPool) F(DestroyCommandPool) F(AllocateCommandBuffers) \
	F(BeginCommandBuffer) F(EndCommandBuffer) F(ResetCommandBuffer) F(CmdBeginRenderPass) F(CmdEndRenderPass) \
	F(CmdCopyImageToBuffer) F(CreateFence) F(DestroyFence) F(ResetFences) F(WaitForFences) F(QueueSubmit) \
	F(CreateShaderModule) F(DestroyShaderModule) F(CreatePipelineLayout) F(DestroyPipelineLayout) \
	F(CreateGraphicsPipelines) F(CreateComputePipelines) F(DestroyPipeline) F(CmdBindPipeline) F(CmdBindVertexBuffers) \
	F(CmdDraw) F(CreateDescriptorSetLayout) F(DestroyDescriptorSetLayout) F(CreateDescriptorPool) F(DestroyDescriptorPool) \
	F(AllocateDescriptorSets) F(UpdateDescriptorSets) F(CmdBindDescriptorSets) F(CmdDispatch)
#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)

static VkPhysicalDevice gpu;
static VkPhysicalDeviceMemoryProperties memory;
static VkDevice device;
static VkQueue queue;
static VkCommandBuffer commands;
static VkFence fence;
static int abandoned;           /* a wait timed out: the GPU may still hold what was made, and nothing is taken down */

/* Memory for something with these requirements, that this program can see: 0 when there is none. */
static VkDeviceMemory
memory_for(const VkMemoryRequirements *need, const char *what)
{
	VkMemoryAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need->size };
	VkDeviceMemory got = VK_NULL_HANDLE;
	char line[96];

	for (info.memoryTypeIndex = 0; info.memoryTypeIndex < memory.memoryTypeCount; info.memoryTypeIndex++) {
		if ((need->memoryTypeBits & (1u << info.memoryTypeIndex)) != 0 &&
		    (memory.memoryTypes[info.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
			break;
		}
	}
	if (info.memoryTypeIndex == memory.memoryTypeCount) {
		say("no memory type this program can see for %s (types %#x)", what, need->memoryTypeBits);
		return VK_NULL_HANDLE;
	}
	snprintf(line, sizeof(line), "memory for %s, %llu bytes aligned %llu", what, (unsigned long long)need->size,
	    (unsigned long long)need->alignment);
	return STEP(line, vkAllocateMemory(device, &info, NULL, &got)) ? got : VK_NULL_HANDLE;
}

struct buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *bytes;
};

static int
buffer_make(struct buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, const char *what)
{
	VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkMemoryRequirements need;
	char line[96];

	memset(b, 0, sizeof(*b));
	snprintf(line, sizeof(line), "%s (a buffer of %llu bytes)", what, (unsigned long long)size);
	if (!STEP(line, vkCreateBuffer(device, &info, NULL, &b->buffer))) {
		return 0;
	}
	vkGetBufferMemoryRequirements(device, b->buffer, &need);
	if ((b->memory = memory_for(&need, what)) == VK_NULL_HANDLE) {
		return 0;
	}
	snprintf(line, sizeof(line), "%s: bound and mapped", what);
	return STEP(line, vkBindBufferMemory(device, b->buffer, b->memory, 0)) &&
	       STEP(line, vkMapMemory(device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->bytes));
}

static void
buffer_drop(struct buffer *b)
{
	if (b->buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, b->buffer, NULL);
	}
	if (b->memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, b->memory, NULL);
	}
}

/* The commands recorded, given to the GPU, and waited for. */
static int
run(const char *what)
{
	VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands };
	char line[96];
	VkResult result;

	snprintf(line, sizeof(line), "%s: submit", what);
	if (!STEP(line, vkResetFences(device, 1, &fence)) || !BIG_STEP(line, vkQueueSubmit(queue, 1, &submit, fence))) {
		return 0;
	}
	snprintf(line, sizeof(line), "%s: wait for the GPU, %llu s at most", what, WAIT_NS / 1000000000ull);
	step_begin(line, 1);
	result = vkWaitForFences(device, 1, &fence, VK_TRUE, WAIT_NS);
	if (result != VK_SUCCESS) {
		abandoned = 1;
	}
	return step_end(line, 1, result);
}

static uint32_t
checksum(const void *bytes, size_t size)       /* FNV-1a */
{
	uint32_t sum = 2166136261u;

	for (size_t i = 0; i < size; i++) {
		sum = (sum ^ ((const uint8_t *)bytes)[i]) * 16777619u;
	}
	return sum;
}

/* What steps c and d draw into, and how its pixels come back. */
static unsigned wide, high;
static VkImage image;
static VkDeviceMemory image_memory;
static uint32_t *image_words;           /* the picture's own memory, as this program sees it */
static size_t image_word_count;
static VkImageView view;
static VkRenderPass pass;
static VkFramebuffer framebuffer;
static struct buffer pixels;

#define UNTOUCHED       0xa5a5a5a5u     /* what the picture's memory is filled with before anything draws */
#define UNCOPIED        0x5a5a5a5au     /* ... and the pixels' copy before every copy */

static int
target_make(unsigned width, unsigned height)
{
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	};
	VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference colour = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colour,
	};
	VkRenderPassCreateInfo pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &view,
		.width = width,
		.height = height,
		.layers = 1,
	};
	VkMemoryRequirements need;
	void *bytes = NULL;

	wide = width;
	high = height;
	if (!STEP("the picture (an image, RGBA8)", vkCreateImage(device, &image_info, NULL, &image))) {
		return 0;
	}
	vkGetImageMemoryRequirements(device, image, &need);
	if ((image_memory = memory_for(&need, "the picture")) == VK_NULL_HANDLE ||
	    !STEP("the picture: its memory mapped", vkMapMemory(device, image_memory, 0, VK_WHOLE_SIZE, 0, &bytes)) ||
	    !STEP("the picture: memory bound", vkBindImageMemory(device, image, image_memory, 0))) {
		return 0;
	}
	image_words = bytes;
	image_word_count = (size_t)need.size / 4;
	for (size_t i = 0; i < image_word_count; i++) {
		image_words[i] = UNTOUCHED;
	}
	view_info.image = image;
	if (!STEP("the picture's view", vkCreateImageView(device, &view_info, NULL, &view)) ||
	    !STEP("the render pass (clear, draw, keep)", vkCreateRenderPass(device, &pass_info, NULL, &pass))) {
		return 0;
	}
	framebuffer_info.renderPass = pass;
	return STEP("the framebuffer", vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer)) &&
	       buffer_make(&pixels, wide * high * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "the pixels' copy");
}

static void
target_drop(void)
{
	if (framebuffer != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(device, framebuffer, NULL);
	}
	if (pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device, pass, NULL);
	}
	if (view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, view, NULL);
	}
	if (image != VK_NULL_HANDLE) {
		vkDestroyImage(device, image, NULL);
	}
	if (image_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, image_memory, NULL);
	}
	buffer_drop(&pixels);
	framebuffer = VK_NULL_HANDLE;
	pass = VK_NULL_HANDLE;
	view = VK_NULL_HANDLE;
	image = VK_NULL_HANDLE;
	image_memory = VK_NULL_HANDLE;
	image_words = NULL;
	memset(&pixels, 0, sizeof(pixels));
}

/* The commands of one frame begun: the picture cleared to `colour`, the render pass open. */
static int
frame_begin(const char *what, const float colour[4])
{
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkClearValue clear = { .color = { .float32 = { colour[0], colour[1], colour[2], colour[3] } } };
	VkRenderPassBeginInfo pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = pass,
		.framebuffer = framebuffer,
		.renderArea = { { 0, 0 }, { wide, high } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	char line[96];

	snprintf(line, sizeof(line), "%s: commands begun", what);
	if (!STEP(line, vkResetCommandBuffer(commands, 0)) || !STEP(line, vkBeginCommandBuffer(commands, &begin))) {
		return 0;
	}
	vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
	return 1;
}

/* ... ended and run; then, as a second piece of work, the picture copied to where it can be read. */
static int
frame_end(const char *what)
{
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { wide, high, 1 },
	};
	char line[96], copy[96];

	vkCmdEndRenderPass(commands);
	snprintf(line, sizeof(line), "%s: commands ended", what);
	if (!STEP(line, vkEndCommandBuffer(commands)) || !run(what)) {
		return 0;
	}
	snprintf(copy, sizeof(copy), "%s: the copy to memory", what);
	memset(pixels.bytes, 0x5a, wide * high * 4);    /* UNCOPIED: what is read where the copy writes nothing */
	if (!STEP(copy, vkResetCommandBuffer(commands, 0)) || !STEP(copy, vkBeginCommandBuffer(commands, &begin))) {
		return 0;
	}
	vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.buffer, 1, &region);
	return STEP(copy, vkEndCommandBuffer(commands)) && run(copy);
}

static const uint8_t *
pixel(unsigned x, unsigned y)
{
	return (const uint8_t *)pixels.bytes + (y * wide + x) * 4;
}

/* The four values most of `count` words are, and how many of each: "4080bfff x512 00000000 x3584" (bytes in memory's order). */
static void
census(const uint32_t *words, size_t count, char *out, size_t size)
{
	uint32_t value[24];
	size_t many[24], others = 0;
	unsigned values = 0, at = 0;

	for (size_t i = 0; i < count; i++) {
		unsigned v;

		for (v = 0; v < values && value[v] != words[i]; v++) {
		}
		if (v == values && values == 24) {
			others++;
			continue;
		}
		if (v == values) {
			value[values] = words[i];
			many[values++] = 0;
		}
		many[v]++;
	}
	out[0] = '\0';
	for (unsigned n = 0; n < 4 && n < values; n++) {
		unsigned most = n;
		uint32_t swap_value;
		size_t swap_many;

		for (unsigned v = n + 1; v < values; v++) {
			if (many[v] > many[most]) {
				most = v;
			}
		}
		swap_value = value[n], value[n] = value[most], value[most] = swap_value;
		swap_many = many[n], many[n] = many[most], many[most] = swap_many;
		at += (unsigned)snprintf(out + at, size - at, "%s%02x%02x%02x%02x x%zu", n ? " " : "", value[n] & 0xff,
		    (value[n] >> 8) & 0xff, (value[n] >> 16) & 0xff, value[n] >> 24, many[n]);
	}
	if (values > 4 || others != 0) {
		snprintf(out + at, size - at, " and %u%s other values", values > 4 ? values - 4 : 0, others ? " or more" : "");
	}
}

/*
 * c: the picture cleared, at one size. Said of it: every tile of 16 x 16 by a letter, the rows of tiles
 * apart by "/" -- C all of it the colour, 0 all zero, 5 all as the copy's memory was before the copy
 * (the copy wrote nothing there), a all as the picture's memory was before the clear (the copy brought
 * what the GPU never drew), x anything else --; what values the copy holds; and what the picture's own
 * memory holds, however the GPU lays it out, which tells a clear that was not made from a copy that
 * did not bring it.
 */
static int
clear_test(unsigned width, unsigned height)
{
	static const float colour[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	static const uint8_t expected[4] = { 0x40, 0x80, 0xbf, 0xff };
	unsigned wrong = 0, at = 0;
	size_t coloured = 0, untouched = 0, zero = 0;
	char what[32], tiles[176], values[160];

	snprintf(what, sizeof(what), "c: clear %ux%u", width, height);
	if (!target_make(width, height) || !frame_begin(what, colour) || !frame_end(what)) {
		say("%s: NOT DONE", what);
		return 0;
	}
	for (unsigned ty = 0; ty < high; ty += 16) {
		for (unsigned tx = 0; tx < wide; tx += 16) {
			unsigned total = 0, c = 0, z = 0, five = 0, a = 0;

			for (unsigned y = ty; y < ty + 16 && y < high; y++) {
				for (unsigned x = tx; x < tx + 16 && x < wide; x++) {
					const uint8_t *p = pixel(x, y);
					uint32_t word;
					int is = 1;

					memcpy(&word, p, 4);
					/* (one either way: 63.75 and 191.25 are rounded) */
					for (unsigned i = 0; i < 4; i++) {
						is = is && p[i] + 1 >= expected[i] && p[i] <= expected[i] + 1;
					}
					total++;
					c += is;
					wrong += !is;
					z += word == 0;
					five += word == UNCOPIED;
					a += word == UNTOUCHED;
				}
			}
			tiles[at++] = c == total ? 'C' : z == total ? '0' : five == total ? '5' : a == total ? 'a' : 'x';
		}
		tiles[at++] = ty + 16 < high ? '/' : '\0';
	}
	census(pixels.bytes, wide * high, values, sizeof(values));
	say("%s: %s, %u of %u pixels wrong; tiles %s", what, wrong == 0 ? "RIGHT" : "WRONG", wrong, wide * high, tiles);
	say("%s: the copy holds %s", what, values);
	for (size_t i = 0; i < image_word_count; i++) {
		coloured += (image_words[i] & 0xfefefefeu) == 0xfebe8040u;      /* 4080bfff, one either way in the low bits */
		untouched += image_words[i] == UNTOUCHED;
		zero += image_words[i] == 0;
	}
	census(image_words, image_word_count, values, sizeof(values));
	say("%s: own memory, %zu words: %zu the colour, %zu as before, %zu zero; %s", what, image_word_count, coloured,
	    untouched, zero, values);
	return wrong == 0;
}

static VkShaderModule
shader(const uint32_t *words, size_t bytes, const char *what)
{
	VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = words };
	VkShaderModule module = VK_NULL_HANDLE;

	return STEP(what, vkCreateShaderModule(device, &info, NULL, &module)) ? module : VK_NULL_HANDLE;
}

/* d: a triangle. Its corners: red at the top in the middle, green at the bottom right, blue at the bottom left. */
static int
triangle_test(void)
{
	static const float black[4] = { 0, 0, 0, 1 };
	static const float corners[3][5] = {    /* x, y (down), red, green, blue */
		{ 0.0f, -0.8f, 1, 0, 0 },
		{ 0.8f, 0.8f, 0, 1, 0 },
		{ -0.8f, 0.8f, 0, 0, 1 },
	};
	VkVertexInputBindingDescription binding = { 0, sizeof(corners[0]), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[2] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 2 * sizeof(float) },
	};
	VkPipelineVertexInputStateCreateInfo input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = 2,
		.pVertexAttributeDescriptions = attributes,
	};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkViewport viewport = { 0, 0, SIDE, SIDE, 0, 1 };
	VkRect2D scissor = { { 0, 0 }, { SIDE, SIDE } };
	VkPipelineViewportStateCreateInfo viewports = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1,
	};
	VkPipelineMultisampleStateCreateInfo samples = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_target = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_target,
	};
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" },
	};
	VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &input,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewports,
		.pRasterizationState = &raster,
		.pMultisampleState = &samples,
		.pColorBlendState = &blend,
		.renderPass = pass,
	};
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	struct buffer vertices = { 0 };
	VkDeviceSize offset = 0;
	unsigned covered = 0;
	int good = 0, done = 0;

	say("d: TRIANGLE: red at the top, green at the bottom right, blue at the bottom left, on black");
	stages[0].module = shader(pvr_vk_test_vertex, sizeof(pvr_vk_test_vertex), "d: the vertex shader's module");
	stages[1].module = shader(pvr_vk_test_fragment, sizeof(pvr_vk_test_fragment), "d: the fragment shader's module");
	if (stages[0].module == VK_NULL_HANDLE || stages[1].module == VK_NULL_HANDLE ||
	    !STEP("d: the pipeline layout", vkCreatePipelineLayout(device, &layout_info, NULL, &layout))) {
		goto out;
	}
	info.layout = layout;
	if (!BIG_STEP("d: the pipeline (the driver compiles both shaders)",
	        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline)) ||
	    !buffer_make(&vertices, sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "d: the corners") ||
	    (memcpy(vertices.bytes, corners, sizeof(corners)), !frame_begin("d: triangle", black))) {
		goto out;
	}
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &vertices.buffer, &offset);
	vkCmdDraw(commands, 3, 1, 0, 0);
	if (!frame_end("d: triangle")) {
		goto out;
	}
	done = 1;
	/* 16 x 16 letters, each the middle of a square of 4 x 4 pixels: . black, R G B the strongest of the three, ? other */
	for (unsigned half = 0; half < 2; half++) {
		char rows[8 * 17 + 1];

		for (unsigned row = 0; row < 8; row++) {
			for (unsigned column = 0; column < 16; column++) {
				const uint8_t *p = pixel(column * 4 + 2, (half * 8 + row) * 4 + 2);

				rows[row * 17 + column] = p[0] + p[1] + p[2] < 48 ? '.' : p[0] >= p[1] && p[0] >= p[2] ? 'R' :
				    p[1] >= p[2] ? 'G' : 'B';
				if (p[3] != 0xff) {
					rows[row * 17 + column] = '?';
				}
			}
			rows[row * 17 + 16] = row == 7 ? '\0' : '/';
		}
		say("d: rows %2u-%2u: %s", half * 8, half * 8 + 7, rows);
	}
	for (unsigned i = 0; i < SIDE * SIDE; i++) {
		const uint8_t *p = pixel(i % SIDE, i / SIDE);

		covered += p[3] == 0xff && p[0] + p[1] + p[2] >= 48;       /* (not what was never drawn or never copied) */
	}
	/* the triangle is 51.2 pixels wide and as high: 1311 of them; its top is red, its bottom corners green and blue */
	good = covered > 1200 && covered < 1420 && pixel(0, 0)[0] + pixel(0, 0)[1] + pixel(0, 0)[2] == 0 &&
	       pixel(32, 12)[0] > 150 && pixel(52, 55)[1] > 150 && pixel(12, 55)[2] > 150;
	say("d: TRIANGLE: %s: %u pixels drawn (about 1311 expected); near the top %02x%02x%02x, the bottom right %02x%02x%02x, "
	    "the bottom left %02x%02x%02x; checksum %08x",
	    good ? "RIGHT" : "WRONG", covered, pixel(32, 12)[0], pixel(32, 12)[1], pixel(32, 12)[2], pixel(52, 55)[0],
	    pixel(52, 55)[1], pixel(52, 55)[2], pixel(12, 55)[0], pixel(12, 55)[1], pixel(12, 55)[2],
	    checksum(pixels.bytes, SIDE * SIDE * 4));
out:
	if (!done) {
		say("d: TRIANGLE: NOT DONE");
	}
	if (!abandoned) {
		buffer_drop(&vertices);
		if (pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, pipeline, NULL);
		}
		if (layout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, layout, NULL);
		}
		for (unsigned i = 0; i < 2; i++) {
			if (stages[i].module != VK_NULL_HANDLE) {
				vkDestroyShaderModule(device, stages[i].module, NULL);
			}
		}
	}
	return good;
}

/* e: a compute shader writes number[i] = 3 i + 7 into a buffer. */
static int
compute_test(void)
{
	VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo set_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1 };
	VkComputePipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		           .pName = "main" },
	};
	VkDescriptorSetAllocateInfo set_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1 };
	VkDescriptorBufferInfo buffer_info = { .range = VK_WHOLE_SIZE };
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &buffer_info,
	};
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	struct buffer numbers = { 0 };
	const uint32_t *number;
	unsigned wrong = 0;
	int done = 0;

	say("e: COMPUTE: %u numbers, number[i] = 3 i + 7, written by the GPU", NUMBERS);
	info.stage.module = shader(pvr_vk_test_compute, sizeof(pvr_vk_test_compute), "e: the compute shader's module");
	if (info.stage.module == VK_NULL_HANDLE ||
	    !STEP("e: the descriptor set's layout", vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout))) {
		goto out;
	}
	layout_info.pSetLayouts = &set_layout;
	set_info.pSetLayouts = &set_layout;
	if (!STEP("e: the pipeline layout", vkCreatePipelineLayout(device, &layout_info, NULL, &layout))) {
		goto out;
	}
	info.layout = layout;
	if (!BIG_STEP("e: the pipeline (the driver compiles the shader)",
	        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline)) ||
	    !STEP("e: the descriptor pool", vkCreateDescriptorPool(device, &pool_info, NULL, &pool))) {
		goto out;
	}
	set_info.descriptorPool = pool;
	if (!STEP("e: the descriptor set", vkAllocateDescriptorSets(device, &set_info, &set)) ||
	    !buffer_make(&numbers, NUMBERS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "e: the numbers")) {
		goto out;
	}
	memset(numbers.bytes, 0x5a, NUMBERS * 4);
	buffer_info.buffer = numbers.buffer;
	write.dstSet = set;
	vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
	if (!STEP("e: commands begun", vkResetCommandBuffer(commands, 0)) ||
	    !STEP("e: commands begun", vkBeginCommandBuffer(commands, &begin))) {
		goto out;
	}
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(commands, NUMBERS / 64, 1, 1);
	if (!STEP("e: commands ended", vkEndCommandBuffer(commands)) || !run("e: compute")) {
		goto out;
	}
	done = 1;
	number = numbers.bytes;
	for (unsigned i = 0; i < NUMBERS; i++) {
		wrong += number[i] != i * 3 + 7;
	}
	say("e: COMPUTE: %s: the first numbers %u %u %u %u, the last %u (expected 7 10 13 16 and %u); %u of %u are wrong; "
	    "checksum %08x", wrong == 0 ? "RIGHT" : "WRONG", number[0], number[1], number[2], number[3], number[NUMBERS - 1],
	    (NUMBERS - 1) * 3 + 7, wrong, NUMBERS, checksum(number, NUMBERS * 4));
out:
	if (!done) {
		say("e: COMPUTE: NOT DONE");
	}
	if (!abandoned) {
		buffer_drop(&numbers);
		if (pool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(device, pool, NULL);
		}
		if (pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, pipeline, NULL);
		}
		if (layout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, layout, NULL);
		}
		if (set_layout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, set_layout, NULL);
		}
		if (info.stage.module != VK_NULL_HANDLE) {
			vkDestroyShaderModule(device, info.stage.module, NULL);
		}
	}
	return done && wrong == 0;
}

int
main(int argc, char **argv)
{
	const char *library = argc > 1 ? argv[1] : "/redwood/lib/libvulkan_powervr_mesa.dylib";
	VkApplicationInfo application = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "pvr_vk_test",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application };
	VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
	VkQueueFamilyProperties families[8];
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
	};
	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	VkCommandBufferAllocateInfo commands_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	PFN_vkGetInstanceProcAddr get;
	PFN_vkCreateInstance create_instance;
	VkInstance instance = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	uint32_t count = 1, family_count = 8, family = ~0u;
	int cleared = 0, drawn = 0, computed = -1;
	pthread_t watcher;
	void *handle;

	/* (the driver is not conformant on this core and says so instead of starting, unless told) */
	setenv("PVR_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
	pthread_create(&watcher, NULL, watch, NULL);
	say("a: the driver %s ...", library);
	handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		say("a: NOT DONE: the library does not load: %s", dlerror());
		return 1;
	}
	get = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
	create_instance = get == NULL ? NULL : (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
	if (create_instance == NULL) {
		say("a: NOT DONE: the library has no vkGetInstanceProcAddr, or that no vkCreateInstance");
		return 1;
	}
	if (!BIG_STEP("a: the instance",
	        create_instance(&instance_info, NULL, &instance))) {
		say("a: NOT DONE");
		return 1;
	}
#define LOAD(name) vk##name = (PFN_vk##name)get(instance, "vk" #name);
	INSTANCE_FUNCTIONS(LOAD)
#undef LOAD
	if (!BIG_STEP("a: the GPUs (the driver finds the kernel's RedwoodPVR and asks what the GPU is)",
	        vkEnumeratePhysicalDevices(instance, &count, &gpu)) || count == 0) {
		say("a: NOT DONE: no GPU (no RedwoodPVR in the kernel -- boot-arg pvr=6 --, or the driver does not know its BVNC)");
		goto instance_out;
	}
	DO("a: what the GPU is", vkGetPhysicalDeviceProperties2(gpu, &properties));
	say("a: \"%s\", Vulkan %u.%u.%u, vendor %#x device %#x, driver \"%s\" %u.%u.%u: %s",
	    properties.properties.deviceName, VK_API_VERSION_MAJOR(properties.properties.apiVersion),
	    VK_API_VERSION_MINOR(properties.properties.apiVersion), VK_API_VERSION_PATCH(properties.properties.apiVersion),
	    properties.properties.vendorID, properties.properties.deviceID, driver.driverName,
	    VK_API_VERSION_MAJOR(properties.properties.driverVersion), VK_API_VERSION_MINOR(properties.properties.driverVersion),
	    VK_API_VERSION_PATCH(properties.properties.driverVersion), driver.driverInfo);
	vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
	for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
		say("a: memory heap %u: %llu MB, flags %#x", i, (unsigned long long)(memory.memoryHeaps[i].size >> 20),
		    memory.memoryHeaps[i].flags);
	}
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		say("a: memory type %u: of heap %u, properties %#x (1 device's, 2 visible here, 4 coherent)", i,
		    memory.memoryTypes[i].heapIndex, memory.memoryTypes[i].propertyFlags);
	}
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families);
	for (uint32_t i = 0; i < family_count; i++) {
		say("a: queue family %u: %u queues, flags %#x (1 graphics, 2 compute, 4 transfer)", i, families[i].queueCount,
		    families[i].queueFlags);
		if (family == ~0u && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			family = i;
		}
	}
	if (family == ~0u) {
		say("a: NOT DONE: no queue family draws");
		goto instance_out;
	}

	queue_info.queueFamilyIndex = family;
	if (!BIG_STEP("b: the device (contexts for rendering, compute and transfers; the free list)",
	        vkCreateDevice(gpu, &device_info, NULL, &device))) {
		say("b: NOT DONE");
		goto instance_out;
	}
#define LOAD(name) vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);
	DEVICE_FUNCTIONS(LOAD)
#undef LOAD
	vkGetDeviceQueue(device, family, 0, &queue);
	pool_info.queueFamilyIndex = family;
	if (!STEP("b: the command pool", vkCreateCommandPool(device, &pool_info, NULL, &pool))) {
		goto device_out;
	}
	commands_info.commandPool = pool;
	if (!STEP("b: the command buffer", vkAllocateCommandBuffers(device, &commands_info, &commands)) ||
	    !STEP("b: the fence", vkCreateFence(device, &fence_info, NULL, &fence))) {
		goto device_out;
	}

	cleared = clear_test(SIDE, SIDE);
	if (!abandoned && framebuffer != VK_NULL_HANDLE) {
		drawn = triangle_test();
	}
	/* the clear at other sizes: fewer tiles, more, and a picture that is not whole tiles */
	quiet = 1;
	for (unsigned i = 0; i < 6 && !abandoned; i++) {
		static const unsigned sizes[6][2] = { { 16, 16 }, { 32, 32 }, { 48, 48 }, { 128, 128 }, { 100, 36 }, { 256, 64 } };

		target_drop();
		cleared = clear_test(sizes[i][0], sizes[i][1]) && cleared;
	}
	quiet = 0;
	if (abandoned) {
		say("e: COMPUTE: not tried after a TIMEOUT");
	} else if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
		say("e: COMPUTE: the driver has none on this queue");
	} else {
		computed = compute_test();
	}

device_out:
	say("RESULT: clear (all seven sizes) %s, triangle %s, compute %s%s", cleared ? "RIGHT" : "not right", drawn ? "RIGHT" : "not right",
	    computed < 0 ? "not tried" : computed ? "RIGHT" : "not right",
	    abandoned ? "; the GPU did not answer in time, and nothing is taken down" : "");
	if (abandoned) {
		_exit(1);
	}
	step_begin("the end: everything taken down", 1);
	target_drop();
	if (fence != VK_NULL_HANDLE) {
		vkDestroyFence(device, fence, NULL);
	}
	if (pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(device, pool, NULL);
	}
	vkDestroyDevice(device, NULL);
	vkDestroyInstance(instance, NULL);
	step_end("the end: everything taken down", 1, VK_SUCCESS);
	return cleared && drawn && computed != 0 ? 0 : 1;

instance_out:
	DO("the end: the instance taken down", vkDestroyInstance(instance, NULL));
	return 1;
}
