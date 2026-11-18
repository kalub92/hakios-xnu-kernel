/*
 * libredwood-tweaks.dylib: the tweak loader of this port.
 *
 * A tweak is a dylib that a package manager put in /var/jb/usr/lib/TweakInject, beside a property
 * list of the same name whose Filter says which programs it belongs in. Something has to be inside
 * every program to read that directory and open the tweaks that name it; on a jailbroken phone the
 * jailbreak's own hook does it, and ElleKit ships one (ellekit/libinjector.dylib). That one opens
 * here but loads nothing and says nothing about why, so this is ours, which says what it does.
 *
 * ElleKit is still what does the work a tweak asks for: the tweaks link against
 * /var/jb/usr/lib/libsubstrate.dylib (libhooker, CydiaSubstrate), which is ElleKit's libellekit,
 * and that is the engine that replaces functions and methods. Only the finding and opening is ours.
 *
 * It goes into a program either as the one library the kernel inserts (etc/inject.conf, see
 * src/init.c) or because a library of ours that is already there opens it (src/inject_sb.c does,
 * for SpringBoard). Loading it twice is harmless: the second time does nothing.
 *
 * A Filter says any of:
 *   Bundles       the identifier of the program's main bundle (com.apple.springboard)
 *   Executables   the file name of the program (SpringBoard)
 *   Classes       an Objective-C class the program has
 * and takes the tweak if any of them matches, unless Mode is "Any", where all must. A list with no
 * Filter at all belongs in every program, as it does on a phone.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

/*
 * Both places a package may put a tweak: the one ElleKit uses now, and the one Cydia Substrate used,
 * which is a symbolic link to it on a phone -- but a package unpacked on this machine can leave it a
 * real directory, and some packages still write there. Looking in both costs nothing.
 */
static const char *const tweak_dirs[] = {
	"/var/jb/usr/lib/TweakInject",
	"/var/jb/Library/MobileSubstrate/DynamicLibraries",
};

static void
say(const char *format, ...)
{
	char line[512];
	va_list args;
	int n, fd;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n <= 0) {
		return;
	}
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
		close(fd);
	}
	/* and to the kernel's buffer for the host, which comes back whole however long the run: what was loaded where is
	 * said when SpringBoard starts, in the part of a run's log the host does not get (run 363) */
	(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strnlen(line, sizeof(line)));
}

/* The name this program was started as, and its bundle identifier when it has one yet. */
static const char *
executable_name(void)
{
	static char path[1024];
	uint32_t size = sizeof(path);
	char *slash;

	if (_NSGetExecutablePath(path, &size) != 0) {
		return NULL;
	}
	slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

static bool
bundle_identifier(char *out, size_t size)
{
	CFBundleRef bundle = CFBundleGetMainBundle();
	CFStringRef identifier;

	if (bundle == NULL) {
		return false;
	}
	identifier = CFBundleGetIdentifier(bundle);
	return identifier != NULL && CFStringGetCString(identifier, out, (CFIndex)size, kCFStringEncodingUTF8);
}

/* Does this array of strings hold `want`? */
static bool
array_has(CFArrayRef array, const char *want)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) == CFStringGetTypeID() &&
		    CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && strcmp(text, want) == 0) {
			return true;
		}
	}
	return false;
}

static bool
array_has_class(CFArrayRef array)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) == CFStringGetTypeID() &&
		    CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && objc_getClass(text) != NULL) {
			return true;
		}
	}
	return false;
}

static CFArrayRef
filter_array(CFDictionaryRef filter, const char *key)
{
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFArrayRef array = NULL;

	if (name != NULL) {
		CFTypeRef value = CFDictionaryGetValue(filter, name);

		if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID()) {
			array = value;
		}
		CFRelease(name);
	}
	return array;
}

