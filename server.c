#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <libpq-fe.h>

// gcc -o run -Wall -Wextra -lpthread -lpq -ljemalloc server.c && ./run
// Test with
// curl -v 'http://localhost:3002/1?name=Michael_Smith'

#define PORT 3002
#define THREAD_POOL_SIZE 4
#define QUEUE_MAX_SIZE 16

typedef uint16_t u16;

// BEGIN string implementation
typedef struct {
	char* buf;
	size_t len;
	size_t cap;
} sstr;

sstr*
sstr_new(size_t cap) {
	printf("sstr_new\n");
	sstr* s = malloc(sizeof(sstr));
	s->len = 0;
	s->cap = cap;
	s->buf = malloc(cap + 1);
	s->buf[0]=0;
	return s;
}

void
sstr_append(sstr* s, char* data) {
	size_t data_len = strlen(data);
	size_t new_total_len = s->len + data_len;
	if (s->cap < 1+ new_total_len) {
		size_t new_cap = new_total_len * 2;
		s->buf = realloc(s->buf, new_cap);
		s->cap = new_cap;
	}
	memcpy(s->buf + s->len, data, data_len);
	s->len = new_total_len;
	s->buf[new_total_len] = 0;
}

void
sstr_reset(sstr* s) {
	printf("sstr_reset\n");
	s->len = 0;
	s->buf[0] = 0;
}

void
sstr_set(sstr* s, char* data) {
	sstr_reset(s);
	sstr_append(s, data);
}

void
sstr_prepend(sstr* s, char* data) {
	size_t data_len = strlen(data);
	size_t new_total_len = s->len + data_len;
	if (s->cap < 1+ new_total_len) {
		size_t new_cap = new_total_len * 2;
		s->buf = realloc(s->buf, new_cap);
		s->cap = new_cap;
	}
	memmove(s->buf + data_len, s->buf, s->len + 1); // With the orig null-term
	memcpy(s->buf, data, data_len);
	s->len += data_len;
}
// END string implementation

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
	char k[32];
	char v[32];
} Param;

int // ok
fillGetParams(Param* getParams, char* endpoint) {
	char* qmark = strchr(endpoint, '?');
	if (qmark == NULL) {
		return 1;
	}
	char* after_qmark = qmark+1;
	int paramPairCount = 20;
	char* paramPairSavePtr;
	char* paramPair = strtok_r(after_qmark, "&", &paramPairSavePtr);
	for (int count = 0; (count < paramPairCount) && (paramPair != NULL); count++) {
		Param* param = &getParams[count];
		int sscanf_result = sscanf(paramPair, "%31[^=]=%31s", param->k, param->v);
		if (sscanf_result != 2) {
			printf("paramPair:%s\nk:%s\nv:%s\n", paramPair, param->k, param->v);
			printf("GET param sscanf processing err.\n");
			return 0;
		}
		paramPair = strtok_r(NULL, "&", &paramPairSavePtr);
	}
	return 1;
}

char*
params_get(Param* params, char* key) {
	for (int i = 0; i<20; i++) {
		Param* param = &params[i];
		char* k = param->k;
		if (k[0] == 0) {
			break; // No more params
		}
		if (strcmp(k, key) != 0) {
			continue; // Not a match
		}
		return param->v;
	}
	return NULL;
}

// END PARAM section

// Basically a golang-style http.Request struct that will be cleared and reused by each thread.
#define HTTPREQ_RESPONSE_MAX_SIZE 2048
typedef struct {
	sstr *response, *response2;
	char request_buf[HTTPREQ_RESPONSE_MAX_SIZE],
	     request_scratch1[HTTPREQ_RESPONSE_MAX_SIZE],
	     http_method[8], endpoint[256], http_version[16],
	     errmsg[256];
	u16 route;
	Param getParams[20];
	Param postParams[20];
	Param request_headers[20];
	PGconn* db;
} httpreq;

void
httpreq_clear(httpreq* req) {
	sstr_reset(req->response);
	sstr_reset(req->response2);
	req->request_buf[0] = 0;
	req->request_scratch1[0] = 0;
	req->http_method[0] = 0;
	req->endpoint[0] = 0;
	req->http_version[0] = 0;
	req->errmsg[0] = 0;
	req->route = 0;
	req->getParams[0].k[0] = 0;
	req->postParams[0].k[0] = 0;
	req->request_headers[0].k[0] = 0;
}

httpreq requests[4];
// END httpreq object.

// Have to loop because "write" isn't guaranteed to do it all in one syscall. It tells us how much it did.
int // ok
write_all(int socket, char* buffer, u16 len) {
	char* ptr = buffer;
	u16 written = 0;
	while (written < len) {
		ssize_t just_wrote = write(socket, ptr, len-written);
		written += just_wrote;
	}
	// TODO print out response size. We'll use this to set the buffer to a good default.
	return 1;
}

int // ok
parse_route(u16* route, char* endpoint) {
	if (endpoint[0] != '/') {
		return 0;
	}
	char* start = endpoint +1;
	char* endptr;
	u16 out = strtoul(start, &endptr, 10);
	if (start == endptr) {
		return 0;
	}
	*route = out;
	return 1;
}

