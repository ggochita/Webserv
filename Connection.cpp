#include "Connection.hpp"
#include "CgiHandler.hpp"
#include <sys/socket.h>  // for recv(), send()
#include <iostream>
#include <cerrno>

Connection::Connection(int fd)
    : fd_(fd), outBufferOffset_(0), closed_(false),
    cgiHandler_(NULL), cgiBytesWritten_(0)
{
    std::cout  << "\n";
    std::cout << fd_ << " : " << "connectted now" << std::endl;
    std::cout  << "\n";
}

Connection::~Connection()
{
    std::cout  << "\n";
    std::cout << fd_ << " : " << "Closed Now(via destructor)" << std::endl;
    std::cout  << "\n";

    if (cgiHandler_)
        delete cgiHandler_;

    if (!closed_ && fd_ >= 0)
    {
        ::close(fd_);
        closed_ = true;
    }
}

int Connection::fd() const
{
    return fd_;
}

std::string& Connection::in()
{
    return inBuffer_;
}

std::string& Connection::out()
{
    return outBuffer_;
}

Request& Connection::request()
{
    return request_;
}

const Request& Connection::request() const
{
    return request_;
}

bool Connection::isClosed() const
{
    return closed_;
}

void Connection::closeNow()
{
    std::cout  << "\n";
    std::cout << fd_ << " : " << "Closed Now" << std::endl;
    std::cout  << "\n";

    if (!closed_ && fd_ >= 0)
    {
        ::close(fd_);
        closed_ = true;
    }
}

void Connection::clearIo()
{
    inBuffer_.clear();
    outBuffer_.clear();
    outBufferOffset_ = 0;
}


int Connection::readFromSocket()
{
    if (inBuffer_.size() > MAX_HEADER_SIZE)
        throw Request::ParseError(431);

    char buf[8192];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);

    if (n > 0)
    {
        inBuffer_.append(buf, n);
        return static_cast<int>(n);
    }
    else if (n == 0)
        return 0;

    perror("recv()");
    closeNow();
    return -1;
}


int Connection::writeToSocket()
{
    if (outBuffer_.empty() || outBufferOffset_ >= outBuffer_.size())
    {
        outBuffer_.clear();
        outBufferOffset_ = 0;
        cgiOutput_.clear();
        return 0;
    }
    
    size_t totalSize = outBuffer_.size();
    size_t remaining = totalSize - outBufferOffset_;

    // Pointer arithmetic: Start sending from where we left off
    const char* dataPtr = outBuffer_.c_str() + outBufferOffset_;

    ssize_t n = ::send(fd_, dataPtr, remaining, 0);

    if (n >= 0)
    {
        outBufferOffset_ += n;
        if (outBufferOffset_ >= totalSize)
        {
            outBuffer_.clear();
            outBufferOffset_ = 0;
            cgiOutput_.clear();
        }
        return static_cast<int>(n);
    }

    perror("send()");
    closeNow();
    return -1;
}
