//
//  tcpProxy.h
//
#ifndef __TCP_PROXY__
#define __TCP_PROXY__

#include <sys/socket.h>     // fd_set
#include <new>              // std::nothrow
#include <string.h>         // strcpy


#define FD_MAX              256     // Highest file descriptor number expected
#define MAX_ADDR_NAME       32      // Max IP length in numbers-and-dots notation
#define MAX_HOST_NAME       256     // Max hostname length
#define RW_BUFSIZE          512     // READ/WRITE buffer size

// Note: The number of TCP connections can be no higher than FD_MAX.
// File descriptors 0, 1, and 2 are already in use as stdin, stdout, and stderr.
// Moreover, libc can make use of a few file descriptors for functions like gethostbyname.
// As a result, the real number of TCP connection will be lower then FD_MAX

//
// TCP proxy class
//
class CTcpProxy 
{
    // Callback to make when a file descriptor is ready
    typedef void(CTcpProxy::*CALLBACK_FUNC)(int);
    
    struct Callback
    {
        CALLBACK_FUNC write_fn = nullptr;   // Function to call
        CALLBACK_FUNC read_fn = nullptr;    // Function to call
        
        int peer_fd = -1;
        unsigned char buf[RW_BUFSIZE]{};
        ssize_t len = 0;
        
        void Reset() { new (this) Callback; } // Re-constract Callback in place
    };
    
    struct Route
    {
        int source_fd = -1;
        char source_ip[MAX_ADDR_NAME+1]{};
        char target_ip[MAX_ADDR_NAME+1]{};
        unsigned short target_port = 0;
        Route* next = nullptr;
    };
    
public:
    CTcpProxy(const char* program_name, const char* configFile);
    virtual ~CTcpProxy();
    
    bool AddRoute(const char* source_host, const char* target_host, unsigned short target_port);
    bool Listen();
    
private:
    Callback cb[FD_MAX+1]{};    // Array of callbacks (for every fd)
    fd_set rfds;                // Set of fds to be checked for readability
    fd_set wfds;                // Set of fds to be checked for writability
    unsigned short port = 0;    // Port to listen
    Route* route = nullptr;     // List of routes
    bool keep_running = true;
    
    void CallbackAdd(int fd, int peer_fd, CALLBACK_FUNC read_fn, CALLBACK_FUNC write_fn);
    void CallbackRemove(int);
    void CallbackSelect();
    inline Callback* GetCallback(int fd);
    
    // Callback: Called by the event loop when ready to read/write/accept connected socket
    void OnRead(int fd);
    void OnWrite(int fd);
    void OnConnect(int fd);
    
    // Callback: Called by the event loop when ready to read command fifo
    void OnCommand(int fd);
    
    // Helpers
    bool ReadConfig(const char* configFile);
    bool MakeFifo(const char* fifo_base_name);
    bool MakeAsync(int fd);
    void ProcessCmd(const char* cmd);
    void CloseSock(int fd1, int fd2=-1, int fd3=-1);
    Route* GetRoute(const char* source_ip);
    Route* GetRoute(int source_fd);
    
    // Utils
    char* TrimString(char* str) const; // Trimming whitespace (both side)
    bool IsProcessRunning(const char* process_name);
};


#endif // __TCP_PROXY__

