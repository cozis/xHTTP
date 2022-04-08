
// build with:
//   $ gcc example.c xserver.c -o example -Wall -Wextra -g

#include <stdio.h>
#include "xserver.h"

static void callback(xs_request *req, xs_response *res)
{
	(void) req;
	res->status_code = 200;
	res->status_text = "OK";
	xs_hadd(res, "Content-Type", "text/plain;charset=utf-8");
	xs_hadd(res, "Content-Language", "en-US");
	res->body = "Hello, world!";
	res->body_len = sizeof("Hello, world!")-1;
}

int main()
{
	xserver(callback, 8080, 512, 1);
	return 0; /* Unreachable */
}
