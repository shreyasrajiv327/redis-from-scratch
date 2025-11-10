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

constexpr size_t MAX_MSG = 32 << 20; // 32 MB buffer size

// Utility: throw error with message
void die(const std::string& msg) {
    throw std::runtime_error(msg + " (errno: " + std::to_string(errno) + ")");
}

// Utility: set file descriptor to non-blocking mode
void setNonBlocking(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) die("fcntl(F_GETFL) failed");

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        die("fcntl(F_SETFL) failed");
}

// A single client connection
struct Connection {
    int fd = -1;
    bool wantRead = false;
    bool wantWrite = false;
    bool wantClose = false;

    std::vector<uint8_t> incoming; // data read from client
    std::vector<uint8_t> outgoing; // data to send to client
};

// Append bytes to a vector buffer
void appendBuffer(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// Consume n bytes from front
void consumeBuffer(std::vector<uint8_t>& buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + std::min(n, buf.size()));
}

// Accept a new client connection
std::unique_ptr<Connection> handleAccept(int listenFd) {
    sockaddr_in clientAddr{};
    socklen_t addrlen = sizeof(clientAddr);

    int connFd = accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &addrlen);
    if (connFd < 0) {
        std::cerr << "accept() failed: " << strerror(errno) << "\n";
        return nullptr;
    }

    uint32_t ip = clientAddr.sin_addr.s_addr;
    std::cout << "New client from "
              << (ip & 255) << '.'
              << ((ip >> 8) & 255) << '.'
              << ((ip >> 16) & 255) << '.'
              << ((ip >> 24) & 255)
              << ":" << ntohs(clientAddr.sin_port) << "\n";

    setNonBlocking(connFd);

    auto conn = std::make_unique<Connection>();
    conn->fd = connFd;
    conn->wantRead = true;
    return conn;
}

// Process one request if enough data is available
bool processOneRequest(Connection& conn) {
    if (conn.incoming.size() < 4) return false;

    uint32_t len = 0;
    memcpy(&len, conn.incoming.data(), 4);
    if (len > MAX_MSG) {
        std::cerr << "Message too long\n";
        conn.wantClose = true;
        return false;
    }

    if (conn.incoming.size() < 4 + len) return false;

    const uint8_t* msg = conn.incoming.data() + 4;
    std::string clientMsg(reinterpret_cast<const char*>(msg), len);

    std::cout << "Client says: " << clientMsg << "\n";

    // Echo response
    appendBuffer(conn.outgoing, reinterpret_cast<const uint8_t*>(&len), 4);
    appendBuffer(conn.outgoing, msg, len);

    consumeBuffer(conn.incoming, 4 + len);
    return true;
}

// Handle writable socket
void handleWrite(Connection& conn) {
    if (conn.outgoing.empty()) return;

    ssize_t n = write(conn.fd, conn.outgoing.data(), conn.outgoing.size());
    if (n < 0) {
        if (errno == EAGAIN) return; // not ready
        std::cerr << "write() error: " << strerror(errno) << "\n";
        conn.wantClose = true;
        return;
    }

    consumeBuffer(conn.outgoing, static_cast<size_t>(n));

    if (conn.outgoing.empty()) {
        conn.wantRead = true;
        conn.wantWrite = false;
    }
}

// Handle readable socket
void handleRead(Connection& conn) {
    uint8_t buf[64 * 1024];
    ssize_t n = read(conn.fd, buf, sizeof(buf));

    if (n < 0) {
        if (errno == EAGAIN) return;
        std::cerr << "read() error: " << strerror(errno) << "\n";
        conn.wantClose = true;
        return;
    }

    if (n == 0) {
        std::cout << "Client closed connection\n";
        conn.wantClose = true;
        return;
    }

    appendBuffer(conn.incoming, buf, static_cast<size_t>(n));

    while (processOneRequest(conn)) {}

    if (!conn.outgoing.empty()) {
        conn.wantWrite = true;
        conn.wantRead = false;
        handleWrite(conn);
    }
}

// Main entry point
int main() {
    try {
        int listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) die("socket() failed");

        int opt = 1;
        if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
            die("setsockopt() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1234);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            die("bind() failed");

        setNonBlocking(listenFd);

        if (listen(listenFd, SOMAXCONN) < 0)
            die("listen() failed");

        std::cout << "Server running on port 1234\n";

        std::vector<std::unique_ptr<Connection>> connections;
        std::vector<pollfd> pollFds;

        while (true) {
            pollFds.clear();

            // Listening socket
            pollfd listenPoll{};
            listenPoll.fd = listenFd;
            listenPoll.events = POLLIN;
            pollFds.push_back(listenPoll);

            // Add all client sockets
            for (auto& conn : connections) {
                if (!conn) continue;
                pollfd pfd{};
                pfd.fd = conn->fd;
                pfd.events = POLLERR;
                if (conn->wantRead) pfd.events |= POLLIN;
                if (conn->wantWrite) pfd.events |= POLLOUT;
                pollFds.push_back(pfd);
            }

            int rv = poll(pollFds.data(), pollFds.size(), -1);
            if (rv < 0) {
                if (errno == EINTR) continue;
                die("poll() failed");
            }

            // Handle listening socket
            if (pollFds[0].revents & POLLIN) {
                auto newConn = handleAccept(listenFd);
                if (newConn) {
                    if (connections.size() <= static_cast<size_t>(newConn->fd))
                        connections.resize(newConn->fd + 1);
                    connections[newConn->fd] = std::move(newConn);
                }
            }

            // Handle clients
            for (size_t i = 1; i < pollFds.size(); ++i) {
                auto& pfd = pollFds[i];
                auto& conn = connections[pfd.fd];
                if (!conn) continue;

                if (pfd.revents & POLLIN) handleRead(*conn);
                if (pfd.revents & POLLOUT) handleWrite(*conn);

                if ((pfd.revents & POLLERR) || conn->wantClose) {
                    close(conn->fd);
                    connections[pfd.fd].reset();
                }
            }
        }

        close(listenFd);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
