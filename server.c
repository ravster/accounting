#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

// gcc server.c -o run -lpthread
// Test with
// echo "fff\t1" | nc localhost 3003

#define PORT 3003

/*
 * Returns the number of splits.
 * Writes into the out param, which is already malloc'd.
 */
int
string_split(char* in, char** out, char* delim, int max_count) {
	char* saveptr;
	int count = 0;
	char* substring = strtok_r(in, delim, &saveptr);

	while (substring != NULL) {
		printf("in loop. substring=%s\n", substring);
		out[count] = substring;
		++count;
		if (count == max_count) {
			// We allocated only this much.
			break;
		}
		substring = strtok_r(NULL, delim, &saveptr);
	}
	printf("count %d\n", count);
	return count;
}

// This function is run in a thread, to handle the request.
void*
handle_request(void* client_socket_ptr) {
	int client_socket = *((int*) client_socket_ptr);
	free(client_socket_ptr);
	char buf[2048];
	int bytes_read = read(client_socket, buf, 2047);
	buf[bytes_read] = 0;

	if (bytes_read == 2047) {
		char* msg = "Request too large. Max is 2KB.";
		char* response = NULL;
		asprintf( &response, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: %lu\r\n\r\n%s", strlen(msg), msg);
		write(client_socket, response, strlen(response));
		return NULL;
	}

	if (bytes_read <= 0) {
		close(client_socket);
		return NULL;
	}

	printf("Received\n%s\n", buf);

	// Get first line. Max 512B.
	char* line_end = strstr(buf, "\r\n");
	if (line_end == NULL) {
		// TODO err to client. bad request headers
	}
	size_t line_len = line_end - buf;
	if (line_len > 512) {
		// TODO err. url too big. bad request
	}
	// TODO This should be an array that is global, and then write to the entry that is for
	// this particular thread-index. This thread should know it's own index.
	char* first_line = malloc(513);
	first_line[0] = 0;
	strncpy(first_line, buf, line_len);
	first_line[line_len] = 0;
	printf("First line:%s\n", first_line);

	// Parse line-1
	char* http_method = malloc(8); http_method[0]=0;
	char* endpoint = malloc(256); endpoint[0]=0;
	char* http_version = malloc(16); http_version[0]=0;
	int sscanf_result = sscanf(first_line, "%7s %255s %15s", http_method, endpoint, http_version);
	if (sscanf_result != 3) {
		// TODO err bad request
	}

	// GET or POST request?
	printf("HTTP METHOD:%s\n", http_method);

	// What is the endpoint integer?
	printf("Endpoint:%s\n", endpoint);

	 char* response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
    write(client_socket, response, strlen(response));
    return NULL;

	// List all get params
	// List all post params

	// String split
	char** strings = calloc(20, sizeof(char*));
	int string_count = 0;
	string_count = string_split(buf, strings, "\t", 20);

	if (string_count < 2) {
		char* errmsg;
		asprintf(&errmsg, "Invalid string count:%d from string:%s\n", string_count, buf);
		fprintf(stderr, "%s", errmsg);
		dprintf(client_socket, "%s", errmsg);
		free(errmsg);
		close(client_socket);
		return NULL;
	}
	char* first = strings[0];
	char* second = strings[1];
	int code = atoi(second);

	char* resp = malloc(32);
	switch (code) {
		case 1:
			resp = "foo";
			break;
		case 2:
			resp = "bar";
			break;
		default:
			resp = "baz";
	}
	printf("1: %s\n2: %s\nResult: %s\n", first, second, resp);

	// Response
	char* out = malloc(256);
	out = resp;
	write(client_socket, out, strlen(out));

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
	int server_fd = listen_on_port();
	while (1) {
		// Build up client socket.
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int* new_socket = malloc(sizeof(int));
		// This blocks till a connection comes through. Easy.
		// In happy path, new_socket is freed by the worker thread.
		*new_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
		if (*new_socket < 0) {
			perror("accept failed");
			free(new_socket);
			continue;
		}

		// TODO: Instead of making a new OS thread, make a thread-pool.
		// Create a queue.
		// Then make 4-8 threads and have them read from the queue.
		// When a new client-socket is made, push that into the queue.
		// Figure out the pthreads way to wake all or some of the threads in the pool.
		// Make new thread to handle request.
		pthread_t thread_id;
		int err = 0;
		err = pthread_create(&thread_id, NULL, handle_request, (void*)new_socket);
		if (err != 0) {
			perror("Couldn't create thread");
			close(*new_socket);
			free(new_socket);
		} else {
			// Thread successfully made. We don't want it to be a zombie after it's done
			// work, so set it to clean up after it finishes execution.
			pthread_detach(thread_id);
		}
	}
}

