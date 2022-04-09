#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "xhttp.h"

/* +-----------------+
 * | OVERVIEW |
 * | 
 */

typedef enum { XH_REQ, XH_RES } struct_type_t;

typedef struct {
	struct_type_t    type;
	xh_response    public;
	xh_header    *headers;
	unsigned int  headerc;
	unsigned int capacity;
	_Bool failed;
} xh_response2;

typedef struct {
	struct_type_t type;
	xh_request  public;
} xh_request2;

typedef struct {
	char    *data;
	uint32_t size;
	uint32_t used;
} buffer_t;

typedef struct conn_t conn_t;
struct conn_t {
	conn_t *prev;
	conn_t *next;
	buffer_t in;
	buffer_t out;
	int fd, served;
	_Bool close_when_uploaded;
	_Bool failed_to_append;
	_Bool head_received;
	uint32_t body_offset;
    uint32_t body_length;
    xh_request2  request;
};

static _Bool set_non_blocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if(flags < 0)
		return 0;

	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* Accepts a connection from [fd], creates a connection
 * struct for it and registers it with the epoll.
 */
static void accept_connection(int fd, int epfd, conn_t **freelist, int *connum)
{
	int cfd = accept(fd, NULL, NULL);

	if(cfd < 0)
		return; // Failed to accept.

	if(!set_non_blocking(cfd))
	{
		(void) close(cfd);
		return;
	}

	if(*freelist == NULL)
	{
		// Connection limit reached.
		(void) close(cfd);
		return;
	}

	conn_t *conn = *freelist;
	*freelist = conn->next;

	memset(conn, 0, sizeof(conn_t));
	conn->fd = cfd;
	conn->request.type = XH_REQ;

	struct epoll_event buffer;
	buffer.events = EPOLLET | EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLRDHUP;
	buffer.data.ptr = conn;
	if(epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &buffer))
	{
		(void) close(cfd);

		conn->next = *freelist;
		*freelist = conn;
		return;
	}

	*connum += 1;
}

static void close_connection(conn_t *conn, conn_t **freelist, int *connum)
{
	(void) close(conn->fd);

	if(conn-> in.data != NULL) 
		free(conn-> in.data);
	
	if(conn->out.data != NULL) 
		free(conn->out.data);

	if(conn->request.public.headers != NULL) 
		free(conn->request.public.headers);

	conn->next = *freelist;
	*freelist = conn;

	*connum -= 1;
}

static _Bool is_uppercase_alpha(char c)
{
	return c >= 'A' && c <= 'Z';
}

static _Bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static _Bool is_space(char c)
{
	return c == ' ';
}

static void skip(char *str, uint32_t len, uint32_t *i, _Bool not, _Bool (*test)(char))
{
	if(not)
		while(*i < len && !test(str[*i]))
			*i += 1;
	else
		while(*i < len && test(str[*i]))
			*i += 1;
}

static void skip_until(char *str, uint32_t len, uint32_t *i, char c)
{
	while(*i < len && str[*i] != c)
		*i += 1;
}

struct parse_err_t { 
	_Bool   internal; 
	char        *msg; 
	unsigned int len; 
};

static struct parse_err_t parse(char *str, uint32_t len, xh_request *req)
{
	#define OK \
		((struct parse_err_t) { .internal = 0, .msg = NULL})

	#define FAILURE(msg_) \
		((struct parse_err_t) { .internal = 0, .msg = msg_, .len = sizeof(msg_)-1 })

	#define INTERNAL_FAILURE(msg_) \
		((struct parse_err_t) { .internal = 1, .msg = msg_, .len = sizeof(msg_)-1 })

	if(len == 0)
		return FAILURE("Empty request");

	uint32_t i = 0;

	uint32_t method_offset = i;

	skip(str, len, &i, 0, is_uppercase_alpha);

	uint32_t method_length = i - method_offset;

	if(method_length == 0)
		return FAILURE("Missing method");

	if(i == len)
		return FAILURE("Missing URL and HTTP version");

	if(!is_space(str[i]))
		return FAILURE("Bad character after method. Methods can only have uppercase alphabetic characters");

