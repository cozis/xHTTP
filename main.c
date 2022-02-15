// build with:
//   $ gcc main.c xserver.c -o app -Wall -Wextra -g

#define XSERVER_IMPL

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
	const unsigned short   port = 8080;
	const unsigned int maxconns = 512;
	fprintf(stderr, "port = %d\n", port);
	fprintf(stderr, "maxconns = %d\n", maxconns);
	xserver(callback, port, maxconns, 1);
	/* Unreachable */
	return 0;
}