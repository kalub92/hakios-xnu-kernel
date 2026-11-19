/*
 * psgrep WORD...: the processes whose path has one of the words in it (case ignored), one line each
 * -- pid, parent, state and flags, and path -- and how many processes there are in all. For a job's output kept by
 * tolog: ps -A is longer than the kernel keeps (96 lines), and the job runner has no shell to grep.
 * psgrep -stack WORD...: and each one's threads, by the kernel (kern.redwood.ustack), with the shared cache's slide.
 * psgrep -sysctl NAME [TIMES]: a string sysctl, TIMES times a second apart (iOS has no sysctl command).
 * psgrep -sample SECS TIMES WORD (only with the boot-arg proc_sample=1): TIMES times, SECS apart, the first process whose path has
 *   WORD in it: its CPU time against the wall clock (kept line), the busy threads' stacks (console, kern.redwood.ustack with bit 30),
 *   and the LaunchServices store files under /private/var (kept lines). docs/app-launch-latency.md, lsd's 60 s at every boot.
 * psgrep -cpu SECONDS: the ten processes that used the most CPU in the next SECONDS, as % of one core.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* libSystem's, without the header, which the iOS SDK leaves out */
int proc_pidpath(int pid, void *buffer, uint32_t size);
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
#define PROC_PIDPATHINFO_MAXSIZE 4096

/* the shared cache's slide in this process, the same in every one: what to take off the addresses kern.redwood.ustack says */
extern const void *_dyld_get_shared_cache_range(size_t *length);

static struct kinfo_proc *
all_processes(unsigned *count)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0;
	struct kinfo_proc *procs;

	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || (procs = malloc(size += 16 * sizeof(*procs))) == NULL ||
	    sysctl(mib, 3, procs, &size, NULL, 0) != 0) {
		perror("psgrep: kern.proc.all");
		exit(1);
	}
	*count = (unsigned)(size / sizeof(*procs));
	return procs;
}

static uint64_t
cpu_time(pid_t pid)
{
	struct rusage_info_v0 usage;

	if (proc_pid_rusage(pid, RUSAGE_INFO_V0, (rusage_info_t *)&usage) != 0) {
		return 0;
	}
	return usage.ri_user_time + usage.ri_system_time;
}

/* -cpu SECONDS: each process's CPU time before and after, the biggest differences said */
static int
cpu_top(int seconds)
{
	enum { MOST = 512 };
	static pid_t pids[MOST];
	static uint64_t before[MOST], used[MOST];
	static char names[MOST][17];
	unsigned count, n = 0;
	struct kinfo_proc *procs = all_processes(&count);
	mach_timebase_info_data_t base;
	uint64_t total = 0;

	for (unsigned i = 0; i < count && n < MOST; i++, n++) {
		pids[n] = procs[i].kp_proc.p_pid;
		strlcpy(names[n], procs[i].kp_proc.p_comm, sizeof(names[n]));
		before[n] = cpu_time(pids[n]);
	}
	free(procs);
	sleep((unsigned)seconds);
	mach_timebase_info(&base);
	for (unsigned i = 0; i < n; i++) {
		uint64_t after = cpu_time(pids[i]);

		used[i] = after > before[i] ? (after - before[i]) * base.numer / base.denom : 0;      /* ns */
		total += used[i];
	}
	printf("psgrep: CPU in %d s: %.0f%% of one core in all\n", seconds, total / (seconds * 1e7));
	for (int k = 0; k < 10; k++) {
		unsigned best = 0;

		for (unsigned i = 1; i < n; i++) {
			if (used[i] > used[best]) {
				best = i;
			}
		}
		if (used[best] == 0) {
			break;
		}
		printf("  %5.1f%% %s (%d)\n", used[best] / (seconds * 1e7), names[best], pids[best]);
		used[best] = 0;
	}
	return 0;
}