	skip(str, len, &i, 0, is_space);

	if(i == len)
		return FAILURE("Missing URL and HTTP version");

	uint32_t URL_offset = i;

	skip_until(str, len, &i, ' ');

	uint32_t URL_length = i - URL_offset;

	assert(URL_length > 0);

	if(i == len)
		return FAILURE("Missing HTTP version");

	assert(is_space(str[i]));

	skip(str, len, &i, 0, is_space);

	if(i == len)
		return FAILURE("Missing HTTP version");

	uint32_t version_offset = i;

	skip_until(str, len, &i, '\r');

	uint32_t version_length = i - version_offset;

	if(version_length == 0)
		return FAILURE("Missing HTTP version");

	if(i == len)
		return FAILURE("Missing CRLF after HTTP version");

	assert(str[i] == '\r');

	i += 1; // Skip the \r.

	if(i == len)
		return FAILURE("Missing LF after CR");

	if(str[i] != '\n')
		return FAILURE("Missing LF after CR");

	i += 1; // Skip the \n.

	int capacity = 0, headerc = 0;
	xh_header *headers = NULL;

	while(1)
	{
		if(i == len)
		{
			if(headers != NULL) free(headers);
			return FAILURE("Missing blank line");
		}

		if(i+1 < len && str[i] == '\r' && str[i+1] == '\n')
		{
			// Blank line.
			i += 2;
			break;
		}

		uint32_t hname_offset = i;

		skip_until(str, len, &i, ':');

		uint32_t hname_length = i - hname_offset;

		if(i == len)
		{
			if(headers != NULL) free(headers);
			return FAILURE("Malformed header");
		}

		if(hname_length == 0)
		{
			if(headers != NULL) free(headers);
			return FAILURE("Empty header name");
		}

		assert(str[i] == ':');

		// Make the header name zero-terminated
		// by overwriting the ':' with a '\0'.
		str[i] = '\0';

		i += 1; // Skip the ':'.

		uint32_t hvalue_offset = i;

		do
		{
			skip_until(str, len, &i, '\r');

			if(i == len)
			{
				if(headers != NULL) free(headers);
				return FAILURE("Malformed header");
			}

			assert(str[i] == '\r');
			
			i += 1; // Skip the \r.

			if(i == len)
			{
				if(headers != NULL) free(headers);
				return FAILURE("Malformed header");
			}
		}
		while(str[i] != '\n');
		assert(str[i] == '\n');
		i += 1; // Skip the '\n'.

		uint32_t hvalue_length = (i - 2) - hvalue_offset;

		if(headerc == capacity)
		{
			int new_capacity = capacity == 0 ? 8 : capacity * 2;

			void *temp = realloc(headers, new_capacity * sizeof(xh_header));
		
			if(temp == NULL)
			{
				if(headers != NULL) free(headers);
				return INTERNAL_FAILURE("No memory");
			}

			capacity = new_capacity;
			headers = temp;
		}

		headers[headerc++] = (xh_header) { 
			.name      = str + hname_offset, 
			.name_len  =       hname_length,
			.value     = str + hvalue_offset, 
			.value_len =       hvalue_length,
		};

		str[ hname_offset +  hname_length] = '\0';
		str[hvalue_offset + hvalue_length] = '\0';
	}

	req->headers = headers;
	req->headerc = headerc;

	req->method  = str +  method_offset;
	req->URL     = str +     URL_offset;

	str[ method_offset +  method_length] = '\0';
	str[    URL_offset +     URL_length] = '\0';
	str[version_offset + version_length] = '\0';

