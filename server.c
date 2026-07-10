#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <libpq-fe.h>

// clang server.c -o run -Wall -Wextra -lpthread && ./run
// gcc -o run -Wall -Wextra -lpthread -lpq server.c && ./run
// Test with
// echo "fff\t1" | nc localhost 3003

#define PORT 3002
#define THREAD_POOL_SIZE 4
#define QUEUE_MAX_SIZE 16

// This queue is implemented as a ring-buffer.
// Producer: Main thread. Accepts new incoming requests and writes the socket to the tail.
// Consumers: Worker threads in the thread-pool. Consume client-sockets from the head and process them.
typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t cond_var;
	int count;
	int head;
	int tail;
	int client_sockets[QUEUE_MAX_SIZE];
} queue_ringbuffer_t;

queue_ringbuffer_t client_socket_queue = {
	PTHREAD_MUTEX_INITIALIZER,
	PTHREAD_COND_INITIALIZER,
	0, 0, 0, {0}
};

void
queue_push(queue_ringbuffer_t* queue, int socket) {
	// Mutex and Cond work as a pair.
	// Get lock on queue
	pthread_mutex_lock(&queue->mutex);
	// noop if queue is full
	if (queue->count >= QUEUE_MAX_SIZE) {
		printf("Queue full! Dropping connection!\n");
		close(socket);
		pthread_mutex_unlock(&queue->mutex);
		return;
	}
	// Write into queue-tail, incr tail, incr count.
	queue->count++;
	queue->client_sockets[queue->tail] = socket;
	// Because we want it to overflow back to 0, because ringbuffer.
	queue->tail = (queue->tail+1) % QUEUE_MAX_SIZE;
	// Wake up one thread
	pthread_cond_signal(&queue->cond_var);
	// Remove lock on queue
	pthread_mutex_unlock(&queue->mutex);
}

int
queue_pop(queue_ringbuffer_t* queue) {
	pthread_mutex_lock(&queue->mutex);
	// Handle spurious wakeups because OSs do that.
	while (queue->count < 1) {
		pthread_cond_wait(
				&queue->cond_var,
				&queue->mutex
				);
	}
	int out = queue->client_sockets[queue->head];
	queue->head = (queue->head + 1) % QUEUE_MAX_SIZE; // % because ringbuffer.
	queue->count--;
	pthread_mutex_unlock(&queue->mutex);
	return out;
}
// END INT QUEUE implementation

typedef struct {
	char k[31];
	char v[51];
} Param;

// Return value is "ok".
int
fillGetParams(Param* getParams, char* endpoint) {
	char* qmark = strchr(endpoint, '?');
	if (qmark == NULL) {
		printf("FF");
		return 1;
	}
	char* after_qmark = qmark+1;
	printf("Query string:%s\n", after_qmark);
	
	int paramPairCount = 20;
	char* paramPairSavePtr;
	char* splitEqualSavePtr;
	int count = 0;
	char* paramPair = strtok_r(after_qmark, "&", &paramPairSavePtr);

	while (paramPair != NULL) {
		char* k_or_v = strtok_r(paramPair, "=", &splitEqualSavePtr);
		Param* param = &getParams[count];
		strncpy(param->k, k_or_v, 20);
		k_or_v = strtok_r(NULL, "=", &splitEqualSavePtr);
		strncpy(param->v, k_or_v, 50);

		++count;
		if (count == paramPairCount) {
			// We allocated only this much.
			break;
		}
		paramPair = strtok_r(NULL, "&", &paramPairSavePtr);
	}
	printf("GET param count:%d\n", count);
	return 1;
}

