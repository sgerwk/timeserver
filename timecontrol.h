#define KEYFILE "/dev/null"
#define TIMESERVER 45631

/*
 * message types
 */
#define NONE                0
#define REGISTER            1
#define UNREGISTER          2
#define PID                 3
#define TIMEOUT             4
#define RUN                 5
#define NOTRUNNING       1000

#define QUERY            1001
#define SLEEP            1002
#define CANCEL           1003
#define TOSERVER         2000

#define CLIENTID         2001
#define TIME             2002
#define WAKE(client)    (3000 + (client))

/*
 * in the RUN message, run up to the next client sleep or wakeup
 */
#define NEXTSLEEP -1
#define NEXTWAKE  -2

/*
 * message structure
 */
struct {
	long mtype;
	long client;
	long time;
} msg;
#define msgsize (sizeof(msg) - sizeof(long))

