#include <stdatomic.h>
#include <semaphore.h>
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

// Far future:
// Should make a separate queue of pointers that need to be freed, and have a worker thread only for freeing temp memory.
// Make this a multi-user program. Useful for people that have multiple businesses or sth.

#define PORT 3002
#define THREAD_POOL_SIZE 4
// Queue size MUST be a power of 2. Makes ring-buffer-wrapping operations way easier.
#define QUEUE_CAPACITY 64
#define QUEUE_MASK (QUEUE_CAPACITY - 1)

#define LOG_FUNC \
    printf("%s\n", __func__)

typedef uint16_t u16;
typedef uint32_t u32;

// Basic struct used for random multiple things.
typedef struct {
	char* name;
	int int1;
	float total;
} StrInt;

// Globals so many functions can write to this.
FILE *account_file, *tx_file;
typedef struct {
	u16 id;
	float amount;
	char* note;
	u16 debit_account_id;
	u16 credit_account_id;
	u32 created_at;
} Tx;
Tx *txs;
u16 txLen, txCap;

Tx*
tx_append(u16 id, float amount, char* note, u16 debit_account_id, u16 credit_account_id, u32 created_at) {
	if (txLen == txCap) {
		txCap *= 2;
		txs = realloc(txs, txCap);
	}
	auto new_tx = &txs[txLen];
	new_tx->id = id;
	new_tx->amount = amount;
	new_tx->note = strdup(note);
	new_tx->debit_account_id = debit_account_id;
	new_tx->credit_account_id = credit_account_id;
	new_tx->created_at = created_at;
	txLen++;
	return new_tx;
}

void
tx_append_to_file(Tx* newTx) {
	fprintf(tx_file, "%hu\t%.2f\t%s\t%hu\t%hu\t%u\n",
			newTx->id, newTx->amount, newTx->note,
			newTx->debit_account_id, newTx->credit_account_id, newTx->created_at);
}

typedef struct {
	u16 id;
	char* name;
	u16 type;
} Account;
Account *accs;
u16 accLen, accCap;

void
acc_append(u16 id, char* name, u16 type) {
	if (accLen == accCap) {
		accCap *= 2;
		accs = realloc(accs, accCap);
	}
	auto new_acc = &accs[accLen];
	new_acc->id = id;
	new_acc->name = strdup(name);
	new_acc->type = type;
	accLen++;
}

int
acc_find_name(char* needle) {
	for (int i = 0; i< accLen; i++) {
		if (strcmp(accs[i].name, needle) == 0) {
			return 1;
		}
	}
	return 0;
}

void
acc_append_file(u16 id, char* name, u16 type) {
	fprintf(account_file, "%hu\t%s\t%hu\n", id, name, type);
}

