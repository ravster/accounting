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
sstr_free(sstr* s) {
	printf("sstr_free\n");
	free(s->buf);
	free(s);
}

void
sstr_append(sstr* s, char* data) {
	size_t data_len = strlen(data);
	size_t new_total_len = s->len + data_len;
	if (s->cap < 1+ new_total_len) {
		printf("sstr_append: raising cap\n");
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

char*
params_get2_newstr(char* haystack, char* needle) {
	if (strlen(haystack) == 0) { return NULL; }
	char* needle_with_US;
	asprintf(&needle_with_US, "%s=", needle);
	char* found = strstr(haystack, needle_with_US);
	if (found == NULL) { return NULL; }
	char* next = haystack + strlen(needle_with_US);
	printf("next:%s\n", next);
	char* record_end = strchr(next, '&');
	char* out = strndup(next, record_end - next);
	return out;
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
	char* getP;
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
	req->postParams[0].k[0] = 0;
	req->request_headers[0].k[0] = 0;
}

int // ok
fillGetParams(httpreq* req) {
	printf("fillGetParams\n");
	char* qmark = strchr(req->endpoint, '?');
	if (qmark == NULL) { return 1; }
	char* after_qmark = qmark+1;
	size_t new_len = strlen(after_qmark);
	req->getP = realloc(req->getP, new_len + 1);
	// +1 so that the target is null-terminated .
	strncpy(req->getP, after_qmark, new_len+1);
	return 1;
}

httpreq requests[4];
// END httpreq object.

// Have to loop because "write" isn't guaranteed to do it all in one syscall. It tells us how much it did.
int // ok
write_all(int socket, char* buffer, size_t len) {
	char* ptr = buffer;
	u16 written = 0;
	while (written < len) {
		ssize_t just_wrote = write(socket, ptr, len-written);
		written += just_wrote;
	}
	printf("metric_response_size:%lu\n", len);
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
parse_request(httpreq* request, int client_socket) {
	printf("parse_request\n");
	char* buf =  request->request_buf;
	int bytes_read = read(client_socket, buf, 2047);

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
	printf("first_line:%s\n", first_line);
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
		write_error( client_socket, request, 404, "Couldn't parse route from endpoint.");
		return 0;
	}

	ok = fillGetParams(request);
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

void
hello_name(httpreq* request) {
	printf("hello_name\n");
	char* name = params_get2_newstr(request->getP, "name");
	printf("name found:%s\n", name);
	u16 tx_count = db_tx_count(request->db);
	char* a1;
	asprintf(&a1, "<p>Hello, %s!</p> <p>There are %d transactions in the db.</p>", name, tx_count);
	sstr_set(request->response2, a1);
	free(a1);
	free(name);
}

char*
read_file_new_string(char* path) {
	printf("read_file_new_string\n");
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		printf("file null\n");
	}
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	char* buf = malloc(file_size + 1);
	size_t bytes_read = fread(buf, 1, file_size, file);
	buf[bytes_read] = 0;
	fclose(file);
	return buf;
}

void
handle_home(httpreq* request) {
	char* body = read_file_new_string("templates/home.html");
	sstr_set(request->response2, body);
	free(body);
}

char*
resolve_account_type_new_str(char* account_type_enum_s) {
	u16 account_type_enum = strtoul(account_type_enum_s, NULL, 10);
	char* actual_acct_type_enum[4] = {
		"Income",
		"Expense",
		"Asset",
		"Liability"
	};
	char* out;
	if (account_type_enum > 3) {
		printf("Got an unknown account_type_enum_s:%s\n", account_type_enum_s);
		out = "Unknown";
	}
	out = strdup(actual_acct_type_enum[account_type_enum]);
	return out;
}

sstr*
tr_of_every_account(PGconn* db) {
	printf("tr_of_every_account1\n");
	PGresult* res = PQexec(db, "select id, name, type from accounts;");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get tx-count:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return 0;
	}
	u16 total_rows = PQntuples(res);
	sstr *out = sstr_new(512);
	char* temp;
	for (u16 i = 0; i < total_rows; i++) {
		char* type = resolve_account_type_new_str(PQgetvalue(res, i, 2));
		asprintf(&temp,
			"<tr>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			"</tr>\n",
			PQgetvalue(res, i, 0),
			PQgetvalue(res, i, 1),
			type
		);
		sstr_append(out, temp);
		free(type);
		free(temp);
	}
	PQclear(res);
	return out;
}

void
listAccounts(httpreq* request) {
	printf("listAccounts\n");
	char* body = read_file_new_string("templates/listAccounts.html");
	char* a1;
	sstr* trs = tr_of_every_account(request->db);
	asprintf(&a1, body, trs->buf);
	sstr_set(request->response2, a1);
	sstr_free(trs);
	free(a1);
	free(body);
}

// Should output the equivalent of 
// <tr>
//  <th>{{.ID}}</th>
//  <td>{{.CreatedAt.Format "2006-01-02 15:04:05 MST"}}</td>
//  <td>{{ index $.accounts .DebitAccountId }}</td>
//  <td>{{ index $.accounts .CreditAccountId }}</td>
//  <td>{{.Note}}</td>
//  <td>${{.Amount}}</td>
//</tr>
sstr*
ledger_newest_30_newstr(PGconn* db) {
	printf("ledger_newest_30_newstr\n");
	PGresult* res = PQexec(db, "SELECT id, created_at, debit_account_id, credit_account_id, note, amount"
		" FROM transactions"
		" ORDER BY created_at DESC"
		" LIMIT 30;");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get ledger_newest_30_newstr:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return 0;
	}
	u16 total_rows = PQntuples(res);
	sstr *out = sstr_new(4096);
	char* temp = malloc(1024); temp[0]=0;
	for (u16 i = 0; i < total_rows; i++) {
		size_t written_to_temp = snprintf(temp, 1024,
			"<tr>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			"</tr>\n",
			PQgetvalue(res, i, 0),
			PQgetvalue(res, i, 1),
			PQgetvalue(res, i, 2),
			PQgetvalue(res, i, 3),
			PQgetvalue(res, i, 4),
			PQgetvalue(res, i, 5)
		);
		if (written_to_temp >= 1024) {
			printf("ERR: ledger_newest_30_newstr: tr truncated to 1024: %s\n", temp);
		}
		printf("wrote temp:%s\n", temp);
		sstr_append(out, temp);
	}
	printf("metric: ledger_newest_30_newstr: out_len:%ld\n", out->len);
	free(temp);
	PQclear(res);
	return out;
}

