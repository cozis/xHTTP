# xHTTP
xHTTP is an HTTP server library designed to be lightweight, fast and easy to use. 

**NOTE**: Since it's my first time building something of this kind, it's still in the early sperimentation phase.

## Features
xHTTP's more relevant features are:
- Speed
- HTTP/1.1
- No global state
- Single-threaded
- Based on Linux's epoll
- No dependencies (other than Linux and the standard library)

while some notably missing features are:
- Only works on Linux
- Doesn't support `Transfer-Encoding: Chunked`

## Installation and Usage
The way you install it is by just copying `xhttp.c` and `xhttp.h` in your source tree and compiling it like it was one of your C files (include the `xhttp.h` and give the `xhttp.c` to `gcc`).

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

## Contributing
Feel free to propose any changes! Though I'd advise to open an issue before sending any non-trivial changes.