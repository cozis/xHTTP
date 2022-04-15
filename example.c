
// Build with:
//   $ gcc example.c xhttp.c -o example

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "xhttp.h"

static xh_handle handle;

static void callback(xh_request *req, xh_response *res)
{
	#define RESPONSE "Hello, world!"

	res->status = 200;

	res->body = RESPONSE;
	res->body_len = sizeof(RESPONSE)-1;
	
	xh_header_add(res, "Content-Type", "text/plain");
}

static void handle_sigterm(int signum) 
{
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
