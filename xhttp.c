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
#include <netinet/in.h>
#include "xhttp.h"

/* +--------------------------------------------------+
 * |                     OVERVIEW                     |
 * | The server starts inside the function [xhttp],   |
 * | where the server waits in a loop for events      |
 * | provided by epoll (the event loop).              |
 * |                                                  |
 * | Each connection to a client is represented by a  |
 * | [conn_t] structure, which is basically composed  |
 * | by a buffer of input data, a buffer of output    |
 * | data, the parsing state of the input buffer plus |
 * | some more fields required to hold the state of   |
 * | the parsing and to manage the connection. These  |
 * | structures are preallocated at start-up time and |
 * | determine the capacity of the server.            |
 * |                                                  |
 * | Whenever a client requests to connect, the server|
 * | decides if it can handle it or not. If it can,   |
 * | then it gives it a [conn_t] structure and        |
 * | registers it into the event loop.                |
 * |                                                  |
 * | When the event loop signals that a connection sent|
 * | some data, the data is copied from the kernel    |
 * | into the user-space buffer inside the [conn_t]   |
 * | structure. The retrieved data has a different    |
 * | meaning based on the parsing state of the        |
 * | connection. If the head of the request wasn't    |
 * | received or was received partially, then         |
 * | the character sequence "\r\n\r\n" (a blank line) |
 * | is searched for inside the downloaded data.      |
 * | The "\r\n\r\n" token signifies the end of the    |
 * | request's head and the start of it's body,       |
 * | therefore if it isn't found, the head wasn't     |
 * | fully received yet. If the head wasn't received  |
 * | the server goes back to waiting for new events.  |
 * | If the token is found, then the head can be      |
 * | parsed. Once the head is parsed, the length of   |
 * | the request's body is determined. If the whole   |
 * | body of the request was received with the head,  |
 * | then it can already be handled. If the body      |
 * | wasn't received, then the servers goes back to   |
 * | waiting for events until the rest of the body    |
 * | is received.                                     |
 * | When the body is fully received, then the user-  |
 * | provided callback can be called to generate a    |
 * | response.                                        |
 * | One thing to note is that multiple requests could|
 * | be read from a single [recv], making it necessary|
 * | to perform these operations on the input buffer  |
 * | in a loop.                                       |
 * |                                                  |
 * | If at any point the request is determined to be  |
 * | invalid or an internal error occurres, then this |
 * | process is aborted and a 4xx or 5xx response is  |
 * | sent.                                            |
 * |                                                  |
 * | While handling input events, the response isn't  |
 * | sent directly to the kernel buffer, because the  |
 * | call to [send] could block the server. Instead,  |
 * | the response is written to the [conn_t]'s output |
 * | buffer. This buffer is only flushed to the kernel|
 * | when a write-ready event is triggered for that   |
 * | connection.                                      |
 * +--------------------------------------------------+
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

typedef struct {
	_Bool exiting;
	int fd, epfd, maxconns, connum;
	conn_t *pool, *freelist;
} context_t;

static const char *statis_code_to_status_text(int code)
{
	switch(code)
	{
		case 100: return "Continue";
		case 101: return "Switching Protocols";
		case 102: return "Processing";

		case 200: return "OK";
		case 201: return "Created";
		case 202: return "Accepted";
		case 203: return "Non-Authoritative Information";
		case 204: return "No Content";
		case 205: return "Reset Content";
		case 206: return "Partial Content";
		case 207: return "Multi-Status";
		case 208: return "Already Reported";

		case 300: return "Multiple Choices";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 303: return "See Other";
		case 304: return "Not Modified";
		case 305: return "Use Proxy";
		case 306: return "Switch Proxy";
		case 307: return "Temporary Redirect";
		case 308: return "Permanent Redirect";

		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 402: return "Payment Required";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 406: return "Not Acceptable";
		case 407: return "Proxy Authentication Required";
		case 408: return "Request Timeout";
		case 409: return "Conflict";
		case 410: return "Gone";
		case 411: return "Length Required";
		case 412: return "Precondition Failed";
		case 413: return "Request Entity Too Large";
		case 414: return "Request-URI Too Long";
		case 415: return "Unsupported Media Type";
		case 416: return "Requested Range Not Satisfiable";
		case 417: return "Expectation Failed";
		case 418: return "I'm a teapot";
		case 420: return "Enhance your calm";
		case 422: return "Unprocessable Entity";
		case 426: return "Upgrade Required";
		case 429: return "Too many requests";
		case 431: return "Request Header Fields Too Large";
		case 449: return "Retry With";
		case 451: return "Unavailable For Legal Reasons";

		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 502: return "Bad Gateway";
		case 503: return "Service Unavailable";
		case 504: return "Gateway Timeout";
		case 505: return "HTTP Version Not Supported";
		case 509: return "Bandwidth Limit Exceeded";
	}
	return "???";
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
		_Static_assert(offsetof(xh_response2, public) == offsetof(xh_request2, public), 
					   "The public portion of xh_response2 and xh_request2 must be aligned the same way");

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

static _Bool set_non_blocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if(flags < 0)
		return 0;

	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void accept_connection(context_t *ctx)
{
	int cfd = accept(ctx->fd, NULL, NULL);

	if(cfd < 0)
		return; // Failed to accept.

	if(!set_non_blocking(cfd))
	{
		(void) close(cfd);
		return;
	}

	if(ctx->freelist == NULL)
	{
		// Connection limit reached.
		(void) close(cfd);
		return;
	}

	conn_t *conn = ctx->freelist;
	ctx->freelist = conn->next;

	memset(conn, 0, sizeof(conn_t));
	conn->fd = cfd;
	conn->request.type = XH_REQ;

	struct epoll_event buffer;
	buffer.events = EPOLLET  | EPOLLIN
	              | EPOLLPRI | EPOLLOUT
	              | EPOLLRDHUP;
	buffer.data.ptr = conn;
	if(epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, cfd, &buffer))
	{
		(void) close(cfd);

		conn->fd = -1;
		conn->next = ctx->freelist;
		ctx->freelist = conn;
		return;
	}

	ctx->connum += 1;
}

static void close_connection(context_t *ctx, conn_t *conn)
{
	(void) close(conn->fd);

	if(conn->in.data != NULL)
	{
		free(conn->in.data);
		conn->in.data = NULL;
	}

	if(conn->out.data != NULL)
	{
		free(conn->out.data);
		conn->out.data = NULL;
	}

	if(conn->request.public.headers != NULL)
		free(conn->request.public.headers);

	conn->fd = -1;

	conn->next = ctx->freelist;
	ctx->freelist = conn;

	ctx->connum -= 1;
}

#if DEBUG
static void close_connection_(context_t *ctx, conn_t *conn, const char *file, int line)
{
	fprintf(stderr, "Closing connection at %s:%d.\n", file, line);
	close_connection(ctx, conn);
}
#define close_connection(ctx, conn) close_connection_(ctx, conn, __FILE__, __LINE__)
#endif

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

	if(total == 0)
		return 1;

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

static void generate_response_by_calling_the_callback(context_t *ctx, conn_t *conn, void (*callback)(xh_request*, xh_response*))
{
	xh_request *req = &conn->request.public;

	_Bool keep_alive;
	{
		const char *h_connection = xh_hget(req, "Connection");

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

	if(keep_alive)
	{
		if(conn->served >= 20)
			keep_alive = 0;

		if(ctx->connum > 0.6 * ctx->maxconns)
			keep_alive = 0;
	}

	_Bool head_only = req->method_id == XH_HEAD;

	if(head_only)
	{
		req->method_id = XH_GET;
		req->method = "GET";
		req->method_len = sizeof("GET")-1;
	}

	xh_response2 res2;
	xh_response *res = &res2.public;
	{
		memset(&res2, 0, sizeof(xh_response2));
		res2.type = XH_RES;
	}

	callback(req, res);

	if(req->headers != NULL)
	{
		free(req->headers);
		req->headers = NULL;
	}

	if(res->close)
		keep_alive = 0;

	xh_hadd(res, "Content-Length", "%d", res->body_len);
	xh_hadd(res, "Connection", keep_alive ? "Keep-Alive" : "Close");

	if(res2.failed)
	{
		// Failed to build the response. We'll send a 500.
		append(conn, "HTTP/1.1 500 Internal Server Error\r\n", -1);
		append(conn, keep_alive ? "Connection: Keep-Alive\r\n" : "Connection: Close\r\n", -1);
	}
	else
	{
		char buffer[256];

		const char *status_text = statis_code_to_status_text(res->status);
		assert(status_text != NULL);

		int n = snprintf(buffer, sizeof(buffer), 
						"HTTP/1.1 %d %s\r\n", 
						res->status, status_text);
		assert(n >= 0);

		if((unsigned int) n > sizeof(buffer)-1)
			n = sizeof(buffer)-1;

		append(conn, buffer, n);

		for(unsigned int i = 0; i < res2.headerc; i += 1)
		{
			append(conn, res2.headers[i].name, res2.headers[i].name_len);
			append(conn, ": ", 2);
			append(conn, res2.headers[i].value, res2.headers[i].value_len);
			append(conn, "\r\n", 2);
		}

		append(conn, "\r\n", 2);

		if(head_only == 0 && res->body != NULL && res->body_len > 0)
			append(conn, res->body, res->body_len);
	}

	{
		for(unsigned int i = 0; i < res2.headerc; i += 1)
		free(res2.headers[i].name);

		if(res2.headers != NULL)
			free(res2.headers);
	}

	conn->served += 1;

	if(!keep_alive)
		conn->close_when_uploaded = 1;
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

static void when_data_is_ready_to_be_read(context_t *ctx, conn_t *conn, void (*callback)(xh_request*, xh_response*))
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
					close_connection(ctx, conn);
					return;
				}

				// TODO: Change the pointers in conn->request
				//       if the head was already parsed.

				b->data = temp;
				b->size = new_size;
			}

			assert(b->size > b->used);

			int n = recv(conn->fd, b->data + b->used, b->size - b->used, 0);

			if(n <= 0)
			{
				if(n == 0)
				{
					// Peer disconnected.
					close_connection(ctx, conn);
					return;
				}

				if(errno == EAGAIN || errno == EWOULDBLOCK)
					break; // Done downloading.

				// ERROR!
#if DEBUG
				perror("recv");
#endif
				close_connection(ctx, conn);
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
				return;
			}

			conn->head_received = 1;
			conn->body_offset = i + 4;
			conn->body_length = len;
		}

		if(conn->head_received && conn->body_offset + conn->body_length <= conn->in.used)
		{
			// The rest of the body arrived.
			xh_request *req = &conn->request.public;

			req->body = conn->in.data + conn->body_offset;
			req->body_len = conn->body_length;

			generate_response_by_calling_the_callback(ctx, conn, callback);

			// Remove the request from the input buffer by
			// copying back its remaining contents.
			uint32_t consumed = conn->body_offset + conn->body_length;
			memmove(conn->in.data, conn->in.data + consumed, conn->in.used - consumed);
			conn->in.used -= consumed;
			conn->head_received = 0;

			served += 1;

			if(conn->close_when_uploaded)
				break;
		}
	}
}

void xh_quit(xh_handle handle)
{
	context_t *ctx = handle;
	ctx->exiting = 1;
}

static const char *init(context_t *context, const char *addr, 
	                    unsigned short port, const xh_config *config)
{
	if(config->maximum_parallel_connections == 0)
		return "The number of maximum parallel connections isn't allowed to be 0";

	if(config->backlog == 0)
		return "The backlog isn't allowed to be 0";

	{
		context->fd = socket(AF_INET, SOCK_STREAM, 0);

		if(context->fd < 0)
			return "Failed to create socket";

		if(config->reuse_address)
		{
			int v = 1;
			if(setsockopt(context->fd, SOL_SOCKET,
						  SO_REUSEADDR, &v, sizeof(v)))
			{
				(void) close(context->fd);
				return "Failed to set socket option";
			}
		}

		struct in_addr inp;
		if(addr == NULL)
			inp.s_addr = INADDR_ANY;
		else
			if(!inet_aton(addr, &inp))
			{
				(void) close(context->fd);
				return "Malformed IPv4 address";
			}

		struct sockaddr_in temp;

		memset(&temp, 0, sizeof(temp));

		temp.sin_family = AF_INET;
		temp.sin_port = htons(port);
		temp.sin_addr = inp;

		if(bind(context->fd, (struct sockaddr*) &temp, sizeof(temp)))
		{
			(void) close(context->fd);
			return "Failed to bind to address";
		}

		if(listen(context->fd, config->backlog))
		{
			(void) close(context->fd);
			return "Failed to listen for connections";
		}
	}

	{
		context->epfd = epoll_create1(0);

		if(context->epfd < 0)
		{
			(void) close(context->fd);
			return "Failed to create epoll";
		}

		struct epoll_event temp;

		temp.events = EPOLLIN;
		temp.data.ptr = NULL;

		if(epoll_ctl(context->epfd, EPOLL_CTL_ADD, context->fd, &temp))
		{
			(void) close(context->fd);
			(void) close(context->epfd);
			return "Failed to add listener to epoll";
		}
	}

	{
		context->pool = malloc(config->maximum_parallel_connections * sizeof(conn_t));

		if(context->pool == NULL)
		{
			(void) close(context->fd);
			(void) close(context->epfd);
			return "Failed to allocate connection pool";
		}

		context->pool[0].prev = NULL;

		for(unsigned int i = 0; i < config->maximum_parallel_connections; i += 1)
		{
			context->pool[i].fd = -1;
			context->pool[i].next = context->pool + i + 1;
			context->pool[i].prev = NULL;
		}

		context->pool[config->maximum_parallel_connections-1].next = NULL;

		context->freelist = context->pool;
	}

	context->connum = 0;
	context->maxconns = config->maximum_parallel_connections;
	context->exiting = 0;
	return NULL;
}

xh_config xh_get_default_configs()
{
	return (xh_config) {
		.reuse_address = 1,
		.maximum_parallel_connections = 512,
		.backlog = 128,
	};
}

const char *xhttp(const char *addr, unsigned short port, 
				  xh_callback callback, xh_handle *handle, 
				  const xh_config *config)
{
	xh_config dummy = xh_get_default_configs();
	if(config == NULL)
		config = &dummy;

	context_t context;

	const char *error = init(&context, addr, port, config);

	if(error != NULL)
		return error;

	if(handle)
		*handle = &context;

	struct epoll_event events[64];

	while(!context.exiting)
	{
		int num = epoll_wait(context.epfd, events, sizeof(events)/sizeof(events[0]), 5000);

		for(int i = 0; i < num; i += 1)
		{
			if(events[i].data.ptr == NULL)
			{
				// New connection.
				accept_connection(&context);
				continue;
			}

			conn_t *conn = events[i].data.ptr;

			if(events[i].events & EPOLLRDHUP)
			{
				// Disconnection.
				close_connection(&context, conn);
				continue;
			}

			if(events[i].events & (EPOLLERR | EPOLLHUP))
			{
				// Connection closed or an error occurred.
				// We continue as nothing happened so that
				// the error is reported on the [recv] or
				// [send] call site.
				events[i].events = EPOLLIN | EPOLLOUT;
			}

			int old_connum = context.connum;

			if((events[i].events & (EPOLLIN | EPOLLPRI)) && conn->close_when_uploaded == 0)
			{
				// Note that this may close the connection. If any logic
			    // were to come after this function, it couldn't refer
			    // to the connection structure.
				when_data_is_ready_to_be_read(&context, conn, callback);
			}

			if(old_connum == context.connum)
			{
				// The connection wasn't closed. Try to
				// upload the data in the output buffer.

				if(!upload(conn))

					close_connection(&context, conn);

				else
					if(conn->out.used == 0 && conn->close_when_uploaded)
						close_connection(&context, conn);
			}
		}
	}

	for(unsigned int i = 0; i < config->maximum_parallel_connections; i += 1)
		if(context.pool[i].fd != -1)
			close_connection(&context, context.pool + i);

	free(context.pool);
	(void) close(context.fd);
	(void) close(context.epfd);
	return NULL;
}
