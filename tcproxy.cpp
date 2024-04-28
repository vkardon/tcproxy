//
//  tcproxy.cpp
//
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <memory.h>
#include <errno.h>
#include <assert.h>
#include <initializer_list> // for range-based loop: for(int fd : {1, 2, 2})...
#include <sys/stat.h>       // mkfifo
#include <fcntl.h>          // O_NONBLOCK and other O_* constants
#include <ctype.h>          // isspace
#include <libgen.h>         // basename
#include <signal.h>
#include "config.h"
#include "tcproxy.h"

const int MAX_LISTEN_BACKLOG = 5;

const char* CONFIG_KEY_PROXY  = "tcp_proxy";
const char* CONFIG_KEY_ROUTES = "tcp_proxy\\routes";

const char* CONFIG_NAME_PORT  = "port";
const char* CONFIG_NAME_ROUTE = "route";

const char* CMD_EXIT = "exit";
const char* CMD_ADD  = "add ";

CTcpProxy::CTcpProxy(const char* program_name, const char* configFile)
{
    // Set fdset to have zero bits for all file descriptors
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    
    keep_running = false; // Initially
    
    // Writing to an unconnected socket will cause a process to receivea SIGPIPE
    // signal. We don't want to die if this happens, so we ignore SIGPIPE.
    signal(SIGPIPE, SIG_IGN);
    
    // Get the base name
    char buf[PATH_MAX]{};
    strcpy(buf, program_name);
    char* name = basename(buf);
    char* tmp = strchr(name, '.');
    if(tmp != nullptr)
        *tmp = '\0';
    strcpy(base_name, name);
    
    // Make sure that no other instances of TcpProxy are running
    if(IsProcessRunning(base_name))
        return;
    
    // Read configuration (port, routes, etc.)
    if(!ReadConfig(configFile))
        return;
    
    // Open fifo to listen on the commands sent to the process
    if(!MakeFifo(base_name))
        return;
    
    // Success
    keep_running = true;
}

CTcpProxy::~CTcpProxy()
{
    // Delete routes
    Route* rt = route;
    while(rt != nullptr)
    {
        Route* rt_next = rt->next;
        delete rt;
        rt = rt_next;
    }
    
    // Delete callbacks
    for(int i = 0; i < FD_SETSIZE; i++)
    {
        if(FD_ISSET(i, &rfds) || FD_ISSET(i, &wfds))
        {
            CallbackRemove(i);
            close(i);
        }
    }
}

void CTcpProxy::CallbackAdd(int fd, int peer_fd, CALLBACK_FUNC read_fn, CALLBACK_FUNC write_fn)
{
    printf("%s: fd=%d\n", __func__, fd);
    
    assert(fd >= 0 && fd < FD_SETSIZE);
    
    Callback& c = cb[fd];
    assert(c.read_fn == nullptr && c.write_fn == nullptr && c.peer_fd < 0 && c.len == 0);
    c.Reset();
    
    c.read_fn = read_fn;
    c.write_fn = write_fn;
    c.peer_fd = peer_fd;
    
    // Set fd to be checked for readability
    if(c.read_fn != nullptr)
        FD_SET(fd, &rfds);
    
    // Set fd to be checked for writability
    if(c.write_fn != nullptr)
        FD_SET(fd, &wfds);
}

void CTcpProxy::CallbackRemove(int fd)
{
    printf("%s: fd=%d\n", __func__, fd);
    
    assert(fd >= 0 && fd < FD_SETSIZE);
    
    FD_CLR(fd, &rfds);
    FD_CLR(fd, &wfds);
    
    cb[fd].Reset();
}

