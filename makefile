# PROTOTHREAD - multicore library
# See LICENSE (LarryRuane@gmail.com)

# optional
W = \
	-Wstrict-prototypes \
	-W \
	-Wshadow \
	-Wpointer-arith \
	-Wcast-qual \
	-Winline \
	-Wall \

CFLAGS = -O0 -g -m64 $(W)

all: pttest sim

protothread.o: protothread.c protothread.h
	gcc $(CFLAGS) -c protothread.c

protothread_test.o: protothread_test.c protothread.h
	gcc $(CFLAGS) -c protothread_test.c

protothread_sem.o: protothread_sem.c protothread.h
	gcc $(CFLAGS) -c protothread_sem.c

protothread_lock.o: protothread_lock.c protothread.h
	gcc $(CFLAGS) -c protothread_lock.c

pttest: protothread.o protothread_test.o protothread_sem.o protothread_lock.o
	gcc $(CFLAGS) -o pttest protothread.o protothread_test.o protothread_sem.o protothread_lock.o

test: pttest
	./pttest

sim: sim.o protothread.o protothread.h
	gcc $(CFLAGS) -o sim protothread.o sim.o -lm

sim.o: sim.c
	gcc $(CFLAGS) -c sim.c

clean:
	rm -f *.o pttest
