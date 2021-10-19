#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <liburing.h>
#include <assert.h>

#define STACK_SIZE	(1024 * 1024)

typedef struct async_context {
	ucontext_t ucontext;
	struct io_uring *uring;
	ucontext_t *main_context;
	bool is_finished;
} async_context_t;

static void
init_context(async_context_t **context, struct io_uring *uring, ucontext_t *main_context)
{
	async_context_t *tmp_context = malloc(sizeof(async_context_t) + STACK_SIZE);

	getcontext(&tmp_context->ucontext);
	tmp_context->ucontext.uc_stack.ss_sp = (void*)&tmp_context[1];
	tmp_context->ucontext.uc_stack.ss_size = STACK_SIZE;
	tmp_context->ucontext.uc_link = main_context;

	tmp_context->uring = uring;
	tmp_context->main_context = main_context;
	tmp_context->is_finished = false;

	*context = tmp_context;
}

static void
worker(async_context_t *context, int fd)
{
	for (;;) {
		char buffer[64 * 1024];
		size_t offset = 0;
		size_t len = 0;
		{
			struct io_uring_sqe *sqe = io_uring_get_sqe(context->uring);
			io_uring_prep_recv(sqe, fd, buffer, sizeof(buffer), 0 /* flags */);
			io_uring_sqe_set_data(sqe, context);
		}

		swapcontext(&context->ucontext, context->main_context);
		{
			struct io_uring_cqe *cqe = NULL;
			io_uring_peek_cqe(context->uring, &cqe);
			int io_res = cqe->res;
			io_uring_cqe_seen(context->uring, cqe);

			if (io_res <= 0) {
				break;
			}
			len += io_res;
		}

		while (len > 0) {
			{
				struct io_uring_sqe *sqe = io_uring_get_sqe(context->uring);
				io_uring_prep_send(sqe, fd, &buffer[offset], len, 0 /* flags */);
				io_uring_sqe_set_data(sqe, context);
			}

			swapcontext(&context->ucontext, context->main_context);

			{
				struct io_uring_cqe *cqe = NULL;
				io_uring_peek_cqe(context->uring, &cqe);
				int io_res = cqe->res;
				io_uring_cqe_seen(context->uring, cqe);

				if (io_res < 0) {
					goto end;
				}

				offset += io_res;
				len -= io_res;
			}
		}
	}

end:

	{
		struct io_uring_sqe *sqe = io_uring_get_sqe(context->uring);
		io_uring_prep_close(sqe, fd);
		io_uring_sqe_set_data(sqe, context);
	}

	swapcontext(&context->ucontext, context->main_context);

	{
			struct io_uring_cqe *cqe = NULL;
			io_uring_peek_cqe(context->uring, &cqe);
			io_uring_cqe_seen(context->uring, cqe);
	}
	context->is_finished = 1;
}

static void
accepter(async_context_t *context)
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

#define DO_ACCEPT() \
		{ \
			struct io_uring_sqe *sqe = io_uring_get_sqe(context->uring); \
			io_uring_prep_accept(sqe, server_fd, NULL, NULL, 0); \
			io_uring_sqe_set_data(sqe, context); \
		} while (0)

	for (;;) {
		DO_ACCEPT();

		swapcontext(&context->ucontext, context->main_context);

		struct io_uring_cqe *cqe = NULL;
		io_uring_peek_cqe(context->uring, &cqe);
		int sd = cqe->res;

		io_uring_cqe_seen(context->uring, cqe);

		if (sd > 0) {
			DO_ACCEPT();

			async_context_t *work_context;
			init_context(&work_context, context->uring, context->main_context);
			makecontext(&work_context->ucontext, (void (*)())worker, 2, work_context, sd);
			swapcontext(&context->ucontext, &work_context->ucontext);
		}
	}
}

int
main(int argc, char *argv[])
{
	struct io_uring stRing;

	assert(io_uring_queue_init(128, &stRing, 0) == 0);

	ucontext_t main_context = {};

	{
		async_context_t *accept_context = NULL;
		init_context(&accept_context, &stRing, &main_context);
		makecontext(&accept_context->ucontext, (void (*)())accepter, 1, accept_context);
		swapcontext(&main_context, &accept_context->ucontext);
	}

	for (;;) {
		struct io_uring_cqe *cqe = NULL;

		io_uring_submit(&stRing);

		io_uring_wait_cqe(&stRing, &cqe);

		async_context_t *context = io_uring_cqe_get_data(cqe);

		swapcontext(&main_context, &context->ucontext);

		if (context->is_finished) {
			free(context);
		}
	}

	return 0;
}
