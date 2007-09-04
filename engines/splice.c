/*
 * splice engine
 *
 * IO engine that transfers data by doing splices to/from pipes and
 * the files.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/poll.h>

#include "../fio.h"

#ifdef FIO_HAVE_SPLICE

struct spliceio_data {
	int pipe[2];
	int vmsplice_to_user;
};

/*
 * vmsplice didn't use to support splicing to user space, this is the old
 * variant of getting that job done. Doesn't make a lot of sense, but it
 * uses splices to move data from the source into a pipe.
 */
static int fio_splice_read_old(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops->data;
	struct fio_file *f = io_u->file;
	int ret, ret2, buflen;
	off_t offset;
	void *p;

	offset = io_u->offset;
	buflen = io_u->xfer_buflen;
	p = io_u->xfer_buf;
	while (buflen) {
		int this_len = buflen;

		if (this_len > SPLICE_DEF_SIZE)
			this_len = SPLICE_DEF_SIZE;

		ret = splice(f->fd, &offset, sd->pipe[1], NULL, this_len, SPLICE_F_MORE);
		if (ret < 0) {
			if (errno == ENODATA || errno == EAGAIN)
				continue;

			return -errno;
		}

		buflen -= ret;

		while (ret) {
			ret2 = read(sd->pipe[0], p, ret);
			if (ret2 < 0)
				return -errno;

			ret -= ret2;
			p += ret2;
		}
	}

	return io_u->xfer_buflen;
}

/*
 * We can now vmsplice into userspace, so do the transfer by splicing into
 * a pipe and vmsplicing that into userspace.
 */
static int fio_splice_read(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops->data;
	struct fio_file *f = io_u->file;
	struct iovec iov;
	int ret, buflen;
	off_t offset;
	void *p;

	offset = io_u->offset;
	buflen = io_u->xfer_buflen;
	p = io_u->xfer_buf;
	io_u->xfer_buf = NULL;
	while (buflen) {
		int this_len = buflen;

		if (this_len > SPLICE_DEF_SIZE)
			this_len = SPLICE_DEF_SIZE;

		ret = splice(f->fd, &offset, sd->pipe[1], NULL, this_len, SPLICE_F_MORE);
		if (ret < 0) {
			if (errno == ENODATA || errno == EAGAIN)
				continue;

			return -errno;
		}

		buflen -= ret;
		iov.iov_base = p;
		iov.iov_len = ret;
		p += ret;

		while (iov.iov_len) {
			ret = vmsplice(sd->pipe[0], &iov, 1, SPLICE_F_MOVE);
			if (ret < 0)
				return -errno;
			else if (!ret)
				return -ENODATA;

			if (!io_u->xfer_buf)
				io_u->xfer_buf = iov.iov_base;
			iov.iov_len -= ret;
			iov.iov_base += ret;
		}
	}

	io_u->unmap = splice_unmap_io_u;
	return io_u->xfer_buflen;
}

/*
 * For splice writing, we can vmsplice our data buffer directly into a
 * pipe and then splice that to a file.
 */
static int fio_splice_write(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops->data;
	struct iovec iov = {
		.iov_base = io_u->xfer_buf,
		.iov_len = io_u->xfer_buflen,
	};
	struct pollfd pfd = { .fd = sd->pipe[1], .events = POLLOUT, };
	struct fio_file *f = io_u->file;
	off_t off = io_u->offset;
	int ret, ret2;

	while (iov.iov_len) {
		if (poll(&pfd, 1, -1) < 0)
			return errno;

		ret = vmsplice(sd->pipe[1], &iov, 1, SPLICE_F_NONBLOCK);
		if (ret < 0)
			return -errno;

		iov.iov_len -= ret;
		iov.iov_base += ret;

		while (ret) {
			ret2 = splice(sd->pipe[0], NULL, f->fd, &off, ret, 0);
			if (ret2 < 0)
				return -errno;

			ret -= ret2;
		}
	}

	return io_u->xfer_buflen;
}

static void splice_unmap_io_u(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops->data;
	struct iovec iov = {
		.iov_base = io_u->xfer_buf,
		.iov_len = io_u->xfer_buflen,
	};

	vmsplice(sd->pipe[0], &iov, 1, SPLICE_F_UNMAP);
}

static int fio_spliceio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops->data;
	int ret;

	if (io_u->ddir == DDIR_READ) {
		if (sd->vmsplice_to_user) {
			ret = fio_splice_read(td, io_u);
			/*
			 * This kernel doesn't support vmsplice to user
			 * space. Reset the vmsplice_to_user flag, so that
			 * we retry below and don't hit this path again.
			 */
			if (ret == -EBADF)
				sd->vmsplice_to_user = 0;
		}
		if (!sd->vmsplice_to_user)
			ret = fio_splice_read_old(td, io_u);
	} else if (io_u->ddir == DDIR_WRITE)
		ret = fio_splice_write(td, io_u);
	else
		ret = fsync(io_u->file->fd);

	if (ret != (int) io_u->xfer_buflen) {
		if (ret >= 0) {
			io_u->resid = io_u->xfer_buflen - ret;
			io_u->error = 0;
			return FIO_Q_COMPLETED;
		} else
			io_u->error = errno;
	}

	if (io_u->error)
		td_verror(td, io_u->error, "xfer");

	return FIO_Q_COMPLETED;
}

static void fio_spliceio_cleanup(struct thread_data *td)
{
	struct spliceio_data *sd = td->io_ops->data;

	if (sd) {
		close(sd->pipe[0]);
		close(sd->pipe[1]);
		free(sd);
		td->io_ops->data = NULL;
	}
}

static int fio_spliceio_init(struct thread_data *td)
{
	struct spliceio_data *sd = malloc(sizeof(*sd));

	if (pipe(sd->pipe) < 0) {
		td_verror(td, errno, "pipe");
		free(sd);
		return 1;
	}

	/*
	 * Assume this work, we'll reset this if it doesn't
	 */
	sd->vmsplice_to_user = 1;

	td->io_ops->data = sd;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "splice",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_spliceio_init,
	.queue		= fio_spliceio_queue,
	.cleanup	= fio_spliceio_cleanup,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.flags		= FIO_SYNCIO,
};

#else /* FIO_HAVE_SPLICE */

/*
 * When we have a proper configure system in place, we simply wont build
 * and install this io engine. For now install a crippled version that
 * just complains and fails to load.
 */
static int fio_spliceio_init(struct thread_data fio_unused *td)
{
	fprintf(stderr, "fio: splice not available\n");
	return 1;
}

static struct ioengine_ops ioengine = {
	.name		= "splice",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_spliceio_init,
};

#endif

static void fio_init fio_spliceio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_spliceio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
