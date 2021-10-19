#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <liburing.h>

#include <assert.h>

enum status {
	ACCEPT,
	READ,
	WRITE,
	CLOSE,
};

struct op_rw {
	enum status status;
	int fd;
	char buffer[64 * 1024];
	size_t offset;
	size_t end_exclusive;
};

struct op {
	enum status status;
	int fd;
};

int
main(int argc, char *argv[])
{
	int server_fd = -1;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_port = htons(8888),
		.sin_addr = {
			.s_addr = 0,
		},
	};

	assert(bind(server_fd, (struct sockaddr*)&sin, sizeof(sin)) == 0);
	assert(listen(server_fd, 32) == 0);

	struct io_uring stRing;

	assert(io_uring_queue_init(128, &stRing, 0) == 0);

	{
		struct op *accept_op = malloc(sizeof(struct op));
		accept_op->status = ACCEPT;
		accept_op->fd = server_fd;
	
		struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
		io_uring_prep_accept(sqe, server_fd, NULL, NULL, 0);
		io_uring_sqe_set_data(sqe, accept_op);
	}

	int submited = io_uring_submit(&stRing);
	printf("Submited: %d\n", submited);
	int running_task = 1;

	for (;;) {
		while (running_task > 0) {
			struct io_uring_cqe *cqe;
			int res = io_uring_wait_cqe(&stRing, &cqe);

			running_task -= 1;

			assert(res == 0);
			
			void *user_data = (void*)cqe->user_data;
			int32_t io_res = cqe->res;

			io_uring_cqe_seen(&stRing, cqe);

			enum status status = *((enum status *)user_data);

			switch (status) {
			case ACCEPT: {
				struct op *op = user_data;
				int client_fd = io_res;
				
				printf("Accept %d\n", client_fd);

				{
					struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
					io_uring_prep_accept(sqe, op->fd, NULL, NULL, 0);
					io_uring_sqe_set_data(sqe, op);
					running_task += 1;
				}

				if (client_fd >= 0) {
					struct op_rw *op_rw = malloc(sizeof(struct op_rw));

					op_rw->fd = client_fd;
					op_rw->status = READ;
					op_rw->offset = 0;
					op_rw->end_exclusive = 0;
					
					struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
					io_uring_prep_recv(sqe, client_fd, op_rw->buffer, sizeof(op_rw->buffer), 0 /* flags */);
					io_uring_sqe_set_data(sqe, op_rw);
					running_task += 1;
				}
			}
				break;
			case READ: {
				struct op_rw *op_rw = user_data;
				
				printf("Read %d\n", io_res);

				if (io_res <= 0) {
					struct op *close_op = malloc(sizeof(struct op));
					close_op->status = CLOSE;
					close_op->fd = op_rw->fd;

					struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
					io_uring_prep_close(sqe, close_op->fd);
					io_uring_sqe_set_data(sqe, close_op);
					running_task += 1;

					free(op_rw);
				} else {
					op_rw->status = WRITE;
					op_rw->end_exclusive += io_res;

					struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
					io_uring_prep_send(sqe, op_rw->fd, op_rw->buffer, op_rw->end_exclusive, 0 /* flags */);
					io_uring_sqe_set_data(sqe, op_rw);
					running_task += 1;
				}
			}
				break;
			case WRITE: {
				struct op_rw *op_rw = user_data;
				printf("Write %d\n", io_res);
				if (io_res < 0) {
					struct op *close_op = malloc(sizeof(struct op));
					close_op->status = CLOSE;
					close_op->fd = op_rw->fd;

					struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
					io_uring_prep_close(sqe, close_op->fd);
					io_uring_sqe_set_data(sqe, close_op);
					running_task += 1;

					free(op_rw);
				} else {
					op_rw->offset += io_res;

					if (op_rw->offset < op_rw->end_exclusive) {
						struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
						io_uring_prep_send(sqe, op_rw->fd, op_rw->buffer + op_rw->offset, op_rw->end_exclusive - op_rw->offset, 0 /* flags */);
						io_uring_sqe_set_data(sqe, op_rw);
						running_task += 1;
					} else {
						struct io_uring_sqe *sqe = io_uring_get_sqe(&stRing);
						op_rw->status = READ;
						op_rw->offset = 0;
						op_rw->end_exclusive = 0;

						io_uring_prep_recv(sqe, op_rw->fd, op_rw->buffer, sizeof(op_rw->buffer), 0 /* flags */);
						io_uring_sqe_set_data(sqe, op_rw);
						running_task += 1;
					}
				}
			}
				break;
			case CLOSE: {
				struct op *op = user_data;
				
				printf("Close %d\n", op->fd);

				free(op);
			}
				break;
			}
			submited = io_uring_submit(&stRing);
			printf("Submited: %d\n", submited);
		}
	}

	io_uring_queue_exit(&stRing);

	return 0;
}