	// Validate the header.
	{
		_Bool unknown_method = 0;

		#define PAIR(p, q) (uint64_t) (((uint64_t) p << 32) | (uint64_t) q)
		switch(PAIR(req->method[0], method_length))
		{
			case PAIR('G', 3): req->method_id = XH_GET;     unknown_method = !!strcmp(req->method, "GET"); 	break;
			case PAIR('H', 4): req->method_id = XH_HEAD;    unknown_method = !!strcmp(req->method, "HEAD"); break;
			case PAIR('P', 4): req->method_id = XH_POST;    unknown_method = !!strcmp(req->method, "POST"); break;
			case PAIR('P', 3): req->method_id = XH_PUT;     unknown_method = !!strcmp(req->method, "PUT"); 	break;
			case PAIR('D', 6): req->method_id = XH_DELETE;  unknown_method = !!strcmp(req->method, "DELETE");  break;
			case PAIR('C', 7): req->method_id = XH_CONNECT; unknown_method = !!strcmp(req->method, "CONNECT"); break;
			case PAIR('O', 7): req->method_id = XH_OPTIONS; unknown_method = !!strcmp(req->method, "OPTIONS"); break;
			case PAIR('T', 5): req->method_id = XH_TRACE;   unknown_method = !!strcmp(req->method, "TRACE"); break;
			case PAIR('P', 5): req->method_id = XH_PATCH;   unknown_method = !!strcmp(req->method, "PATCH"); break;
			default: unknown_method = 1; break;
		}
		#undef PAIR

		if(unknown_method)
		{
			if(headers != NULL) free(headers);
			return FAILURE("Unknown method");
		}
	}

	// Validate the HTTP version
	{
		_Bool bad_version = 0;
		switch(version_length)
		{
			case sizeof("HTTP/M.N")-1:

			if(!strcmp(str + version_offset, "HTTP/0.9"))
			{
				req->version_major = 0;
				req->version_minor = 9;
				break;
			}

			if(!strcmp(str + version_offset, "HTTP/1.0"))
			{
				req->version_major = 1;
				req->version_minor = 0;
				break;
			}

			if(!strcmp(str + version_offset, "HTTP/1.1"))
			{
				req->version_major = 1;
				req->version_minor = 1;
				break;
			}

			if(!strcmp(str + version_offset, "HTTP/2.0"))
			{
				req->version_major = 2;
				req->version_minor = 0;
				break;
			}

			if(!strcmp(str + version_offset, "HTTP/3.0"))
			{
				req->version_major = 3;
				req->version_minor = 0;
				break;
			}
			
			bad_version = 1;	
			break;

			case sizeof("HTTP/M")-1:

			if(!strcmp(str + version_offset, "HTTP/1"))
			{
				req->version_major = 1;
				req->version_minor = 0;
				break;
			}

			if(!strcmp(str + version_offset, "HTTP/2"))
			{
				req->version_major = 2;
				req->version_minor = 0;
				break;
			}

			if(!strcmp(str + version_offset, "HTTP/3"))
			{
				req->version_major = 3;
				req->version_minor = 0;
				break;
			}

			bad_version = 1;
			break;

			default:
			bad_version = 1;
			break;
		}

		if(bad_version)
		{
			if(headers != NULL) free(headers);
			return FAILURE("Bad HTTP version");
		}
	}

	return OK;

	#undef OK
	#undef FAILURE
	#undef INTERNAL_FAILURE
}

