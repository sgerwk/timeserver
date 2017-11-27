PROGS=timeserver timerun timeexec timeclient.so example client

CFLAGS=-g -Wall -Wextra -fPIC

all: $(PROGS)

%.so: %.o
	ld -o $@ -ldl -shared $<

clean:
	rm -f $(PROGS) *.o

