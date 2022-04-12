# xHTTP
This is the implementation in a single C file of an embeddable HTTP server.

I'm building this to have an easy way to create HTTP services for my personal projects. The main idea is that for most basic purposes big web servers aren't necessary and a single C file of well-tested routines can do the job.

**NOTE**: Since it's my first time building something of this kind, it's still in an early sperimentation phase.

## Installation and Usage
The way you install it is by just copying `xhttp.c` and `xhttp.h` in your source tree and compiling it like it was one of your C files (include the `xhttp.h` and give the `xhttp.c` to `gcc`).

To start a server instance, you need to call `xhttp` by providing it with a callback that generates the HTTP response that the library will forward to the user.

The callback's interface must be
```c
void callback(xh_request *req, xh_response *res);
```

The request information is provided through the `req` argument, while `res` is an output argument. The callback will respond to the request by setting the fields of `res`. These two arguments are never `NULL`.

## Contributing
Feel free to propose any changes! Though I'd advise to open an issue before sending any non-trivial changes.