#pragma once
#include "Request.hpp"
#include "Config.hpp"


class Server {
    ServerConfig    config_;
    int              listen_fd_;

public:
    Server(const ServerConfig& config);
    ~Server();

    bool bindAndListen();     // calls create_listen_socket
    int  acceptClient();        // returns new client fd or -1
    
    int  getFd() const { return listen_fd_; }
    const ServerConfig& getConfig() const { return config_; }

};