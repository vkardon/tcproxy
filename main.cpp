//
// TCP Proxy
//
#include <stdio.h>
#include "tcproxy.h"

void SetStdOut()
{
    //
    // Standard output is line buffered if it can be detected to refer to an interactive
    // device, otherwise it's fully buffered. So there are situations where printf won't
    // flush, even if it gets a newline to send out, such as:
    //
    // myprog > myfile.txt
    //
    // This makes sense for efficiency since, if you're interacting with a user, they
    // probably want to see every line. If you're sending the output to a file, it's most
    // likely that there's not a user at the other end (though not impossible, they could
    // be tailing the file).
    //
    // As to how to deal with that:
    // a) call fflush(stdout) after every output call that you want to see immediately, or
    // b) use setvbuf before operating on stdout, to set it to line buffered or unbuffered.
    //
    // That may affect performance quite a bit if you are sending the output to a file.
    // Also support for this is implementation-defined, not guaranteed by the standard.
    //
    // ISO C99 section 7.19.3/3 is the relevant bit:
    //
    //     When a stream is unbuffered, characters are intended to appear from the source
    //     or at the destination as soon as possible. Otherwise characters may be accumulated
    //     and transmitted to or from the host environment as a block.
    //
    //     When a stream is fully buffered, characters are intended to be transmitted to or
    //     from the host environment as a block when a buffer is filled.
    //
    //     When a stream is line buffered, characters are intended to be transmitted to or
    //     from the host environment as a block when a new-line character is encountered.
    //
    //     Furthermore, characters are intended to be transmitted as a block to the host
    //     environment when a buffer is filled, when input is requested on an unbuffered
    //     stream, or when input is requested on a line buffered stream that requires the
    //     transmission of characters from the host environment.
    //
    //     Support for these characteristics is implementation-defined, and may be affected via
    //     the setbuf and setvbuf functions.
    //
    
    // Set stdout and stoerr to "unbuffered" by disabled buffering for the stream
    //setbuf(stdout, NULL);
    //setbuf(stderr, NULL);
    
    // Set stdout and stderr to "line buffered": On output, data is written when
    // a newline character is inserted into the stream or when the buffer is full
    // (or flushed), whatever happens first.
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);
}

int main(int argc, char** argv)
{
    // Make sure we have a config file
    if(argc < 2)
    {
        printf("%s: No configuration file specified.\n", __func__);
        return 1;
    }
    
    // Set stdout and stoerr to "line buffered": On output, data is written when
    // a newline character is inserted into the stream or when the buffer is full
    // (or flushed), whatever happens first.
    SetStdOut();
    
    // Start listening
    CTcpProxy proxy(argv[0] /*path*/, argv[1] /*config file*/);
    if(!proxy.Listen())
        return 1;
    
    printf("%s: Done\n", __func__);
    return 0;
}

