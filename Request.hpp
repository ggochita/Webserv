#pragma once

#include <sys/stat.h>
#include <vector>
#include <map>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <exception>
#include <netinet/in.h>
#include "HttpError.hpp"
#include "utils.hpp"


struct HeaderLimits
{
    size_t max_line;
    size_t max_fields;
    size_t max_total;
    size_t max_body;
    size_t max_uri; 
    HeaderLimits() : max_line(4096), max_fields(100),
     max_total(32768), max_body(1048576), max_uri(2048) {}
};


class Request
{
private:
    std::string method_;
    std::string path_;
    std::string query_;
    std::string version_;
    std::map<std::string, std::string> header_;

    bool        isChunked_;
    bool        chunkedCompleted_;
    size_t      currentChunkSize_;
    int         chunkState_;         // 0= ReadSize, 1=ReadData, 2=ReadCRLF
    std::string unchunkedBody_;

    void    HeaderDuplicateCheckAndAppend(const std::string &key, const std::string &value);

public:
    Request();
    ~Request();
    void    reset();

    int     parseMethod();
    int     parsePath();
    int     parseVersion();
    int     validateHeaders(const HeaderLimits& lim);
    
    const std::string& getPath(void) const;
    const std::string& getMethod(void) const;
    const std::string& getVersion(void) const;
    const std::string& getQuery(void) const;
    const std::map<std::string, std::string>& getHeaders() const;

    std::string getValFromMap(std::string key) const;
    std::string trim(std::string str);
    
    void    parseHeader(const char *buffer);

    size_t  getContentLength() const;

    bool    isChunked() const;
    bool    parseChunkedBody(std::string& rawData, std::string& decodedBody);
    bool    isChunkedCompleted() const;
    std::string& getUnchunkedBody();
    const std::string& getUnchunkedBody() const;
    void setUnchunkedBody(const std::string& b);

    class ParseError : public HttpError
    {
        public:
            explicit ParseError(int status);
            ~ParseError() throw();
    };
};