static _Bool upload(conn_t *conn)
{
	if(conn->failed_to_append)
		return 0;

	uint32_t sent, total;

	sent = 0;
	total = conn->out.used;

	while(sent < total)
	{
		int n = send(conn->fd, conn->out.data + sent, total - sent, 0);

		if(n < 0)
		{
			if(errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			// ERROR!
			return 0;
		}

		assert(n >= 0);
		sent += n;
	}

	memmove(conn->out.data, conn->out.data + sent, total - sent);
	conn->out.used -= sent;
	return 1;
}

static uint32_t find(const char *str, uint32_t len, const char *seq)
{
	if(seq == NULL || seq[0] == '\0')
		return UINT32_MAX;

	if(str == NULL || len == 0)
		return UINT32_MAX;

	uint32_t i = 0, seqlen = strlen(seq);
	while(1)
	{
		while(i < len && str[i] != seq[0])
			i += 1;

		if(i == len)
			return UINT32_MAX;

		assert(str[i] == seq[0]);

		if(i > len - seqlen)
			return UINT32_MAX;

		if(!strncmp(seq, str + i, seqlen))
			return i;

		i += 1;
	}
}

static void append(conn_t *conn, const char *str, int len)
{
	if(conn->failed_to_append)
		return;

	if(str == NULL || len == 0)
		return;

	if(len < 0)
		len = strlen(str);

	assert(len > 0);

	if(conn->out.size - conn->out.used < (uint32_t) len)
	{
		uint32_t new_size = 2 * conn->out.size;
		
		if(new_size < conn->out.used + (uint32_t) len)
			new_size = conn->out.used + len;

		void *temp = realloc(conn->out.data, new_size);

		if(temp == NULL)
		{
			conn->failed_to_append = 1;
			return;
		}

		conn->out.data = temp;
		conn->out.size = new_size;
	}

	memcpy(conn->out.data + conn->out.used, str, len);
	conn->out.used += len;
	return;
}

static void xh_response_init(xh_response2 *res)
{
	res->public.status_code = -1;
	res->public.status_text = NULL;
	res->public.headers = NULL;
	res->public.headerc = 0;
	res->public.body = NULL;
	res->public.body_len = 0;
	res->public.close = 0;
	res->type = XH_RES;
	res->headers = NULL;
	res->headerc = 0;
	res->capacity = 0;
	res->failed = 0;
}

static void xh_response_deinit(xh_response2 *res)
{
	for(unsigned int i = 0; i < res->headerc; i += 1)
		free(res->headers[i].name);

	if(res->headers != NULL)
		free(res->headers);
}

static _Bool respond(conn_t *conn, conn_t **freelist, int *connum, void (*callback)(xh_request*, xh_response*))
{
	_Bool keep_alive;
	{
		const char *h_connection = xh_hget(&conn->request.public, "Connection");

		if(h_connection == NULL)
			// No [Connection] header. No keep-alive.
			keep_alive = 0;
		else
		{
			// TODO: Make string comparisons case and whitespace insensitive.
			if(!strcmp(h_connection, " Keep-Alive"))
				keep_alive = 1;
			else if(!strcmp(h_connection, " Close"))
				keep_alive = 0;
			else
				keep_alive = 0;
		}
	}

	_Bool head_only = conn->request.public.method_id == XH_HEAD;

	if(head_only)
	{
		conn->request.public.method_id = XH_GET;
		conn->request.public.method = "HEAD";
		conn->request.public.method_len = sizeof("HEAD")-1;
	}

	xh_response2 res;
	xh_response_init(&res);

	callback(&conn->request.public, (xh_response*) &res.public);

	if(conn->request.public.headers != NULL)
	{
		free(conn->request.public.headers);
		conn->request.public.headers = NULL;
	}

	if(res.public.close)
		keep_alive = 0;

	xh_hadd(&res.public, "Content-Length", "%d", res.public.body_len);
	xh_hadd(&res.public, "Connection", keep_alive ? "Keep-Alive" : "Close");

	if(res.failed)
	{
		// Failed to build the response. We'll send a 500.
		append(conn, "HTTP/1.1 500 Internal Server Error\r\n", -1);
		append(conn, keep_alive ? "Connection: Keep-Alive\r\n" : "Connection: Close\r\n", -1);
	}
	else
	{
		char buffer[256];

		int n = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %d %s\r\n", res.public.status_code, res.public.status_text);
		assert(n >= 0);

		if((unsigned int) n > sizeof(buffer)-1)
			n = sizeof(buffer)-1;

		append(conn, buffer, n);

		for(unsigned int i = 0; i < res.headerc; i += 1)
		{
			append(conn, res.headers[i].name, res.headers[i].name_len);
			append(conn, ": ", 2);
			append(conn, res.headers[i].value, res.headers[i].value_len);
			append(conn, "\r\n", 2);
		}

		append(conn, "\r\n", 2);

		if(head_only == 0 && res.public.body != NULL && res.public.body_len > 0)
			append(conn, res.public.body, res.public.body_len);
	}

	xh_response_deinit(&res);

	conn->served += 1;

	if(!upload(conn))
	{
		close_connection(conn, freelist, connum);
		return 0;
	}

	if(!keep_alive)
	{
		if(conn->out.used == 0)
			close_connection(conn, freelist, connum);
		else 
			conn->close_when_uploaded = 1;
		return 0;
	}

	return 1;
}

static uint32_t determine_content_length(xh_request *req)
{
	unsigned int i;
	for(i = 0; i < req->headerc; i += 1)
		if(!strcmp(req->headers[i].name, "Content-Length")) // TODO: Make it case-insensitive.
			break;

	if(i == req->headerc)
		// No Content-Length header.
		// Assume a length of 0.
		return 0;

	const char *s = req->headers[i].value;
	unsigned int k = 0;

	while(is_space(s[k]))
		k += 1;

	if(s[k] == '\0')
		// Header Content-Length is empty.
		// Assume a length of 0.
		return 0;

	if(!is_digit(s[k]))
		// The first non-space character
		// isn't a digit. That's bad.
		return UINT32_MAX;

	uint32_t result = s[k] - '0';

	k += 1;

	while(is_digit(s[k]))
	{
		result = result * 10 + s[k] - '0';
		k += 1;
	}

	while(is_space(s[k]))
		k += 1;

	if(s[k] != '\0')
		// The header contains something other
		// than whitespace and digits. Bad.
		return UINT32_MAX;

	return result;
}

static void when_data_is_ready_to_be_read(conn_t *conn, conn_t **freelist, int *connum, void (*callback)(xh_request*, xh_response*))
{
	// Download the data in the input buffer.
	uint32_t downloaded;
	{
		buffer_t *b = &conn->in;
		uint32_t before = b->used;
		while(1)
		{
			if(b->size - b->used < 128)
			{
				uint32_t new_size = (b->size == 0) ? 512 : (2 * b->size);
			
				void *temp = realloc(b->data, new_size);

				if(temp == NULL)
				{
					// ERROR!
					close_connection(conn, freelist, connum);
					return;
				}

				// TODO: Change the pointers in conn->request
				//       if the head was already parsed.
				
				b->data = temp;
				b->size = new_size;
			}

			int n = recv(conn->fd, b->data + b->used, b->size - b->used, 0);

			if(n <= 0)
			{
				if(n == 0)
				{
					// Peer disconnected. 
					close_connection(conn, freelist, connum);
					return;
				}
		
				if(errno == EAGAIN || errno == EWOULDBLOCK)
					break; // Done downloading.

				// ERROR!
				close_connection(conn, freelist, connum);
				return;
			}

			b->used += n;
		}
		downloaded = b->used - before;
	}

	int served = 0;

	while(1)
	{
		if(!conn->head_received)
		{
			// Search for an \r\n\r\n.
			uint32_t i;
			{
				uint32_t start = 0;
				if(served == 0 && conn->in.used > downloaded + 3)
					start = conn->in.used - downloaded - 3;

				i = find(conn->in.data + start, conn->in.used - start, "\r\n\r\n");

				if(i == UINT32_MAX)
					// No \r\n\r\n found. The head of the request wasn't fully received yet.
					return;
					
				// i is relative to start.
				i += start;
			}

			struct parse_err_t err = parse(conn->in.data, i+4, &conn->request.public);

			uint32_t len = 0; // Anything other than UINT32_MAX goes.
			if(err.msg == NULL)
				len = determine_content_length(&conn->request.public); // Returns UINT32_MAX on failure.

			if(err.msg != NULL || len == UINT32_MAX)
			{
				char buffer[512];
				if(len == UINT32_MAX)
				{
					static const char msg[] = "Couldn't determine the content length";
					(void) snprintf(buffer, sizeof(buffer), 
						"HTTP/1.1 400 Bad Request\r\n"
						"Content-Type: text/plain;charset=utf-8\r\n"
						"Content-Length: %ld\r\n"
						"Connection: Close\r\n"
						"\r\n%s", sizeof(msg)-1, msg);
				}
				else if(err.internal)
				{
					(void) snprintf(buffer, sizeof(buffer), 
						"HTTP/1.1 500 Internal Server Error\r\n"
						"Content-Type: text/plain;charset=utf-8\r\n"
						"Content-Length: %d\r\n"
						"Connection: Close\r\n"
						"\r\n%s", err.len, err.msg);
				}
				else
				{
					// 400 Bad Request.
					(void) snprintf(buffer, sizeof(buffer), 
						"HTTP/1.1 400 Bad Request\r\n"
						"Content-Type: text/plain;charset=utf-8\r\n"
						"Content-Length: %d\r\n"
						"Connection: Close\r\n"
						"\r\n%s", err.len, err.msg);
				}

				// NOTE: If the static buffer [buffer] is too small
				//       to hold the response then the response will
				//       be sent truncated. But that's not a problem
				//       since we'll close the connection after this
				//       response either way. 

				append(conn, buffer, -1);

				conn->close_when_uploaded = 1;

				if(!upload(conn))
				{
					// The socket wasn't found blocking
					// so the upload started, but then
					// it failed.
					close_connection(conn, freelist, connum);
					return;
				}

				if(conn->out.used == 0)
					close_connection(conn, freelist, connum);
				return;
			}

			conn->head_received = 1;
			conn->body_offset = i + 4;
			conn->body_length = len;
		}

		if(!conn->head_received || conn->body_offset + conn->body_length > conn->in.used)
			// The rest of the body didn't arrive yet.
			return;

		if(!respond(conn, freelist, connum, callback))
			return;

		// Remove the request from the input buffer by
		// copying back its remaining contents.
		uint32_t consumed = conn->body_offset + conn->body_length;
		memmove(conn->in.data, conn->in.data + consumed, conn->in.used - consumed);
		conn->in.used -= consumed;
		conn->head_received = 0;

		served += 1;
	}
}

void xhttp(void (*callback)(xh_request*, xh_response*), unsigned short port, unsigned int maxconns, _Bool reuse)
{
	int fd;
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);

		if(fd < 0)
		{
			// ERROR!
			fprintf(stderr, "Failed to create socket\n");
			return;
		}

		if(reuse)
		{
			int v = 1;
			if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)))
			{
				// ERROR: Failed to set socket option.
				fprintf(stderr, "Failed to set socket option\n");
				(void) close(fd);
				return;
			}
		}

		struct sockaddr_in temp;
		memset(&temp, 0, sizeof(temp));
		temp.sin_family = AF_INET;
		temp.sin_port = htons(port);
		temp.sin_addr.s_addr = INADDR_ANY;
		if(bind(fd, (struct sockaddr*) &temp, sizeof(temp)))
		{
			// ERROR!
			fprintf(stderr, "Failed to bind to address\n");
			(void) close(fd);
			return;
		}

		if(listen(fd, 32))
		{
			// ERROR!
			fprintf(stderr, "Failed to listen for connections\n");
			(void) close(fd);
			return;
		}
	}

	int epfd;
	{
		epfd = epoll_create1(0);

		if(epfd < 0)
		{
			// ERROR!
			fprintf(stderr, "Failed to create epoll\n");
			(void) close(fd);
			return;
		}

		struct epoll_event temp;
		temp.events = EPOLLIN;
		temp.data.ptr = NULL;
		if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &temp))
		{
			// ERROR!
			fprintf(stderr, "Failed to add listener to epoll\n");
			(void) close(fd);
			(void) close(epfd);
			return;
		}
	}

	conn_t *pool, *freelist;
	{
		pool = malloc(maxconns * sizeof(conn_t));

		if(pool == NULL)
		{
			fprintf(stderr, "Failed to allocate connection pool\n");
			(void) close(fd);
			(void) close(epfd);
			return;
		}

		pool[0].prev = NULL;

		for(unsigned int i = 0; i < maxconns; i += 1)
		{
			pool[i].next = pool + i + 1;
			pool[i].prev = NULL;
		}

		pool[maxconns-1].next = NULL;

		freelist = pool;
	}

	int connum = 0;

	struct epoll_event events[64];

	while(1)
	{
		int num = epoll_wait(epfd, events, sizeof(events)/sizeof(events[0]), 5000);

		for(int i = 0; i < num; i += 1)
		{
			if(events[i].data.ptr == NULL)
			{
				// New connection.
				assert(events[i].events == EPOLLIN);
				accept_connection(fd, epfd, &freelist, &connum);
				continue;
			}

			conn_t *conn = events[i].data.ptr;

			if(events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
			{
				// Error or disconnection.
				close_connection(conn, &freelist, &connum);
				continue;
			}

			if(events[i].events & EPOLLOUT)
			{
				if(!upload(conn))
				{
					close_connection(conn, &freelist, &connum);
					continue;
				}

				if(conn->out.used == 0 && conn->close_when_uploaded)
				{
					close_connection(conn, &freelist, &connum);
					continue;
				}
			}

			if((events[i].events & (EPOLLIN | EPOLLPRI)) && conn->close_when_uploaded == 0)
			{
				// Note that this may close the connection. If any logic
			    // were to come after this function, it couldn't refer
			    // to the connection structure.
				when_data_is_ready_to_be_read(conn, &freelist, &connum, callback); 
			}
		}
	}
}