char*
acc_name(u16 id) {
	return accs[id - 1].name;
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

// BEGIN lock-free queue implementation
// Not using the latest hardware is a moral failing. You are essentially nerfing your own hardware simply to think less. Don't be lazy. It's like buying a 2026 machine and then running it in 2006 mode. So wasteful.
typedef struct {
	int buffer[QUEUE_CAPACITY];
	// These numbers will constantly grow through the life of the program. They will not be wrapped back to zero by the application. To get the actual index in the buffer, we'll push each size_t through the QUEUE_MASK. Bit math is mind-bending and awesome.
	atomic_size_t head_ctr; // Consumers pop from here.
	atomic_size_t tail_ctr; // Producer pushes here.
	sem_t* sem;
} lock_free_queue;

// MUST be done by only 1 thread.
void
lfq_init(lock_free_queue* q) {
	atomic_init(&q->head_ctr, 0);
	atomic_init(&q->tail_ctr, 0);
	sem_unlink("/ravi1");
	q->sem = sem_open("/ravi1", O_CREAT, 0644, 0);
	if (q->sem == SEM_FAILED) {
		printf("SEM_OPEN failed.\n");
		exit(1);
	}
}

void
lfq_destroy(lock_free_queue* q) {
	if (q->sem != SEM_FAILED) {
		sem_close(q->sem);
		sem_unlink("/ravi1");
	}
}

// Push to the tail. Think of it like people coming lining up at a queue at TimHortons.
// Single Producer Multiple Consumers (SPMC)
// This is done only by one thread in this program, so we can safely use more relaxed memory_order_* settings.
int
lfq_push(lock_free_queue* q, int newVal) {
	// _relaxed gives us the fastest read from the local L1/L2 cache. In a generic program, this might be an outdated value. The atomic_store_explicit below will double-check this with the freshest value across all caches of all processors, so it isn't terribly unsafe.
	// Of course, because this program is SPMC, we know that we will get the correct value.
	auto current_tail_ctr = atomic_load_explicit(&q->tail_ctr, memory_order_relaxed);
	// _acquire will force this core to load up the freshest value of this variable from across all caches. this is done transparently by the hardware.
	// Think of think as acquiring the latest version of a variable from a source of truth. Or think of it as doing a git pull to get the latest commit in a branch.
	auto current_head_ctr = atomic_load_explicit(&q->head_ctr, memory_order_acquire);
	if ((current_tail_ctr - current_head_ctr) >= QUEUE_CAPACITY) {
		return 0; // TODO bad. queue full.
			// Drop this socket connection, main thread.
	}
	// Usually we'd worry about a race condition here. But because SPMC, we don't have that concern.
	q->buffer[current_tail_ctr & QUEUE_MASK] = newVal;
	// Memory writes in this thread that are above this line in the code WILL NOT be reordered by the CPU to be after this line below.
	// _release flushes the write the shared L3 cache, and signals to all the other processors that the value of this variable has changed.
	// Think of it like doing a git push to a remote.
	atomic_store_explicit(&q->tail_ctr, current_tail_ctr + 1, memory_order_release);
	sem_post(q->sem); // Increment the semaphore, so the kernel wakes up a thread.
	return 1;
}

// Pop from the head. The person at the front of the queue gets served next at TimHortons.
int
lfq_pop(lock_free_queue* q) {
	auto current_head_ctr = atomic_load_explicit(&q->head_ctr, memory_order_relaxed);
	while (true) {
		auto current_tail_ctr = atomic_load_explicit(&q->tail_ctr, memory_order_acquire);
		if (current_head_ctr == current_tail_ctr) {
			return 0; // Nothing found
		}
		auto possible_correct_head_value = q->buffer[current_head_ctr & QUEUE_MASK];
		auto is_written = atomic_compare_exchange_weak_explicit(
				&q->head_ctr, &current_head_ctr, current_head_ctr + 1,
				memory_order_release,
				memory_order_relaxed
				);
		if (is_written) {
			return possible_correct_head_value;
		} // If not written, then CAS failed. Some other consumer must have gotten this particular possible-head-value a few nanoseconds earlier. Redo the loop and try again. Spinloop this thread. The atomic func has updated current_head_ctr value so it doesn't need to be refreshed in the loop.
	}
}

lock_free_queue client_socket_queue = {0};

// END lock_free queue implementation

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
fillGetParams(httpContext* req) {
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
	char* reqBodyStart = strstr(req->request_buf, "\r\n\r\n");
	// Nothing to do.
	if ((reqBodyStart == NULL) || (strlen(reqBodyStart) == 0)) { return 1; }
	reqBodyStart += 4; // 2 CR and 2 NL.
	size_t newLen = strlen(reqBodyStart);
	req->postP = realloc(req->postP, newLen + 1);
	// +1 to get the automatic null-termination.
	strncpy(req->postP, reqBodyStart, newLen + 1);
	return 1;
}

httpContext requests[4];
// END httpContext object.

int // err
write_all(int socket, char* buffer, size_t len) {
	char* ptr = buffer;
	size_t written = 0;
	while (written < len) {
		auto just_wrote = write(socket, ptr, len - written);
		if (just_wrote < 1) {
			printf("write_all. Failed. just_wrote=%zd\n", just_wrote);
			return 1;
		}
		written += just_wrote;
		ptr += just_wrote;
	}
	return 0;
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
	char* buf = malloc(file_size + 1);
	size_t bytes_read = fread(buf, 1, file_size, file);
	buf[bytes_read] = 0;
	fclose(file);
	return buf;
}

// BEGIN Layer below request handlers.

sstr*
tr_of_every_account() {
	sstr *out = sstr_new(512);
	char* temp;
	char* acc_types[4] = {
		"Income",
		"Expense",
		"Asset",
		"Liability"
	};

	for (u16 i = 0; i < accLen; i++) {
		auto acc = accs[i];
		char* type = acc_types[acc.type];
		asprintf(&temp,
			"<tr>"
			  "<td>%hu</td>"
			  "<td>%s</td>"
			  "<td>%s</td>"
			"</tr>\n",
			acc.id,
			acc.name,
			type
		);
		sstr_append(out, temp);
		free(temp);
	}
	return out;
}

char* account_name_from_id_;
// Called at start of program.
u16 // ok
account_name_from_id_prepopulate() {
	char* a1 = malloc(1024); a1[0]=0;
	size_t a1len = 0;
	size_t a1cap = 1024;
	char* eachPair = malloc(512);
	auto total_rows = 3;
	for (u16 i=0; i < total_rows; i++) {
		snprintf(eachPair, 512,
				"%s=%s&",
				"foo",
				"foo"
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
ledger_newest_30_newstr() {
	sstr *out = sstr_new(4096);
	char* temp = calloc(1024, 1);
	auto total_rows = 30;
	if (txLen < 30) { total_rows = txLen; }
	for (int i = txLen - 1; i >= txLen - total_rows; i--) {
		auto tx = txs[i];
		auto debit_acct_name = acc_name(tx.debit_account_id);
		auto credit_acct_name = acc_name(tx.credit_account_id);
		size_t written_to_temp = snprintf(temp, 1024,
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

char*
account_selection_options_new() {
	sstr* out = sstr_new(1024);
	char* temp = calloc(1024, 1);
	for (u16 i = 0; i < accLen; i++) {
		auto acc = accs[i];
		size_t written_to_temp = snprintf(temp, 1024,
			"<option value=\"%hu\">%s</option>",
			acc.id,
			acc.name
		);
		if (written_to_temp >= 1024) {
			printf("ERR: account_selection_options: temp truncated to 1024: %s\n", temp);
		}
		sstr_append(out, temp);
	}
	char* out2 = strdup(out->buf);
	free(temp);
	free(out);
	return out2;
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
	auto getPresult = sscanf(getP, "m=%hd&y=%hd", month, year);
	if (getPresult != 2) {
		auto t1 = time(NULL); // Use current month & year
		auto* t2 = localtime(&t1);
		*year = t2->tm_year + 1900;
		*month = t2->tm_mon+ 1;
	}
	*start = (*year*10000) + (*month * 100) + 1;
	auto endMonth = *month + 1;
	auto endYear = *year;
	if (endMonth == 13) {
		endMonth = 1;
		endYear = *year + 1;
	}
	*stop = (endYear * 10000) + (endMonth * 100) + 1;

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
	auto len = strints[0].int1;
	u16 outLen = 0; u16 outCap = 2048; char* out = calloc(2048, 1);
	char tr[128];
	char* trTemplate;
	switch (accType) {
		case 2:
			trTemplate = "<tr> <td>%s</td> <td>%.2f</td> <td></td> </tr>\n";
			break;
		case 1:
			trTemplate = "<tr> <td>%s</td> <td></td> <td>%.2f</td> </tr>\n";
			break;
	}
	for (int i = 1; i<= len; i++) {
		auto it = strints[i];
		auto trLen = snprintf(tr, 128, trTemplate, it.name, it.total);
		if (trLen >=128) {
			printf("WARN: Truncated trLen when doing incomeStatement. name:%s tot:%f\n",
					it.name, it.total);
			trLen--; // For the benefit of memcpy below;
		}
		if (1+ outLen + trLen > outCap) {
			outCap *= 2;
			out = realloc(out, outCap);
		}
		memcpy(out + outLen, tr, trLen + 1);
		outLen += trLen;
	}
	return out;
}

int
compare_strint_desc(const void* a, const void* b) {
	auto sa = (StrInt*)a;
	auto sb = (StrInt*)b;
	return sb->total - sa->total;
}

typedef struct {
	u16 id;
	int32_t total;
	char* name;
} BsAccTotal;
typedef struct {
	BsAccTotal* data;
	u16 len;
	u16 cap;
} BsAccs;

BsAccs*
bs_accs_new() {
	BsAccs* out = malloc(sizeof(BsAccs));
	out->cap = 20;
	out->len = 0;
	out->data = calloc(out->cap, sizeof(BsAccs));
	return out;
}

void bs_accs_append(BsAccs* bsAccs, u16 id, char* name) {
	if (bsAccs->len == bsAccs->cap) {
		bsAccs->cap *= 2;
		bsAccs->data = realloc(bsAccs->data, bsAccs->cap * sizeof(BsAccs));
	}
	auto acc = &bsAccs->data[bsAccs->len];
	acc->id = id;
	acc->name = name;
	bsAccs->len++;
}

void bs_accs_populate_new(BsAccs *assets, BsAccs *liabilities) {
	for (u16 i = 0; i < accLen; i++) {
		auto acc = accs[i];
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
		auto *it = &bsAccs->data[i];
		if (it->id == id) {
			return it;
		}
	}
	return NULL;
}

void bs_accs_calc_totals(BsAccs* bs_accs_a, BsAccs* bs_accs_l, u32 stop) {
	for (u16 i=0; i<txLen; i++) {
		auto tx = txs[i];
		if (tx.created_at > stop) { continue; }

		// TODO maybe bs_accs_a and bs_accs_l should be one array? Don't know. Pray on it.
		auto* bsacc = bs_accs_find_by_id(bs_accs_a, tx.debit_account_id);
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
bs_accs_trs_new(BsAccs* accs, uint8_t accType) {
	u16 outLen=0; u16 outCap=1024; char* out= calloc(outCap, 1);
	for (u16 i=0; i<accs->len; i++) {
		auto it = accs->data[i];
		if (outLen > outCap-90) {
			outCap *=2;
			out = realloc(out, outCap);
		}
		u16 written = 100;
		switch (accType) {
			case ASSET:
				written = snprintf(out+outLen, 89,
						"<tr><td>%s</td><td>%d</td><td></td></tr>\n", it.name, it.total);
				break;
			case LIABILITY:
				written = snprintf(out+outLen, 89,
						"<tr><td>%s</td><td></td><td>%d</td></tr>\n", it.name, it.total);
				break;
			default:
				LOG_FUNC;
				printf("Shouldn't have gotten to this default case.\n");
		}
		if (written > 89) {
			LOG_FUNC;
			printf("snprintf fail. name=%s total=%u\n", it.name, it.total);
		}
		outLen += written;
	}
	return out;
}

// END Layer below request handlers.

void
incomeStatement(httpContext* request) {
	auto template = read_file_newstr("templates/incomeStatement.html");

	u16 month, year;
	u32 start, stop;
	char prevLink[12], nextLink[12];
	calc_month(&month, &year, &start, &stop, prevLink, nextLink, request->getP);
	char* body;
	float netProfitDollars = 0;

	// List of Tx that are in the time period.
	u16 periodTxsLen = 0; u16 periodTxsCap = 64; Tx* periodTxs = calloc(periodTxsCap, sizeof(Tx));
	for (u16 i = 0; i < txLen; i++) {
		auto tx = txs[i];
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
	StrInt* incomeAccs = calloc(accLen, sizeof(StrInt));
	incomeAccs[0].total = 0; // Use this as array length
	StrInt* expenseAccs = calloc(accLen, sizeof(StrInt));
	expenseAccs[0].total = 0; // Use this as array length
	StrInt* newStrInt;

	for (u16 i = 0; i < accLen; i++) {
		auto acc = accs[i];
		switch (acc.type) {
			case INCOME:
				tot = 0;
				for (u16 i = 0; i < periodTxsLen; i++) {
					auto tx = periodTxs[i];
					if (tx.credit_account_id != acc.id) { continue; }
					tot += tx.amount;
				}
				netProfitDollars += tot;
				auto iaLen = incomeAccs[0].int1;
				newStrInt = &incomeAccs[iaLen+1];
				newStrInt->name = strdup(acc.name);
				newStrInt->total = tot;
				incomeAccs[0].int1++;
				break;
			case EXPENSE:
				tot = 0;
				for (u16 i = 0; i < periodTxsLen; i++) {
					auto tx = periodTxs[i];
					if (tx.debit_account_id != acc.id) { continue; }
					tot += tx.amount;
				}
				netProfitDollars -= tot;
				auto eaLen = expenseAccs[0].int1;
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
	qsort(incomeAccs+1, incomeAccs[0].int1, sizeof(StrInt), compare_strint_desc);
	qsort(expenseAccs+1, expenseAccs[0].int1, sizeof(StrInt), compare_strint_desc);

	auto itrs = incomeStatementTrsNew(incomeAccs, INCOME);
	auto etrs = incomeStatementTrsNew(expenseAccs, EXPENSE);
	auto itrlen = strlen(itrs);
	auto etrlen = strlen(etrs);

	char* trs = calloc(itrlen + etrlen + 1, 1);
	memcpy(trs, itrs, itrlen + 1);
	memcpy(trs + itrlen, etrs, etrlen + 1);

	asprintf(&body, template, prevLink, nextLink, trs, netProfitDollars);
	write_to_client(request, 200, body);
	free(itrs); free(etrs); free(trs);
	free(periodTxs);
	free(body);
	free(template);
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
		return;
	}
	// TODO Check debit and credit id map to actual accounts
	Tx* lastTx = &txs[txLen - 1];
	u16 newId = lastTx->id + 1;
	time_t t1 = time(NULL);
	struct tm* t2 = localtime(&t1);
	char timeBuf[9];
	strftime(timeBuf, 9, "%Y%m%d", t2);
	auto newTx = tx_append(newId, atof(amount), note, atoi(debitID), atoi(creditID), atoi(timeBuf));
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
	int is_name_found = acc_find_name(name2);
	if (is_name_found) {
		char* out;
		asprintf(&out, "The account name:%s is already taken.", name2);
		write_to_client(request, 422, out);
		free(out); free(name2); free(name); free(type);
		return;
	}
	u16 new_account_id = accLen + 1;
	int type_i = atoi(type);
	if ((type_i > LIABILITY) || (type_i < INCOME)) {
		char* out;
		asprintf(&out, "type_i=%d is invalid", type_i);
		write_to_client(request, 422, out);
		free(out); free(name2); free(name); free(type);
		return;
	}
	acc_append(new_account_id, name2, type_i);
	acc_append_file(new_account_id, name2, type_i);
	write_redirect(request, 303, "/2");
	free(name2); free(name); free(type);
	return;
}

void
testPost(httpContext* req) {
	char* a1;
	char* bigText = params_get_newstr(req->postP, "note");
	char* a2 = strdup(bigText);
	url_decode(a2);
	asprintf(&a1, "<p>got this:%s</p> <p>After url-decoding it is:%s</p>", bigText, a2);
	write_to_client(req, 200, a1);
	free(a1);
}

void
listLedger(httpContext* request) {
	char* body = read_file_newstr("templates/ledger.html");
	char* a1;
	sstr* ln30 = ledger_newest_30_newstr();
	char* aso = account_selection_options_new();
	asprintf(&a1, body, aso, aso, ln30->buf);
	write_to_client(request, 200, a1);
	sstr_free(ln30);
	free(aso);
	free(a1);
	free(body);
}

void
listAccounts(httpContext* request) {
	char* body = read_file_newstr("templates/listAccounts.html");
	char* a1;
	sstr* trs = tr_of_every_account();
	asprintf(&a1, body, trs->buf);
	write_to_client(request, 200, a1);
	sstr_free(trs);
	free(a1);
	free(body);
}

void
homePage(httpContext* request) {
	char* body = read_file_newstr("templates/home.html");
	write_to_client(request, 200, body);
	free(body);
}

void
balanceSheet(httpContext* request) {
	auto template = read_file_newstr("templates/balanceSheet.html");
	u16 month, year;
	u32 start, stop;
	char prevLink[12], nextLink[12];
	calc_month(&month, &year, &start, &stop, prevLink, nextLink, request->getP);
	char* body;
	auto bs_accs_a = bs_accs_new();
	auto bs_accs_l = bs_accs_new();
	bs_accs_populate_new(bs_accs_a, bs_accs_l); // Asset, Liability
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
	free(template);
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
	size_t line_len = line_end - buf;
	if (line_len > 512) {
		write_to_client(request, 413, "Request endpoint too large. We aren't parsing over 512B.");
		return 0;
	}

	char* first_line = calloc(1, 512);
	strncpy(first_line, buf, line_len);
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
	int thread_idx = *((int*)arg);
	free(arg);
	httpContext* ctx;
	ctx = &requests[thread_idx];

	// TODO right now we replicate HTTP1.0 by closing every HTTP connection after giving the response to the
	// browser. This is bad. HTTP1.1 allows for persistent connections, and we should use it. Each thread should
	// hold on to it's client socket and do the recv() call in a loop. Browsers already reuse HTTP 1.1 conns,
	// so we just need to recv() on the conn till we get more information. Set a timeout of 5 minutes to
	// kill the connection if we get no data.
	//
	// Note that keeping connections open requires that we have a multi-threaded program. So much for the plan
	// of switching to a single-thread. Oh well. If the world has made keep-alive a default expectation, I
	// should roll with it, and not be too much of a hipster.
	while (1) {
		sem_wait(client_socket_queue.sem); // Apparently semaphores just don't have spurious wakeups. Nice.
		int client_socket = lfq_pop(&client_socket_queue);
		struct timeval timeout;
		timeout.tv_sec = 120;
		auto sso = setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		if (sso < 0) {
			printf("Couldn't set the socket timeout.\n");
			close(client_socket);
			return 0;
		}
		// We got a client socket from the main thread. Now we have to serve this client multiple times till the client times out or closes the conn.

		while (1) {
			httpContext_clear(ctx);
			char* buf = ctx->request_buf;
			// TODO What happens if we don't get the full request in one network packet/chunk? Figure this out later. Do the happy path first.
			int bytes_read = recv(client_socket, buf, 2047, 0);
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

// Should blow up program if fail.
void
load_filedata() {
	char buf[256];

	rewind(account_file);
	accCap = 128; accLen = 0;
	accs = calloc(accCap, sizeof(Account));
	u16 id;
	char* name = calloc(32, 1);
	u16 type;
	fgets(buf, 256, account_file); // Skip headers
	printf("Reading account_file. Headers:%s", buf);
	while(fgets(buf, 256, account_file) != NULL) {
		int count = sscanf(buf, "%hu\t%31[^\t]\t%hu", &id, name, &type);
		if (count != 3) {
			printf("Epic fail parsing account_file.\nsscanf returned:%d\nbuf:%s\n", count, buf);
			exit(1);
		}
		acc_append(id, name, type);
	}
	printf("Loaded %hu accounts\n", accLen);

	rewind(tx_file);
	txCap = 128; txLen = 0;
	txs = calloc(txCap, sizeof(Tx));
	float amount;
	char* note = calloc(128, 1);
	u16 debit_account_id;
	u16 credit_account_id;
	u32 created_at;
	fgets(buf, 256, tx_file); // Skip headers.
	printf("Reading tx_file. Headers:%s", buf);
	while(fgets(buf, 256, tx_file) != NULL) {
		int count = sscanf(buf, "%hu\t%f\t%127[^\t]\t%hu\t%hu\t%u", &id, &amount, note, &debit_account_id, &credit_account_id, &created_at);
		if (count != 6) {
			printf("Fail: Can't parse tx-file right.\nsscanf returned:%d\nbuf%s\n", count, buf);
			exit(1);
		}
		tx_append(id, amount, note, debit_account_id, credit_account_id, created_at);
	}
	printf("Loaded %hu txs\n", txLen);
}

int
main(int argc, char** argv) {
	if (argc != 2) {
		printf("Are you sure about that?\nUsage:\n\tserver directory/containing/files/\n\n");
		exit(1);
	}

	char* dirpath = argv[1];
	char* account_path;
	asprintf(&account_path, "%saccounts.tsv", dirpath);
	account_file = fopen(account_path, "a+");
	setvbuf(account_file, NULL, _IONBF, 0);
	if (!account_file) { printf("Can't load path:%s\n", account_path); return 1; }
	char* tx_path;
	asprintf(&tx_path, "%stransactions.tsv", dirpath);
	tx_file = fopen(tx_path, "a+");
	setvbuf(tx_file, NULL, _IONBF, 0);
	if (!tx_file) { printf("Can't load path:%s\n", tx_path); return 1; }
	free(account_path);
	free(tx_path);
	load_filedata();

	lfq_init(&client_socket_queue);
	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
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
			lfq_destroy(&client_socket_queue);
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
		lfq_push(&client_socket_queue, client_socket);
	}
	lfq_destroy(&client_socket_queue);
	fclose(account_file);
	fclose(tx_file);
	return 0;
}

