#include "Config.hpp"
#include "Request.hpp"
#include "Connection.hpp"
#include "Server.hpp"
#include "utils.hpp"
#include "CgiHandler.hpp"

#include <netinet/in.h>
#include <sys/socket.h> 
#include <signal.h>
#include <poll.h>
#include <map>
#include <vector>
#include <iostream>

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int){ g_running = 0; }




// Helper to add a file descriptor to the pollfd vector
void addToPoll(std::vector<struct pollfd>& fds, int fd, short events) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    fds.push_back(pfd);
}

void closeClient(int fd, std::map<int, Connection*>& conns, 
                 std::vector<struct pollfd>& fds, size_t& i,
                 std::map<int, const ServerConfig*>& cfg_map) 
{
    close(fd);
    
    // SAFETY: Check if it exists before deleting
    if (conns.find(fd) != conns.end())
    {
        delete conns[fd]; // Calls ~Connection()
        conns.erase(fd);
    }
    
    cfg_map.erase(fd);
    fds.erase(fds.begin() + i);
    i--; // Adjust iterator
}

// Returns a pointer to the best matching Location, or NULL if none found.
const Location* findBestLocation(const ServerConfig* srvCfg, const std::string& uri)
{
    const Location* bestMatch = NULL;
    size_t bestLength = 0;

    for (size_t i = 0; i < srvCfg->locations.size(); ++i)
    {
        const Location& loc = srvCfg->locations[i];
        
        // Check if the URI starts with this location's path (Prefix Match)
        if (uri.compare(0, loc.uri.length(), loc.uri) == 0)
        {    
            // We found a match! Is it better (longer) than the previous one?
            if (loc.uri.length() > bestLength)
            {
                bestMatch = &loc;
                bestLength = loc.uri.length();
            }
        }
    }
    return bestMatch;
}

std::string processCgiResponse(const std::string& rawOutput, const Request& req) {
    std::string headers;
    std::string body;
    
    std::string statusLine = "HTTP/1.1 200 OK\r\n"; 
    int statusInt = 200; // <--- NEW: Default integer status
    
    // 1. Separate Headers from Body
    size_t headerEnd = rawOutput.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        headers = rawOutput.substr(0, headerEnd + 2); 
        body = rawOutput.substr(headerEnd + 4);       
    } else {
        body = rawOutput;
    }

    // 2. Check for the "Status:" Header
    size_t statusPos = headers.find("Status:");
    if (statusPos != std::string::npos) {
        size_t endOfLine = headers.find("\r\n", statusPos);
        if (endOfLine != std::string::npos) {
            std::string statusCodeStr = headers.substr(statusPos + 7, endOfLine - (statusPos + 7));
            statusCodeStr = trim(statusCodeStr); // e.g., "404 Not Found"
            
            statusInt = std::atoi(statusCodeStr.c_str()); 
            
            statusLine = "HTTP/1.1 " + statusCodeStr + "\r\n";
            headers.erase(statusPos, endOfLine - statusPos + 2); 
        }
    }

    // 3. Build the Final Response
    std::ostringstream ss;
    ss << statusLine;
    ss << "Server: webserv/1.0\r\n";
    
    if (shouldKeepAlive(req, statusInt))
        ss << "Connection: keep-alive\r\n";
    else
        ss << "Connection: close\r\n";
    
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << headers;

    if (headers.find("Content-Type:") == std::string::npos)
        ss << "Content-Type: text/html\r\n";
    if (!headers.empty() && headers.compare(headers.size() - 2, 2, "\r\n") == 0)
        ss << "\r\n"; 
    else
        ss << "\r\n\r\n"; 

    ss << body;
    return ss.str();
}