/* -sample: see the top. Nothing at all unless the kernel's arguments have proc_sample=1. */
static int
sample(int seconds, int times, const char *word)
{
	static const char *const dirs[] = { "/private/var/mobile/Library/Caches", "/private/var/db", "/private/var/mobile/Library/Caches/com.apple.LaunchServices" };
	char args[1024] = "", line[200];
	size_t size = sizeof(args) - 1;
	mach_timebase_info_data_t base;
	uint64_t t0 = mach_absolute_time();

	sysctlbyname("kern.bootargs", args, &size, NULL, 0);
	if (strstr(args, "proc_sample=1") == NULL) {
		return 0;
	}
	mach_timebase_info(&base);
	{
		static const char *const apps[] = { "/Applications", "/System/Library/CoreServices", "/private/var/jb/Applications", "/private/var/containers/Bundle/Application" };

		for (unsigned d = 0; d < sizeof(apps) / sizeof(apps[0]); d++) {
			DIR *dir = opendir(apps[d]);
			struct dirent *e;
			int n = 0, bundles = 0;

			while (dir != NULL && (e = readdir(dir)) != NULL) {
				n++;
				bundles += strstr(e->d_name, ".app") != NULL;
			}
			if (dir != NULL) {
				closedir(dir);
			}
			snprintf(line, sizeof(line), "!sample: %s: %d entries, %d .app", apps[d], n, bundles);
			sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
		}
	}
	static pid_t prev_pid[512];
	static uint64_t prev_cpu[512];
	unsigned prev_n = 0;
	uint64_t prev_t = 0;

	for (int i = 0; i < times; i++) {
		unsigned count;
		struct kinfo_proc *procs = all_processes(&count);
		pid_t pid = 0;
		int arg;
		/* the three biggest CPU users since the last tick, whoever they are (is it lsd alone, or the daemons it waits on?) */
		{
			uint64_t now = mach_absolute_time(), best[3] = { 0, 0, 0 };
			char names[3][17] = { "", "", "" };
			pid_t bp[3] = { 0, 0, 0 };
			static pid_t cur_pid[512];
			static uint64_t cur_cpu[512];
			unsigned n = 0;

			for (unsigned k = 0; k < count && n < 512; k++) {
				uint64_t c = cpu_time(procs[k].kp_proc.p_pid), before = 0;

				cur_pid[n] = procs[k].kp_proc.p_pid;
				cur_cpu[n++] = c;
				for (unsigned q = 0; q < prev_n; q++) {
					if (prev_pid[q] == cur_pid[n - 1]) {
						before = prev_cpu[q];
					}
				}
				c = c > before && prev_t != 0 ? c - before : 0;
				for (int b = 0; b < 3; b++) {
					if (c > best[b]) {
						for (int m = 2; m > b; m--) {
							best[m] = best[m - 1];
							bp[m] = bp[m - 1];
							memcpy(names[m], names[m - 1], 17);
						}
						best[b] = c;
						bp[b] = procs[k].kp_proc.p_pid;
						strlcpy(names[b], procs[k].kp_proc.p_comm, 17);
						break;
					}
				}
			}
			if (prev_t != 0) {
				double wall = (double)(now - prev_t) * base.numer / base.denom;

				snprintf(line, sizeof(line), "!sample top: %s %.0f%%, %s %.0f%%, %s %.0f%% of one core", names[0], best[0] * base.numer / base.denom * 100 / wall, names[1],
				    best[1] * base.numer / base.denom * 100 / wall, names[2], best[2] * base.numer / base.denom * 100 / wall);
				sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
			}
			memcpy(prev_pid, cur_pid, n * sizeof(pid_t));
			memcpy(prev_cpu, cur_cpu, n * sizeof(uint64_t));
			prev_n = n;
			prev_t = now;
		}

		for (unsigned k = 0; k < count && pid == 0; k++) {
			char path[PROC_PIDPATHINFO_MAXSIZE] = "";

			if (proc_pidpath(procs[k].kp_proc.p_pid, path, sizeof(path)) > 0 && strstr(path, word) != NULL) {
				pid = procs[k].kp_proc.p_pid;
			}
		}
		free(procs);
		snprintf(line, sizeof(line), "!sample +%llu s: %s pid %d cpu %llu ms slide %#lx", (unsigned long long)((mach_absolute_time() - t0) * base.numer / base.denom / 1000000000ULL),
		    word, pid, (unsigned long long)(cpu_time(pid) * base.numer / base.denom / 1000000ULL), (unsigned long)((uintptr_t)_dyld_get_shared_cache_range(&size) - 0x180000000UL));
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
		if (pid != 0) {
			arg = pid | 0x40000000;
			sysctlbyname("kern.redwood.ustack", NULL, NULL, &arg, sizeof(arg));
		}
		for (unsigned d = 0; (i % 5 == 0 || i + 1 == times) && d < sizeof(dirs) / sizeof(dirs[0]); d++) {
			DIR *dir = opendir(dirs[d]);
			struct dirent *e;

			while (dir != NULL && (e = readdir(dir)) != NULL) {
				if (strstr(e->d_name, "LaunchServices") != NULL || strstr(e->d_name, "csstore") != NULL || strstr(e->d_name, "lsd") != NULL) {
					struct stat st;
					char path[600];

					snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
					if (stat(path, &st) == 0) {
						snprintf(line, sizeof(line), "!sample: %.150s %lld bytes", path, (long long)st.st_size);
						sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
					}
				}
			}
			if (dir != NULL) {
				closedir(dir);
			}
		}
		if (i + 1 < times) {
			sleep((unsigned)seconds);
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int stacks = argc > 1 && strcmp(argv[1], "-stack") == 0;       /* -stack: and the threads of each, by the kernel */

	if (argc > 4 && strcmp(argv[1], "-sample") == 0) {
		return sample(atoi(argv[2]), atoi(argv[3]), argv[4]);
	}
	if (argc > 2 && strcmp(argv[1], "-cpu") == 0) {
		return cpu_top(atoi(argv[2]) > 0 ? atoi(argv[2]) : 10);
	}
	if (argc > 2 && strcmp(argv[1], "-sysctl") == 0) {
		/* -sysctl NAME [TIMES]: a string sysctl said TIMES times a second apart (iOS has no sysctl command) */
		int times = argc > 3 ? atoi(argv[3]) : 1;

		for (int i = 0; i < times; i++) {
			char value[1024] = "";
			size_t size = sizeof(value) - 1;

			if (sysctlbyname(argv[2], value, &size, NULL, 0) != 0) {
				perror(argv[2]);
				return 1;
			}
			if (size == sizeof(int)) {
				printf("%s: %#x\n", argv[2], *(int *)value);     /* (an integer one) */
			} else {
				/* (in pieces: a kept line holds about 200 characters) */
				size_t length = strlen(value);

				for (size_t at = 0; at == 0 || at < length; at += 180) {
					printf("%s%s: %.180s\n", argv[2], at ? " (more)" : "", value + at);
				}
			}
			fflush(stdout);
			if (i + 1 < times) {
				sleep(1);
			}
		}
		return 0;
	}

	if (stacks) {
		size_t length = 0;

		printf("psgrep: the shared cache's slide %#lx\n", (unsigned long)((uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL));
		argv++;
		argc--;
	}
	unsigned count, shown = 0;
	struct kinfo_proc *procs = all_processes(&count);

	for (unsigned i = 0; i < count; i++) {
		char path[PROC_PIDPATHINFO_MAXSIZE] = "";
		pid_t pid = procs[i].kp_proc.p_pid;

		if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
			strlcpy(path, procs[i].kp_proc.p_comm, sizeof(path));
		}
		for (int w = 1; w < argc; w++) {
			const char *at;
			size_t n = strlen(argv[w]);

			for (at = path; *at && strncasecmp(at, argv[w], n) != 0; at++) {
			}
			if (*at) {
				/* p_stat: 1 being made, 2 running, 3 asleep, 4 STOPPED, 5 a zombie */
				printf("%d (parent %d, state %d, flags %#x) %s\n", pid, procs[i].kp_eproc.e_ppid,
				    procs[i].kp_proc.p_stat, procs[i].kp_proc.p_flag, path);
				shown++;
				if (stacks) {
					fflush(stdout);
					if (sysctlbyname("kern.redwood.ustack", NULL, NULL, &pid, sizeof(pid)) != 0) {
						perror("psgrep: kern.redwood.ustack");
					}
				}
				break;
			}
		}
	}
	printf("psgrep: %u of %u processes\n", shown, count);
	return 0;
}
