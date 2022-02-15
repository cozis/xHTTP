#ifndef XSERVER_H
#define XSERVER_H

typedef struct {
	char *name, *value;
	unsigned int name_len;
	unsigned int value_len;
} xs_header;

typedef struct {
	const char  *method;
	unsigned int method_len;

	const char  *URL;
	unsigned int URL_len;

	unsigned int version_minor;
	unsigned int version_major;

	xs_header   *headers;
	unsigned int headerc;

	const char  *body;
	unsigned int body_len;
} xs_request;

typedef struct {
	int          status_code;
	const char  *status_text;

	xs_header   *headers;
	unsigned int headerc;

	const char  *body;
	unsigned int body_len;

	_Bool close;
} xs_response;

void 		xserver(void (*callback)(xs_request*, xs_response*), unsigned short port, unsigned int maxconns, _Bool reuse);
void        xs_hadd(xs_response *res, const char *name, const char *valfmt, ...);
void        xs_hrem(xs_response *res, const char *name);
const char *xs_hget(void *req_or_res, const char *name);
_Bool 		xs_hcmp(const char *a, const char *b);

#endif // #ifndef XSERVER_H