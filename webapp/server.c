#define _GNU_SOURCE
#include <signal.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#define local_persist static
#define global_variable static
#define THREAD_POOL_SIZE 4

typedef uint16_t u16;
typedef uint32_t u32;

// Basic struct used for random multiple things.
typedef struct {
	char* name;
	int int1; // len
	float total;
} StrInt;

// Globals so many functions can write to this.
global_variable FILE *AccountFile, *TxFile;
typedef struct {
	u16 id; // len
	float amount;
	char* note;
	u16 debit_account_id; // cap
	u16 credit_account_id;
	u32 created_at;
} Tx;
global_variable Tx *Txs; // This is 1-based. The 0th element is the sentinel value that captures length and capacity of the array, in the id and debit_account_id fields ints, respectively.

Tx*
tx_append(u16 id, float amount, char* note, u16 debit_account_id, u16 credit_account_id, u32 created_at) {
	u16 *len = &Txs[0].id;
	u16 *cap = &Txs[0].debit_account_id;
	if (*len == *cap) {
		*cap *= 2;
		Txs = realloc(Txs, *cap * sizeof(Tx));
	}
	Tx* new_tx = &Txs[*len + 1];
	new_tx->id = id;
	new_tx->amount = amount;
	new_tx->note = strdup(note);
	new_tx->debit_account_id = debit_account_id;
	new_tx->credit_account_id = credit_account_id;
	new_tx->created_at = created_at;
	(*len)++;
	return new_tx;
}

void
tx_append_to_file(Tx* newTx) {
	fprintf(TxFile, "%hu\t%.2f\t%s\t%hu\t%hu\t%u\n",
			newTx->id, newTx->amount, newTx->note,
			newTx->debit_account_id, newTx->credit_account_id, newTx->created_at);
}

typedef struct {
	u16 id; // len
	char* name;
	u16 type; // cap
} Account;
global_variable Account *Accs;

int
acc_compare_name_asc(const void* a, const void* b) {
	Account* aa = (Account*)a;
	Account* ab = (Account*)b;
	return strcmp(aa->name, ab->name);
}

// Add an Account to the global array Accs. And then sort it by name ASC.
void
acc_append(u16 id, char* name, u16 type) {
	u16 *len = &Accs[0].id;
	u16 *cap = &Accs[0].type;
	if (*len == *cap) {
		*cap *= 2;
		Accs = realloc(Accs, *cap * sizeof(Account));
	}
	Account* new_acc = &Accs[*len + 1];
	new_acc->id = id;
	new_acc->name = strdup(name);
	new_acc->type = type;
	(*len)++;
	qsort(&Accs[1], *len, sizeof(Account), acc_compare_name_asc);
}

int
acc_find_by_name(char* needle) {
	for (int i = 1; i <= Accs[0].id; i++) {
		if (strcmp(Accs[i].name, needle) == 0) {
			return 1;
		}
	}
	return 0;
}

int
acc_find_by_id(u16 needle) {
	for (int i = 1; i <= Accs[0].id; i++) {
		if (Accs[i].id == needle) {
			return 1;
		}
	}
	return 0;
}

void
acc_append_file(u16 id, char* name, u16 type) {
	fprintf(AccountFile, "%hu\t%s\t%hu\n", id, name, type);
}

enum AccTypes {
	INCOME,
	EXPENSE,
	ASSET,
	LIABILITY
};

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
	sstr* s = malloc(sizeof(sstr));
	s->len = 0;
	s->cap = cap;
	s->buf = malloc(cap + 1);
	s->buf[0]=0;
	return s;
}

