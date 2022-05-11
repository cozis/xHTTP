
// Build with:
//   $ gcc example2.c xhttp.c -o example2

#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "xhttp.h"

static xh_handle handle;
static char buffer[1024];

static void callback(xh_request *req, xh_response *res, void *userp)
{
    (void) req;
    (void) userp;

    int post;
    char username[32];

    if(!xh_urlcmp(req->URL.str, "/users/:s", 
                  sizeof(username), username)) {

        snprintf(buffer, sizeof(buffer), "Hello, %s!\n", username);
        res->status = 200;
        res->body.str = buffer;

    } else if(!xh_urlcmp(req->URL.str, "/users/:s/posts/:d", 
              sizeof(username), username, &post)) {

        snprintf(buffer, sizeof(buffer), "Hello, %s! You asked for post no. %d!\n", username, post);
        res->status = 200;
        res->body.str = buffer;

    } else {
        res->status = 404;
        res->body.str = "It seems like what you're looking for isn't here! :S";
    }
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