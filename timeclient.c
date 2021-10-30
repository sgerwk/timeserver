/*
 * timeclient.c
 *
 * run a program under timserver
 *
 * timeexec program args
 *
 * is the same as:
 * LD_PRELOAD=./timeclient.so program args
 * where timeclient.so is from this file
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/msg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "timecontrol.h"

/*
 * number of queue and client, log file
 */
int queue;
long client;
char logfile[1000];
char *timeclient;

/*
 * logging
 *
 * cannot be done with regular io stream operations, and especially not opening
 * a file only once in the constructor, since the application may close file
 * descriptors at will; this is what cronie does: before running a task, it
 * closes all file descriptors > stderr but does not fclose their FILE*, making
 * fopen/fprintf unusable
 *
 * since multiple processes may write concurrently, this function should be
 * regulated by a semaphore; but O_APPEND seems enough for now
 *
 * the log file has to be rw-rw-rw because cron may run processes as regular
 * users, and the log file is initially owned by root
 */
int logprintf(char *fmt, ...) {
	mode_t m;
	int fd;
	char line[500];
	va_list va;

	m = umask(0);
	fd = open(logfile,
	          O_WRONLY | O_CREAT | O_APPEND,
	          S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (fd == -1) {
		perror(logfile);
		return -1;
	}
	umask(m);

	va_start(va, fmt);
	vsnprintf(line, 500, fmt, va);
	write(fd, line, strlen(line));
	va_end(va);

	close(fd);
	return 0;
}

/*
 * original system calls
 */
unsigned int (* sleep_orig)(unsigned int seconds);
int (* nanosleep_orig)(const struct timespec *req, struct timespec *rem);
time_t (*time_orig)(time_t *tloc);
int (* gettimeofday_orig)(struct timeval *restrict tp, void *restrict tzp);
int (* clock_gettime_orig)(clockid_t clock_id, struct timespec *tp);

pid_t (* fork_orig)(void);
void (* _exit_orig)(int status);
void (* exit_group_orig)(int status);
int (* execve_orig)(const char *filename, char *const argv[],
	char *const envp[]);
int (* execle_orig)(const char *filename, const char *arg, ...);

/*
 * new system calls for time
 */

void cancel() {
	int res;

	logprintf("%d: cancel()\n", getpid());

	msg.mtype = CANCEL;
	msg.client = client;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		logprintf("\tmsgsnd: %s\n", strerror(errno));
		return;
	}

	/* here we really need the wakeup message before returning, since the
	 * program may then sleep again; it will wake up immediately if we had
	 * not consumed the reply to the cancel message */
	do {
		res = msgrcv(queue, &msg, msgsize, WAKE(client), 0);
		if (res == -1)
			logprintf("\tmsgrcv: %s\n", strerror(errno));
	} while (res == -1 && errno == EINTR);
}

unsigned int sleep(unsigned int seconds) {
	int res;
	long start, left;
	pid_t pid;

	pid = getpid();
	logprintf("%d: sleep(%u)\n", pid, seconds);

	start = time(NULL);

	msg.mtype = SLEEP;
	msg.client = client;
	msg.time = seconds;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		logprintf("\tmsgsnd: %s\n", strerror(errno));
		logprintf("\tsleep_orig(%d)\n", seconds);
		return sleep_orig(seconds);
	}

	res = msgrcv(queue, &msg, msgsize, WAKE(client), 0);
	if (res == -1) {
		logprintf("%d:\t\tsleep, msgrcv: %s\n", pid, strerror(errno));

		if (errno != EINTR)
			return sleep_orig(seconds);
		
		cancel();
		left = start + seconds - time(NULL);
		logprintf("%d:\t\tsleep, left: %d\n", pid, left);
		return left;
	}

	logprintf("%d: woken(%u): %ld\n", pid, seconds, msg.time);

	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
	int res;

	logprintf("%d: nanosleep(%d,...)\n", getpid(), req->tv_sec);

	res = sleep(req->tv_sec);
	if (res == 0)
		return 0;

	if (rem != NULL) {
		rem->tv_sec = res;
		rem->tv_nsec = 121;
	}
	return -1;
}