/* Symbol: find_header
 *
 *   Finds the header from a header array.
 *
 * Arguments:
 *
 *   - headers: The header array.
 *
 *   - count: The length of the header array.
 *            It can't be negative.
 *
 *   - name: Zero-terminated string that contains
 *           the header's name. The comparison with
 *           each header's name is made using [xh_hcmp],
 *           so it's not case-sensitive.
 *
 * Returns: 
 *   The index in the array of the matched header, or
 *   -1 is no header was found.
 */
static int find_header(xh_header *headers, int count, const char *name)
{
	for(int i = 0; i < count; i += 1)
		if(xh_hcmp(name, headers[i].name))
			return i;
	return -1;
}

/* Symbol: xh_hadd
 *
 *   Add or replace a header into a response object.
 *
 * Arguments:
 *
 *   - res: The response object.
 *
 *   - name: Zero-terminated string that contains
 *           the header's name. The comparison with
 *           each header's name is made using [xh_hcmp],
 *           so it's not case-sensitive.
 *
 *   - valfmt: A printf-like format string that evaluates
 *             to the header's value.
 *
 * Returns: 
 *   Nothing. The header may or may not be added 
 *   (or replaced) to the request.
 *
 * Notes:
 *
 *   - The name "xh_hadd" stands for "XHttp 
 *     Header ADD".
 */