// Should output the equivalent of 
// {{range $k, $v := .accounts }}
// <option value="{{$k}}">{{$v}}</option>
// {{end}}
sstr* account_selection_options_;
char*
account_selection_options(PGconn* db) {
	printf("account_selection_options\n");
	if (account_selection_options_ != NULL) {
		return account_selection_options_->buf;
	}
	// Memoize this. It should pull the data from the db the first time and write to a str.
	// After that it should always return the str. After a new account is created, the program
	// should be restarted.

	account_selection_options_ = sstr_new(1024);
	sstr* out = account_selection_options_;
	PGresult* res = PQexec(db, "SELECT id, name from accounts;");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get account_selection_options:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return 0;
	}
	u16 total_rows = PQntuples(res);
	char* temp = malloc(1024); temp[0]=0;
	for (u16 i = 0; i < total_rows; i++) {
		size_t written_to_temp = snprintf(temp, 1024,
			"<option value=\"%s\">%s</option>",
			PQgetvalue(res, i, 0),
			PQgetvalue(res, i, 1)
		);
		if (written_to_temp >= 1024) {
			printf("ERR: account_selection_options: temp truncated to 1024: %s\n", temp);
		}
		sstr_append(out, temp);
	}
	printf("metric: account_selection_options: out_len:%ld\n", out->len);
	free(temp);
	PQclear(res);
	return account_selection_options_->buf;
}

void
listLedger(httpreq* request) {
	printf("listLedger\n");
	char* body = read_file_new_string("templates/ledger.html");
	char* a1;
	// If we start writing these into response3, we wouldn't need to malloc.
	// Then use strstr to build up response2.
	sstr* ln30 = ledger_newest_30_newstr(request->db);
	printf("ln30:%s\n", ln30->buf);
	char* aso = account_selection_options(request->db);
	printf("aso:%s\n", aso);
	asprintf(&a1, body, ln30->buf, aso, aso);
	sstr_set(request->response2, a1);
	sstr_free(ln30);
	free(a1);
	free(body);
}

void
createAccount(httpreq* request) {
	return;
	printf("createAccount\n");
	// Take post params
	// Write to DB
	// Redirect back
	char* a1;
	sstr_set(request->response2, a1);
	free(a1);
}

// This function is called from a threadpool worker, to handle the request.
void*
handle_request(int client_socket, httpreq* request) {
	printf("handle_request\n");
	httpreq_clear(request);
	int ok = parse_request(request, client_socket);
	if (!ok) {
		// parse_request will send response to client.
		return NULL;
	}

	switch (request->route) {
		case 0:
			handle_home(request);
			break;
		case 1:
			listLedger(request);
			break;
		case 2:
			listAccounts(request);
			break;
		case 3:
			hello_name(request);
		// 	createAccount(request);
			break;
		default:
			write_error(client_socket, request, 404, "Not found");
			return NULL;
	}

	char *a1;
	asprintf(&a1, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
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
		handle_request(client_socket, request);
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
		req->response = sstr_new(128);
		req->response2 = sstr_new(128);
		req->getP = malloc(1024);
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