void CTcpProxy::CallbackSelect()
{
    // Note: select will change fd_set passed, so we need to make a copy
    fd_set trfds = rfds;
    fd_set twfds = wfds;

    int n = select(FD_SETSIZE, &trfds, &twfds, nullptr, nullptr);
    if(n < 0)
    {
        printf("select error: %s\n", strerror(errno));
        return;
    }
    
    // Call callbacks for all ready file descriptors
    // Note: Start from file descriptors 3 since 0, 1, and 2 are stdin, stdout, and stderr.
    //for(int i = 0; n && i < FD_SETSIZE; i++)
    for(int i = 3; n && i < FD_SETSIZE; i++)
    {
        //printf("%s: entering FD_ISSEST...\n", __func__);
        
        // Note: any callbacks might in turn update rfds/wfds by calling
        // CallbackRemove on a higher numbered file descriptor.
        // We need to check rfds/wfds to make sure that callback we are
        // about to call is still wanted.
        
        if(FD_ISSET(i, &trfds))
        {
            n--;
            if(FD_ISSET(i, &rfds)) // Is callback still wanted?
            {
                //printf("%s: Calling read_fn for fd=%d\n", __func__, i);
                (this->*cb[i].read_fn)(i);
            }
        }
        
        if(FD_ISSET(i, &twfds))
        {
            n--;
            if(FD_ISSET(i, &wfds)) // Is callback still wanted?
            {
                //printf("%s: Calling write_fn for fd=%d\n", __func__, i);
                (this->*cb[i].write_fn)(i);
            }
        }
    }
}

inline CTcpProxy::Callback* CTcpProxy::GetCallback(int fd)
{
    assert(fd >= 0 && fd < FD_SETSIZE);
    return (fd >= 0 && fd < FD_SETSIZE ? &cb[fd] : nullptr);
}

bool CTcpProxy::MakeAsync(int fd)
{
    // Make file file descriptor nonblocking.
    int n = fcntl(fd, F_GETFL);
    if(n < 0)
    {
        printf("fcntl(F_GETFL) error: %s\n", strerror(errno));
        return false;
    }
    
    if(fcntl(fd, F_SETFL, n | O_NONBLOCK) < 0)
    {
        printf("fcntl(O_NONBLOCK) error: %s\n", strerror(errno));
        return false;
    }
    
    // Enable keepalives to make sockets time out if servers go away.
    n = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &n, sizeof(n)) < 0)
    {
        printf("setsockopt(SO_KEEPALIVE) error: %s\n", strerror(errno));
        return false;
    }
    
#ifdef SO_NOSIGPIPE
    // Don't send SIGPIPE signal on writing where the other end has been closed
    n = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &n, sizeof(n)) < 0)
    {
        printf("setsockopt(SO_NOSIGPIPE) error: %s\n", strerror(errno));
        return false;
    }
#endif // SO_NOSIGPIPE
    
    // Many asynchronous programming errors occur only when slow peers trigger
    // short writes.  To simulate this during testing, we set the buffer size
    // on the socket to 4 bytes.  This will ensure that each read and write
    // operation works on at most 4 bytes--a good stress test.
    //#define SMALL_LIMITS // Enable stress-tests
    
#ifdef SMALL_LIMITS
  #if defined(SO_RCVBUF) && defined(SO_SNDBUF)
    // Make sure this really is a stream socket(like TCP). Code using datagram
    // sockets will simply fail miserably if it can never transmit a packet
    // larger than 4 bytes.
    int type = 0;
    socklen_t sn = sizeof(n);
    
    if(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &sn) < 0)
    {
        printf("getsockopt(SO_TYPE) error: %s\n", strerror(errno));
        return false;
    }
    
    if(type != SOCK_STREAM)
    {
        printf("getsockopt(SO_TYPE) != SOCK_STREAM\n");
        return false;
    }
    
    n = 4;
    if(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n)) < 0)
    {
        printf("setsockopt(SO_RCVBUF) error: %s\n", strerror(errno));
        return false;
    }
    
    if(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n)) < 0)
    {
        printf("setsockopt(SO_SNDBUF) error: %s\n", strerror(errno));
        return false;
    }
  #else  
    #error "Need SO_RCVBUF/SO_SNDBUF for SMALL_LIMITS"
  #endif  // SO_RCVBUF && SO_SNDBUF