void xh_hadd(xh_response *res, const char *name, const char *valfmt, ...)
{
	xh_response2 *res2 = (xh_response2*) ((char*) res - offsetof(xh_response2, public));

	assert(&res2->public == res);

	if(res2->failed)
		return;

	int i = find_header(res2->headers, res2->headerc, name);

	unsigned int name_len, value_len;

	name_len = name == NULL ? 0 : strlen(name);

	char value[512];
	{
		va_list args;
		va_start(args, valfmt);
		int n = vsnprintf(value, sizeof(value), valfmt, args);
		va_end(args);

		if(n < 0)
		{
			// Bad format.
			res2->failed = 1;
			return;
		}

		if((unsigned int) n >= sizeof(value))
		{
			// Static buffer is too small.
			res2->failed = 1;
			return;
		}

		value_len = n;
	}

	// Duplicate name and value.
	char *name2, *value2;
	{
		void *mem = malloc(name_len + value_len + 2);

		if(mem == NULL)
		{
			// ERROR!
			res2->failed = 1;
			return;
		}

		name2  = (char*) mem;
		value2 = (char*) mem + name_len + 1;
	
		strcpy(name2, name);
		strcpy(value2, value);
	}

	if(i < 0)
	{
		if(res2->headerc == res2->capacity)
			{
				int new_capacity = res2->capacity == 0 ? 8 : res2->capacity * 2;

				void *tmp = realloc(res2->headers, new_capacity * sizeof(xh_header));

				if(tmp == NULL)
				{
					// ERROR!
					res2->failed = 1;
					free(name2);
					return;
				}

				res2->public.headers = tmp;
				res2->headers = tmp;
				res2->capacity = new_capacity;
			}

		res2->headers[res2->headerc] = (xh_header) { 
			.name = name2, .value = value2, 
			.name_len = name_len, .value_len = value_len };

		res2->headerc += 1;
		res2->public.headerc = res2->headerc;
	}
	else
	{
		free(res2->headers[i].name);
		res2->headers[i] = (xh_header) { 
			.name = name2, .value = value2, 
			.name_len = name_len, .value_len = value_len };
	}
}

