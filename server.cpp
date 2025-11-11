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

class Connection {
public :
    int fd = -1;
    bool wantRead = false;
    bool wantWrite = false;
    bool wantClose = false;
    vector<uint8_t> incoming;
    vector<uint8_t> outgoing;
    Connection(int fd_)
        : fd(fd_), wantRead(true), wantWrite(false), wantClose(false) {}

    bool processOneRequest(Connection& conn)
    {
      if (conn.incoming.size()<4) return false;

      uint32_t len = 0;
      memcpy(&len,conn.incoming.data(), 4);
      if(len>MAX_MSG)
      {
        cerr<<"Message too long\n";
        conn.wantClose = true;
        return false;
      }

      if(conn.incoming.size()<4+len) return false;

      const uint8_t* msg = conn.incoming.data()+4;
      string clientMsg(reinterpret_cast<const char*>(msg), len);

      cout<<"Client says: "<<clientMsg<<"\n";
      appendBuffer(conn.outgoing, reinterpret_cast<const uint8_t*>(&len), 4);
      appendBuffer(conn.outgoing, msg, len);

      consumeBuffer(conn.incoming, 4 + len);
      return true;
    }

    void appendBuffer(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
     }

// Consume n bytes from front
    void consumeBuffer(std::vector<uint8_t>& buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + std::min(n, buf.size()));
   }

   void handleWrite(Connection& conn)
   {
    if(conn.outgoing.empty()) return;

    ssize_t n = write(conn.fd, conn.outgoing.data(), conn.outgoing.size());
    if(n<0)
    {
      if(errno==EAGAIN) return;
      cerr<< "write() error: " << strerror(errno) << "\n";
      conn.wantClose = true;
      return;
    }
    
    consumeBuffer(conn.outgoing, static_cast<size_t>(n));

    if (conn.outgoing.empty()) {
        conn.wantRead = true;
        conn.wantWrite = false;
    }

   }

   void handleRead(Connection& conn)
   {
    uint8_t buf[64*1024];
    ssize_t n = read(conn.fd,buf,sizeof(buf));
    if (n == 0) {
    cout << "Client closed connection\n";
    conn.wantClose = true;
    return;
}
    if(n<0)
    {
       if (errno == EAGAIN) return;
        cerr << "read() error: " << strerror(errno) << "\n";
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

};




class Server{
    public:
          void die(const string& msg)
          {
            throw runtime_error(msg + "(errno: " + to_string(errno) +")");
          }
          
          void setNonBlocking(int fd)
          {
            errno = 0;
            int flags = fcntl(fd,F_GETFL,0);
            if(flags==-1) die("fcntl(F_GETFL) failed");

            if(fcntl(fd,F_SETFL, flags | O_NONBLOCK) == -1)
            {
                die("fcntl(F_SETFL) failed");
            }
          }

          void createServer()
          {
            try{
            int listenFD =socket(AF_INET,SOCK_STREAM,0);
            if(listenFD<0) {
                  die("socket() failed");
            }
            int opt = 1;
            if(setsockopt(listenFD,SOL_SOCKET,SO_REUSEADDR, &opt, sizeof(opt))<0)
            {
                die("setsockopt() failed");
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(1234);
            addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(listenFD, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            die("bind() failed");
             
            if (listen(listenFD, SOMAXCONN) < 0)
    die("listen() failed");
            setNonBlocking(listenFD);

            vector<unique_ptr<Connection>> connections;
            vector<pollfd> pollFds;

             
        while (true) {
            pollFds.clear();

            // Listening socket
            pollfd listenPoll{};
            listenPoll.fd = listenFD;
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
                auto newConn = handleAccept(listenFD);
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

                if (pfd.revents & POLLIN) conn->handleRead(*conn);
                if (pfd.revents & POLLOUT) conn->handleWrite(*conn);

                if ((pfd.revents & POLLERR) || conn->wantClose) {
                    ::close(conn->fd);
                    connections[pfd.fd].reset();
                }
            }
        }} catch (const std::exception& e) {
        cerr << "Fatal error: " << e.what() << "\n";
        
    }


          }

          unique_ptr<Connection> handleAccept(int listenFD)
          {
            sockaddr_in clientAddr{};
            socklen_t addrlen = sizeof(clientAddr);

            int connFD = accept(listenFD, reinterpret_cast<sockaddr*>(&clientAddr), &addrlen);
            if(connFD<0)
            {
               cerr<<"accept() failed: " <<strerror(errno)<<"\n";
               return nullptr;
            }
          
            uint32_t ip = clientAddr.sin_addr.s_addr;
            cout<<"New client from"
              << (ip & 255) << '.'
              << ((ip >> 8) & 255) << '.'
              << ((ip >> 16) & 255) << '.'
              << ((ip >> 24) & 255)
              << ":" << ntohs(clientAddr.sin_port) << "\n";
            
              setNonBlocking(connFD);

             auto conn = make_unique<Connection>(connFD);
              conn->fd = connFD;
              conn->wantRead = true;
              return conn;
          }




};

    
int main(){
   try{
    Server s;
    s.createServer();
   }catch (const exception& e) {
        cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

   return 0;
}