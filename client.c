/*
 * client.c
 *
 * a standalone client for the timeserver
 * only for testing; for real programs:
 *	timeexec program args...
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/msg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include "timecontrol.h"

void handler(int sig) {
	(void) sig;
}

int main() {
	int queue;
	key_t key;
	int res;
	long client;

	srandom(time(NULL) + getpid());

	signal(SIGINT, handler);

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

				/* obtain client number */

	msg.mtype = REGISTER;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		perror("msgsnd");
		exit(EXIT_FAILURE);
	}
	
	res = msgrcv(queue, &msg, msgsize, CLIENTID, 0);
	if (res == -1) {
		perror("msgrcv");
		exit(EXIT_FAILURE);
	}
	client = msg.client;
	printf("client: %ld\n", client);
	if (client == -1) {
		printf("cannot register\n");
		exit(EXIT_FAILURE);
	}

				/* sleep */

	msg.mtype = SLEEP;
	msg.client = client;
	msg.time = random() % 100;
	printf("sleep(%ld)\n", msg.time);
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		perror("msgsnd");
		exit(EXIT_FAILURE);
	}
	printf("sleep\n");
	res = msgrcv(queue, &msg, msgsize, WAKE(client), 0);
	if (res == -1) {
		perror("msgrcv");
		if (errno != EINTR)
			exit(EXIT_FAILURE);
		msg.mtype = CANCEL;
		msg.client = client;
		msgsnd(queue, &msg, msgsize, 0);
		msgrcv(queue, &msg, msgsize, WAKE(client), 0);
	}
	printf("sleep done\n");

				/* query time */

	msg.mtype = QUERY;
	msg.client = client;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		perror("msgsnd");
		exit(EXIT_FAILURE);
	}
	res = msgrcv(queue, &msg, msgsize, TIME, 0);
	if (res == -1) {
		perror("msgrcv");
		exit(EXIT_FAILURE);
	}
	printf("time: %ld\n", msg.time);

				/* unregister */

	msg.mtype = UNREGISTER;
	msg.time = client;
	res = msgsnd(queue, &msg, msgsize, 0);
	if (res == -1) {
		perror("msgsnd");
		exit(EXIT_FAILURE);
	}
	
	return 0;
}