/* Whether the list at `path` names this program. */
static bool
wanted_here(const char *path, const char *name, const char *identifier)
{
	CFDictionaryRef plist, filter;
	CFArrayRef bundles, executables, classes;
	CFStringRef mode_key, filter_key;
	CFDataRef data;
	CFTypeRef mode;
	bool all, hits[3], wanted;
	int fd, used = 0;
	uint8_t buffer[65536];
	ssize_t n;

	fd = open(path, O_RDONLY);
	n = fd >= 0 ? read(fd, buffer, sizeof(buffer)) : -1;
	if (fd >= 0) {
		close(fd);
	}
	if (n <= 0) {
		return false;
	}
	data = CFDataCreate(NULL, buffer, (CFIndex)n);
	if (data == NULL) {
		return false;
	}
	plist = (CFDictionaryRef)CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		if (plist != NULL) {
			CFRelease(plist);
		}
		say("redwood: tweaks: %s is not a property list\n", path);
		return false;
	}
	filter_key = CFSTR("Filter");
	filter = CFDictionaryGetValue(plist, filter_key);
	if (filter == NULL || CFGetTypeID(filter) != CFDictionaryGetTypeID()) {
		CFRelease(plist);                       /* no Filter: it belongs everywhere */
		return true;
	}
	bundles = filter_array(filter, "Bundles");
	executables = filter_array(filter, "Executables");
	classes = filter_array(filter, "Classes");
	hits[0] = bundles != NULL && identifier != NULL && array_has(bundles, identifier);
	hits[1] = executables != NULL && name != NULL && array_has(executables, name);
	hits[2] = classes != NULL && array_has_class(classes);
	/* Mode "Any" means every kind that is given has to match; by default one is enough. */
	mode_key = CFSTR("Mode");
	mode = CFDictionaryGetValue(filter, mode_key);
	all = mode != NULL && CFGetTypeID(mode) == CFStringGetTypeID() &&
	    CFStringCompare(mode, CFSTR("Any"), 0) == kCFCompareEqualTo;
	used = (bundles != NULL) + (executables != NULL) + (classes != NULL);
	if (used == 0) {
		wanted = true;
	} else if (all) {
		wanted = (bundles == NULL || hits[0]) && (executables == NULL || hits[1]) && (classes == NULL || hits[2]);
	} else {
		wanted = hits[0] || hits[1] || hits[2];
	}
	CFRelease(plist);
	return wanted;
}

static void
load_the_tweaks(void)
{
	char identifier[256];
	const char *name = executable_name();
	const char *who = bundle_identifier(identifier, sizeof(identifier)) ? identifier : NULL;
	int opened = 0, tried = 0;

	for (unsigned d = 0; d < sizeof(tweak_dirs) / sizeof(tweak_dirs[0]); d++) {
		const char *where = tweak_dirs[d];
		struct dirent *entry;
		struct stat st;
		DIR *dir;

		/* the second is usually a link to the first: then it is the same directory, done already */
		if (d > 0 && lstat(where, &st) == 0 && S_ISLNK(st.st_mode)) {
			continue;
		}
		dir = opendir(where);
		if (dir == NULL) {
			continue;                       /* no prefix, or no tweaks in it */
		}
		while ((entry = readdir(dir)) != NULL) {
			char plist[1024], dylib[1024];
			size_t length = strlen(entry->d_name);

			if (length < 7 || strcmp(entry->d_name + length - 6, ".plist") != 0) {
				continue;
			}
			snprintf(plist, sizeof(plist), "%s/%s", where, entry->d_name);
			snprintf(dylib, sizeof(dylib), "%s/%.*s.dylib", where, (int)(length - 6), entry->d_name);
			if (!wanted_here(plist, name, who)) {
				continue;
			}
			tried++;
			if (dlopen(dylib, RTLD_NOW | RTLD_GLOBAL) != NULL) {
				opened++;
				say("redwood: tweaks: %s\n", dylib);
			} else {
				say("redwood: tweaks: %s: %s\n", dylib, dlerror());
			}
		}
		closedir(dir);
	}
	if (tried > 0) {
		say("redwood: tweaks: %s (%s): %d of %d opened\n", name != NULL ? name : "?",
		    who != NULL ? who : "no bundle", opened, tried);
	}
}

/* Settings aborted (userabort, 2026-11-17) from an uncaught Objective-C exception: say which, to the host's log */
static void
exception_seen(CFTypeRef exception)
{
	char name[160] = "?", reason[300] = "?";
	CFTypeRef n = ((CFTypeRef (*)(CFTypeRef, SEL))objc_msgSend)(exception, sel_registerName("name"));
	CFTypeRef r = ((CFTypeRef (*)(CFTypeRef, SEL))objc_msgSend)(exception, sel_registerName("reason"));

	if (n != NULL) {
		CFStringGetCString(n, name, sizeof(name), kCFStringEncodingUTF8);
	}
	if (r != NULL) {
		CFStringGetCString(r, reason, sizeof(reason), kCFStringEncodingUTF8);
	}
	say("redwood: tweaks: %s uncaught exception %s: %s\n", executable_name(), name, reason);
	CFTypeRef a = ((CFTypeRef (*)(CFTypeRef, SEL))objc_msgSend)(exception, sel_registerName("callStackReturnAddresses"));
	if (a != NULL) {
		char line[400] = "";
		size_t n_addr = (size_t)CFArrayGetCount(a), o = 0;
		for (size_t i = 0; i < n_addr && i < 24 && o < sizeof(line) - 20; i++) {
			unsigned long long v = ((unsigned long long (*)(CFTypeRef, SEL))objc_msgSend)(CFArrayGetValueAtIndex(a, (CFIndex)i), sel_registerName("unsignedLongLongValue"));
			o += (size_t)snprintf(line + o, sizeof(line) - o, " 0x%llx", v);
		}
		say("redwood: tweaks: exception stack:%s\n", line);
	}
}

