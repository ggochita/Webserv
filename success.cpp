#include "Request.hpp"
#include "utils.hpp"

#include <ctime>

static std::string toLowerStr(std::string s) {
    for (std::string::size_type i = 0; i < s.size(); ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = char(s[i] + 32);
    }
    return s;
}

static std::string fileExt(const std::string& path) {
    std::string::size_type dot = path.rfind('.');
    if (dot == std::string::npos) return std::string();
    return toLowerStr(path.substr(dot + 1));
}

static const char* mimeFromExt(const std::string& ext)
{
    if (ext == "html" || ext == "htm") return "text/html; charset=UTF-8";
    if (ext == "css")  return "text/css";
    if (ext == "js")   return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "xml")  return "application/xml";
    if (ext == "txt")  return "text/plain; charset=UTF-8";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "pdf")  return "application/pdf";
    return "application/octet-stream";
}

std::string buildSuccess(const Request& req,const std::string& fsPath, const ServerConfig* cfg)
{
    const std::string version = req.getVersion().empty() ? "HTTP/1.1" : req.getVersion();

    // Read file (binary)
    std::ifstream f(fsPath.c_str(), std::ios::in | std::ios::binary);
    if (!f)
        return buildError(404, req, cfg);

    std::ostringstream bodyBuf;
    bodyBuf << f.rdbuf();
    const std::string body = bodyBuf.str();

    // Content-Type from extension
    const std::string ext = fileExt(fsPath);
    const char* contentType = mimeFromExt(ext);

    const bool keepAlive = shouldKeepAlive(req, 200);

    // Build headers
    std::ostringstream out;
    out << version << " 200 OK\r\n"
        << "Date: " << httpDateNow() << "\r\n"
        << "Server: webserv/0.1\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
        << "\r\n";

    // HEAD: headers only; GET: headers + body
    if (req.getMethod() == "HEAD") {
        return out.str();
    }
    return out.str() + body;
}