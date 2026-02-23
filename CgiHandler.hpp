#pragma once

#include "Request.hpp"
#include <string>
#include <map>
#include <vector>
#include <unistd.h>
#include <cstdlib>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "HttpError.hpp"

class CgiHandler {
private:
    std::map<std::string, std::string> envMap_;
    char** envp_;
    
    int pipeIn_[2];  // Server -> CGI (stdin)
    int pipeOut_[2]; // CGI -> Server (stdout)
    pid_t cgiPid_;

    std::string scriptPath_;
    std::string workingDir_;
    std::string interpreterPath_;

    // Private helpers
    void initEnv(const Request& req);
    char** mapToEnvp();
    void freeEnvp();

public:
    // Constructor sets up the environment and paths
    CgiHandler(const Request& req, const std::string& scriptPath, std::string interpreter);
    ~CgiHandler();

    // The function that actually calls fork() and execve()
    void executeCgi(); 
    
    // Getters so your webserv.cpp can monitor these pipes with poll()
    int getReadFd() const { return pipeOut_[0]; }
    int getWriteFd() const { return pipeIn_[1]; }
    pid_t getPid() const { return cgiPid_; }
    void    clearPid() { cgiPid_ = -1; }
};