/*
 * Settings builds the Wallet row from PKIconImageCache's icon for com.apple.Passbook, which this system does not have:
 * the icon is nil, the row's dictionary literal throws NSInvalidArgumentException (objects[3] = "iconImage") and Settings
 * aborts (set0..set5, 2026-11-17). Leave out the pairs with a nil key or object, as an app that tolerates a missing icon
 * would (the row then has no icon). Settings only.
 */
static id (*real_init_dict)(id, SEL, const id *, const id *, unsigned long);

static id
init_dict(id self, SEL cmd, const id *objects, const id *keys, unsigned long count)
{
	id o[64], k[64];
	unsigned long n = 0;

	if (count > 64) {
		return real_init_dict(self, cmd, objects, keys, count);
	}
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] != NULL && keys[i] != NULL) {
			o[n] = objects[i];
			k[n++] = keys[i];
		}
	}
	return real_init_dict(self, cmd, o, k, n);
}

static void
tolerate_nil_in_dictionaries(void)
{
	Class c = objc_getClass("__NSPlaceholderDictionary");
	Method m = c != NULL ? class_getInstanceMethod(c, sel_registerName("initWithObjects:forKeys:count:")) : NULL;

	if (m != NULL && strcmp(executable_name(), "Preferences") == 0) {
		real_init_dict = (id (*)(id, SEL, const id *, const id *, unsigned long))method_setImplementation(m, (IMP)init_dict);
		say("redwood: tweaks: Preferences: dictionaries drop nil pairs\n");
	}
}

/* Settings dies of SIGSEGV (san2, 2026-11-17): fault address, pc, lr and a frame-pointer walk, each as image+offset (and the
 * nearest exported symbol), to the kept host log; then the default action. Preferences only. */
static void
say_addr(char *line, size_t size, int *used, uintptr_t a)
{
	Dl_info di;

	if (dladdr((void *)a, &di) != 0 && di.dli_fname != NULL) {
		const char *base = strrchr(di.dli_fname, '/');
		*used += snprintf(line + *used, size - (size_t)*used, " %s+0x%lx(%s)", base != NULL ? base + 1 : di.dli_fname,
		    (unsigned long)(a - (uintptr_t)di.dli_fbase), di.dli_sname != NULL ? di.dli_sname : "?");
	} else {
		*used += snprintf(line + *used, size - (size_t)*used, " 0x%lx", (unsigned long)a);
	}
}

static void
fault_handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	char line[500];
	int used = snprintf(line, sizeof(line), "redwood: tweaks: %s FAULT signal %d addr %p pc", executable_name(), sig, info->si_addr);

	say_addr(line, sizeof(line), &used, (uintptr_t)uc->uc_mcontext->__ss.__pc);
	used += snprintf(line + used, sizeof(line) - (size_t)used, " lr");
	say_addr(line, sizeof(line), &used, (uintptr_t)uc->uc_mcontext->__ss.__lr);
	say("%s\n", line);
	used = snprintf(line, sizeof(line), "redwood: tweaks: fault frames:");
	for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used < (int)sizeof(line) - 90; i++) {
		say_addr(line, sizeof(line), &used, ((uintptr_t *)fp)[1] & 0xfffffffffULL);
		fp = ((uintptr_t *)fp)[0];
	}
	say("%s\n", line);
	signal(sig, SIG_DFL);
}

static void
install_fault_handler(void)
{
	struct sigaction sa;

	if (strcmp(executable_name(), "Preferences") != 0) {
		return;
	}
	static char altstack[65536];                    /* a stack overflow leaves no stack to run the handler on (main thread only) */
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0 };

	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fault_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	say("redwood: tweaks: Preferences: fault handler installed\n");
}

/*
 * The root cause of the Settings crash (setC, 2026-11-17): this system has no Wallet app, so Settings' Wallet row asks
 * MobileIcons for the icon of com.apple.Passbook, MobileIcons makes a default icon with CoreImage (LICreateDefaultIcon ->
 * CoreImage's macro context) and calls a NULL function pointer (SIGSEGV, pc 0). -[PSUIPrefsListController specifiers] adds the row
 * only when passbookSpecifier returns non-nil, so returning nil is the stock way to have no Wallet row on a device without Wallet.
 */
static id
no_passbook_specifier(id self, SEL cmd)
{
	return NULL;
}

/*
 * ... and the block of -[PSUIPrefsListController _insertOrRemovePaymentSpecifierAsNeededCompletion:] (run after PassKit's answers, which
 * come without passd) asserts passbookSpecifier non-nil when it decides to insert the row (userabort, setD: NSAssert, PreferencesUI+0x366b8):
 * it does nothing here, and says "nothing changed" (NO, NO) to its completion block (a block: invoke pointer at +0x10).
 */
