#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

constexpr size_t k_max_msg = 4096;

void die(const std::string& msg) {
    throw std::runtime_error(msg + " (" + std::to_string(errno) + ")");
}

ssize_t read_full(int fd, char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1; // error or EOF
        n -= static_cast<size_t>(rv);
        buf += rv;
    }
    return 0;
}

ssize_t write_all(int fd, const char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1; // error
        n -= static_cast<size_t>(rv);
        buf += rv;
    }
    return 0;
}

int32_t handle_request(int connfd) {
    std::vector<char> rbuf(4 + k_max_msg);
    if (read_full(connfd, rbuf.data(), 4)) {
        std::cerr << (errno == 0 ? "EOF" : "read() error") << std::endl;
        return -1;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf.data(), 4);
    if (len > k_max_msg) {
        std::cerr << "Too long message received" << std::endl;
        return -1;
    }

    if (read_full(connfd, rbuf.data() + 4, len)) {
        std::cerr << "read() error" << std::endl;
        return -1;
    }

    std::string client_msg(rbuf.data() + 4, len);
    std::cerr << "Client says: " << client_msg << std::endl;

    // Reply
    std::string reply = "world";
    uint32_t reply_len = static_cast<uint32_t>(reply.size());
    std::vector<char> wbuf(4 + reply_len);
    memcpy(wbuf.data(), &reply_len, 4);
    memcpy(wbuf.data() + 4, reply.data(), reply_len);

    return write_all(connfd, wbuf.data(), wbuf.size());
}

int main() {
    try {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) die("socket()");

        int val = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
            die("setsockopt()");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1234);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            die("bind()");

        if (listen(fd, SOMAXCONN) < 0)
            die("listen()");

        std::cout << "Server listening on port 1234..." << std::endl;

        while (true) {
            sockaddr_in client_addr{};
            socklen_t addrlen = sizeof(client_addr);
            int connfd = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &addrlen);
            if (connfd < 0) {
                std::cerr << "Accept failed, continuing..." << std::endl;
                continue;
            }

            std::cout << "Client connected." << std::endl;

            while (true) {
                if (handle_request(connfd)) break;
            }

            close(connfd);
            std::cout << "Client disconnected." << std::endl;
        }

        close(fd);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
