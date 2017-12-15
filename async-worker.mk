ASYNC_WORKER_DIR      = $(DEPS)/async-worker
ASYNC_WORKER_INCLUDES = $(ASYNC_WORKER_DIR)/include/

$(ASYNC_WORKER_DIR):
	git clone https://github.com/nodesource/async-worker.h.git $@
