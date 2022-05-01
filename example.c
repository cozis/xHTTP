
// Build with:
//   $ gcc example.c xhttp.c -o example
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "xhttp.h"

static xh_handle handle;

static void callback(xh_request *req, xh_response *res, void *userp)
{
	(void) req;
	(void) userp;
	res->status = 200;
	if(!strcmp(req->URL, "/file"))
		res->file = "example.c";
	else
		res->body.str = "Hello, world!";
	xh_header_add(res, "Content-Type", "text/plain");
}

static void handle_sigterm(int signum) 
{
	(void) signum;
	xh_quit(handle);
}

int main()
{
	signal(SIGTERM, handle_sigterm);
	signal(SIGQUIT, handle_sigterm);
	signal(SIGINT,  handle_sigterm);
	
	const char *error = xhttp(NULL, 8080, callback, 
		                      NULL, &handle, NULL);
	if(error != NULL)
	{
		fprintf(stderr, "ERROR: %s\n", error);
		return 1;
	}
	fprintf(stderr, "OK\n");
	return 0;
}