#endif  // SMALL_LIMITS
    
    return true;
}

bool CTcpProxy::AddRoute(const char* route_conf)
{
    if(route_conf == nullptr || *route_conf == '\0')
    {
        printf("%s: Error: invalid route format '%s'\n", __func__,
            (route_conf ? route_conf : "null"));
        return false;
    }

    // Route string expected format: "add 192.168.0.1 192.168.0.1:8080"
    // printf("%s: cmd=\"%s\"\n", __func__, routeStr);

    char source_host[HOST_NAME_MAX+1]{};
    char target_host[HOST_NAME_MAX+1]{};
    unsigned short target_port = 0;

    // Compose format string to scan up to HOST_NAME_MAX for host name/ip
    char format[32]{};
    sprintf(format, "%%%ds %%%d[^:]:%%hu", HOST_NAME_MAX, HOST_NAME_MAX);

    if(sscanf(route_conf, format, source_host, target_host, &target_port) != 3)
    {
        printf("%s: Invalid route configuration: \"%s\"\n", __func__, route_conf);
        return false;
    }

    return AddRoute(TrimString(source_host), TrimString(target_host), target_port);
}

bool CTcpProxy::AddRoute(const char* source_host, const char* target_host, unsigned short target_port)
{
    if(source_host == nullptr || *source_host == '\0' ||
       target_host == nullptr || *target_host == '\0' || 
       target_port == 0)
    {
        printf("%s: Error: invalid arguments\n", __func__);
        return false;
    }

    //
    // Get the target host addr
    //
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;        // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;    // TCP (connection-based protocol)
    hints.ai_flags = 0;
    hints.ai_protocol = 0;              // Any protocol

    addrinfo* addr = nullptr;
    int status = getaddrinfo(target_host, nullptr, &hints, &addr);
    if(status != 0)
    {
        printf("%s: getaddrinfo(%s) error: %s\n", __func__, target_host, gai_strerror(status));
        return false;
    }

    int target_ip_family{0};
    char target_ip[INET6_ADDRSTRLEN]{};

    // Note: getaddrinfo() returns a list of address structures
    // Get first AF_INET or AF_INET6 addr.
    for(const addrinfo* next = addr; next; next = next->ai_next)
    {
        if(next->ai_family == AF_INET)
        {
            target_ip_family = AF_INET;
            const struct in_addr& target_addr = ((sockaddr_in*)next->ai_addr)->sin_addr;
            inet_ntop(AF_INET, &target_addr, target_ip, INET_ADDRSTRLEN);
            break;
        }
        else if(next->ai_family == AF_INET6)
        {
            target_ip_family = AF_INET6;
            const struct in6_addr& target6_addr = ((sockaddr_in6*)next->ai_addr)->sin6_addr;
            inet_ntop(AF_INET6, &target6_addr, target_ip, INET6_ADDRSTRLEN);
            break;
        }
    }
    freeaddrinfo(addr);

    if(target_ip_family == 0)
    {
        printf("%s: No IPv4 nor IPv6 addresses available for '%s'\n", __func__, target_host);
        return false;
    }

    //
    // Get the source host addr
    //
    addr = nullptr;
    status = getaddrinfo(source_host, nullptr, &hints, &addr);
    if(status != 0)
    {
        printf("%s: getaddrinfo(%s) error: %s\n", __func__, source_host, gai_strerror(status));
        return false;
    }

    // Note: getaddrinfo() returns a list of address structures
    // Add new route for every AF_INET or AF_INET6 addr.
    int newRouteCount = 0;
    for(const addrinfo* next = addr; next; next = next->ai_next)
    {
        int source_ip_family{0};
        char source_ip[INET6_ADDRSTRLEN]{};

        if(next->ai_family == AF_INET)
        {
            source_ip_family = AF_INET;
            const struct in_addr& source_addr = ((sockaddr_in*)next->ai_addr)->sin_addr;
            inet_ntop(AF_INET, &source_addr, source_ip, INET_ADDRSTRLEN); 
        }
        else if(next->ai_family == AF_INET6)
        {
            source_ip_family = AF_INET6;
            const struct in6_addr& source6_addr = ((sockaddr_in6*)next->ai_addr)->sin6_addr;
            inet_ntop(AF_INET6, &source6_addr, source_ip, INET6_ADDRSTRLEN); 
        }
        else
        {
            continue;
        }

        // Create new route
        Route* new_route = new (std::nothrow) Route;
        if(new_route == nullptr)
        {
            printf("%s: Out of memory: new_route is NULL\n", __func__);
            freeaddrinfo(addr);
            return false;
        }
        newRouteCount++;

        new_route->source_ip_family = source_ip_family;
        strcpy(new_route->source_ip, source_ip);  
        new_route->target_ip_family = target_ip_family;
        strcpy(new_route->target_ip, target_ip);
        new_route->target_port = target_port;

        printf("%s: Adding route %s (%s) --> %s:%hu (%s)\n", __func__,
               source_host, new_route->source_ip,
               target_host, target_port, new_route->target_ip);

        // Set new route
        if(route == nullptr)
        {
            route = new_route;
            continue;
        }
        
        Route* rt = GetRoute(new_route->source_ip);
        if(rt == nullptr)
        {
            new_route->next = route;
            route = new_route;
            continue;
        }
        
        // The route is duplicated. Is it already connected?
        if(rt->source_fd < 0)
        {
            // The route is not connected. Just update target ip/port
            strcpy(rt->target_ip, new_route->target_ip);
            rt->target_port = new_route->target_port;
            delete new_route;
            continue;
        }
        
        // The route is already connected.
        printf("%s: Duplicated route for %s --> %s:%hu\n", __func__,
               source_host, target_host, target_port);
        
        // Get the route's target socket
        Callback* cb = GetCallback(rt->source_fd);
        if(cb == nullptr)
        {
            printf("%s: fd=%d, callback is NULL\n", __func__, rt->source_fd);
            delete new_route;
            newRouteCount--; // Since we can't proceed for this route
            continue;
        }
        
        // Close both source and target sockets (and remove associated callbacks)
        // Note: it will reset rt->source_fd to -1
        CloseSock(rt->source_fd, cb->peer_fd);
        assert(rt->source_fd < 0);

        // Update target ip/port
        strcpy(rt->target_ip, new_route->target_ip);
        rt->target_port = new_route->target_port;
        rt->source_fd = -1;
        delete new_route;
    }
    freeaddrinfo(addr);

    if(newRouteCount == 0)
    {
        printf("%s: Error adding new route for '%s'\n", __func__, source_host);
    }
    else
    {
        printf("%s: %d new route(s) added: %s --> %s:%hu\n", __func__,
               newRouteCount, source_host, target_host, target_port);
    }

    return (newRouteCount > 0);
}

