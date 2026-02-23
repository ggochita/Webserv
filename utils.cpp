#include "utils.hpp"
#include "Request.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <ctime>
#include <dirent.h>
#include <iostream>
#include <sys/socket.h>
#include <cerrno>

// === string utilities ===
std::string toLower(const std::string& key)
{
    std::string cpKey(key);

    for (std::string::size_type i = 0; i < cpKey.size(); ++i)
    {
        if (cpKey[i] >= 'A' && cpKey[i] <= 'Z')
            cpKey[i] += 32;
    }

    return cpKey;
}

std::string trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// === filesystem helpers ===
bool isDir(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return (false);
    return (S_ISDIR(st.st_mode));

}

bool isFile(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

bool isReadable(const std::string& path)
{
    return (access(path.c_str(), R_OK) == 0);
}

std::string resolveFullPath(const Location& loc, const Request& req)
{
    std::string remaining =req.getPath();
    const std::string& locationPrefix = loc.uri;
    const std::string& root = loc.root;

    if (remaining.find(locationPrefix) == 0)
        remaining.erase(0, locationPrefix.length());
    
    if (remaining.empty() || remaining[0] != '/')
        remaining = "/" + remaining;
    
    std::string full = root;
    if (!full.empty() && full[full.size() - 1] == '/')
        full.erase(full.size() - 1);
    
    full += remaining;

    if (isDir(full))
        return full;

    if (isFile(full))
    {
        if (!isReadable(full))
            throw HttpError(403);
        return full;
    }
    throw HttpError(404);
}

// === HTTP date helper ===
std::string httpDateNow()
{
    char buf[64];
    std::time_t t = std::time(0);
    struct tm gm;
    gmtime_r(&t, &gm);
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gm);
    return std::string(buf);
}

static const char* errorReason(int code)
{
    switch (code)
    {
        // 4xx http code -> Client side errors
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        // 5xx http code -> Server side error
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Error";
    }
}

static const char* redirectReason(int code)
{
    // 3xx http code -> redirections
    switch (code)
    {
        case 301: return "Moved Permanently";
        default:  return "Redirect";
    }
}

bool shouldKeepAlive(const Request& request, int status)
{
    const std::string version = request.getVersion();
    std::string connection = toLower(request.getValFromMap("connection"));

    if (status == 400 || status == 408 || status == 413 ||
        status == 414 || status == 431 || status >= 500 || status >= 501)
        return false;

    if (version == "HTTP/1.1")
    {
        if (connection == "close")
            return (false);
        return (true);
    }
    if (version == "HTTP/1.0")
        if (connection == "keep-alive")
            return (true);
    
    return false;
}

std::string buildError(int code, const Request& request, const ServerConfig* cfg)
{
    const std::string version = request.getVersion().empty() ? "HTTP/1.1" : request.getVersion();
    const bool keepAlive = shouldKeepAlive(request, code);
    const char* reason = errorReason(code);

    std::string body = "";
    bool foundRoot = false;
    if (cfg != NULL && cfg->error_pages.count(code) > 0)
    {
        std::cout << "[DEBUG] Found custom error page for code " << code << std::endl;
        std::string errRoot = ".";
        for (size_t i = 0; i < cfg->locations.size(); ++i)
        {
            if (cfg->locations[i].uri == "/")
            {
                if (!cfg->locations[i].root.empty())
                {
                    errRoot = cfg->locations[i].root;
                    foundRoot = true;
                }
                break;
            }
        }
        if (!foundRoot) std::cout << "[DEBUG] WARNING: Could not find 'location /' root. Defaulting to '.'" << std::endl;
        std::string errPath = errRoot;

        if (!errPath.empty() && errPath[errPath.size() - 1] == '/')
            errPath.erase(errPath.size() - 1);
        
        if (cfg->error_pages.at(code)[0] != '/')
            errPath += "/";
        errPath += cfg->error_pages.at(code);

        std::cout << "[DEBUG] Trying to open custom error file: [" << errPath << "]" << std::endl;

        std::ifstream f(errPath.c_str(), std::ios::in | std::ios::binary);
        if (f.is_open())
        {
            std::ostringstream ss;
            ss << f.rdbuf();
            body = ss.str();
            std::cout << "[DEBUG] Successfully read " << body.size() << " bytes." << std::endl;
        }
        else
            std::cerr << "[DEBUG] FAILED to open file. Check permissions or path?" << std::endl;
    }
    else
        std::cout << "[DEBUG] No custom error page config found for code " << code << std::endl;

    if (body.empty())
    {
        std::ostringstream bodyStream;
        bodyStream << "<!doctype html><title>" << code << " " << reason
            << "</title><h1>" << code << " " << reason << "</h1>";
        
        body = bodyStream.str();
    }

    
    std::ostringstream result;
    result << version << " " << code << " " << reason << "\r\n"
        << "Date: " << httpDateNow() << "\r\n"
        << "Server: webserv/0.1\r\n"
        << "Content-Type: text/html; charset=UTF-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
        << "\r\n"
        << body;
    
    return result.str();
}

std::string buildRedirect(const Request& request, int status, const std::string& location)
{
    const std::string version = request.getVersion().empty() ? "HTTP/1.1" : request.getVersion();
    const char* reason = redirectReason(status);
    const bool keepAlive = shouldKeepAlive(request, status);

    std::ostringstream bodyStream;
    bodyStream << "<!doctype html><title>" << status << ' ' << reason << "</title>"
         << "<h1>" << reason << "</h1>"
         << "<p>Resource moved to <a href=\"" << location << "\">" << location << "</a>.</p>";
    const std::string body = bodyStream.str();

    std::ostringstream out;
    out << version << ' ' << status << ' ' << reason << "\r\n"
        << "Date: " << httpDateNow() << "\r\n"
        << "Server: webserv/0.1\r\n"
        << "Location: " << location << "\r\n"
        << "Content-Type: text/html; charset=UTF-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
        << "\r\n"
        << body;
    return out.str();
}

std::string buildDirectoryListing(const Request& req, const std::string& fsPath)
{
    DIR* dir = opendir(fsPath.c_str());
    if (!dir) throw std::runtime_error("Could not open directory");

    std::string reqPath = req.getPath();
    std::string html = "<html><head><title>Index of " + reqPath + "</title></head><body>";
    html += "<h1>Index of " + reqPath + "</h1><hr><pre>";

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name = entry->d_name;
        if (name == ".")
            continue; // Skip current directory self-link

        // If it's a directory, append a slash for proper navigation
        std::string displayName = name;
        if (entry->d_type == DT_DIR)
            displayName += "/";

        html += "<a href=\"" + name + "\">" + displayName + "</a><br>";
    }
    html += "</pre><hr></body></html>";
    closedir(dir);

    bool keepAlive = shouldKeepAlive(req, 200);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/html; charset=UTF-8\r\n";
    response << "Content-Length: " << html.size() << "\r\n";
    response << "Date: " << httpDateNow() << "\r\n";
    response << "Server: webserv/0.1\r\n";
    response << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
    response << "\r\n"; // End of headers
    
    // 3. Combine
    response << html;

    return response.str();
}