// Returns FALSE if connection should be closed, TRUE to keep alive.
bool handleClient(Connection* conn, const ServerConfig* srvCfg, short revents, short& poll_events,
                  std::vector<struct pollfd>& fds,
                  std::map<int, Connection*>& cgi_read_map,
                  std::map<int, Connection*>& cgi_write_map)
{
    // 1. Handle Errors/Hangups immediately
    if (revents & (POLLHUP | POLLERR | POLLNVAL))
        return false;

    // 2. READ (Client sent data)
    if (revents & POLLIN)
    {
        try {
            int bytes = conn->readFromSocket();
            if (bytes == 0)
                return false; // Client closed

            // Check for end of headers
            size_t headerEnd = conn->in().find("\r\n\r\n");
            if (headerEnd != std::string::npos)
            {
                // A. Parse Headers
                conn->request().reset();
                conn->request().parseHeader(conn->in().c_str());

                // B. Check Config Limits 
                size_t contentLength = conn->request().getContentLength();
                if (contentLength > srvCfg->client_max_body_size)
                    throw Request::ParseError(413);

                // C. Check for Full Body
                size_t headerSize = headerEnd + 4;
                std::string finalBody;
                size_t bytesToConsume = 0;

                if (conn->request().isChunked())
                {
                    // 1. isolate body from the socket buffer
                    std::string rawBody = conn->in().substr(headerSize);
                    // 2. feed it to unchunker
                    bool isDone = conn->request().parseChunkedBody(rawBody, conn->request().getUnchunkedBody());
                    // 3. left overs goes back to socket buffer
                    conn->in() = conn->in().substr(0, headerSize) + rawBody;
                    if (!isDone)
                        return true; // body incomplete, wait for more data
                    // 4. finnal body
                    finalBody = conn->request().getUnchunkedBody();
                    bytesToConsume = headerSize;
                }
                else
                {
                    bytesToConsume = headerSize + contentLength;
                    size_t totalRequestSize = headerSize + contentLength;
                    if (conn->in().size() < totalRequestSize)
                        return true; // body incomplete
                    
                    finalBody = conn->in().substr(headerSize, contentLength);
                    conn->request().setUnchunkedBody(finalBody);
                }
                // =============================================================
                // D. ROUTING LOGIC
                // =============================================================
                
                // 1. Find Best Location
                const Location* loc = findBestLocation(srvCfg, conn->request().getPath());
                if (!loc)
                    throw HttpError(404);

                // 2. Check HTTP Method
                bool methodAllowed = false;
                if (loc->methods.empty())
                    methodAllowed = (conn->request().getMethod() == "GET");
                else
                {
                    for (size_t k = 0; k < loc->methods.size(); ++k)
                    {
                        if (loc->methods[k] == conn->request().getMethod())
                        {
                            methodAllowed = true;
                            break;
                        }
                    }
                }
                if (!methodAllowed)
                    throw HttpError(405); // Method Not Allowed

                // 3. Handle Redirection
                if (!loc->return_url.empty())
                    throw HttpError(loc->return_code, loc->return_url);

                // 4. Resolve Path
                std::string root = loc->root.empty() ? "/tmp/www" : loc->root;
                std::string fsPath;

                bool isCgiRequest = false;
                if (!loc->cgi_extension.empty())
                {
                    std::string path = conn->request().getPath();
                    std::string ext = loc->cgi_extension;
                    if (path.length() >= ext.length() && 
                        path.substr(path.length() - ext.length()) == ext)
                        isCgiRequest = true;
                }

                // manually constructing path without checking existence of file/dir for POST
                if (conn->request().getMethod() == "POST" && !isCgiRequest)
                {
                    fsPath = root;
                    if (!fsPath.empty() && fsPath[fsPath.size() - 1] == '/')
                        fsPath.erase(fsPath.size() - 1);
                    fsPath += conn->request().getPath();    
                }
                else
                    fsPath = resolveFullPath(*loc, conn->request());

                // =============================================================
                // [NEW] CGI INTERCEPTION
                // =============================================================
                if (loc && !loc->cgi_extension.empty())
                {
                    std::string ext = loc->cgi_extension;
                    if (fsPath.length() >= ext.length() && fsPath.substr(fsPath.length() - ext.length()) == ext) 
                    {
                        if (!isFile(fsPath)) throw HttpError(404);
                        std::cout << "\n[DEBUG] Path requested: " << conn->request().getPath() << std::endl;
                        std::cout << "[DEBUG] Query string:   " << conn->request().getQuery() << std::endl;
                        std::cout << "[DEBUG] Target file:    " << fsPath << std::endl;
                        if (access(fsPath.c_str(), R_OK) != 0) throw HttpError(403);

                        // 1. Create and execute the CGI process
                        conn->setCgiHandler(new CgiHandler(conn->request(), fsPath, loc->cgi_path));
                        conn->getCgiHandler()->executeCgi();

                        // 2. Get the new pipe File Descriptors
                        int readFd = conn->getCgiHandler()->getReadFd();
                        int writeFd = conn->getCgiHandler()->getWriteFd();

                        // 3. Add them to the main poll() array
                        addToPoll(fds, readFd, POLLIN);
                        addToPoll(fds, writeFd, POLLOUT);

                        // 4. Map them to THIS client connection
                        cgi_read_map[readFd] = conn;
                        cgi_write_map[writeFd] = conn;

                        // 5. Tell the server to consume the headers from the input buffer
                        conn->in().erase(0, bytesToConsume);

                        // 6. Keep connection alive while CGI runs!
                        return true; 
                    }
                }
                if (conn->request().getMethod() == "GET" || conn->request().getMethod() == "HEAD")
                {
                    // 5. Handle Directory
                    if (isDir(fsPath)) 
                    {
                        // 1. Redirect if trailing slash is missing (e.g. "/images" -> "/images/")
                        // This is required so relative links in HTML work correctly.
                        std::string path = conn->request().getPath();
                        if (path.empty() || path[path.length() - 1] != '/')
                        {
                            std::string newLoc = conn->request().getPath() + "/";
                            if (!conn->request().getQuery().empty()) 
                                newLoc += "?" + conn->request().getQuery();
                            throw HttpError(301, newLoc);
                        }

                        // 2. Try Index File
                        if (!loc->index.empty())
                        {
                            std::string indexPath = fsPath + (fsPath[fsPath.length() - 1] == '/' ? "" : "/") + loc->index;
                            if (isFile(indexPath))
                            {
                                fsPath = indexPath; 
                                goto serve_file; 
                            }
                        }

                        // 3. Try Autoindex
                        if (loc->autoindex)
                        {
                            conn->out() = buildDirectoryListing(conn->request(), fsPath);
                            conn->in().erase(0, bytesToConsume);

                            poll_events = POLLIN | POLLOUT;
                            return true; 
                        }

                        // 4. If neither worked -> Forbidden
                        throw HttpError(403);
                    }

                    serve_file:
                    // E. Build Response
                    conn->out() = buildSuccess(conn->request(), fsPath, srvCfg);
                }
                else if (conn->request().getMethod() == "DELETE")
                {
                    // 1. check if it exists
                    if (!isFile(fsPath))
                        throw HttpError(404);
                    // 2. check permissions (write access needed to delete)
                    if (access(fsPath.c_str(), W_OK) != 0)
                        throw HttpError(403);
                    // 3. delete
                    if (std::remove(fsPath.c_str()) != 0)
                        throw HttpError(500); // interal error code
                    // 4. send 204 No content
                    bool keepAlive = shouldKeepAlive(conn->request(), 204);
                    std::ostringstream response;

                    response    <<  "HTTP/1.1 204 No Content\r\n"
                                <<  "Server: webserv/0.1\r\n"
                                <<  "Date: " << httpDateNow() << "\r\n"
                                <<  "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
                                <<  "\r\n";
                    conn->out() = response.str();  
                }
                else if (conn->request().getMethod() == "POST")
                {
                    // 1. Check if upload store is configured
                    if (loc->upload_store.empty())
                    {
                        std::cerr << "Error: upload store not set for this location" << std::endl;
                        throw HttpError(500);
                    }

                    // 2. Determine filename from URI
                    // Example: POST /uploads/cat.png -> filename = "cat.png"
                    std::string uri = conn->request().getPath();
                    std::string filename = uri.substr(uri.find_last_of('/') + 1);

                    if (filename.empty())
                        throw HttpError(400); // cannot upload to a directory root
                    
                    // 3. construct full path
                    std::string outPath = loc->upload_store;
                    // ensure traling slash on directory
                    if (!outPath.empty() && outPath[outPath.size() - 1] != '/')
                        outPath += "/";
                    outPath += filename;

                    // 4. write to file (binary mode is for images)
                    std::ofstream outFile(outPath.c_str(), std::ios::out | std::ios::binary);
                    if (!outFile.is_open())
                    {
                        std::cerr << "Error: could not open " << outPath << " for writing" << std::endl;
                        throw HttpError(500);
                    }
                    // write the body data
                    outFile.write(finalBody.c_str(), finalBody.size());
                    outFile.close();

                    if (outFile.fail())
                        throw HttpError(500);

                    // 5. send 201 Created response
                    bool keepAlive = shouldKeepAlive(conn->request(), 201);

                    std::ostringstream response;
                    response << "HTTP/1.1 201 Created\r\n"
                             << "Server: webserv/0.1\r\n"
                             << "Date: " << httpDateNow() << "\r\n"
                             << "Content-Length: 0\r\n"
                             << "Location: " << conn->request().getPath() << "\r\n"
                             << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
                             << "\r\n";

                    conn->out() = response.str();
                }
                // F. Consume Input
                conn->in().erase(0, bytesToConsume);

            }
        }
        catch (const Request::ParseError& e)
        {
            conn->out() = buildError(e.status(), conn->request(), srvCfg);
            conn->in().clear();
        }
        catch (const HttpError& he)
        {
            if (he.status() >= 300 && he.status() < 400)
                conn->out() = buildRedirect(conn->request(), he.status(), he.location());
            else
                conn->out() = buildError(he.status(), conn->request(), srvCfg);
            conn->in().clear();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Request Error: " << e.what() << std::endl;
            conn->out() = buildError(500, conn->request(), srvCfg);
            conn->in().clear();
        }
    }

    // 3. WRITE (Ready to send response)
    if (revents & POLLOUT)
    {
        int bytes = conn->writeToSocket();
        if (bytes < 0)
            return false;

        if (conn->out().empty())
        {
            if (!shouldKeepAlive(conn->request(), 200))
                return false;
        }
    }

    // 4. Update Poll Flags
    if (!conn->out().empty())
        poll_events = POLLIN | POLLOUT;
    else
        poll_events = POLLIN;

    return true;
}