CTcpProxy::Route* CTcpProxy::GetRoute(const char* source_ip)
{
    if(source_ip == nullptr)
        return nullptr;
    
    Route* rt = route;
    while(rt != nullptr)
    {
        if(strcmp(rt->source_ip, source_ip) == 0)
            break;
        rt = rt->next;
    }
    return rt;
}

CTcpProxy::Route* CTcpProxy::GetRoute(int source_fd)
{
    assert(source_fd >= 0 && source_fd < FD_SETSIZE);
    
    if(source_fd < 0 || source_fd >= FD_SETSIZE)
        return nullptr;
    
    Route* rt = route;
    while(rt != nullptr)
    {
        if(rt->source_fd == source_fd)
            break;
        rt = rt->next;
    }
    return rt;
}

bool CTcpProxy::ReadConfig(const char* configFile)
{
    //
    // Read port
    //
    Config config(configFile, CONFIG_KEY_PROXY);
    if(!config.IsValid())
    {
        printf("%s: Failed to read key=%s.\n", __func__, CONFIG_KEY_PROXY);
        return false;
    }
    
    int tmp = 0;
    if(!config.GetIntValue(CONFIG_NAME_PORT, tmp))
    {
        printf("%s: Failed to read key=%s, name=%s.\n", __func__, CONFIG_KEY_PROXY, CONFIG_NAME_PORT);
        return false;
    }
    else if(tmp <= 0 || tmp > USHRT_MAX)
    {
        printf("%s: Invalid port number specified \"%d\"\n", __func__, tmp);
        return false;
    }
    port = (unsigned short)tmp;
    
    //
    // Read routes
    //
    config.Init(configFile, CONFIG_KEY_ROUTES);
    if(!config.IsValid())
    {
        printf("%s: Failed to read key=%s.\n", __func__, CONFIG_KEY_ROUTES);
        return false;
    }
    
    auto OnEnum = [&](const char* route)->bool
    {
        return AddRoute(route);
    };
    
    // Enum routes
    return config.EnumValue(CONFIG_NAME_ROUTE, OnEnum);
}