time_t time(time_t *tloc) {
	int res;
	pid_t pid;

	pid = getpid();
	logprintf("%d: time()\n", pid);

	msg.mtype = QUERY;
	msg.client = client;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		logprintf("%d:\t\ttime, msgrcv: %s\n", pid, strerror(errno));
		return time_orig(tloc);
	}
	res = msgrcv(queue, &msg, msgsize, TIME, 0);
	if (res == -1) {
		logprintf("%d:\t\ttime, msgrcv: %s\n", pid, strerror(errno));
		return time_orig(tloc);
	}

	logprintf("%d: time(): %ld\n", pid, msg.time);

	return msg.time;
}

int gettimeofday(struct timeval *restrict tp, void *restrict tzp) {
	time_t t;

	t = time(NULL);

	if (tp) {
		tp->tv_sec = t;
		tp->tv_usec = 12;
	}
	if (tzp) {
		/* unspecified behavior, this is */
		(void) tzp;
	}

	return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
	time_t t;

	(void) clock_id;

	t = time(NULL);
	tp->tv_sec = t;
	tp->tv_nsec = 1234;

	return 0;
}

/*
 * client registration and unregistration
 */

void registerclient() {
	key_t key;
	int res;
	pid_t pid;

	pid = getpid();
	logprintf("%d: registerclient()\n", pid);

				/* open queue */

	key = ftok(KEYFILE, TIMESERVER);
	if (key == -1) {
		logprintf("%d:\t\t%s: %s\n", pid, KEYFILE, strerror(errno));
		queue = -1;
		return;
	}

	queue = msgget(key, 0700);
	if (queue == -1) {
		logprintf("%d:\t\tmsgget: %s\n", pid, strerror(errno));
		return;
	}

				/* request client number */

	msg.mtype = REGISTER;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		logprintf("%d:\t\tmsgsnd: %s\n", pid, strerror(errno));
		return;
	}

				/* obtain client id */

	res = msgrcv(queue, &msg, msgsize, CLIENTID, 0);
	if (res == -1) {
		logprintf("%d:\t\tmsgrcv: %s\n", pid, strerror(errno));
		return;
	}
	client = msg.client;
	logprintf("%d: client(): %ld\n", getpid(), client);
	if (client == -1) {
		logprintf("%d:\t\tcannot register\n", pid);
		return;
	}

				/* send pid */

	msg.mtype = PID;
	msg.client = client;
	msg.time = getpid();
	msgsnd(queue, &msg, msgsize, 0);
}

void unregisterclient() {
	int res;
	pid_t pid;

	if (queue == -1)
		return;

	pid = getpid();
	logprintf("%d: unregister(%ld)\n", pid, client);

	msg.mtype = UNREGISTER;
	msg.client = client;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		logprintf("%d:\t\tmsgsnd: %s\n", pid, strerror(errno));
		return;
	}
}

/*
 * process-handling system calls
 */

pid_t fork(void) {
	pid_t ret;
	logprintf("%d: fork()\n", getpid());
	ret = fork_orig();
	if (ret == 0) {
		logprintf("%d: child\n", getpid());
		registerclient();
	}
	return ret;
}

void _exit(int status) {
	logprintf("%d: _exit(%d)\n", getpid(), status);
	unregisterclient();
	_exit_orig(status);
	_exit(status);			/* avoid warning */
}

void exit_group(int status) {
	logprintf("%d: exit_group(%d)\n", getpid(), status);
	unregisterclient();
	exit_group_orig(status);
	exit_group(status);		/* avoid warning */
}

int str2cmp(char *a, char *b) {
	if (a == NULL)
		return -1;
	if (strlen(a) < strlen(b))
		return -1;
	return memcmp(a, b, strlen(b));
}

