/*
 * timerun.c
 *
 * run some simulated time
 *
 * timeserver
 * timeexec program1 args
 * timeexec program2 args
 * timeexec program3 args
 * timerun 20			# run 20 seconds of simulation
 * timerun 30			# other 30
 * timerun			# run until next wakeup
 * timerun 100			# other 100 seconds of simulation
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/msg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <string.h>

#include "timecontrol.h"

/*
 * main
 */
int main(int argn, char *argv[]) {
	int queue;
	key_t key;
	int seconds;
	int res;

				/* argument */

	if (argn - 1 < 1 || ! strcmp(argv[1], "sleep"))
		seconds = NEXTSLEEP;
	else if (! strcmp(argv[1], "wake"))
		seconds = NEXTWAKE;
	else if (! strcmp(argv[1], "-h")) {
		printf("usage:\n\ttimeexec [seconds|\"sleep\"|\"wake\"|-h]\n");
		exit(EXIT_SUCCESS);
	}
	else
		seconds = atoi(argv[1]);

				/* open queue */

	key = ftok(KEYFILE, TIMESERVER);
	if (key == -1) {
		perror(KEYFILE);
		exit(EXIT_FAILURE);
	}

	queue = msgget(key, 0700);
	if (queue == -1) {
		perror("msgget");
		exit(EXIT_FAILURE);
	}

				/* run simulation */

	msg.mtype = RUN;
	msg.time = seconds;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		perror("msgsnd");
		exit(EXIT_FAILURE);
	}
	
	return 0;
}