bool CTcpProxy::MakeFifo(const char* fifo_base_name)
{
    // Open fifo to listen on the commands sent to the process
    char fifo_name[PATH_MAX]{};
    sprintf(fifo_name, "/tmp/%s.fifo", fifo_base_name);
    
    // Remove fifo if existed from previous run. It's OK if it doesn't exist
    unlink(fifo_name);
    
    // Make fifo
    if(mkfifo(fifo_name, S_IRUSR | S_IWUSR | S_IWGRP) == -1 && errno != EEXIST)
    {
        printf("%s: mkfifo error: %s\n", __func__, strerror(errno));
        return false;
    }
    
    // Open nonblocking
    int fifo = open(fifo_name, O_RDONLY | O_NONBLOCK);
    if(fifo == -1)
    {
        printf("%s: open error: %s\n", __func__, strerror(errno));
        return false;
    }
    
    // Add fifo callback
    CallbackAdd(fifo, -1, &CTcpProxy::OnCommand, nullptr);
    return true;
}

bool CTcpProxy::Listen()
{
    if(!keep_running)
    {
        printf("%s: keep_running=false, cannot continue\n", __func__);
        return false;
    }
    
    // Open up the TCP socket the proxy listens on
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0)
    {
        printf("%s: socket error: %s\n", __func__, strerror(errno));
        return false;
    }
    
    // Bind the socket to all local addresses
    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof(struct sockaddr_in));
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY; // bind to all local addresses
    proxy_addr.sin_port = htons(port);
    
    int flag = 1;
    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0)
    {
        printf("%s: setsockopt(SO_REUSEADDR) error: %s\n", __func__, strerror(errno));
        close(sock);
        return false;
    }
    
    if(bind(sock, (struct sockaddr*)&proxy_addr, sizeof(proxy_addr)) < 0)
    {
        printf("%s: bind error: %s\n", __func__, strerror(errno));
        //printf("%s: bind error: %s\n", __func__, hstrerror(h_errno));
        close(sock);
        return false;
    }
    
    // Make the socket async
    if(!MakeAsync(sock))
    {
        printf("%s: make_async(proxy_sock) failed\n", __func__);
        close(sock);
        return false;
    }
    
    // Start listening...
    if(listen(sock, MAX_LISTEN_BACKLOG) != 0)
    {
        printf("%s: listen error: %s\n", __func__, strerror(errno));
        close(sock);
        return false;
    }
    
    // Add callback
    CallbackAdd(sock, -1, &CTcpProxy::OnConnect, nullptr);
    
    // Success
    printf("%s: fd=%d, listening on port %d for incomming connections....\n", __func__, sock, port);
    
    // Enter events loop...
    while(keep_running)
    {
        //printf("%s: CallbackSelect() ...\n", __func__);
        CallbackSelect();
        //printf("%s: CallbackSelect() end\n", __func__);
    }
    
    return true;
}

