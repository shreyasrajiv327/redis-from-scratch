#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <cassert>

// POSIX headers
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

constexpr size_t MAX_MSG = 32 << 20; //Buffer Size = 32MB
using namespace std;
void die (const string& msg)
{
    throw runtime_error(msg + "errNo: " + to_string(errno));
}

struct Connection {
    int fd = -1;
    bool wantRead = false;
    bool wantWrite = false;
    bool wantClose = false;

    vector<uint8_t> incoming;
    vector<uint8_t> outgoing;
};

void setNonBlocking(int fd)
{
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags  == -1 ) die("fcntl(F_GETFL) failed");
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) die("fcntl(F_SETFL) failed");
}