/*
 * timexec.c
 *
 * calls another problem with timeclient.so as a preload library
 * before, search timeclient.so in a path
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

char *libpath = "/lib:/usr/lib:/usr/local/lib:.";
char *timeclient = "timeclient.so";

int main(int argn, char *argv[]) {
	char *dlibpath, *dir, *libname;
	int res;
	struct stat sb;

	if (argn - 1 < 1) {
		printf("no program given\n");
		printf("usage:\n\ttimeexec program args...\n");
		exit(EXIT_FAILURE);
	}

	dlibpath = strdup(libpath);
	for (dir = strtok(dlibpath, ":"); dir; dir = strtok(NULL, ":")) {
		libname = malloc(strlen(dir) + strlen(timeclient) + 10);
		strcpy(libname, dir);
		strcat(libname, "/");
		strcat(libname, timeclient);
		res = stat(libname, &sb);
		if (! res)
			break;
		free(libname);
	}
	free(dlibpath);
	if (dir == NULL) {
		printf("cannot locate %s\n", timeclient);
		exit(EXIT_FAILURE);
	}

	setenv("LD_PRELOAD", libname, 1);
	printf("LD_PRELOAD=%s\n", getenv("LD_PRELOAD"));

	return execvp(argv[1], argv + 1);
}