/* Symbol: xh_hrem
 *
 *   Remove a header from a response object.
 *
 * Arguments:
 *
 *   - res: The response object that contains the
 *          header to be removed.
 *
 *   - name: Zero-terminated string that contains
 *           the header's name. The comparison with
 *           each header's name is made using [xh_hcmp],
 *           so it's not case-sensitive.
 *
 * Returns: 
 *   Nothing.
 *
 * Notes:
 *
 *   - The name "xh_hrem" stands for "XHttp 
 *     Header REMove".
 */
void xh_hrem(xh_response *res, const char *name)
{
	xh_response2 *res2 = (xh_response2*) ((char*) res - offsetof(xh_response2, public));

	assert(&res2->public == res);

	if(res2->failed)
		return;

	int i = find_header(res2->headers, res2->headerc, name);

	if(i < 0)
		return;

	free(res2->headers[i].name);

	assert(i >= 0);

	for(; (unsigned int) i < res2->headerc-1; i += 1)
		res2->headers[i] = res2->headers[i+1];

	res2->headerc -= 1;
	res2->public.headerc -= 1;
}

/* Symbol: xh_hget
 *
 *   Find the contents of a header given it's
 *   name from a response or request object.
 *
 * Arguments:
 *
 *   - req_or_res: The request or response object
 *                 that contains the header. This
 *                 argument must originally be of
 *                 type [xh_request*] or [xh_response*].
 *
 *   - name: Zero-terminated string that contains
 *           the header's name. The comparison with
 *           each header's name is made using [xh_hcmp],
 *           so it's not case-sensitive.
 *
 * Returns: 
 *   A zero-terminated string containing the value of
 *   the header or NULL if the header isn't contained
 *   in the request/response.
 *
 * Notes:
 *
 *   - The name "xh_hget" stands for "XHttp 
 *     Header GET".
 *
 *   - The returned value is invalidated if
 *     the header is removed using [xh_hrem].
 */
