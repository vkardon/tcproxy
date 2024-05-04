//
//  tcproxy.h
//
#ifndef __TCP_PROXY__
#define __TCP_PROXY__

#include <sys/socket.h>     // fd_set
#include <new>              // std::nothrow
#include <arpa/inet.h>      // INET6_ADDRSTRLEN
#include <limits.h>         // NAME_MAX

#define RW_BUFSIZE  512     // The size of READ/WRITE buffer

// Note: The number of TCP connections can be no higher than FD_SETSIZE
// since select() can monitor only file descriptors numbers that are less
// than FD_SETSIZE (1024). File descriptors 0, 1, and 2 are already in use
// as stdin, stdout, and stderr. Moreover, libc can make use of a few file
// descriptors for functions like gethostbyname. As a result, the real 
// number of TCP connection will be lower then FD_SETSIZE.

//
// TCP proxy class
//
class CTcpProxy 
{
    // Callback to make when a file descriptor is ready
    typedef void(CTcpProxy::*CALLBACK_FUNC)(int);
    
    struct Callback
    {
        CALLBACK_FUNC write_fn{nullptr};   // Function to call
        CALLBACK_FUNC read_fn{nullptr};    // Function to call
        
        int peer_fd{-1};
        unsigned char buf[RW_BUFSIZE]{};
        ssize_t len{0};
        
        void Reset() { new (this) Callback; } // Re-constract Callback in place
    };
    
    struct Route
    {
        int source_fd{-1};
        int source_ip_family{0};
        char source_ip[INET6_ADDRSTRLEN]{};
        int target_ip_family{0};
        char target_ip[INET6_ADDRSTRLEN]{};
        unsigned short target_port{0};
        Route* next{nullptr};
    };
    
public:
    CTcpProxy(const char* program_name, const char* configFile);
    ~CTcpProxy();

    bool Start();

private:
    bool Listen();
    bool AddRoute(const char* route_conf);
    bool AddRoute(const char* source_host, const char* target_host, unsigned short target_port);
    
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
    bool ReadConfig(const char* config_file);
    bool MakeCmdPipe();
    bool MakeAsync(int fd);
    void ProcessCmd(const char* cmd);
    void CloseSock(int fd1, int fd2=-1);
    Route* GetRoute(const char* source_ip);
    Route* GetRoute(int source_fd);
    
    // Utils
    char* TrimString(char* str) const; // Trimming whitespace (both side)
    bool IsProcessRunning() const;

    // Class data
    char base_name[NAME_MAX+1]{}; // Base name of the program
    char conf_name[PATH_MAX+1]{}; // The name of the config file
    Callback cb[FD_SETSIZE]{};    // Array of callbacks (for every fd)
    fd_set rfds;                  // Set of fds to be checked for readability
    fd_set wfds;                  // Set of fds to be checked for writability
    int fd_max{-1};               // Highest-numbered file descriptor in set
    unsigned short port{0};       // Port to listen
    Route* route{nullptr};        // List of routes
    bool keep_running{false};
};

#endif // __TCP_PROXY__

