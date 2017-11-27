/*
 * timeserver.c
 *
 * maintain a simulated time for clients that call sleep(), nanosleep(), time()
 * or gettimeofday(), redirected by timeclient.c; the simulated time is
 * controlled by timerun.c
 *
 * -t origin
 *	starting time of the simulation in seconds since epoch, or "now";
 *	default is 0
 *
 * -i microseconds
 *	after this number of microseconds of client inactivity, jump to the
 *	next wakeup time; if no wakeup is programmed jump to the end of the
 *	run; default is 50000; see also -j
 *
 * -j seconds
 *	in case of client inactivity, increase time by this number of seconds,
 *	rather than jumping to the next wakeup time
 *
 * -b num
 *	allow busywaiting by increasing time at each query by one second with
 *	probability 1/num; default is 2, and 0 disables this increase
 *
 * -f
 *	assume that clients do not fork() and do not execve() other programs:
 *	if all clients are sleeping and the ending time of the simulation has
 *	not been reached, jump to the next wakeup time; this speeds up
 *	simulation, but does not work in general (see README)
 *
 * example:
 *
 * timeserver
 * timeexec program1 args
 * timeexec program2 args
 * timeexec program3 args
 * timerun 20			# run 20 seconds of simulation
 * timerun			# run until next wakeup time
 * timerun 30			# run 30 seconds of simulation
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/msg.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "timecontrol.h"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

/*
 * interrupts are used only to stop msgrcv
 */
int timeout;
int terminated;
void handler(int s) {
	// printf("signal: %d\n", s);

	if (s == SIGALRM && ! terminated)
		timeout = 1;
	else
		terminated = 1;
}

/*
 * database of clients
 */

#define MAXCLIENTS 200
int numclients;
int numsleeping;
long clients[MAXCLIENTS];
long pids[MAXCLIENTS];

#define EMPTY 0
#define RUNNING 1
#define SLEEPING 2

void clients_init() {
	int c;
	for (c = 0; c < MAXCLIENTS; c++)
		clients[c] = EMPTY;
	numclients = 0;
	numsleeping = 0;
}

int clients_register() {
	int c;
	for (c = 0; c < MAXCLIENTS; c++)
		if (clients[c] == EMPTY) {
			clients[c] = RUNNING;
			pids[c] = 0;
			return c;
		}
	return -1;
}

void clients_unregister(int c) {
	if (c >= 0 && c < MAXCLIENTS)
		clients[c] = EMPTY;
}

/*
 * remove clients that no longer exists (clients killed by signals)
 */
void clients_check(int queue) {
	int i;
	long c;

	for (i = 0; i < MAXCLIENTS; i++) {
		c = clients[i];

		if (c == EMPTY || pids[i] == 0)
			continue;

		if (kill(pids[i], 0) == 0 || errno != ESRCH)
			continue;

		while (-1 != msgrcv(queue, &msg, msgsize, WAKE(c), IPC_NOWAIT))
			;
		if (c >= SLEEPING)
			numsleeping--;
		clients[i] = EMPTY;
		numclients--;
	}
}

/*
 * client of first wakeup time
 */
long clients_next() {
	long c, min;

	min = -1;

	for (c = 0; c < MAXCLIENTS; c++)
		if (clients[c] >= SLEEPING &&
		    (min == -1 || clients[c] < clients[min]))
		    	min = c;

	return min;
}

/*
 * print time
 */
void printtime(long origin, long now) {
	char line[30];
	long cur;
	struct tm *date;

	if (origin != 0) {
		cur = origin + now;
		date = localtime(&cur);
		strftime(line, 25, "%F %T", date);
		printf("%-25s", line);
	}

	printf("%-9ld", now);
}

