#pragma once

#include <string>
#include <vector>
#include <map>
#include <iostream>

/*
    Location (Route) Block
    ----------------------
    Represents specific rules for a path (e.g., "location /images { ... }").
    Source: IV.3 Configuration file [cite: 160]
*/
struct Location {
    std::string                 uri;             // The path match (e.g., "/kapouet")
    
    // 1. Root Directory [cite: 163]
    // "Directory where the requested file should be located"
    std::string                 root;            

    // 2. Default File [cite: 165]
    // "Default file to serve when the requested resource is a directory"
    std::string                 index;           

    // 3. Directory Listing [cite: 164]
    // "Enabling or disabling directory listing"
    bool                        autoindex;       

    // 4. HTTP Methods [cite: 161]
    // "List of accepted HTTP methods for the route" (GET, POST, DELETE)
    std::vector<std::string>    methods;         

    // 5. Redirection [cite: 162]
    // "HTTP redirection" (e.g., return 301 https://google.com)
    std::string                 return_url;
    int                         return_code;     

    // 6. File Uploads [cite: 166]
    // "Uploading files... and storage location is provided"
    std::string                 upload_store;    

    // 7. CGI [cite: 170]
    // "Execution of CGI, based on file extension". 
    // Kept simple: if this location handles CGI, which ext and which path?
    std::string                 cgi_extension;   // e.g., ".php"
    std::string                 cgi_path;        // e.g., "/usr/bin/php-cgi"

    // Constructor to set safe defaults
    Location() : autoindex(false), return_code(0) {} 
};

/*
    Server Block
    ------------
    Represents a virtual server listening on a specific IP:Port.
    Source: IV.3 Configuration file [cite: 155]
*/
struct ServerConfig {
    // 1. Port and Host [cite: 157]
    // "Define all the interface:port pairs on which your server will listen"
    int                         port;            
    std::string                 host;            // e.g., "127.0.0.1"
    
    // 2. Server Name [cite: 180]
    // "a server name... if you plan to implement virtual hosts".
    // Even if VHOSTs are out of scope, parsing this is standard NGINX behavior.
    std::string                 server_name;     

    // 3. Error Pages [cite: 158]
    // "Set up default error pages" (e.g., 404 -> /404.html)
    std::map<int, std::string>  error_pages;     

    // 4. Body Size Limit [cite: 159]
    // "Set the maximum allowed size for client request bodies"
    size_t                      client_max_body_size; 

    // 5. Routes [cite: 160]
    // "Specify rules or configurations on a URL/route"
    std::vector<Location>       locations;       

    // Constructor with subject-compliant defaults
    ServerConfig() : port(80), host("127.0.0.1"), client_max_body_size(1000000) {}
};

/*
    Main Config Wrapper
    -------------------
    Just holds the list of servers found in the file.
*/
class Config {
public:
    // We use a vector because the subject implies multiple servers:
    // "Your server must be able to listen to multiple ports" [cite: 138]
    std::vector<ServerConfig> servers;

    Config();
    ~Config();

    // The only complex function you need: parse the file and fill the vector.
    void    parse(const std::string& filename);
};
