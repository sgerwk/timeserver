/*
 * example.c
 *
 * test time() and sleep() under timeserver
 *
 * timeexec example
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

int main(int argn, char *argv[]) {
	int seconds;
	time_t t, start;
	int i;

	if (argn - 1 >= 1)
		seconds = atoi(argv[1]);
	else {
		srandom(time(NULL) + getpid());
		seconds = random() % 100;
	}

	t = time(NULL);
	start = t;
	printf("start: %ld\n", start);

	printf("sleeping %d seconds\n", seconds);
	sleep(seconds);

	i = 0;
	do {
		t = time(NULL);
		i++;
	} while(t < start + seconds + 8);
	printf("busy wait iterations: %d\n", i);
	return 0;
}