/*
 * main
 *
 * all time variables are in number of seconds, always starting from 0 even if
 * -t is given; this option only provides an offset of all time sent to client,
 * but is otherwise internally ignored
 *
 * now		current time in the simulation
 * end		end of the current simulation run
 *		the run is over when now == end
 *		if end < 0 the run end depends on what the clients do:
 *		NEXTSLEEP: end when one of the clients sleeps or unregister
 *		NEXTWAKE: end when one of the clients wakes
 * client[c] - SLEEPING
 *		if >=0, is the wakeup time for client c
 *		client is woken when now > this
 *
 * interaction with clients is via a message queue; during a simulation run
 * (when now < end), messages are read from the queue with a timeout; this way,
 * if non-sleeping tasks are busy with other operations, some else is woken
 * (see README)
 */
int main(int argn, char *argv[]) {
	int opt;
	int idletime, idlejump, busywait, nofork;
	int queue;
	key_t key;
	int res, err;
	struct itimerval timer;
	long client;
	long origin, now, end;
	char line[200];

				/* arguments */

	origin = 0;
	idletime = 50000;
	idlejump = -1;
	busywait = 2;
	nofork = 0;
	while (-1 != (opt = getopt(argn, argv, "t:i:j:b:fh")))
		switch (opt) {
		case 't':
			origin = ! strcmp(optarg, "now") ?
				time(NULL) : atol(optarg);
			break;
		case 'i':
			idletime = atol(optarg);
			break;
		case 'j':
			idlejump = atol(optarg);
			break;
		case 'b':
			busywait = atoi(optarg);
			break;
		case 'f':
			nofork = 1;
			break;
		case 'h':
			printf("usage:...\n");
			break;
		}
	srandom(time(NULL) + getpid());

				/* create the message queue */

	key = ftok(KEYFILE, TIMESERVER);
	if (key == -1) {
		perror(KEYFILE);
		exit(EXIT_FAILURE);
	}

	queue = msgget(key, IPC_CREAT | 0700);
	if (queue == -1) {
		perror("msgget");
		exit(EXIT_FAILURE);
	}

				/* signal handlers */

	signal(SIGINT, handler);
	signal(SIGTERM, handler);
	signal(SIGALRM, handler);

				/* init simulation */

	now = 0;
	end = now;

	clients_init();

	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 0;
	timer.it_value.tv_sec = 0;
	terminated = 0;

	printf("%s%-9s %-8s %-15s %-10s\n",
	       origin == 0 ? "" : "date                     ",
	       "seconds", "client", "command", "result");

				/* main loop */

	while (! terminated) {

				/* receive message */

		timeout = 0;

		if (now >= end && end >= 0) {
			/* simulation run ended, not yet (re)started:
			 * do not read messages related to time */
			res = msgrcv(queue, &msg, msgsize, -NOTRUNNING, 0);
			err = errno;
		}

		else if (nofork && numclients == numsleeping) {
			/* non-forking clients are all sleeping: jump to next
			 * wakeup time or to the end of the simulation run */
			res = 0;
			msg.mtype = TIMEOUT;
		}

		else {
			/* simulation is running: wait for some time for
			 * messages from the client */
			timer.it_value.tv_usec = idletime;
			setitimer(ITIMER_REAL, &timer, NULL);
			res = msgrcv(queue, &msg, msgsize, -TOSERVER, 0);
			err = errno;
			timer.it_value.tv_usec = 0;
			setitimer(ITIMER_REAL, &timer, NULL);
		}

		if (res == -1 && err == EINTR && timeout && ! terminated)
			msg.mtype = TIMEOUT;
		else if (res == -1)
			break;

		printtime(origin, now);

				/* process message */

		switch (msg.mtype) {

		case NONE:
			printf(" %-8s %-15s", "", "none()");
			break;

		case REGISTER:
			printf(" %-8s %-15s", "", "register()");

			clients_check(queue);

			client = clients_register();
			if (client == -1) {
				printf(" %-10s", "cannot register\n");
				terminated = 1;
			}
			else {
				printf(" id=%ld", client);

				msg.mtype = CLIENTID;
				msg.client = client;
				msg.time = origin + now;
				res = msgsnd(queue, &msg, msgsize, 0);
				if (res == -1) {
					perror("msgsnd");
					break;
				}
				numclients++;
			}
			break;

		case UNREGISTER:
			printf(" %-8ld %-15s", msg.client, "unregister()");

			clients_unregister(msg.client);
			numclients--;

			if (end == NEXTSLEEP) {
				end = now;
				printf(" end=%ld", end);
			}
			break;

		case PID:
			printf(" %-8ld", msg.client);
			sprintf(line, "pid(%ld)", msg.time);
			printf(" %-15s", line);

			pids[msg.client] = msg.time;
			break;

		case TIMEOUT:
			printf(" %-8s", "");
			printf(" %-15s", res ? "timeout()" : "jump()");

			clients_check(queue);
			client = clients_next();

			if (idlejump != -1) {
				now += idlejump;
				if (now >= end && end >= 0)
					now = end;
				if (client != -1 &&
				    now > clients[client] - SLEEPING + 1)
					now = clients[client] - SLEEPING + 1;
			}
			else {
				if (client != -1 &&
				    (clients[client] - SLEEPING < end ||
				     end < 0))
					now = clients[client] - SLEEPING + 1;
				else if (end >= 0)
					now = end;
				else if (nofork)
					end = now;
			}

			printf(" now=%ld end=%ld", now, end);

			break;

		case RUN:
			printf(" %-8s", "");
			sprintf(line, "run(%ld)", msg.time);
			printf(" %-15s", line);

			end = msg.time < 0 ? msg.time : end + msg.time;

			printf(" end=%ld", end);
			break;

		case QUERY:
			client = msg.client;

			printf(" %-8ld", client);
			printf(" %-15s", "query()");

			msg.mtype = TIME;
			msg.client = client;
			msg.time = origin + now;
			msgsnd(queue, &msg, msgsize, 0);
			if (busywait && random() % busywait == 0)
				now++;
			break;

		case SLEEP:
			client = msg.client;

			printf(" %-8ld", client);
			sprintf(line, "sleep(%ld)", msg.time);
			printf(" %-15s", line);

			clients[client] = SLEEPING + now + msg.time - 1;
			printf(" wakeup=%ld", clients[client] - SLEEPING + 1);
			numsleeping++;

			if (end == NEXTSLEEP) {
				end = now;
				printf(" end=%ld", end);
			}
			break;

		case CANCEL:
			client = msg.client;

			printf(" %-8ld", client);
			printf(" %-15s", "cancel()");
			printf(" wakeup(%ld)", client);

			msg.mtype = WAKE(client);
			msg.client = client;
			msg.time = origin + now;
			msgsnd(queue, &msg, msgsize, 0);

			if (clients[client] >= SLEEPING)
				numsleeping--;
			clients[client] = RUNNING;
			break;

		default:
			printf("unknown mtype: %ld\n", msg.mtype);
		}

		printf("\n");

				/* wake clients */

		for (client = 0; client < MAXCLIENTS; client++) {
			if (clients[client] < SLEEPING)
				continue;
			if (clients[client] - SLEEPING >= now)
				continue;

			printtime(origin, now);
			printf(" %-8s %-15s", "", "");
			printf(" wake(%ld)", client);

			msg.mtype = WAKE(client);
			msg.client = client;
			msg.time = origin + now;
			msgsnd(queue, &msg, msgsize, 0);

			clients[client] = RUNNING;
			numsleeping--;

			if (end == NEXTWAKE) {
				end = now + 1;
				printf(" end=%ld", end);
			}
			printf("\n");
		}
	}

				/* remove queue */

	res = msgctl(queue, IPC_RMID, NULL);
	if (res == -1) {
		perror("msgctl");
		exit(EXIT_FAILURE);
	}

				/* summary */

	printtime(origin, now);
	printf(" %-8s %-15s", "", "quit()");
	printf(" registered=%d sleeping=%d\n", numclients, numsleeping);

	return 0;
}

