#pragma once

#include <exception>
#include <string>

class HttpError : public std::exception {
    int status_;
    std::string location_; // optional: for redirects
public:
    explicit HttpError(int s) throw() : status_(s) {}
    HttpError(int s, const std::string& loc) : status_(s), location_(loc) {}

    ~HttpError() throw() {}

    int status() const throw() { return status_; }
    const std::string& location() const { return location_; }

    const char* what() const throw() { return "http error"; }
};