// Called by the event loop when ready to read connected socket
void CTcpProxy::OnRead(int fd)
{
    Callback* cb = GetCallback(fd);
    if(cb == nullptr)
    {
        printf("%s: fd=%d, cb is NULL\n", __func__, fd);
        return;
    }
    
    // Write to peer socket buffer
    Callback* peer_cb = GetCallback(cb->peer_fd);
    if(peer_cb == nullptr)
    {
        printf("%s: fd=%d, peer_fd=%d: peer_cb=nullptr\n", __func__, fd, cb->peer_fd);
        CloseSock(fd, cb->peer_fd); // Note: arg is no longer valid
        return;
    }
    
    if(peer_cb->len != 0)
        return; // Still have date to write from previous read
    
    ssize_t n = read(fd, peer_cb->buf, sizeof(peer_cb->buf));
    
    if(n == 0)
    {
        // The connection has been gracefully closed by the client
        printf("%s: fd=%d, the client closed the connection\n", __func__, fd);
        CloseSock(fd, cb->peer_fd); // Note: arg is no longer valid
    }
    else if(n < 0)
    {
        // Client communication error
        if(errno != EAGAIN && errno != EINTR)
        {
            printf("%s: fd=%d, read error: %s\n", __func__, fd, strerror(errno));
            CloseSock(fd, cb->peer_fd); // Note: arg is no longer valid
        }
    }
    else
    {
        // Success
        peer_cb->len = n;
    }
}

// Called by the event loop when ready to write connected socket
void CTcpProxy::OnWrite(int fd)
{
    Callback* cb = GetCallback(fd);
    if(cb == nullptr)
    {
        printf("%s: fd=%d, cb is NULL\n", __func__, fd);
        return;
    }
    
    if(cb->len == 0)
        return; // Nothing to write
    
    ssize_t n = write(fd, cb->buf, cb->len);
    
    if(n == 0)
    {
        printf("%s: fd=%d, write error EOF: %s\n", __func__, fd, strerror(errno));
        CloseSock(fd, cb->peer_fd); // Note: cb is no longer valid
    }
    else if(n < 0)
    {
        if(errno != EAGAIN && errno != EINTR)
        {
            printf("%s: fd=%d, write error: %s\n", __func__, fd, strerror(errno));
            CloseSock(fd, cb->peer_fd); // Note: arg is no longer valid
        }
    }
    else
    {
        // Success
        if(n < cb->len)
        {
            // Still have data to write.
            // Shift remaining date to the beginning of the buffer
            memmove(cb->buf, cb->buf + n, cb->len - n);
            cb->len = cb->len - n;
        }
        else
        {
            // No data to write left
            cb->len = 0;
        }
    }
}

