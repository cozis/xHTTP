#ifndef XHTTP_H
#define XHTTP_H

typedef void *xh_handle;

typedef enum {
	XH_GET     = 1 << 0, 
	XH_HEAD    = 1 << 1,
	XH_POST    = 1 << 2,
	XH_PUT     = 1 << 3,
	XH_DELETE  = 1 << 4,
	XH_CONNECT = 1 << 5,
	XH_OPTIONS = 1 << 6,
	XH_TRACE   = 1 << 7,
	XH_PATCH   = 1 << 8,
} xh_method;

typedef struct {
	char *name, *value;
	unsigned int name_len;
	unsigned int value_len;
} xh_header;

typedef struct {
	xh_method    method_id;
	const char  *method;
	unsigned int method_len;

	const char  *URL;
	unsigned int URL_len;

	unsigned int version_minor;
	unsigned int version_major;

	xh_header   *headers;
	unsigned int headerc;

	const char  *body;
	unsigned int body_len;
} xh_request;

typedef struct {
	int status;

	xh_header   *headers;
	unsigned int headerc;

	struct {
		const char *str;
		long long   len;
	} body;

	const char *file;

	_Bool close;
} xh_response;

typedef struct {
	_Bool        reuse_address;
	unsigned int maximum_parallel_connections;
	unsigned int backlog;
} xh_config;

typedef void (*xh_callback)(xh_request*, xh_response*, void*);

const char *xhttp(const char *addr, unsigned short port, 
	              xh_callback callback, void *userp, 
	              xh_handle *handle, const xh_config *config);
void        xh_quit(xh_handle handle);
xh_config   xh_get_default_configs();

void        xh_header_add(xh_response *res, const char *name, const char *valfmt, ...);
void        xh_header_rem(xh_response *res, const char *name);
const char *xh_header_get(void *req_or_res, const char *name);
_Bool       xh_header_cmp(const char *a, const char *b);

#endif // #ifndef XHTTP_H