static void
no_payment_specifier_update(id self, SEL cmd, void *completion)
{
	if (completion != NULL) {
		((void (*)(void *, bool, bool))(*(void **)((char *)completion + 0x10)))(completion, false, false);
	}
}

static void
hide_wallet_row(void)
{
	Class c = objc_getClass("PSUIPrefsListController");
	Method m = c != NULL ? class_getInstanceMethod(c, sel_registerName("passbookSpecifier")) : NULL;

	if (m != NULL && strcmp(executable_name(), "Preferences") == 0) {
		method_setImplementation(m, (IMP)no_passbook_specifier);
		m = class_getInstanceMethod(c, sel_registerName("_insertOrRemovePaymentSpecifierAsNeededCompletion:"));
		if (m != NULL) {
			method_setImplementation(m, (IMP)no_payment_specifier_update);
		}
		say("redwood: tweaks: Preferences: no Wallet row (payment hook %s)\n", m != NULL ? "on" : "MISSING");
	} else if (strcmp(executable_name(), "Preferences") == 0) {
		say("redwood: tweaks: Preferences: passbookSpecifier NOT found (class %p)\n", (void *)c);
	}
}

/*
 * CoreImage guard (placeholder icons, 2026-11-17): a default CIContext finds no Metal device and falls back to
 * +[CIContext internalGLContextWithOptions:], over Apple's own EAGLContext, which has no GL dispatch table
 * (getMacroContextPrivate NULL) and then calls a zeroed glDisable slot. Refuse the fallback (nil, which
 * -initWithOptions: handles) when there is no macro context. On by default in every process that loads this library; ci_guard=0 off.
 */
static id (*real_gl_context)(id, SEL, id);

static id
guarded_gl_context(id self, SEL cmd, id options)
{
	static bool said;
	Class eagl = objc_getClass("EAGLContext");
	SEL macro = sel_registerName("getMacroContextPrivate");
	id probe = eagl != NULL ? ((id (*)(id, SEL))objc_msgSend)((id)eagl, sel_registerName("alloc")) : NULL;
	bool ok;

	if (probe != NULL) {
		probe = ((id (*)(id, SEL, unsigned long))objc_msgSend)(probe, sel_registerName("initWithAPI:"), 2);
	}
	ok = probe != NULL && class_respondsToSelector(object_getClass(probe), macro) &&
	    ((void *(*)(id, SEL))objc_msgSend)(probe, macro) != NULL;
	if (probe != NULL) {
		((void (*)(id, SEL))objc_msgSend)(probe, sel_registerName("release"));
	}
	if (!ok) {
		if (!said) {
			said = true;
			say("redwood: tweaks: CIContext: no OpenGL ES macro context, GL fallback refused (nil)\n");
		}
		return NULL;
	}
	return real_gl_context(self, cmd, options);
}

static void
guard_coreimage(void)
{
	char args[512] = "";
	size_t size = sizeof(args) - 1;
	Class c;
	Method m;
	const char *enc;

	bool on;

	/* default on in every process that loads this library (Preferences, iconservicesagent, SpringBoard); ci_guard=0 turns it off */
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		args[0] = 0;
	}
	on = strstr(args, "ci_guard=0") == NULL;
	if (!on) {
		return;
	}
	dlopen("/System/Library/Frameworks/CoreImage.framework/CoreImage", RTLD_NOW);
	c = objc_getClass("CIContext");
	m = c != NULL ? class_getClassMethod(c, sel_registerName("internalGLContextWithOptions:")) : NULL;
	enc = m != NULL ? method_getTypeEncoding(m) : NULL;
	/* (the method returns the context as an object or as a raw pointer: "@24@0:8@16" or "^v24@0:8@16"; either is a pointer return, nil = NULL) */
	if (m == NULL || enc == NULL || (strncmp(enc, "@24@0:8@16", 10) != 0 && strncmp(enc, "^v24@0:8@16", 11) != 0)) {
		say("redwood: tweaks: %s: CIContext guard off (%s %s)\n", executable_name(), c == NULL ? "no CIContext" : m == NULL ? "no method" : "type", enc != NULL ? enc : "");
		return;
	}
	real_gl_context = (id (*)(id, SEL, id))method_setImplementation(m, (IMP)guarded_gl_context);
	say("redwood: tweaks: CIContext guard on\n");
}

__attribute__((constructor))
static void
redwood_tweaks_init(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	void (*set_handler)(void (*)(CFTypeRef)) = dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
	if (set_handler != NULL) {
		set_handler(exception_seen);
	}
	tolerate_nil_in_dictionaries();
	hide_wallet_row();
	guard_coreimage();
	install_fault_handler();
	load_the_tweaks();
}