// Called by the event loop when ready to read/accept connected socket
void CTcpProxy::OnConnect(int fd)
{
    socklen_t addr_len = sizeof(struct sockaddr); // in/out parameter
    struct sockaddr source_addr;
    memset(&source_addr, 0, sizeof(struct sockaddr));
    
    int source_fd = accept(fd, &source_addr, &addr_len);
    if(source_fd < 0)
    {
        if(errno != EAGAIN && errno != EINTR) // nonblocking, retry
            printf("%s: fd=%d, accept error: %s\n", __func__, fd, strerror(errno));
        CloseSock(fd);
        return;
    }
    
    if(source_fd >= FD_SETSIZE)
    {
        printf("%s: fd=%d, source_fd=%d exeedes the max file descriptor %d\n",
                __func__, fd, source_fd, FD_SETSIZE-1);
        CloseSock(source_fd);
        return;
    }
    
    if(!MakeAsync(source_fd)) // this may very well be redundant
    {
        printf("%s: fd=%d, make_async(source_fd) failed\n", __func__, fd);
        CloseSock(source_fd);
        return;
    }
    
    char source_ip[INET6_ADDRSTRLEN]{};
    in_port_t source_port{0};

    if(source_addr.sa_family == AF_INET)
    { 
        const sockaddr_in& addr = (sockaddr_in&)source_addr;
        source_port = addr.sin_port;
        inet_ntop(AF_INET, &addr.sin_addr, source_ip, INET_ADDRSTRLEN);
    }
    else if(source_addr.sa_family == AF_INET6)
    {
        const sockaddr_in6& addr6 = (sockaddr_in6&)source_addr;
        source_port = addr6.sin6_port;
        inet_ntop(AF_INET6, &addr6.sin6_addr, source_ip, INET6_ADDRSTRLEN);
    }
    else
    {
        printf("%s: fd=%d, unsupported socket address family\n", __func__, fd);
        CloseSock(source_fd);
        return;
    }
    
    // Lookup server name and establish control connection
    Route* rt = GetRoute(source_ip);
    if(rt == nullptr)
    {
        printf("%s: fd=%d, GetRoute failed for source_ip=%s\n", __func__, fd, source_ip);
        CloseSock(source_fd);
        return;
    }
    
    int target_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(target_fd < 0)
    {
        printf("%s: fd=%d, socket error: %s\n", __func__, fd, strerror(errno));
        CloseSock(source_fd);
        return;
    }
    
    if(target_fd >= FD_SETSIZE)
    {
        printf("%s: fd=%d, target_fd=%d exeedes the max file descriptor %d\n",
                __func__, fd, target_fd, FD_SETSIZE-1);
        CloseSock(source_fd, target_fd);
        return;
    }
    
    if(!MakeAsync(target_fd))
    {
        printf("%s: fd=%d, make_async(target_fd) failed\n", __func__, fd);
        CloseSock(source_fd, target_fd);
        return;
    }
    
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(struct sockaddr_in));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(rt->target_port);
    inet_aton(rt->target_ip, &target_addr.sin_addr);
    
    if(connect(target_fd, (struct sockaddr*)&target_addr, sizeof(struct sockaddr_in)) < 0)
    {
        if(errno != EINPROGRESS) // nonblocking, connection stalled
        {
            printf("%s: fd=%d, connect error: %s\n", __func__, fd, strerror(errno));
            CloseSock(source_fd, target_fd);
            return;
        }
    }
    
    printf("%s: fd=%d, connection proxied: %s:%d (fd=%d) --> %s:%hu (fd=%d)\n", __func__, fd,
           source_ip, ntohs(source_port), source_fd, 
           rt->target_ip, rt->target_port, target_fd);
    
    // Add client/server callbacks
    CallbackAdd(source_fd, target_fd, &CTcpProxy::OnRead, &CTcpProxy::OnWrite);
    CallbackAdd(target_fd, source_fd, &CTcpProxy::OnRead, &CTcpProxy::OnWrite);
    
    // Update source/target route table
    rt->source_fd = source_fd;
}