// Write a string out as the HTTP response.
void
write_error(int client_socket, httpreq* request, int http_status, char* errmsg) {
	char *a1;
	asprintf(&a1, "HTTP/1.1 %d \r\nContent-Length: %lu\r\n\r\n%s",
		http_status,
		strlen(errmsg),
		errmsg
	);
	sstr_set(request->response, a1);
	write_all(client_socket, request->response->buf, request->response->len);
	free(a1);
}

// This will take in the whole request and parse out the usable parts like params, endpoint, headers, etc.
int // OK
parse_request(httpreq* request, int client_socket, int thread_idx) {
	char* buf =  request->request_buf;
	int bytes_read = read(client_socket, buf, 2047);
	Param* getParams = request->getParams;

	// TODO print out request size. Use this to pick a good default in the future.
	if (bytes_read == 2047) {
		write_error( client_socket, request, 413, "Request too large. Max is 2KB.");
		return 0;
	}

	// Get first line. Max 512B.
	char* line_end = strstr(buf, "\r\n");
	if (line_end == NULL) {
		write_error( client_socket, request, 422, "Couldn't identify the first header. Fix your request.");
		return 0;
	}
	size_t line_len = line_end - buf;
	if (line_len > 512) {
		write_error( client_socket, request, 413, "Request endpoint too large. We aren't parsing over 512B.");
		return 0;
	}

	char* first_line = request->request_scratch1;
	strncpy(first_line, buf, line_len);
	char* http_method = request->http_method;
	char* endpoint = request->endpoint;
	char* http_version = request->http_version;
	int sscanf_result = sscanf(first_line, "%7s %255s %15s", http_method, endpoint, http_version);
	if (sscanf_result != 3) {
		write_error( client_socket, request, 422, "Failed to parse HTTP line 1. Fix your request.");
		return 0;
	}

	int ok = parse_route(&request->route, endpoint);
	if (!ok) {
		write_error( client_socket, request, 422, "Couldn't parse route from endpoint.");
		return 0;
	}

	ok = fillGetParams(getParams, endpoint);
	if (!ok) {
		write_error( client_socket, request, 422, "Couldn't parse GET params.");
		return 0;
	}

	// TODO post params

	return 1;
}

unsigned long
db_tx_count(PGconn* db) {
	printf("db_tx_count\n");
	PGresult* res = PQexec(db, "select count(1) from transactions;");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get tx-count:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return 0;
	}
	char* count_str = PQgetvalue(res, 0, 0);
	unsigned long out = strtoul(count_str, NULL, 10);
	PQclear(res);
	return out;
}

// Say hello to the GET-name.
void
hello_name(httpreq* request) {
	printf("hello_name\n");
	char* name = params_get(request->getParams, "name");
	u16 tx_count = db_tx_count(request->db);
	char* a1;
	asprintf(&a1, "<p>Hello, %s!</p> <p>There are %d transactions in the db.</p>", name, tx_count);
	sstr_set(request->response2, a1);
	free(a1);
}

char*
read_file(char* path) {
	printf("read_file\n");
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		printf("file null\n");
	}
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	char* buf = malloc(file_size + 1);
	// TODO free
	size_t bytes_read = fread(buf, 1, file_size, file);
	buf[bytes_read] = 0;
	fclose(file);
	return buf;
}

void
handle_home(httpreq* request) {
	char* body = read_file("templates/home.html");
	sstr_set(request->response2, body);
}

// This function is called from a threadpool worker, to handle the request.
void*
handle_request(int thread_idx, int client_socket, httpreq* request) {
	printf("handle_request\n");
	httpreq_clear(request);
	int ok = parse_request(request, client_socket, thread_idx);
	if (!ok) {
		// parse_request will send response to client.
		return NULL;
	}

	switch (request->route) {
		case 0:
			handle_home(request);
			break;
		case 1:
			hello_name(request);
			break;
		default:
	}

	char *a1;
	asprintf(&a1, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n",
		request->response2->len);
	sstr_set(request->response, a1);
	sstr_append(request->response, request->response2->buf);
	write_all(client_socket, request->response->buf, request->response->len);
	free(a1);
	return NULL;
}

void*
threadpool_worker(void* arg) {
	printf("threadpool_worker\n");
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

	httpreq* request;
	request = &requests[thread_idx];
	request->db = db;

	while (1) {
		// Blocking call
		int client_socket = queue_pop(&client_socket_queue);
		struct timespec start_time;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
		handle_request(thread_idx, client_socket, request);
		struct timespec end_time;
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		double elapsed_ms = (double)(end_time.tv_sec - start_time.tv_sec) * 1000.0 +
			(double)(end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
		printf("DONE request:%.3f ms\n", elapsed_ms);
	}
	PQfinish(db);
	return NULL;
}

int
listen_on_port() {
	int server_fd;
	struct sockaddr_in address;
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
	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		httpreq *req = &requests[i];
		req->response = sstr_new(2048);
		req->response2 = sstr_new(2048);
	}

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

