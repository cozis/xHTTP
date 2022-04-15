# xHTTP
xHTTP is an HTTP server library designed to be lightweight, fast and easy to use. 

**NOTE**: It's still in the early sperimentation phase (I'm still figuring things out!)

## Features
xHTTP's more relevant features are:
- It's fast
- HTTP/1.1
- Supports `Connection: Keep-Alive`
- No global state
- Single-threaded
- Based on Linux's epoll
- No dependencies (other than Linux and the standard library)

while some notably missing features are:
- Only works on Linux
- Doesn't support `Transfer-Encoding: Chunked`
- No IPv6

## Installation
The way you install it is by just copying `xhttp.c` and `xhttp.h` in your source tree and compiling it like it was one of your C files: include the `xhttp.h` where you want to use it and compile `xhttp.c` with your files.

## Usage
To start a server instance, you need to call the `xhttp` function
```c
const char *xhttp(const char *addr, unsigned short port, 
                  xh_callback callback, xh_handle *handle, 
                  const xh_config *config);
```
by providing it with a callback that generates the HTTP response that the library will forward to the user.

The callback's interface must be
```c
void callback(xh_request *req, xh_response *res);
```
The request information is provided through the `req` argument, while `res` is an output argument. The callback will respond to the request by setting the fields of `res`. These two arguments are never `NULL`.

Here's an example of a basic server which always responds with a "Hello, world!" message:

```c
#include <stdio.h>
#include "xhttp.h"

void callback(xh_request *req, xh_response *res)
{
    #define RESPONSE "Hello, world!"

    res->status = 200;
    res->body = RESPONSE;
    res->body_len = sizeof(RESPONSE)-1;
    xh_header_add(res, "Content-Type", "text/plain");
}

int main()
{
    const char    *addr = NULL;
    unsigned short port = 8080;
    const char *error = xhttp(addr, port, callback, &handle, NULL);

    if(error != NULL)
    {
        fprintf(stderr, "ERROR: %s\n", error);
        return 1;
    }
    return 0;
}

``` 
if this were your `main.c` file, you'd compile it with
```sh
$ gcc main.c xhttp.c -o main
```

You can find a slightly more complete example in `example.c`.

## Contributing
Feel free to propose any changes! Though I'd advise to open an issue before sending any non-trivial changes.