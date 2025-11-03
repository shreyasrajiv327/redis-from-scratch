#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

constexpr size_t kMaxMsg = 4096;

void die(const std::string &msg)
{
    int err = errno;
    std::cerr<<"[" << err << "]" << std::endl;
    throw std::runtime_error(msg);
}

int read_full(int fd, char *buf, size_t n)
{
   while(n>0)
   {
     ssize_t rv = read(fd, buf, n);
     if(rv<=0) return -1;
      n -= static_cast<size_t>(rv);
      buf += rv;
   }
   return 0;
}

int write_all(int fd, const char *buf, size_t n)
{
    while(n>0)
    {
        ssize_t rv = write(fd, buf, n);
        if(rv<=0) return -1;
        n -= static_cast<size_t>(rv);
        buf += rv;
    }
    return 0;
}

int query (int fd, const std::string &text)
{
    uint32_t len = static_cast<uint32_t>(text.size());
    if(len>kMaxMsg)
    {
        throw std::runtime_error("message too long");
    }

    std::vector<char> wbuf(4+len);
    std::memcpy(wbuf.data(),&len,4);
    std::memcpy(wbuf.data()+4,text.data(),len);

    if(write_all(fd, wbuf.data(), wbuf.size())<0){
        die("write_all() failed");
    }

    std::vector<char> rbuf(4+kMaxMsg);
    if(read_full(fd, rbuf.data(),4)<0)
    {
         die("read_full(header) failed");
    }

    std::memcpy(&len, rbuf.data(), 4);
    if (len > kMaxMsg)
        throw std::runtime_error("server response too long");

    // Read body
    if (read_full(fd, rbuf.data() + 4, len) < 0)
        die("read_full(body) failed");

    std::string reply(rbuf.data() + 4, len);
    std::cout << "Server says: " << reply << std::endl;
    return 0;
}

int main() {
    try {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            die("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1234);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

        if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
            die("connect() failed");

        // Multiple requests
        query(fd, "hello1");
        query(fd, "hello2");
        query(fd, "hello3");

        close(fd);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}