const char *xh_hget(void *req_or_res, const char *name)
{
	xh_header   *headers;
	unsigned int headerc;

	{
		_Static_assert(offsetof(xh_response2, public) == offsetof(xh_request2, public));

		struct_type_t type = ((xh_request2*) ((char*) req_or_res - offsetof(xh_request2, public)))->type;

		if(type == XH_REQ)
		{
			headers = ((xh_request*) req_or_res)->headers;
			headerc = ((xh_request*) req_or_res)->headerc;
		}
		else
		{
			assert(type == XH_RES);
			headers = ((xh_response*) req_or_res)->headers;
			headerc = ((xh_response*) req_or_res)->headerc;
		}
	}

	int i = find_header(headers, headerc, name);

	if(i < 0)
		return NULL;

	return headers[i].value;
}

/* Symbol: xh_hcmp
 *
 *   This function compares header names.
 *   The comparison isn't case-sensitive.
 *
 * Arguments:
 *
 *   - a: Zero-terminated string that contains
 *        the first header's name.
 *
 *   - b: Zero-terminated string that contains
 *        the second header's name.
 *
 * Returns: 
 *   1 if the header names match, 0 otherwise.
 *
 * Notes:
 *   - The name "xh_hcmp" stands for "XHttp 
 *     Header CoMPare"
 */
_Bool xh_hcmp(const char *a, const char *b)
{
	if(a == NULL || b == NULL)
		return a == b;

	while(*a != '\0' && *b != '\0' && tolower(*a) == tolower(*b))
		a += 1, b += 1;

	return tolower(*a) == tolower(*b);
}