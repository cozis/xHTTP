// build with:
//   $ gcc example.c xserver.c -o example -Wall -Wextra -g

#include <stdio.h>
#include "xserver.h"

static void callback(xs_request *req, xs_response *res)
{
	(void) req;

	static const char body[] = "Hello, world!";

	res->status_code = 200;
	res->status_text = "OK";
	res->body = body;
	res->body_len = sizeof(body)-1;

	xs_hadd(res, "Content-Type", "text/plain;charset=utf-8");
}

int main()
{
	xserver(callback, 8080, 512, 1);
	return 0; /* Unreachable */
}