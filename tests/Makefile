CC ?= cc
PYTHON ?= python3
CPPFLAGS ?= -I../src/common
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic

.PHONY: all check clean

all: test_requeue_engine

test_requeue_engine: test_requeue_engine.c ../src/common/requeue_engine.c \
                     ../src/common/requeue_engine.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_requeue_engine.c \
		../src/common/requeue_engine.c -o $@

check: test_requeue_engine
	./test_requeue_engine

clean:
	rm -f test_requeue_engine