// This function is called from a threadpool worker, to handle the request.
void*
handle_request(int client_socket) {
	char buf[2048];
	int bytes_read = read(client_socket, buf, 2047);
	buf[bytes_read] = 0;
	char* response = NULL;
	// TODO The above needs to be freed right now. But we wouldn't have to bother with that once we
	// switch to thread-local vars since then the memory will be reused for each request. So much simpler.
	Param* getParams = calloc(20, sizeof(Param));
	for (int i = 0; i<20; ++i) {
		getParams[i].k[0]=0;
		getParams[i].v[0]=0;
	}

	if (bytes_read == 2047) {
		char* msg = "Request too large. Max is 2KB.";
		asprintf( &response, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}

	if (bytes_read <= 0) {
		char* msg = "Request too small. You sent nothing.";
		asprintf( &response, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}

	printf("Received\n%s\n", buf);

	// Get first line. Max 512B.
	char* line_end = strstr(buf, "\r\n");
	if (line_end == NULL) {
		char* msg = "Couldn't identify the first header. Fix your request.";
		asprintf( &response, "HTTP/1.1 422 Unprocessable Entity\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}
	size_t line_len = line_end - buf;
	if (line_len > 512) {
		char* msg = "Request endpoint too large. We aren't parsing over 512B.";
		asprintf( &response, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}
	// TODO This should be an array that is global, and then write to the entry that is for
	// this particular thread-index. This thread should know it's own index.
	char* first_line = calloc(513, 1);
	// Using calloc because I chooose to believe the things I read on the internet.
	// https://vorpus.org/blog/why-does-calloc-exist/
	strncpy(first_line, buf, line_len);
	printf("First line:%s\n", first_line);

	// Parse line-1
	char* http_method = calloc(8,1);
	char* endpoint = calloc(256, 1);
	char* http_version = calloc(16,1);
	int sscanf_result = sscanf(first_line, "%7s %255s %15s", http_method, endpoint, http_version);
	if (sscanf_result != 3) {
		char* msg = "Failed to parse HTTP line 1. Fix your request.";
		asprintf( &response, "HTTP/1.1 422 Unprocessable entry\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}

	printf("HTTP METHOD:%s\n", http_method);
	printf("Endpoint:%s\n", endpoint);

	// List all get params
	int ok = fillGetParams(getParams, endpoint);
	if (!ok) {
		char* msg = "Couldn't parse GET params.";
		asprintf( &response, "HTTP/1.1 422 Unprocessable entry\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}
	for (int i = 0; i<20; ++i) {
		Param param = getParams[i];
		if (param.k[0] == 0) { break; }
		printf("Param %d: k:%s v:%s\n", i, param.k, param.v);
	}
	// List all post params
	// Identify the route
	// Switch on route
	// Collect response string from route-handler
	// Return response to client

	response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
	write(client_socket, response, strlen(response));
	return NULL;
}

void*
threadpool_worker(void* arg) {
	int thread_idx = *((int*)arg);
	free(arg);
	// Each thread gets it's own connection because these conns are not thread-safe.
	PGconn* db = PQconnectdb(""); // Read from ENV
	if (PQstatus(db) != CONNECTION_OK) {
		printf("DB connection failed:%s\n", PQerrorMessage(db));
		PQfinish(db);
		printf("Killing thread\n");
		return NULL;
	}
	printf("DB conn made by thread:%d.\n", thread_idx);
	while (1) {
		// Blocking call
		int client_socket = queue_pop(&client_socket_queue);
		handle_request(client_socket);
	}
	PQfinish(db);
	return NULL;
}

int
listen_on_port() {
	int server_fd;
	struct sockaddr_in address;
	int opt = 1;
	server_fd = socket(AF_INET, SOCK_STREAM, 0);

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(PORT);

	bind(server_fd, (struct sockaddr*)&address, sizeof(address));
	listen(server_fd, 100);
	printf("Server listening on port %d...\n", PORT);

	return server_fd;
}

int
main() {

	// Set up thread pool
	// param pool_size uint
	// param threadpool_worker func
	// Right now the worker knows which queue-datastructure to use, an cond_var, and mutex. We should
	// pass that in in the future.
	pthread_t thread_pool[THREAD_POOL_SIZE];
	for (int i = 0; i<THREAD_POOL_SIZE; i++) {
		int* thread_idx = malloc(sizeof(int));
		*thread_idx = i;
		int err = pthread_create(&thread_pool[i],
				NULL,
				threadpool_worker,
				thread_idx);
		if (err != 0) {
			perror("Couldn't create thread in pool!\n");
			return 1;
		}
		pthread_detach(thread_pool[i]);
	}
	printf("Threadpool started with %d workers.\n", THREAD_POOL_SIZE);
	// END threadpool setup. Should extract to func.

	int server_fd = listen_on_port();
	while (1) {
		// Build up client socket.
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int* new_socket = malloc(sizeof(int));
		// This blocks till a connection comes through. Easy.
		// In happy path, new_socket is freed by the worker thread.
		int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
		if (client_socket < 0) {
			perror("accept failed");
			free(new_socket);
			continue;
		}


		// Push client_socket file-descriptor directly onto queue that is consumed by the thread-pool.
		queue_push(&client_socket_queue, client_socket);
	}
	return 0;
}

