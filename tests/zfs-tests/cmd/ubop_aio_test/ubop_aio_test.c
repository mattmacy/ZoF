#include <sys/cdefs.h>
#include <sys/errno.h>
#include <err.h>
#include <aio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

#define BUFFER_SIZE 32*1024*1024
#define BUFFER_COUNT BUFFER_SIZE/4
#define READ_WAIT_TIME 4


static int
do_aio_read(struct aiocb *iocb, int fd, void *buf, int size)
{
	int rc;

	bzero(iocb, sizeof (*iocb));
	iocb->aio_buf = buf;
	iocb->aio_nbytes = BUFFER_SIZE;
	iocb->aio_fildes = fd;
	rc = aio_read(iocb);
	if (rc < 0)
		errx(1, "aio_read failed: %s", strerror(errno));

	return (rc);
}

static int
do_aio_write(struct aiocb *iocb, int fd, void *buf, int size)
{
	int rc;

	bzero(iocb, sizeof (*iocb));
	iocb->aio_buf = buf;
	iocb->aio_nbytes = BUFFER_SIZE;
	iocb->aio_fildes = fd;
	rc = aio_write(iocb);
	if (rc < 0)
		errx(1, "aio_read failed: %s", strerror(errno));

	return (rc);
}

int
main(int argc, char **argv)
{
	char template[256];
	uint32_t *write_buffer, *read_buffer;
	struct aiocb iocb, *iocbp;
	struct timespec ts;
	int fd, rc;

	if (argc != 2)
		return (-1);
	snprintf(template, 256, "%s/ubop_aio_test.XXXXXX", argv[1]);
	write_buffer = malloc(64*1024*1024);
	read_buffer = malloc(64*1024*1024);
	
	for (int i = 0; i < BUFFER_COUNT; i++)
		write_buffer[i] = i;
	
	fd = mkstemp(template);
	write(fd, write_buffer, BUFFER_SIZE);
	fsync(fd);
	do_aio_read(&iocb, fd, read_buffer, BUFFER_SIZE);
	ts.tv_sec = READ_WAIT_TIME;
	rc = aio_waitcomplete(&iocbp, &ts);
	if (rc < 0)
		errx(1, "aio_waitcomplete failed: %s", strerror(errno));
	assert(iocbp == &iocb);
	for (int i = 0; i < BUFFER_COUNT; i++)
		assert(read_buffer[i] == i);
	printf("aio_read completed successfuly\n");

	for (int i = 0; i < BUFFER_COUNT; i++)
		write_buffer[i] = BUFFER_COUNT - i;
	do_aio_write(&iocb, fd, write_buffer, BUFFER_SIZE);
	rc = aio_waitcomplete(&iocbp, &ts);
	if (rc < 0)
		errx(1, "aio_waitcomplete failed: %s", strerror(errno));
	assert(iocbp == &iocb);

	/*
	 * validate aio_write
	 */
	do_aio_read(&iocb, fd, read_buffer, BUFFER_SIZE);
	ts.tv_sec = READ_WAIT_TIME;
	rc = aio_waitcomplete(&iocbp, &ts);
	if (rc < 0)
		errx(1, "aio_waitcomplete failed: %s", strerror(errno));
	assert(iocbp == &iocb);
	for (int i = 0; i < BUFFER_COUNT; i++)
		assert(read_buffer[i] == BUFFER_COUNT - i);

	printf("aio_write validated\n");
}