void
sstr_free(sstr* s) {
	free(s->buf);
	free(s);
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

// BEGIN socket_queue_t
typedef struct {
	int sockets[16];
	int head;
	int tail;
	int count;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} socket_queue_t;
global_variable socket_queue_t SocketQueue;

void
socketqueue_init(socket_queue_t *q) {
	q->head = 0;
	q->tail = 0;
	q->count = 0;
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
}

void
socketqueue_push(socket_queue_t *queue, int socket) {
	pthread_mutex_lock(&queue->mutex);
	if (queue->count < 16) {
		queue->sockets[queue->tail] = socket;
		queue->tail = (queue->tail + 1) % 16; // This is how it wraps around.
		queue->count++;
		pthread_cond_signal(&queue->cond); // Wake one thread.
	} else {
		printf("SocketQueue is full. Can't serve this client.\n");
	}
	pthread_mutex_unlock(&queue->mutex);
}

int
socketqueue_pop(socket_queue_t *q) {
	pthread_mutex_lock(&q->mutex);
	while (q->count == 0){ // Guard against spurious wakeups
		pthread_cond_wait(&q->cond, &q->mutex);
	}

	int sock = q->sockets[q->head];
	q->head = (q->head + 1) % 16;
	q->count--;

	pthread_mutex_unlock(&q->mutex);
	return sock;
}
// END socket_queue_t

void
printStrInts(StrInt* in) {
	int len = in[0].int1;
	printf("StrInt: len=%d\n  Name\tInt1\tTotal\n", len);
	for (int i = 1; i <= len; i++) {
		StrInt it = in[i];
		printf("  %s\t%d\t%f\n", it.name, it.int1, it.total);
	}
}

void
printTxs(Tx* in) {
	u16 len = in[0].id;
	printf("Tx: len=%hu cap=%hu\n  Name\tInt1\tTotal\n", len, in[0].debit_account_id);
	for (int i = 1; i <= len; i++) {
		Tx it = in[i];
		printf("%.2f\t%s\t%hu\t%hu\t%u\n", it.amount, it.note, it.debit_account_id, it.credit_account_id, it.created_at);
	}
}

// TODO asprintf & strndup here are bad. Linear lookup not so bad, but there's a problem if looking for the last KV. Switch to parsing this all once, and produce either an array of KV structs (pointers can be to the existing data with the '&' & '=' converted to '0'), or a struct of 2 arrays, one of Ks and one of Vs. Then the lookup runs through the Ks, and pulls the V of the matching index. Of course, it should be thread_local.
// Probably just use AoS, because SoA only helps when working with thousands of entities and this program will never get to that point for a single-user.
char*
params_get_newstr(char* haystack, char* needle) {
	if (strlen(haystack) == 0) { return NULL; }
	char* needle_with_US;
	asprintf(&needle_with_US, "%s=", needle);
	char* found = strstr(haystack, needle_with_US);
	if (found == NULL) { return NULL; }
	char* value = found + strlen(needle_with_US);
	char* record_end = strchr(value, '&');
	char* out = strndup(value, (size_t)(record_end - value));
	free(needle_with_US);
	return out;
}

// This started out as a golang-style request struct, but has grown into essentially thread-local data.
// The main func produces an array of these, and each thread owns one of them. The idea was to have a
// giant zero-allocation datastructure so that each thread does less malloc/free. With the use of
// jemalloc, I don't know if I really need this all that much. Of course, we don't really NEED this
// at all at the scale this program runs.
typedef struct {
	char request_buf[2048], http_method[8], endpoint[256], http_version[16],
	     errmsg[256];
	u16 route;
	char* getP;
	char* postP;
	int client_socket;
} httpContext;

void
httpContext_clear(httpContext* ctx) {
	ctx->http_method[0] = 0;
	ctx->endpoint[0] = 0;
	ctx->http_version[0] = 0;
	ctx->errmsg[0] = 0;
	ctx->route = 0;
	ctx->getP[0] = 0;
	ctx->postP[0] = 0;
}

int // ok
fillGetParams(httpContext* ctx) {
	char* qmark = strchr(ctx->endpoint, '?');
	if (qmark == NULL) { return 1; }
	char* after_qmark = qmark+1;
	size_t new_len = strlen(after_qmark);
	ctx->getP = realloc(ctx->getP, new_len + 1);
	// +1 so that the target is null-terminated .
	strncpy(ctx->getP, after_qmark, new_len+1);
	return 1;
}

int // ok
fillPostParams(httpContext* ctx) {
	char* reqBodyStart = strstr(ctx->request_buf, "\r\n\r\n");
	// Nothing to do.
	if ((reqBodyStart == NULL) || (strlen(reqBodyStart) == 0)) { return 1; }
	reqBodyStart += 4; // 2 CR and 2 NL.
	size_t newLen = strlen(reqBodyStart);
	ctx->postP = realloc(ctx->postP, newLen + 1);
	// +1 to get the automatic null-termination.
	strncpy(ctx->postP, reqBodyStart, newLen + 1);
	return 1;
}

// TODO Wouldn't need this. Move to using thread_local local_persist variables in the thread-mains.
httpContext requests[4];
// END httpContext object.

int // err
write_all(int socket, char* buffer, size_t len) {
	char* ptr = buffer;
	size_t written = 0;
	while (written < len) {
		ssize_t just_wrote = write(socket, ptr, len - written);
		if (just_wrote < 1) {
			printf("write_all. Failed. just_wrote=%zd\n", just_wrote);
			return 1;
		}
		written += (size_t)just_wrote;
		ptr += just_wrote;
	}
	return 0;
}

int // ok
parse_route(u16 *route, char *endpoint) {
	if (endpoint[0] != '/') {
		return 0;
	}
	char* start = endpoint +1;
	char* endptr;
	unsigned long out = strtoul(start, &endptr, 10);
	if (start == endptr) {
		return 0;
	}
	*route = (u16)out;
	return 1;
}

// Write HTTP response to client.
void
write_to_client(httpContext* req, int httpStatus, char* body) {
	char *a1;
	asprintf(&a1, "HTTP/1.1 %d \r\nContent-Length: %lu\r\n\r\n%s",
		httpStatus, strlen(body), body);
	write_all(req->client_socket, a1, strlen(a1));
	free(a1);
}

void
write_redirect(httpContext* req, int httpStatus, char* newLocation) {
	char* a1;
	asprintf(&a1, "HTTP/1.1 %d \r\nContent-Length: 0\r\nLocation: %s\r\n\r\n",
		httpStatus, newLocation);
	write_all(req->client_socket, a1, strlen(a1));
	free(a1);
}

char*
read_file_newstr(char* path) {
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		printf("file null. path:%s\n", path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	char* buf = malloc((size_t)file_size + 1);
	size_t bytes_read = fread(buf, 1, (size_t)file_size, file);
	buf[bytes_read] = 0;
	fclose(file);
	return buf;
}

// BEGIN Layer below request handlers.

sstr*
tr_of_every_account() {
	sstr *out = sstr_new(512);
	char* acc_types[4] = {
		"Income",
		"Expense",
		"Asset",
		"Liability"
	};

	local_persist thread_local char* temp;
	if (temp == NULL) { temp = malloc(90); }
	for (u16 i = 1; i <= Accs[0].id; i++) {
		Account acc = Accs[i];
		char* type = acc_types[acc.type];
		int written = snprintf(temp, 90,
			"<tr>" "<td>%hu</td>" "<td>%s</td>" "<td>%s</td>" "</tr>\n",
			acc.id, acc.name, type);
		if (written >= 90) {
			printf("err. tr_of_every_account. written over 90 chars. id=%hu name=%s type=%s\n",
					acc.id, acc.name, type);
		}
		sstr_append(out, temp);
	}
	return out;
}

sstr*
ledger_newest_30_newstr() {
	sstr *out = sstr_new(4096);
	// Should be local_persist that is shared between all threads. Actually, addTx should write this
	// whole thing to memory, and we shouldn't recalc this every page-load.
	char* temp = calloc(1024, 1);
	Account (^account_with_id)(uint16_t) = ^(uint16_t x) {
		u16 len = Accs[0].id;
		for (int i = 1; i<=len; i++) {
			Account acc = Accs[i];
			if (acc.id == x) { return acc; }
		}
		return Accs[0];
	};
	int total_rows = 30;
	u16 *txLen = &Txs[0].id;
	if (*txLen < 30) { total_rows = *txLen; }
	for (int i = *txLen; i > *txLen - total_rows; i--) {
		Tx tx = Txs[i];
		char* debit_acct_name = account_with_id(tx.debit_account_id).name;
		char* credit_acct_name = account_with_id(tx.credit_account_id).name;
		int written_to_temp = snprintf(temp, 1024,
			"<tr>"
			  "<td>%hu</td>"
			  "<td>%u</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			  "<td>%.2f</td>"
			"</tr>\n",
			tx.id,
			tx.created_at,
			debit_acct_name,
			credit_acct_name,
			tx.note,
			tx.amount
		);
		if (written_to_temp >= 1024) {
			printf("ERR: ledger_newest_30_newstr: tr truncated to 1024: %s\n", temp);
		}
		sstr_append(out, temp);
	}
	free(temp);
	return out;
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
calc_month(u16 *month, u16 *year, u32 *start, u32* stop, char* prevLink, char* nextLink, const char* getP) {
	char* mStr = params_get_newstr((char*)getP, "m");
	char* yStr = params_get_newstr((char*)getP, "y");
	if ((mStr == NULL) || (yStr == NULL)) {
		time_t t1 = time(NULL); // Use current month & year
		struct tm* t2 = localtime(&t1);
		*year = (u16)(t2->tm_year + 1900);
		*month = (u16)(t2->tm_mon + 1);
	} else {
		*month = (u16)atoi(mStr);
		*year = (u16)atoi(yStr);
	}
	free(mStr);
	free(yStr);
	*start = (*year*10000) + (*month * 100) + 1;
	u16 endMonth = *month + 1;
	u16 endYear = *year;
	if (endMonth == 13) {
		endMonth = 1;
		endYear = *year + 1;
	}
	*stop = (u32)((endYear * 10000) + (endMonth * 100) + 1);

	u16 prevYear = *year;
	u16 prevMonth = *month - 1;
	if (prevMonth == 0) {
		prevMonth = 12;
		prevYear--;
	}
	snprintf(prevLink, 12, "m=%hu&y=%hu", prevMonth, prevYear);
	u16 nextYear = *year;
	u16 nextMonth = *month + 1;
	if (nextMonth == 13) {
		nextMonth = 1;
		nextYear++;
	}
	snprintf(nextLink, 12, "m=%hu&y=%hu", nextMonth, nextYear);
}

char*
incomeStatementTrsNew(StrInt* strints, int accType) {
	int len = strints[0].int1;
	u16 outLen = 0; u16 outCap = 2048; char* out = calloc(2048, 1);
	char tr[128];
	char* trTemplate = "";
	switch (accType) {
		case 0:
			trTemplate = "<tr> <td>%s</td> <td>%.2f</td> <td></td> </tr>\n";
			break;
		case 1:
			trTemplate = "<tr> <td>%s</td> <td></td> <td>%.2f</td> </tr>\n";
			break;
	}
	for (int i = 1; i<= len; i++) {
		StrInt it = strints[i];
		int trLen = snprintf(tr, 128, trTemplate, it.name, it.total);
		if (trLen >=128) {
			printf("WARN: Truncated trLen when doing incomeStatement. name:%s tot:%f\n",
					it.name, it.total);
			trLen--; // For the benefit of memcpy below;
		}
		if (1+ outLen + trLen > outCap) {
			outCap *= 2;
			out = realloc(out, outCap);
		}
		memcpy(out + outLen, tr, (size_t)(trLen + 1));
		outLen += trLen;
	}
	return out;
}

int
compare_strint_desc(const void* a, const void* b) {
	StrInt* sa = (StrInt*)a;
	StrInt* sb = (StrInt*)b;
	float diff = sb->total - sa->total;
	return (int)((diff > 0) - (diff < 0));
}

typedef struct {
	u16 id;
	float total;
	char* name;
} BsAccTotal;
typedef struct {
	BsAccTotal* data;
	u16 len;
	u16 cap;
} BsAccs;

void
bs_accs_print(BsAccs *data) {
	printf("Len: %d\n", data->len);
	for (int i = 0; i< data->len; i++) {
		BsAccTotal it = data->data[i];
		printf("%d\t%.2f\t%s\n", it.id, it.total, it.name);
	}
}

BsAccs*
bs_accs_new() {
	BsAccs* out = malloc(sizeof(BsAccs));
	out->cap = 20;
	out->len = 0;
	out->data = calloc(out->cap, sizeof(BsAccTotal));
	return out;
}

void bs_accs_append(BsAccs* bsAccs, u16 id, char* name) {
	if (bsAccs->len == bsAccs->cap) {
		bsAccs->cap *= 2;
		bsAccs->data = realloc(bsAccs->data, bsAccs->cap * sizeof(BsAccTotal));
	}
	BsAccTotal* acc = &bsAccs->data[bsAccs->len];
	acc->id = id;
	acc->name = name;
	bsAccs->len++;
}

void bs_accs_populate_new(BsAccs *assets, BsAccs *liabilities) {
	for (u16 i = 1; i <= Accs[0].id; i++) {
		Account acc = Accs[i];
		switch (acc.type) {
			case ASSET:
				bs_accs_append(assets, acc.id, acc.name);
				break;
			case LIABILITY:
				bs_accs_append(liabilities, acc.id, acc.name);
				break;
			default:
				continue;
		}
	}
}

BsAccTotal*
bs_accs_find_by_id(BsAccs* bsAccs, u16 id) {
	for (u16 i=0; i<bsAccs->len; i++) {
		BsAccTotal *it = &bsAccs->data[i];
		if (it->id == id) {
			return it;
		}
	}
	return NULL;
}

void bs_accs_calc_totals(BsAccs* bs_accs_a, BsAccs* bs_accs_l, u32 stop) {
	u16 len = Txs[0].id;
	for (u16 i=1; i <= len; i++) {
		Tx tx = Txs[i];
		if (tx.created_at > stop) { continue; }

		// TODO maybe bs_accs_a and bs_accs_l should be one array? Don't know. Pray on it.
		BsAccTotal* bsacc = bs_accs_find_by_id(bs_accs_a, tx.debit_account_id);
		if (bsacc) {
			bsacc->total += tx.amount;
		} else {
			bsacc = bs_accs_find_by_id(bs_accs_l, tx.debit_account_id);
			if (bsacc) {
				bsacc->total -= tx.amount;
			}
		}

		bsacc = bs_accs_find_by_id(bs_accs_a, tx.credit_account_id);
		if (bsacc) {
			bsacc->total -= tx.amount;
		} else {
			bsacc = bs_accs_find_by_id(bs_accs_l, tx.credit_account_id);
			if (bsacc) {
				bsacc->total += tx.amount;
			} else { continue; }
		}
	}
}

char*
bs_accs_trs_new(BsAccs* Accs, uint8_t accType) {
	u16 outLen=0; u16 outCap=1024; char* out= calloc(outCap, 1);
	for (u16 i=0; i<Accs->len; i++) {
		BsAccTotal it = Accs->data[i];
		if (outLen > outCap-90) {
			outCap *=2;
			out = realloc(out, outCap);
		}
		int written = 100;
		switch (accType) {
			case ASSET:
				written = snprintf(out+outLen, 89,
						"<tr><td>%s</td><td>%.2f</td><td></td></tr>\n", it.name, it.total);
				break;
			case LIABILITY:
				written = snprintf(out+outLen, 89,
						"<tr><td>%s</td><td></td><td>%.2f</td></tr>\n", it.name, it.total);
				break;
			default:
				printf("Shouldn't have gotten to this default case.\n");
		}
		if (written > 89) {
			printf("snprintf fail. name=%s total=%f\n", it.name, it.total);
		}
		outLen += (u16)written;
	}
	return out;
}

// END Layer below request handlers.

void
incomeStatement(httpContext* request) {
	local_persist char *template;
	if (!template) { template = read_file_newstr("templates/incomeStatement.html"); }

	u16 month, year;
	u32 start, stop;
	char prevLink[12], nextLink[12];
	calc_month(&month, &year, &start, &stop, prevLink, nextLink, request->getP);
	char* body;
	float netProfitDollars = 0;

	// List of Tx that are in the time period.
	u16 periodTxsLen = 0; u16 periodTxsCap = 64; Tx* periodTxs = calloc(periodTxsCap, sizeof(Tx));
	u16 *txLen = &Txs[0].id;
	for (u16 i = 1; i <= *txLen; i++) {
		Tx tx = Txs[i];
		if ((tx.created_at < start) || (tx.created_at >= stop)) {
			continue;
		}
		if (periodTxsLen == periodTxsCap) {
			periodTxsCap *=2;
			periodTxs = realloc(periodTxs, periodTxsCap * sizeof(Tx));
		}
		periodTxs[periodTxsLen] = tx;
		periodTxsLen++;
	}

	float tot = 0;
	StrInt* incomeAccs = calloc(Accs[0].id, sizeof(StrInt));
	incomeAccs[0].int1 = 0; // Use this as array length
	StrInt* expenseAccs = calloc(Accs[0].id, sizeof(StrInt));
	expenseAccs[0].int1 = 0; // Use this as array length
	StrInt* newStrInt;

	for (u16 i = 1; i <= Accs[0].id; i++) {
		Account acc = Accs[i];
		switch (acc.type) {
			case INCOME:
				tot = 0;
				for (u16 i = 0; i < periodTxsLen; i++) {
					Tx tx = periodTxs[i];
					if (tx.credit_account_id != acc.id) { continue; }
					tot += tx.amount;
				}
				netProfitDollars += tot;
				int iaLen = incomeAccs[0].int1;
				newStrInt = &incomeAccs[iaLen+1];
				newStrInt->name = strdup(acc.name);
				newStrInt->total = tot;
				incomeAccs[0].int1++;
				break;
			case EXPENSE:
				tot = 0;
				for (u16 i = 0; i < periodTxsLen; i++) {
					Tx tx = periodTxs[i];
					if (tx.debit_account_id != acc.id) { continue; }
					tot += tx.amount;
				}
				netProfitDollars -= tot;
				int eaLen = expenseAccs[0].int1;
				newStrInt = &expenseAccs[eaLen+1];
				newStrInt->name = strdup(acc.name);
				newStrInt->total = tot;
				expenseAccs[0].int1++;
				break;
			default:
				continue;
		}
	}

	// Got to start from the idx-1 because idx0 is a sentinel that only has the counts. Yeah, this
	// makes sorting a bit wonky.
	qsort(incomeAccs+1, (size_t)incomeAccs[0].int1, sizeof(StrInt), compare_strint_desc);
	qsort(expenseAccs+1, (size_t)expenseAccs[0].int1, sizeof(StrInt), compare_strint_desc);

	char* itrs = incomeStatementTrsNew(incomeAccs, INCOME);
	char* etrs = incomeStatementTrsNew(expenseAccs, EXPENSE);
	size_t itrlen = strlen(itrs);
	size_t etrlen = strlen(etrs);

	char* trs = calloc(itrlen + etrlen + 1, 1);
	memcpy(trs, itrs, itrlen + 1);
	memcpy(trs + itrlen, etrs, etrlen + 1);

	asprintf(&body, template, prevLink, nextLink, trs, netProfitDollars);
	write_to_client(request, 200, body);
	free(itrs); free(etrs); free(trs);
	free(periodTxs);
	free(body);
}

void
createLedgerEntry(httpContext* request) {
	char* debitID = params_get_newstr(request->postP, "debit_account_id");
	char* creditID = params_get_newstr(request->postP, "credit_account_id");
	char* note = params_get_newstr(request->postP, "note");
	url_decode(note);
	char* amount = params_get_newstr(request->postP, "amount");
	if ((debitID == NULL) || (creditID == NULL) || (note == NULL) || (amount == NULL)) {
		write_to_client(request, 422, "Required params: [debit_account_id, credit_account_id, note, amount]");
		free(debitID); free(creditID); free(note); free(amount);
		// TODO change some of the above to thread_local static.
		return;
	}
	auto debit_i = atoi(debitID);
	auto credit_i = atoi(creditID);
	if (debit_i == credit_i) {
		write_to_client(request, 422, "debit_account_id & credit_account_id can't be the same");
		free(debitID); free(creditID); free(note); free(amount);
		return;
	}
	if (!acc_find_by_id((u16)debit_i)) {
		char* out;
		asprintf(&out, "debit_account_id=%d doesn't map to an existing account", debit_i);
		write_to_client(request, 422, out);
		free(out); free(debitID); free(creditID); free(note); free(amount);
		return;
	}
	if (!acc_find_by_id((u16)credit_i)) {
		char* out;
		asprintf(&out, "credit_account_id=%d doesn't map to an existing account", credit_i);
		write_to_client(request, 422, out);
		free(out); free(debitID); free(creditID); free(note); free(amount);
		return;
	}
	u16 newId = Txs[0].id + 1;
	time_t t1 = time(NULL);
	struct tm* t2 = localtime(&t1);
	char timeBuf[9];
	strftime(timeBuf, 9, "%Y%m%d", t2);
	Tx* newTx = tx_append(newId, (float)atof(amount), note, (u16)atoi(debitID), (u16)atoi(creditID), (u32)atoi(timeBuf));
	tx_append_to_file(newTx);
	write_redirect(request, 303, "/1");
	free(debitID); free(creditID); free(note); free(amount);
	return;
}

void
createAccount(httpContext* request) {
	char* name = params_get_newstr(request->postP, "name");
	char* type = params_get_newstr(request->postP, "type");
	if ((name == NULL) || (type == NULL)) {
		write_to_client(request, 422, "Param 'name' or 'type' is missing");
		free(name); free(type);
		return;
	}
	char* name2 = strdup(name);
	url_decode(name2);
	int is_name_found = acc_find_by_name(name2);
	if (is_name_found) {
		char* out;
		asprintf(&out, "The account name:%s is already taken.", name2);
		write_to_client(request, 422, out);
		free(out); free(name2); free(name); free(type);
		return;
	}
	u16 new_account_id = Accs[0].id + 1;
	int type_i = atoi(type);
	if ((type_i > LIABILITY) || (type_i < INCOME)) {
		char* out;
		asprintf(&out, "type_i=%d is invalid", type_i);
		write_to_client(request, 422, out);
		free(out); free(name2); free(name); free(type);
		return;
	}
	acc_append(new_account_id, name2, (u16)type_i);
	acc_append_file(new_account_id, name2, (u16)type_i);
	write_redirect(request, 303, "/2");
	free(name2); free(name); free(type);
	return;
}


void
listLedger(httpContext* request) {
	local_persist char* body;
	if (!body) { body = read_file_newstr("templates/ledger.html"); }
	char* a1;
	sstr* ln30 = ledger_newest_30_newstr();

	// Memoize/Cache the option-tags from Accs.
	local_persist char *aso;
	local_persist u16 *last_known_id;
	u16 accs_len = Accs[0].id;
	if ((last_known_id == NULL) || (*last_known_id != accs_len)) {
		size_t total_length = accs_len * 90;
		size_t remaining_length = total_length;
		aso = realloc(aso, total_length);
		size_t written = 0;
		for (u16 i = 1; i <= accs_len; i++) {
			Account acc = Accs[i];
			int written_to_temp = snprintf(aso+written, remaining_length,
					"<option value=\"%hu\">%s</option>",
					acc.id, acc.name);
			if (written_to_temp < 0 || (size_t)written_to_temp >= remaining_length) {
				printf("ERR: account_selection_options: temp truncated to remaining_length: %s\n", aso); }
			written += (size_t)written_to_temp;
			remaining_length -= (size_t)written_to_temp;
		}
		last_known_id = malloc(sizeof(u16));
		*last_known_id = Accs[accs_len].id;
	}

	asprintf(&a1, body, aso, aso, ln30->buf);
	write_to_client(request, 200, a1);
	sstr_free(ln30); free(a1);
}

void
listAccounts(httpContext* request) {
	local_persist char* body;
	if (!body) { body = read_file_newstr("templates/listAccounts.html"); }
	char* a1;
	// TODO This doesn't change on each page load, so we shouldn't recalc this on each page load. We should cache the new value of this after the addAccount operation instead.
	sstr* trs = tr_of_every_account();
	asprintf(&a1, body, trs->buf);
	write_to_client(request, 200, a1);
	sstr_free(trs);
	free(a1);
}


void
balanceSheet(httpContext* request) {
	local_persist char *template;
	if (!template) { template = read_file_newstr("templates/balanceSheet.html"); }
	u16 month, year;
	u32 start, stop;
	char prevLink[12], nextLink[12];
	calc_month(&month, &year, &start, &stop, prevLink, nextLink, request->getP);
	char* body;
	auto bs_accs_a = bs_accs_new();
	auto bs_accs_l = bs_accs_new();
	bs_accs_populate_new(bs_accs_a, bs_accs_l); // Asset, Liability
	// bs_accs_print(bs_accs_a);
	bs_accs_calc_totals(bs_accs_a, bs_accs_l, stop);
	auto trs_a = bs_accs_trs_new(bs_accs_a, 2);
	auto trs_l = bs_accs_trs_new(bs_accs_l, 3);
	asprintf(&body, template, prevLink, nextLink, trs_a, trs_l);
	write_to_client(request, 200, body);
	free(bs_accs_a);
	free(bs_accs_l);
	free(body);
	free(trs_a);
	free(trs_l);
}

// This will take in the whole request and parse out the usable parts like params, endpoint, headers, etc.
int // OK
parse_request(httpContext* request) {
	char* buf = request->request_buf;

	// Get first line. Max 512B.
	char* line_end = strstr(buf, "\r\n");
	if (line_end == NULL) {
		write_to_client(request, 422, "Couldn't identify the first header. Fix your request.");
		return 0;
	}
	size_t line_len = (size_t)(line_end - buf);
	if (line_len > 512) {
		write_to_client(request, 413, "Request endpoint too large. We aren't parsing over 512B.");
		return 0;
	}

	char* first_line = calloc(1, 512);
	memcpy(first_line, buf, line_len);
	char* http_method = request->http_method;
	char* endpoint = request->endpoint;
	char* http_version = request->http_version;
	int sscanf_result = sscanf(first_line, "%7s %255s %15s", http_method, endpoint, http_version);
	if (sscanf_result != 3) {
		write_to_client(request, 422, "Failed to parse HTTP line 1. Fix your request.");
		free(first_line);
		return 0;
	}

	int ok = parse_route(&request->route, endpoint);
	if (!ok) {
		write_to_client(request, 404, "Couldn't parse route from endpoint.");
		free(first_line);
		return 0;
	}

	ok = fillGetParams(request);
	if (!ok) {
		write_to_client(request, 422, "Couldn't parse GET params.");
		free(first_line);
		return 0;
	}

	ok = fillPostParams(request);
	if (!ok) {
		write_to_client(request, 422, "Couldn't parse POST params.");
		free(first_line);
		return 0;
	}

	free(first_line);
	return 1;
}

// This function is called from a threadpool worker, to handle the request.
void*
handle_request(httpContext* request) {
	int ok = parse_request(request);
	if (!ok) {
		// parse_request will send response to client.
		return NULL;
	}

	switch (request->route) {
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
		default:
			write_to_client(request, 404, "Not found");
			return NULL;
	}
	return NULL;
}

void*
threadpool_worker(void* arg) {
	int thread_idx = *((int*)arg);
	free(arg);
	httpContext* ctx;
	ctx = &requests[thread_idx];

	while (1) {
		int client_socket = socketqueue_pop(&SocketQueue);

		// 2m timout for client_socket.
		struct timeval timeout;
		timeout.tv_sec = 120;
		timeout.tv_usec = 0;
		auto sso = setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		if (sso < 0) {
			printf("Couldn't set the socket timeout.\n");
			close(client_socket);
			return NULL;
		}

		while (1) {
			httpContext_clear(ctx);
			char* buf = ctx->request_buf;
			// TODO What happens if we don't get the full request in one network packet/chunk? Figure this out later. Do the happy path first.
			ssize_t bytes_read = recv(client_socket, buf, 2047, 0);
			if (bytes_read < 1) {
				printf("Client socket. Either timeout or error. Closing.\n");
				break;
			} else if (bytes_read == 2047) {
				write_to_client(ctx, 413, "Request too large. Max is 2KB.");
				break;
			}

			ctx->client_socket = client_socket;
			handle_request(ctx);
		}
		close(client_socket);
	}
	return NULL;
}

int
listen_on_port(u16 port) {
	int server_fd;
	struct sockaddr_in address;
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	// To prevent macos from holding onto the port after the process completes. This safety net prevents
	// me from quickly starting a new server within seconds.
	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
		perror("CRITICAL ERROR. Bind failed. OS has probably locked the port in TIME_WAIT.");
		exit(1);
	}
	listen(server_fd, 100);
	printf("Server listening on port %d...\n", port);

	return server_fd;
}

// Should blow up program if fail.
void
load_filedata() {
	char buf[256];

	Accs = calloc(128, sizeof(Account));
	Accs[0].id = 0;
	Accs[0].type = 127;
	u16 id;
	char* name = calloc(32, 1);
	u16 type;
	if (fgets(buf, 256, AccountFile) == NULL) { printf("AccountFile empty.\n"); exit(1); }
	printf("Reading AccountFile. Headers:%s", buf);
	while(fgets(buf, 256, AccountFile) != NULL) {
		int count = sscanf(buf, "%hu\t%31[^\t]\t%hu", &id, name, &type);
		if (count != 3) {
			printf("Epic fail parsing AccountFile.\nsscanf returned:%d\nbuf:%s\n", count, buf);
			exit(1);
		}
		acc_append(id, name, type);
	}
	fseek(AccountFile, 0, SEEK_CUR);
	printf("Loaded %hu accounts\n", Accs[0].id);

	Txs = calloc(128, sizeof(Tx));
	Txs[0].id = 0;
	Txs[0].debit_account_id = 127;
	float amount;
	char* note = calloc(128, 1);
	u16 debit_account_id;
	u16 credit_account_id;
	u32 created_at;
	if (fgets(buf, 256, TxFile) == NULL) { printf("TxFile empty.\n"); exit(1); }
	printf("Reading TxFile. Headers:%s", buf);
	while(fgets(buf, 256, TxFile) != NULL) {
		int count = sscanf(buf, "%hu\t%f\t%127[^\t]\t%hu\t%hu\t%u", &id, &amount, note, &debit_account_id, &credit_account_id, &created_at);
		if (count != 6) {
			printf("Fail: Can't parse tx-file right.\nsscanf returned:%d\nbuf%s\n", count, buf);
			exit(1);
		}
		tx_append(id, amount, note, debit_account_id, credit_account_id, created_at);
	}
	fseek(TxFile, 0, SEEK_CUR);
	printf("Loaded %hu Txs\n", Txs[0].id);
}

void
handle_sigint(int sig) {
	printf("Signal %d rcvd. Closing files...\n", sig);
	if (AccountFile != NULL) {
		fflush(AccountFile);
		fclose(AccountFile);
		printf("Account file closed.\n");
	}
	if (TxFile != NULL) {
		fflush(TxFile);
		fclose(TxFile);
		printf("Tx file closed.\n");
	}
	exit(0);
}

int
main(int argc, char** argv) {
	if (argc != 3) {
		printf("Are you sure about that?\nUsage:\n\tserver 3002 directory/containing/files/\n\n");
		exit(1);
	}
	struct sigaction sa;
	sa.sa_handler = handle_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGINT, &sa, NULL) < 0) {
		printf("Error registering sigaction for SIGINT (Ctrl-c)\n");
		exit(1);
	}

	socketqueue_init(&SocketQueue);
	u16 port = (u16)atoi(argv[1]);
	char* dirpath = argv[2];
	char* account_path;
	asprintf(&account_path, "%saccounts.tsv", dirpath);
	AccountFile = fopen(account_path, "r+");
	if (!AccountFile) { printf("Can't load path:%s\n", account_path); return 1; }
	setvbuf(AccountFile, NULL, _IONBF, 0);
	char* tx_path;
	asprintf(&tx_path, "%stransactions.tsv", dirpath);
	TxFile = fopen(tx_path, "r+");
	if (!TxFile) { printf("Can't load path:%s\n", tx_path); return 1; }
	setvbuf(TxFile, NULL, _IONBF, 0);
	free(account_path);
	free(tx_path);
	load_filedata();

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		// TODO Switch to having each thread-worker declare it's own thread_local static var of this. This doesn't need to be a global.
		httpContext *req = &requests[i];
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

	int server_fd = listen_on_port(port);
	while (1) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		// This blocks till a connection comes through. Easy.
		int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
		if (client_socket < 0) {
			perror("accept failed");
			continue;
		}
		socketqueue_push(&SocketQueue, client_socket);
	}
	fclose(AccountFile);
	fclose(TxFile);
	return 0;
}

