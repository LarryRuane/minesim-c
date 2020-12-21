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

all: pttest

protothread_test.o: protothread_test.c
	gcc $(CFLAGS) -c protothread_test.c

protothread_sem.o: protothread_sem.c
	gcc $(CFLAGS) -c protothread_sem.c

protothread_lock.o: protothread_lock.c
	gcc $(CFLAGS) -c protothread_lock.c

pttest: protothread_test.o protothread_sem.o protothread_lock.o
	gcc $(CFLAGS) -o pttest protothread_test.o protothread_sem.o protothread_lock.o

test: pttest
	./pttest

clean:
	rm -f *.o pttest
