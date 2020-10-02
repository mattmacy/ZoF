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


int
main(int argc, char **argv)
{
	char template[256];
	uint32_t *buffer, *aio_buffer;
	struct aiocb iocb, *iocbp;
	struct timespec ts;
	int fd, rc;

	if (argc != 2)
		return (-1);
	snprintf(template, 256, "%s/ubop_aio_test.XXXXXX", argv[1]);
	buffer = malloc(64*1024*1024);
	aio_buffer = malloc(64*1024*1024);
	
	for (int i = 0; i < BUFFER_COUNT; i++)
		buffer[i] = i;
	
	fd = mkstemp(template);
	write(fd, buffer, BUFFER_SIZE);
	fsync(fd);

	bzero(&iocb, sizeof (iocb));
	iocb.aio_buf = aio_buffer;
	iocb.aio_nbytes = BUFFER_SIZE;
	iocb.aio_fildes = fd;
	iocb.aio_offset = 0;

	rc = aio_read(&iocb);
	if (rc < 0)
		errx(1, "aio_read failed: %s", strerror(errno));
	ts.tv_sec = READ_WAIT_TIME;
	rc = aio_waitcomplete(&iocbp, &ts);
	if (rc < 0)
		errx(1, "aio_waitcomplete failed: %s", strerror(errno));
	assert(iocbp == &iocb);
	for (int i = 0; i < BUFFER_COUNT; i++)
		assert(aio_buffer[i] == i);
}
