#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <libpq-fe.h>

// Far future:
// Should make a separate queue of pointers that need to be freed, and have a worker thread only for freeing temp memory.
// Make this a multi-user program. Useful for people that have multiple businesses or sth.

#define PORT 3002
#define THREAD_POOL_SIZE 4
#define QUEUE_MAX_SIZE 16

#define LOG_INFO(fmt, ...) \
    printf("[INFO] [%s:%d -> %s()] " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define LOG_FUNC \
    printf("%s\n", __func__)


typedef uint16_t u16;

// BEGIN string implementation
// Basic string manipulation isn't that complicated, but sometimes it is nice to have things taken care of.
// My intention is to use this struct for those few times. I'm happy with malloc/free and basic
// arithmetic most of the time.
typedef struct {
	char* buf;
	size_t len;
	size_t cap;
} sstr;

sstr*
sstr_new(size_t cap) {
	LOG_FUNC;
	sstr* s = malloc(sizeof(sstr));
	s->len = 0;
	s->cap = cap;
	s->buf = malloc(cap + 1);
	s->buf[0]=0;
	return s;
}

void
sstr_free(sstr* s) {
	LOG_FUNC;
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
	memcpy(s->buf + s->len, data, data_len +1); // +1 copies the trailing NUL
	s->len = new_total_len;
}

void
sstr_set(sstr* s, char* data) {
	s->len = 0;
	s->buf[0] = 0;
	sstr_append(s, data);
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
	while (queue->count < 1) { // Handle spurious wakeups because OSs do that.
		pthread_cond_wait(&queue->cond_var, &queue->mutex);
	}
	int out = queue->client_sockets[queue->head];
	queue->head = (queue->head + 1) % QUEUE_MAX_SIZE; // % because ringbuffer.
	queue->count--;
	pthread_mutex_unlock(&queue->mutex);
	return out;
}
// END INT QUEUE implementation

char*
params_get_newstr(char* haystack, char* needle) {
	if (strlen(haystack) == 0) { return NULL; }
	char* needle_with_US;
	asprintf(&needle_with_US, "%s=", needle);
	char* found = strstr(haystack, needle_with_US);
	if (found == NULL) { return NULL; }
	char* value = found + strlen(needle_with_US);
	char* record_end = strchr(value, '&');
	char* out = strndup(value, record_end - value);
	free(needle_with_US);
	return out;
}

// TODO rename to something more appropriate.
// This started out as a golang-style request struct, but has grown into essentially thread-local data.
// The main func produces an array of these, and each thread owns one of them. The idea was to have a
// giant zero-allocation datastructure so that each thread does less malloc/free. With the use of
// jemalloc, I don't know if I really need this all that much. Of course, we don't really NEED this
// at all at the scale this program runs.
#define HTTPREQ_RESPONSE_MAX_SIZE 2048
typedef struct {
	sstr *response, *response2;
	char request_buf[HTTPREQ_RESPONSE_MAX_SIZE],
	     request_scratch1[HTTPREQ_RESPONSE_MAX_SIZE],
	     http_method[8], endpoint[256], http_version[16],
	     errmsg[256];
	u16 route;
	char* getP;
	char* postP;
	PGconn* db;
	int client_socket;
} httpContext;

void
httpContext_clear(httpContext* req) {
	req->request_buf[0] = 0;
	req->request_scratch1[0] = 0;
	req->http_method[0] = 0;
	req->endpoint[0] = 0;
	req->http_version[0] = 0;
	req->errmsg[0] = 0;
	req->route = 0;
}

int // ok
fillGetParams(httpContext* req) {
	LOG_FUNC;
	char* qmark = strchr(req->endpoint, '?');
	if (qmark == NULL) { return 1; }
	char* after_qmark = qmark+1;
	size_t new_len = strlen(after_qmark);
	req->getP = realloc(req->getP, new_len + 1);
	// +1 so that the target is null-terminated .
	strncpy(req->getP, after_qmark, new_len+1);
	return 1;
}

int // ok
fillPostParams(httpContext* req) {
	LOG_FUNC;
	char* reqBodyStart = strstr(req->request_buf, "\r\n\r\n");
	// Nothing to do.
	if ((reqBodyStart == NULL) || (strlen(reqBodyStart) == 0)) { return 1; }
	reqBodyStart += 4; // 2 CR and 2 NL.
	size_t newLen = strlen(reqBodyStart);
	req->postP = realloc(req->postP, newLen + 1);
	// +1 to get the automatic null-termination.
	strncpy(req->postP, reqBodyStart, newLen + 1);
	printf("Post params overwritten. Now:%s\n", req->postP);
	return 1;
}

httpContext requests[4];
// END httpContext object.

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

// Write HTTP response to client.
void
write_to_client(httpContext* req, int httpStatus, char* body) {
	char* a1;
	asprintf(&a1, "HTTP/1.1 %d \r\nConnection: close\r\nContent-Length: %lu\r\n\r\n%s",
		httpStatus,
		strlen(body),
		body
	);
	write_all(req->client_socket, a1, strlen(a1));
	free(a1);
}

void
write_redirect(httpContext* req, int httpStatus, char* newLocation) {
	char* a1;
	asprintf(&a1, "HTTP/1.1 %d \r\nContent-Length: 0\r\nLocation: %s\r\nConnection: close\r\n\r\n",
		httpStatus,
		newLocation
	);
	write_all(req->client_socket, a1, strlen(a1));
	free(a1);
}

// This will take in the whole request and parse out the usable parts like params, endpoint, headers, etc.
int // OK
parse_request(httpContext* request) {
	LOG_FUNC;
	int client_socket = request->client_socket;
	char* buf = request->request_buf;
	int bytes_read = read(client_socket, buf, 2047);
	printf("metric: request_size: %d\n", bytes_read);

	if (bytes_read == 2047) {
		write_to_client(request, 413, "Request too large. Max is 2KB.");
		return 0;
	}

	// Get first line. Max 512B.
	char* line_end = strstr(buf, "\r\n");
	if (line_end == NULL) {
		write_to_client(request, 422, "Couldn't identify the first header. Fix your request.");
		return 0;
	}
	size_t line_len = line_end - buf;
	if (line_len > 512) {
		write_to_client(request, 413, "Request endpoint too large. We aren't parsing over 512B.");
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
		write_to_client(request, 422, "Failed to parse HTTP line 1. Fix your request.");
		return 0;
	}

	int ok = parse_route(&request->route, endpoint);
	if (!ok) {
		write_to_client(request, 404, "Couldn't parse route from endpoint.");
		return 0;
	}

	ok = fillGetParams(request);
	if (!ok) {
		write_to_client(request, 422, "Couldn't parse GET params.");
		return 0;
	}

	ok = fillPostParams(request);
	if (!ok) {
		write_to_client(request, 422, "Couldn't parse POST params.");
		return 0;
	}

	return 1;
}

unsigned long
db_tx_count(PGconn* db) {
	LOG_FUNC;
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
hello_name(httpContext* request) {
	LOG_FUNC;
	char* name = params_get_newstr(request->getP, "name");
	printf("name found:%s\n", name);
	u16 tx_count = db_tx_count(request->db);
	char* a1;
	asprintf(&a1, "<p>Hello, %s!</p> <p>There are %d transactions in the db.</p>", name, tx_count);
	sstr_set(request->response2, a1);
	free(a1);
	free(name);
}

char*
read_file_newstr(char* path) {
	LOG_FUNC;
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		printf("file null\n");
		exit(1);
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
homePage(httpContext* request) {
	char* body = read_file_newstr("templates/home.html");
	write_to_client(request, 200, body);
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
	LOG_FUNC;
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
listAccounts(httpContext* request) {
	LOG_FUNC;
	char* body = read_file_newstr("templates/listAccounts.html");
	char* a1;
	sstr* trs = tr_of_every_account(request->db);
	asprintf(&a1, body, trs->buf);
	write_to_client(request, 200, a1);
	sstr_free(trs);
	free(a1);
	free(body);
}

char* account_name_from_id_;
// Called at start of program.
u16 // ok
account_name_from_id_prepopulate(PGconn* db) {
	LOG_FUNC;
	PGresult* res = PQexec(db, "select id, name from accounts;");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get tx-count:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return 0;
	}
	u16 total_rows = PQntuples(res);
	char* a1 = malloc(1024); a1[0]=0;
	size_t a1len = 0;
	size_t a1cap = 1024;
	char* eachPair = malloc(512);
	for (u16 i=0; i < total_rows; i++) {
		snprintf(eachPair, 512,
				"%s=%s&",
				PQgetvalue(res, i, 0),
				PQgetvalue(res, i, 1)
			);
		size_t pairlen = strlen(eachPair);
		size_t newlen = pairlen + a1len;
		if (newlen+1 > a1cap) {
			a1cap *= 2;
			a1 = realloc(a1, a1cap);
		}
		memcpy(a1 + a1len, eachPair, pairlen);
		a1len = newlen;
		a1[newlen]=0;
	}
	account_name_from_id_ = a1;
	free(eachPair);
	return 1;
}

sstr*
ledger_newest_30_newstr(PGconn* db) {
	LOG_FUNC;
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
		char* debit_acct_name = params_get_newstr(account_name_from_id_, PQgetvalue(res, i, 2));
		char* credit_acct_name = params_get_newstr(account_name_from_id_, PQgetvalue(res, i, 3));
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
			debit_acct_name,
			credit_acct_name,
			PQgetvalue(res, i, 4),
			PQgetvalue(res, i, 5)
		);
		if (written_to_temp >= 1024) {
			printf("ERR: ledger_newest_30_newstr: tr truncated to 1024: %s\n", temp);
		}
		sstr_append(out, temp);
		free(debit_acct_name);
		free(credit_acct_name);
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
	LOG_FUNC;
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
listLedger(httpContext* request) {
	LOG_FUNC;
	char* body = read_file_newstr("templates/ledger.html");
	char* a1;
	// If we start writing these into response3, we wouldn't need to malloc.
	// Then use strstr to build up response2.
	sstr* ln30 = ledger_newest_30_newstr(request->db);
	char* aso = account_selection_options(request->db);
	asprintf(&a1, body, ln30->buf, aso, aso);
	write_to_client(request, 200, a1);
	sstr_free(ln30);
	free(a1);
	free(body);
}

// Helper to convert a single hex character to its integer value
static char hex_to_val(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return 0;
}

// In-place URL decoder. Modifies 'str' directly. Zero heap allocations.
// Straight up Gemini copy-pasta
void url_decode(char* str) {
    if (str == NULL) return;
    char* reader = str;
    char* writer = str;
    while (*reader != '\0') {
        if (*reader == '+') {
            // 1. Convert plus signs back to standard spaces
            *writer = ' ';
            reader++;
            writer++;
        } else if (*reader == '%' && isxdigit((unsigned char)reader[1]) && isxdigit((unsigned char)reader[2])) {
            // 2. Convert %XX hex sequences back to characters
            char high = hex_to_val(reader[1]);
            char low  = hex_to_val(reader[2]);
            // Combine the two hex nibbles into a single byte character
            *writer = (char)((high << 4) | low);
            reader += 3; // Skip past the %, X, and X characters
            writer++;
        } else {
            // 3. Keep plain alphanumeric characters exactly as they are
            *writer = *reader;
            reader++;
            writer++;
        }
    }
    // Explicitly place a fresh null-terminator at our new shorter boundary
    *writer = '\0';
}

void
testPost(httpContext* req) {
	LOG_FUNC;
	char* a1;
	char* bigText = params_get_newstr(req->postP, "note");
	char* a2 = strdup(bigText);
	url_decode(a2);
	asprintf(&a1, "<p>got this:%s</p> <p>After url-decoding it is:%s</p>", bigText, a2);
	write_to_client(req, 200, a1);
	free(a1);
}

void
createAccount(httpContext* request) {
	LOG_FUNC;
	char* name = params_get_newstr(request->postP, "name");
	char* type = params_get_newstr(request->postP, "type");
	if ((name == NULL) || (type == NULL)) {
		sstr_set(request->response2, "Param 'name' or 'type' is missing");
		return;
	}
	char* query = "insert into accounts (name, type) values ($1, $2);";
	const char* values[2];
	values[0] = name;
	values[1] = type;
	PGresult* res = PQexecParams(request->db, query, 2,
		NULL, values, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		char* a1;
		asprintf(&a1, "DBERR: failed to create account: %s\n", PQresultErrorMessage(res));
		printf("%s", a1);
		sstr_set(request->response2, a1);
		free(a1);
		return;
	}
	PQclear(res);
	write_redirect(request, 303, "/2");
	return;
}

void
createLedgerEntry(httpContext* request) {
	LOG_FUNC;
	char* debitID = params_get_newstr(request->postP, "debit_account_id");
	char* creditID = params_get_newstr(request->postP, "credit_account_id");
	char* note = params_get_newstr(request->postP, "note");
	url_decode(note);
	char* amount = params_get_newstr(request->postP, "amount");
	if ((debitID == NULL) || (creditID == NULL) || (note == NULL) || (amount == NULL)) {
		write_to_client(request, 422, "Required params: [debit_account_id, credit_account_id, note, amount]");
		return;
	}
	char* query = "insert into transactions (debit_account_id, credit_account_id, note, amount)"
		"values ($1, $2, $3, $4);";
	const char* values[4];
	values[0] = debitID;
	values[1] = creditID;
	values[2] = note;
	values[3] = amount;
	PGresult* res = PQexecParams(request->db, query, 4,
		NULL, values, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		char* a1;
		asprintf(&a1, "DBERR: failed to create transaction: %s\n", PQresultErrorMessage(res));
		printf("%s", a1);
		write_to_client(request, 422, a1);
		free(a1);
		return;
	}
	PQclear(res);
	write_redirect(request, 303, "/1");
	return;
}

void
incomeStatement(httpContext* request) {
	LOG_FUNC;
	auto template = read_file_newstr("templates/incomeStatement.html");
	u16 month, year;
	int getPresult = sscanf(request->getP, "m=%hd&y=%hd", &month, &year);
	if (getPresult != 2) {
		// Use current month & year
		time_t t1 = time(NULL);
		struct tm* t2 = localtime(&t1);
		year = t2->tm_year + 1900;
		month = t2->tm_mon+ 1;
	}
	char* startDate;
	asprintf(&startDate, "%04d%02d01", year, month);
	auto endMonth = month + 1;
	if (endMonth == 13) {
		endMonth = 1;
		year += 1;
	}
	char* stopDate;
	asprintf(&stopDate, "%04d%02d01", year, endMonth);
	char* body;

	// TODO income when I have some.
	// TODO This captures only increases to the expense accounts. It doesn't capture when an acct
	// has a decrease. E.g. when the ISP provides a refund to compensate for an outage.
	auto query_expenses = "select a.name, a.type, sum(t.amount) from transactions t join accounts a on t.debit_account_id = a.id AND a.type = 1 where t.created_at between $1 and $2 group by a.name, a.type;";
	const char* values[2];
	values[0] = startDate;
	values[1] = stopDate;
	PGresult* res = PQexecParams(request->db, query_expenses, 2, NULL, values, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get tx-count:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return;
	}
	u16 total_rows = PQntuples(res);
	u16 trsCap = 1024; u16 trsLen = 0; char* trs = malloc(trsCap); trs[0]=0;
	char tr[512];
	int netProfitCents = 0;
	for (u16 i = 0; i < total_rows; i++) {
		char* amount = PQgetvalue(res, i, 2);
		double amount_d = atof(amount);
		unsigned int amount_i_cents = round(100 * amount_d);
		netProfitCents -= amount_i_cents;
		u16 trLen = snprintf(tr, 512,
			"<tr>"
			  "<td>%s</td>"
			  "<td></td>"
			  "<td>%s</td>"
			"</tr>\n",
			PQgetvalue(res, i, 0),
			PQgetvalue(res, i, 2)
		);
		if (trLen >=512) {
			printf("WARN: Truncated trLen when doing incomeStatement. res0:%s res2:%s\n",
				PQgetvalue(res, i, 0),
				PQgetvalue(res, i, 2)
			);
		}

		auto newtrsLen = trsLen + trLen;
		if (newtrsLen +1 > trsCap) {
			auto newtrsCap = newtrsLen *2;
			trs = realloc(trs, newtrsCap);
			trsCap = newtrsCap;
		}
		memcpy(trs + trsLen, tr, trLen + 1); // The +1 includes NUL.
		trsLen += trLen;
	}
	PQclear(res);

	auto netProfitDollars = netProfitCents / 100.00;
	asprintf(&body, template, trs, netProfitDollars);
	write_to_client(request, 200, body);
	free(trs);
	free(body);
	free(template);
	free(stopDate);
	free(startDate);
}

void
balanceSheet(httpContext* request) {
	LOG_FUNC;
	auto template = read_file_newstr("templates/balanceSheet.html");
	u16 month, year;
	int getPresult = sscanf(request->getP, "m=%hd&y=%hd", &month, &year);
	if (getPresult != 2) {
		// Use current month & year
		time_t t1 = time(NULL);
		struct tm* t2 = localtime(&t1);
		year = t2->tm_year + 1900;
		month = t2->tm_mon+ 1;
	}
	char* startDate;
	asprintf(&startDate, "%04d%02d01", year, month);
	auto endMonth = month + 1;
	if (endMonth == 13) {
		endMonth = 1;
		year += 1;
	}
	char* stopDate;
	asprintf(&stopDate, "%04d%02d01", year, endMonth);
	char* body;
	auto query_bs = read_file_newstr("db/balanceSheet");
	const char* values[1];
	printf("stopdate is:%s\n", stopDate);
	values[0] = stopDate;
	PGresult* res = PQexecParams(request->db, query_bs, 1, NULL, values, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		printf("resstat:%d\n", PQresultStatus(res));
		printf("Couldn't get tx-count:%s\n", PQresultErrorMessage(res));
		PQclear(res);
		return;
	}
	u16 total_rows = PQntuples(res);
	size_t trsCap = 1024; size_t trsLen = 0; char* trs = malloc(trsCap); trs[0]=0;
	char tr[512];
	auto trTemplate = sstr_new(1024);
	for (u16 i = 0; i < total_rows; i++) {
		auto type = atoi(PQgetvalue(res, i, 2));
		switch (type) {
			case 2:
				sstr_set(trTemplate, "<tr><td>%s</td><td>%s</td><td></td></tr>\n");
				break;
			case 3:
				sstr_set(trTemplate, "<tr><td>%s</td><td></td><td>%s</td></tr>\n");
				break;
			default:
				sstr_set(trTemplate, "Something wrong with trTemplate");
		}
		auto trLen = snprintf(tr, 512,
			trTemplate->buf,
			PQgetvalue(res, i, 1),
			PQgetvalue(res, i, 5)
		);
		if (trLen >=512) {
			printf("WARN: Truncated trLen when doing balanceSheet. res0:%s res2:%s\n",
				PQgetvalue(res, i, 0),
				PQgetvalue(res, i, 2)
			);
		}
		auto newtrsLen = trsLen + trLen;
		if (newtrsLen +1 > trsCap) {
			auto newtrsCap = newtrsLen *2;
			trs = realloc(trs, newtrsCap);
			trsCap = newtrsCap;
		}
		memcpy(trs + trsLen, tr, trLen + 1); // The +1 includes NUL.
		trsLen += trLen;
	}
	asprintf(&body, template, trs);
	write_to_client(request, 200, body);
	PQclear(res);
	sstr_free(trTemplate);
	free(trs);
	free(body);
	free(template);
	free(query_bs);
	free(stopDate);
	free(startDate);
}

// This function is called from a threadpool worker, to handle the request.
void*
handle_request(httpContext* request) {
	LOG_FUNC;
	httpContext_clear(request);
	int ok = parse_request(request);
	if (!ok) {
		// parse_request will send response to client.
		return NULL;
	}

	switch (request->route) {
		case 0:
			homePage(request);
			break;
		case 1:
			listLedger(request);
			break;
		case 2:
			listAccounts(request);
			break;
		case 3:
			createAccount(request);
			break;
		case 4:
			createLedgerEntry(request);
			break;
		case 5:
			incomeStatement(request);
			break;
		case 6:
			balanceSheet(request);
			break;
		case 7:
			testPost(request);
			break;
		default:
			write_to_client(request, 404, "Not found");
			return NULL;
	}
	return NULL;
}

void*
threadpool_worker(void* arg) {
	LOG_FUNC;
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
	if (thread_idx == 0) {
		account_name_from_id_prepopulate(db);
	}
	httpContext* request;
	request = &requests[thread_idx];
	request->db = db;

	while (1) {
		// Blocking call
		int client_socket = queue_pop(&client_socket_queue);
		struct timespec start_time;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
		request->client_socket = client_socket;
		handle_request(request);
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
	// To prevent macos from holding onto the port after the process completes. This safety net prevents
	// me from quickly starting a new server within seconds.
	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(PORT);

	if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
		perror("CRITICAL ERROR. Bind failed. OS has probably locked the port in TIME_WAIT.");
		exit(1);
	}
	listen(server_fd, 100);
	printf("Server listening on port %d...\n", PORT);

	return server_fd;
}

int
main() {
	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		httpContext *req = &requests[i];
		req->response = sstr_new(128);
		req->response2 = sstr_new(128);
		req->getP = malloc(256);
		req->postP = malloc(256);
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
		// This blocks till a connection comes through. Easy.
		int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
		if (client_socket < 0) {
			perror("accept failed");
			continue;
		}
		// Push client_socket file-descriptor directly onto queue that is consumed by the thread-pool.
		queue_push(&client_socket_queue, client_socket);
	}
	return 0;
}

