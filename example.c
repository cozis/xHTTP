
// build with:
//   $ gcc example.c xhttp.c -o example -Wall -Wextra -g

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "xhttp.h"

static void callback(xh_request *req, xh_response *res)
{
	(void) req;
	res->status = 200;
	res->body = "Hello, world!";
	res->body_len = sizeof("Hello, world!")-1;
	xh_hadd(res, "Content-Type", "text/plain");
}

static xh_handle handle;

static void handle_sigterm(int signum) {
  (void) signum;
  xh_quit(handle);
}

int main()
{
	signal(SIGTERM, handle_sigterm);
	signal(SIGQUIT, handle_sigterm);
	signal(SIGINT,  handle_sigterm);

	const char *error = xhttp(NULL, 8080, callback, &handle, NULL);
	if(error != NULL)
	{
		fprintf(stderr, "ERROR: %s\n", error);
		return 1;
	}
	fprintf(stderr, "OK\n");
	return 0;
}
