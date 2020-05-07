CFLAGS += -Wall -Wextra -g -D_GNU_SOURCE

%: %.c
	$(CC) $(CFLAGS) -o $@ $^


ubop_io_uring_test: ubop_io_uring_test.o syscall.o setup.o queue.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) *.o ubop_io_uring_test *~
