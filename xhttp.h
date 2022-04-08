#ifndef XHTTP_H
#define XHTTP_H

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
	int          status_code;
	const char  *status_text;

	xh_header   *headers;
	unsigned int headerc;

	const char  *body;
	unsigned int body_len;

	_Bool close;
} xh_response;

void 		xhttp(void (*callback)(xh_request*, xh_response*), unsigned short port, unsigned int maxconns, _Bool reuse);
void        xh_hadd(xh_response *res, const char *name, const char *valfmt, ...);
void        xh_hrem(xh_response *res, const char *name);
const char *xh_hget(void *req_or_res, const char *name);
_Bool 		xh_hcmp(const char *a, const char *b);

#endif // #ifndef XHTTP_H