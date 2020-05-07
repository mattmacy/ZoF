#include <sys/cdefs.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <stdint.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include "liburing.h"


#define BUFFER_SIZE 32*1024*1024
#define BUFFER_COUNT BUFFER_SIZE/4
#define READ_WAIT_TIME 4
#define QUEUE_DEPTH 16

struct io_data {
    int read;
	int fd;
    off_t first_offset, offset;
    size_t first_len;
    struct iovec iov;
	struct io_uring *ring;
};


static void
io_data_init(struct io_data *data, struct io_uring *ring, int fd, void *buf, int size, off_t offset, int read)
{

	data->read = read;
	data->fd = fd;
	data->offset = data->first_offset = offset;
	data->iov.iov_base = buf;
	data->iov.iov_len = size;
	data->first_len = size;
	data->ring = ring;
}

static int
queue_op(struct io_data *data)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(data->ring);

    if (sqe == NULL) {
        return (ENXIO);
	}
	if (data->read)
		io_uring_prep_readv(sqe, data->fd, &data->iov, 1, data->offset);
	else
		io_uring_prep_writev(sqe, data->fd, &data->iov, 1, data->offset);
    io_uring_sqe_set_data(sqe, data);

	return (io_uring_submit(data->ring));
}


int
main(int argc, char **argv)
{
	char template[256];
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	uint32_t *write_buffer, *read_buffer;
	struct io_data io_data_read, io_data_write;
	int fd, rc;

	if (argc != 2)
		return (-1);
	snprintf(template, 256, "%s/ubop_aio_test.XXXXXX", argv[1]);
	write_buffer = malloc(64*1024*1024);
	read_buffer = malloc(64*1024*1024);

	if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
		errx(1, "io_uring_queue_init failed: %s", strerror(errno));
	}
	
	for (int i = 0; i < BUFFER_COUNT; i++)
		write_buffer[i] = i;
	
	fd = mkstemp(template);
	write(fd, write_buffer, BUFFER_SIZE);
	fsync(fd);
	io_data_init(&io_data_read, &ring, fd, read_buffer, BUFFER_SIZE, 0, 1 /* read */);
	queue_op(&io_data_read);
	rc = io_uring_wait_cqe(&ring, &cqe);
	assert(rc == 0);
	assert(cqe->user_data == (__u64)&io_data_read);
	io_uring_cqe_seen(&ring, cqe);

	for (uint32_t i = 0; i < BUFFER_COUNT; i++)
		assert(read_buffer[i] == i);
	printf("aio_read completed successfuly\n");

	for (int i = 0; i < BUFFER_COUNT; i++)
		write_buffer[i] = BUFFER_COUNT - i;

	io_data_init(&io_data_write, &ring, fd, write_buffer, BUFFER_SIZE, 0, 0 /* !read */);
	queue_op(&io_data_write);
	rc = io_uring_wait_cqe(&ring, &cqe);
	assert(rc == 0);
	assert(cqe->user_data == (__u64)&io_data_write);
	io_uring_cqe_seen(&ring, cqe);

	/*
	 * validate aio_write
	 */
	io_data_init(&io_data_read, &ring, fd, read_buffer, BUFFER_SIZE, 0, 1 /* read */);
	queue_op(&io_data_read);
	rc = io_uring_wait_cqe(&ring, &cqe);
	assert(rc == 0);
	assert(cqe->user_data == (__u64)&io_data_read);
	io_uring_cqe_seen(&ring, cqe);

	for (uint32_t i = 0; i < BUFFER_COUNT; i++)
		assert(read_buffer[i] == BUFFER_COUNT - i);

	printf("aio_write validated\n");
	io_uring_queue_exit(&ring);
}
