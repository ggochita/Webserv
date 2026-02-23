#include "CgiHandler.hpp"
#include <sstream>
#include <iostream>

CgiHandler::CgiHandler(const Request& req, const std::string& scriptPath, std::string interpreter) 
    : envp_(NULL), cgiPid_(-1), scriptPath_(scriptPath), interpreterPath_(interpreter)
{
    pipeIn_[0] = -1; pipeIn_[1] = -1;
    pipeOut_[0] = -1; pipeOut_[1] = -1;

    // The subject says the CGI should be run in the correct directory 
    // So we extract the directory path from the script path.
    size_t lastSlash = scriptPath_.find_last_of('/');
    if (lastSlash != std::string::npos)
        workingDir_ = scriptPath_.substr(0, lastSlash);
    else
        workingDir_ = ".";

    initEnv(req);
    envp_ = mapToEnvp();
}

CgiHandler::~CgiHandler()
{
    freeEnvp();

    if (cgiPid_ > 0)
    {
        kill(cgiPid_, SIGKILL);
        waitpid(cgiPid_, NULL, 0);
    }

    if (pipeIn_[0] != -1) close(pipeIn_[0]);
    if (pipeIn_[1] != -1) close(pipeIn_[1]);
    if (pipeOut_[0]!= -1) close(pipeOut_[0]);
    if (pipeOut_[1]!= -1) close(pipeOut_[1]);
}

void CgiHandler::initEnv(const Request& req)
{
    // 1. Mandatory CGI 1.1 Variables
    envMap_["GATEWAY_INTERFACE"] = "CGI/1.1";
    envMap_["SERVER_PROTOCOL"]   = req.getVersion();
    envMap_["SERVER_SOFTWARE"]   = "webserv/0.1";
    envMap_["REQUEST_METHOD"]    = req.getMethod();
    envMap_["QUERY_STRING"]      = req.getQuery();
    
    // PHP-CGI specifically requires SCRIPT_FILENAME to locate the file on disk
    envMap_["SCRIPT_FILENAME"]   = scriptPath_; 
    
    // PHP-CGI requires REDIRECT_STATUS=200 to execute properly (security feature)
    envMap_["REDIRECT_STATUS"]   = "200";

    // 2. Handle the Body Size (Works dynamically for both Normal and Chunked!)
    // If there is a body, we use the size of our fully decoded unchunkedBody_
    // because that represents the TRUE size of the data we will pipe to the CGI.
    if (req.getMethod() == "POST" || req.getMethod() == "PUT")
    {
        std::stringstream ss;
        // Assuming you added getUnchunkedBody() earlier!
        // If it's a normal request, you stored the body there too.
        ss << req.getUnchunkedBody().size(); 
        envMap_["CONTENT_LENGTH"] = ss.str();
        
        std::string contentType = req.getValFromMap("content-type");
        if (!contentType.empty())
            envMap_["CONTENT_TYPE"] = contentType;
    }

    // 3. Dynamically map ALL client headers to HTTP_ variables
    const std::map<std::string, std::string>& headers = req.getHeaders();
    for (std::map<std::string, std::string>::const_iterator it = headers.begin(); it != headers.end(); ++it)
    {
        std::string key = it->first; // e.g., "user-agent"
        
        // Convert to UPPERCASE and replace '-' with '_'
        for (size_t i = 0; i < key.length(); ++i)
        {
            if (key[i] == '-') 
                key[i] = '_';
            else 
                key[i] = std::toupper(key[i]);
        }
        
        // CONTENT_LENGTH and CONTENT_TYPE are special cases that don't get the HTTP_ prefix
        if (key != "CONTENT_TYPE" && key != "CONTENT_LENGTH")
            envMap_["HTTP_" + key] = it->second; // e.g., "HTTP_USER_AGENT"
    }
}

char** CgiHandler::mapToEnvp()
{
    char** env = new char*[envMap_.size() + 1];
    int i = 0;
    for (std::map<std::string, std::string>::iterator it = envMap_.begin(); it != envMap_.end(); ++it)
    {
        std::string envStr = it->first + "=" + it->second;
        env[i] = new char[envStr.size() + 1];
        std::strcpy(env[i], envStr.c_str());
        i++;
    }
    env[i] = NULL;
    return env;
}

void CgiHandler::freeEnvp()
{
    if (envp_)
    {
        for (int i = 0; envp_[i] != NULL; ++i)
            delete[] envp_[i];
        delete[] envp_;
        envp_ = NULL;
    }
}

void CgiHandler::executeCgi()
{
    // 1. Create the two pipes
    // pipeIn: Server writes to [1], CGI reads from [0]
    if (pipe(pipeIn_) < 0)
        throw HttpError(500); // Internal Server Error
    
    // pipeOut: CGI writes to [1], Server reads from [0]
    if (pipe(pipeOut_) < 0)
    {
        close(pipeIn_[0]);
        close(pipeIn_[1]);
        throw HttpError(500);
    }

    // 2. Fork the process
    cgiPid_ = fork();
    if (cgiPid_ < 0)
    {
        close(pipeIn_[0]); close(pipeIn_[1]);
        close(pipeOut_[0]); close(pipeOut_[1]);
        throw HttpError(500);
    }

    // ==========================================================
    // 3. CHILD PROCESS (The CGI Script)
    // ==========================================================
    if (cgiPid_ == 0)
    {
        // A. Close the pipe ends the child doesn't need
        close(pipeIn_[1]);  // Child doesn't write to its own stdin
        close(pipeOut_[0]); // Child doesn't read its own stdout

        // B. Redirect Standard Input & Output using dup2
        dup2(pipeIn_[0], STDIN_FILENO);
        dup2(pipeOut_[1], STDOUT_FILENO);

        // C. Close the original file descriptors (they are duplicated now)
        close(pipeIn_[0]);
        close(pipeOut_[1]);

        // D. Change working directory (Subject requirement for relative paths)
        if (chdir(workingDir_.c_str()) != 0)
        {
            std::cerr << "CGI Error: chdir failed for " << workingDir_ << std::endl;
            exit(1); 
        }

        // E. Setup argv for execve
        // Conventionally, argv[0] is the path to the program itself.
        char* argv[3];
        argv[0] = const_cast<char*>(interpreterPath_.c_str());
        argv[1] = const_cast<char*>(scriptPath_.c_str());
        argv[2] = NULL;

        // F. Execute the script!
        // If this succeeds, the child process is replaced by the script.
        execve(interpreterPath_.c_str(), argv, envp_);

        // G. If execve returns, it means it FAILED (e.g., file not found/permissions)
        std::cerr << "CGI Error: execve failed for " << scriptPath_ << std::endl;
        exit(1); 
    }
    
    // ==========================================================
    // 4. PARENT PROCESS (The Web Server)
    // ==========================================================
    else
    {
        // A. Close the pipe ends the server doesn't need
        close(pipeIn_[0]);  // Server doesn't read from CGI's stdin
        close(pipeOut_[1]); // Server doesn't write to CGI's stdout

        // B. Make the Server's ends NON-BLOCKING
        // If the CGI script hangs, or if the client
        // sends a massive body, standard read/write would freeze entire server.
        fcntl(pipeIn_[1], F_SETFL, O_NONBLOCK);
        fcntl(pipeOut_[0], F_SETFL, O_NONBLOCK);
    }
}