int execve(const char *filename, char *const argv[],
                  char *const envp[]) {
	int i;
	char **newenvp;
	char ldpreload[1020], logfilename[1020];
	int oldld, oldlog;
	int res;

	logprintf("%d: execve(%s,...)\n", getpid(), filename);
	for (i = 0; i == 0 || argv[i - 1]; i++)
		logprintf("\targv[%d]: %s\n", i, argv[i]);
	logprintf("\t------------\n");

	oldld = 0;
	oldlog = 0;
	for (i = 0; i == 0 || envp[i - 1]; i++) {
		logprintf("\tenvp[%d]: %s\n", i, envp[i]);
		if (! str2cmp(envp[i], "LD_PRELOAD="))
			oldld = 1;
		if (! str2cmp(envp[i], "TIMECLIENTLOGFILE="))
			oldlog = 1;
	}
	logprintf("\t------------\n");

	/* add LD_PRELOAD again, since the application may call
	 * execve() with an arbitrary environment */

	newenvp = malloc((i + 2) * sizeof(char *));
	memcpy (newenvp, envp, i * sizeof(char *));
	snprintf(ldpreload, 1020, "LD_PRELOAD=%s", timeclient);
	snprintf(logfilename, 1020, "TIMECLIENTLOGFILE=%s", logfile);
	i--;
	if (! oldld)
		newenvp[i++] = ldpreload;
	if (! oldlog)
		newenvp[i++] = logfilename;
	newenvp[i++] = NULL;

	for (i = 0; i == 0 || newenvp[i - 1]; i++)
		logprintf("\tnewenvp[%d]: %s\n", i, newenvp[i]);

	/* cannot keep client_id across an execve();
	 * just unregister for now; if execve() fails, register again */

	unregisterclient();
	res = execve_orig(filename, argv, newenvp);
	registerclient();
	return res;
}

/* for some reason, redefining execve only is not enough */
int execle(const char *path, const char *arg, ...) {
	va_list ap;
	char *s;
	char **envp;
	char **res, **argv;
	int argn;
	int err;

	logprintf("%d: execle(%s,...)\n", getpid(), path);

	argn = 1;
	argv = malloc((argn + 1) * sizeof(char *));
	argv[argn - 1] = (char *) arg;
	va_start(ap, arg);
	for (argn++; NULL != (s = va_arg(ap, char *)); argn++) {
		res = realloc(argv, (argn + 1) * sizeof(char *));
		if (res == NULL) {
			va_end(ap);
			free(argv);
			errno = ENOMEM;
			return -1;
		}
		argv = res;
		argv[argn - 1] = s;
	}
	argv[argn - 1] = NULL;
	envp = va_arg(ap, char **);
	va_end(ap);

	execve(path, argv, envp);
	err = errno;
	free(argv);
	errno = err;
	return -1;
}

/*
 * constructor and destructor
 */

static void __attribute__((constructor)) init() {
	char *ldpreload, *envlogfile, cwd[1000];

	ldpreload = getenv("LD_PRELOAD");
	if (ldpreload[0] != '.')
		timeclient = strdup(ldpreload);
	else {
		getcwd(cwd, 1000);
		timeclient = malloc(strlen(cwd) + strlen(ldpreload) + 2);
		sprintf(timeclient, "%s/%s", cwd, ldpreload);
	}

	envlogfile = getenv("TIMECLIENTLOGFILE");
	if (envlogfile == NULL)
		snprintf(logfile, 1000, "/tmp/timeclient.%d", getpid());
	else
		snprintf(logfile, 1000, "%s", envlogfile);

	sleep_orig = dlsym(RTLD_NEXT, "sleep");
	nanosleep_orig = dlsym(RTLD_NEXT, "nanosleep");
	time_orig = dlsym(RTLD_NEXT, "time");
	gettimeofday_orig = dlsym(RTLD_NEXT, "gettimeofday");
	clock_gettime_orig = dlsym(RTLD_NEXT, "clock_gettime_orig");

	fork_orig = dlsym(RTLD_NEXT, "fork");
	_exit_orig = dlsym(RTLD_NEXT, "_exit");
	exit_group_orig = dlsym(RTLD_NEXT, "exit_group");
	execve_orig = dlsym(RTLD_NEXT, "execve");
	execle_orig = dlsym(RTLD_NEXT, "execle");

	registerclient();
}

static void __attribute__((destructor)) fini() {
	unregisterclient();
}