void CTcpProxy::OnCommand(int fd)
{
    Callback* cb = GetCallback(fd);
    if(cb == nullptr)
    {
        printf("%s: fd=%d, cb is NULL\n", __func__, fd);
        return;
    }
    
    ssize_t n = read(fd, cb->buf + cb->len, sizeof(cb->buf) - cb->len);
    
    if(n == 0)
    {
        // The connection has been gracefully closed by the client
        //printf("%s fd=%d: The client closed the connection\n", __func__, fd);
        
        // Process command
        char* cmd = TrimString((char*)cb->buf);
        printf("%s: fd=%d, cmd=\"%s\"\n", __func__, fd, cmd);
        
        ProcessCmd(cmd);

        // Close fifo & reopen since connection is closed
        CloseSock(fd);
        MakeFifo(base_name);
        
        // Reset cmd buffer
        memset(cb->buf, 0, sizeof(cb->buf));
        cb->len = 0;
    }
    else if(n < 0)
    {
        // Client communication error
        if(errno != EAGAIN && errno != EINTR)
        {
            printf("%s: fd=%d, read error: %s\n", __func__, fd, strerror(errno));
            
            // Reset cmd buffer
            memset(cb->buf, 0, sizeof(cb->buf));
            cb->len = 0;
        }
    }
    else
    {
        // Success
        cb->len += n;
    }
}

void CTcpProxy::CloseSock(int fd1, int fd2)
{
    for(int fd : {fd1, fd2})
    {
        if(fd >= 0)
        {
            close(fd);
            if(fd < FD_SETSIZE)
                CallbackRemove(fd);
            
            // If fd represents a source host, then update routing table
            Route* rt = GetRoute(fd);
            if(rt != nullptr)
                rt->source_fd = -1;
        }
    }
}

void CTcpProxy::ProcessCmd(const char* cmd)
{
    if(strcasecmp(cmd, CMD_EXIT) == 0)
    {
        keep_running = false;
    }
    else if(strncasecmp(cmd, CMD_ADD, strlen(CMD_ADD)) == 0)
    {
        // Expected command format is "add 192.168.0.1 192.168.0.1:8080";
        printf("%s: cmd=\"%s\"\n", __func__, cmd);
        const char* route_conf = cmd + strlen(CMD_ADD);
        AddRoute(route_conf);
    }
    else
    {
        printf("%s: Unknown command \"%s\"\n", __func__, cmd);
    }
}

char* CTcpProxy::TrimString(char* str) const
{
    if(str == nullptr)
        return nullptr;
    
    // Trim left side
    char* ptr = str;
    while(isspace(*ptr))
        ptr++;
    
    str = ptr;
    
    // Trim right side
    char* ptrLast = nullptr;
    while(*ptr != '\0')
    {
        //if(*ptr == ' ')
        if(isspace(*ptr))
        {
            if(ptrLast == nullptr)
                ptrLast = ptr;
        }
        else
        {
            ptrLast = nullptr;
        }
        ptr++;
    }
    
    // Truncate at trailing spaces
    if(ptrLast != nullptr)
        *ptrLast = '\0';
    
    // Remove new-line character at the end - if any
    if(*(ptr - 1) == '\n')
        *(ptr - 1) = '\0';
    
    return str;
}

bool CTcpProxy::IsProcessRunning(const char* process_name) const
{
    if(process_name == nullptr || process_name[0] == '\0')
        return false;
    
    char lock_file[PATH_MAX]{};
    sprintf(lock_file, "/tmp/%s.lock", process_name);
    
    int fd = open(lock_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if(fd == -1)
    {
        //perror("Cannot open lock file\n");
        printf("%s: Cannot open lock file \"%s\": %s\n", __func__, lock_file, strerror(errno));
        return false;
    }
    
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_WRLCK;      // F_RDLCK, F_WRLCK, F_UNLCK
    fl.l_whence = SEEK_SET;     // SEEK_SET, SEEK_CUR, SEEK_END
    fl.l_start  = 0;            // Offset from l_whence
    fl.l_len    = 0;            // length, 0 = to EOF
    fl.l_pid    = getpid();     // our PID
    
    // Try to create a file lock
    if(fcntl(fd, F_SETLK, &fl) == -1)   /* F_GETLK, F_SETLK, F_SETLKW */
    {
        // we failed to create a file lock, meaning it's already locked
        if(errno == EACCES || errno == EAGAIN)
        {
            printf("%s: Another instance of %s is already running\n", __func__, process_name);
            return true;
        }
    }
    
    return false;
}