int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: ./webserv [config_file]" << std::endl;
        return 1;
    }

    signal(SIGINT, on_sigint);

    try {
        // ====================================================================
        // 1. CONFIGURATION PHASE
        // ====================================================================
        Config config;
        config.parse(argv[1]);

        // ====================================================================
        // 2. SERVER SETUP PHASE
        // ====================================================================
        std::vector<Server*> servers;
        std::vector<struct pollfd> poll_fds;

        // Map to quickly identify if an FD belongs to a Listening Server
        // Key: FD, Value: Pointer to Server object
        std::map<int, Server*> listener_map;

        // Loop 1: Create and Bind
        for (size_t i = 0; i < config.servers.size(); ++i)
        {
            Server* srv = new Server(config.servers[i]);
            
            if (!srv->bindAndListen())
            {
                std::cerr << "Error: Failed to bind port " << config.servers[i].port 
                          << ". Skipping." << std::endl;
                delete srv;
                continue; 
            }
            servers.push_back(srv);
        }

        if (servers.empty())
        {
            std::cerr << "No servers could be started." << std::endl;
            return 1;
        }

        // Initialize Poll and Listener Map
        for (size_t i = 0; i < servers.size(); ++i)
        {
            addToPoll(poll_fds, servers[i]->getFd(), POLLIN);
            listener_map[servers[i]->getFd()] = servers[i];
            
            std::cout << "Server listening on " << servers[i]->getConfig().host 
                      << ":" << servers[i]->getConfig().port << std::endl;
        }

        // ====================================================================
        // 3. MAIN EVENT LOOP
        // ====================================================================
        // Key: Client FD, Value: Connection Object (POINTER)
        std::map<int, Connection*> connections;
        // Allows us to know which Config rules apply to a specific Client FD
        std::map<int, const ServerConfig*> client_to_config;

        // [NEW] CGI Maps
        std::map<int, Connection*> cgi_read_map;
        std::map<int, Connection*> cgi_write_map;

        std::cout << "Server is running..." << std::endl;

        while (g_running)
        {
            // A. Wait for events (1000ms timeout)
            int ret = poll(poll_fds.data(), poll_fds.size(), 1000);
            
            if (ret < 0)
            {
                if (errno == EINTR)
                    continue; // Signal received, just loop back
                perror("poll");
                break; // Real error, exit
            }
            if (ret == 0)
                continue; // Timeout, loop back

            // B. Iterate through events
            
            for (size_t i = 0; i < poll_fds.size(); ++i)
            {
                int fd = poll_fds[i].fd;
                short revents = poll_fds[i].revents;

                if (revents == 0)
                    continue;

                // ---------------------------------------------------------
                // CASE 1: NEW CONNECTION (Listener)
                // ---------------------------------------------------------
                if (listener_map.count(fd))
                {
                    if (revents & POLLIN)
                    {
                        int client_fd = listener_map[fd]->acceptClient();
                        if (client_fd >= 0)
                        {
                            // Create new connection object (Allocate on HEAP)
                            Connection* new_conn = new Connection(client_fd);
                            connections[client_fd] = new_conn;
                            
                            // Map the client to the server config that accepted it
                            client_to_config[client_fd] = &listener_map[fd]->getConfig();

                            // Only listen for POLLIN initially!
                            addToPoll(poll_fds, client_fd, POLLIN);
                            
                            std::cout << "New Client Connected: " << client_fd << std::endl;
                        }
                    }
                }

                // ---------------------------------------------------------
                // CASE 2: CGI SCRIPT HAS OUTPUT TO READ
                // ---------------------------------------------------------
                else if (cgi_read_map.count(fd))
                {
                    Connection *conn = cgi_read_map[fd];
                    if ((revents & POLLIN) || (revents & POLLHUP))
                    {
                        char buffer[4096];
                        ssize_t bytes = read(fd, buffer, sizeof(buffer));

                        if (bytes > 0){
                            std::cout << "[DEBUG] Read " << bytes << " bytes from Python script." << std::endl;
                            conn->getCgiOutput().append(buffer, bytes);}
                        else if (bytes == 0) // EOF
                        {
                            std::cout << "[DEBUG] Python script finished! Processing response..." << std::endl;
                            // 1. wait for child logic
                            int status;
                            waitpid(conn->getCgiHandler()->getPid(), &status, 0);
                            conn->getCgiHandler()->clearPid();
                            // 2. process response
                            conn->out() = processCgiResponse(conn->getCgiOutput(), conn->request());
                            // 3. cleanup
                            std::cout << "[DEBUG] Final HTTP Response generated. Length: " << conn->out().length() << std::endl;
                            delete conn->getCgiHandler();
                            conn->setCgiHandler(NULL);
                            // 4. close pipe
                            close(fd);
                            cgi_read_map.erase(fd);
                            poll_fds.erase(poll_fds.begin() + i);
                            i--;
                            // 5. switch client back to write mode
                            for (size_t k = 0; k < poll_fds.size(); ++k)
                            {
                                if (poll_fds[k].fd == conn->fd())
                                {
                                    std::cout << "[DEBUG] Switching Client FD " << conn->fd() << " to POLLOUT!" << std::endl;
                                    poll_fds[k].events = POLLIN | POLLOUT;
                                    break;
                                }
                            }
                        }
                    }
                    else if (revents & (POLLERR | POLLHUP))
                    {
                        // Handle crash/hangup
                        close(fd);
                        cgi_read_map.erase(fd);
                        poll_fds.erase(poll_fds.begin() + i);
                        i--;
                    }
                }

                // ---------------------------------------------------------
                // CASE 3: CGI SCRIPT IS READY TO RECEIVE POST DATA
                // ---------------------------------------------------------
                else if (cgi_write_map.count(fd))
                {
                    if (revents & POLLOUT)
                    {
                        Connection *conn = cgi_write_map[fd];

                        // 1. Get the data
                        const std::string& body = conn->request().getUnchunkedBody();
                        size_t bytesToSend = body.size();
                        size_t bytesAlreadySent = conn->getCgiBytesWritten();

                        // 2, write whats left
                        size_t remaining = bytesToSend - bytesAlreadySent;
                        if (remaining > 0)
                        {
                            size_t written = write(fd, body.c_str() + bytesAlreadySent, remaining);
                            if (written > 0)
                                conn->addCgiBytesWritten(written);
                        }
                        // 3. Close if done
                        if (conn->getCgiBytesWritten() >= bytesToSend)
                        {
                            close(fd);
                            cgi_write_map.erase(fd);
                            poll_fds.erase(poll_fds.begin() + i);
                            i--;
                        }
                    }
                    // handle Errors
                    else if (revents & (POLLERR | POLLHUP))
                    {
                        close(fd);
                        cgi_write_map.erase(fd);
                        poll_fds.erase(poll_fds.begin() + i);
                        i--;
                    }
                }

                // ---------------------------------------------------------
                // CASE 4: EXISTING CLIENT
                // ---------------------------------------------------------
                else
                {
                    // If FD is not in connections map, it's a ghost. Safety check.
                    if (connections.find(fd) == connections.end())
                    {
                        closeClient(fd, connections, poll_fds, i, client_to_config);
                        continue;
                    }

                    // Delegate all logic to the handler
                    // We pass the poll_events by reference so the handler can toggle POLLOUT
                    bool keepAlive = handleClient(
                        connections[fd], 
                        client_to_config[fd], 
                        revents, 
                        poll_fds[i].events,
                        poll_fds,
                        cgi_read_map,
                        cgi_write_map
                    );

                    if (!keepAlive)
                        closeClient(fd, connections, poll_fds, i, client_to_config);
                }
            }
        }
        
        // CLEANUP AT EXIT
        for (std::map<int, Connection*>::iterator it = connections.begin(); it != connections.end(); ++it)
        {
            close(it->first);
            delete it->second;
        }
        for (size_t i = 0; i < servers.size(); ++i)
        {
            delete servers[i];
        }

    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\nServer shutting down..." << std::endl;
    return 0;
}