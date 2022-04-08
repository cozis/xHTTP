
// build with:
//   $ gcc example.c xhttp.c -o example -Wall -Wextra -g

#include <stdio.h>
#include "xhttp.h"

static void callback(xh_request *req, xh_response *res)
{
	(void) req;
	res->status_code = 200;
	res->status_text = "OK";
	xh_hadd(res, "Content-Type", "text/plain;charset=utf-8");
	xh_hadd(res, "Content-Language", "en-US");
	res->body = "Hello, world!";
	res->body_len = sizeof("Hello, world!")-1;
}

int main()
{
	xhttp(callback, 8080, 512, 1);
	return 0; /* Unreachable */
}
