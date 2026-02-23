#include "Config.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib> // for atoi, strtoul
#include <algorithm> // for std::find
#include <stdexcept>

// =============================================================================
// Helper: Tokenizer
// =============================================================================
// Reads the file and splits it into "tokens" (words).
// Handles comments (#) by ignoring the rest of the line.
// Treats semi-colons (;) and braces ({ }) as separate tokens for easier parsing.
static std::vector<std::string> tokenize(const std::string& filename)
{
    std::ifstream file(filename.c_str());
    if (!file.is_open())
        throw std::runtime_error("Config: Could not open file " + filename);

    std::vector<std::string> tokens;
    std::string line;
    
    while (std::getline(file, line))
    {
        // 1. Remove comments
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos)
            line = line.substr(0, commentPos);

        // 2. Replace structural characters with surrounded spaces
        // This ensures "server{" becomes "server {" and "listen 80;" becomes "listen 80 ;"
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '{' || line[i] == '}' || line[i] == ';') {
                std::string s(1, line[i]);
                line.replace(i, 1, " " + s + " ");
                i++; // skip the char we just added
            }
        }

        // 3. Split by whitespace
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

// =============================================================================
// Helper: Stream Conversions (C++98)
// =============================================================================
static void checkValue(const std::string& token, const std::string& directive) {
    if (token == "{" || token == "}" || token == ";") {
        throw std::runtime_error("Config: Unexpected structural token '" + token + "' where a value was expected for '" + directive + "'");
    }
}

static int toInt(const std::string& str) {
    for (size_t i = 0; i < str.size(); ++i) {
        if (!std::isdigit(str[i]))
            throw std::runtime_error("Config: Invalid integer value: " + str);
    }
    return std::atoi(str.c_str());
}

static size_t toSizeT(const std::string& str) {
    if (str.empty() || str[0] == '-')
        throw std::runtime_error("Config: Invalid size value: " + str);
    for (size_t i = 0; i < str.size(); ++i) {
        if (!std::isdigit(str[i]))
            throw std::runtime_error("Config: Invalid size value: " + str);
    }
    return static_cast<size_t>(std::strtoul(str.c_str(), NULL, 10));
}

static bool isValidMethod(const std::string& m) {
    // Based on your subject requirements + HEAD support
    return (m == "GET" || m == "POST" || m == "DELETE" || m == "HEAD");
}

static bool isValidStatusCode(int code) {
    // Standard HTTP status codes are between 100 and 599
    return (code >= 100 && code <= 599);
}

// =============================================================================
// Config Class Implementation
// =============================================================================

Config::Config() {}

Config::~Config() {}

/*
    The Main Parsing Loop
    ---------------------
    This function uses a simple state machine logic:
    1. We iterate through the list of tokens.
    2. We check "Where are we?" (Global scope? Inside Server? Inside Location?).
    3. We consume tokens to fill the structs.
*/
void Config::parse(const std::string& filename)
{
    std::vector<std::string> tokens = tokenize(filename);
    
    // Iterators to walk through the tokens
    size_t i = 0;
    
    // We will fill this temporarily and push it to servers vector when done
    bool inServer = false;
    bool inLocation = false;
    ServerConfig currentServer;
    Location currentLocation;

    while (i < tokens.size())
    {
        const std::string& token = tokens[i];

        // -----------------------------------------------------------
        // 1. GLOBAL SCOPE (Looking for "server")
        // -----------------------------------------------------------
        if (!inServer)
        {
            if (token == "server") {
                // Expect '{' next
                if (i + 1 >= tokens.size() || tokens[i+1] != "{")
                    throw std::runtime_error("Config: Expected '{' after server");
                
                inServer = true;
                currentServer = ServerConfig(); // Reset to defaults
                i += 2; // Skip "server" and "{"
            } 
            else {
                throw std::runtime_error("Config: Unexpected token in global scope: " + token);
            }
        }
        // -----------------------------------------------------------
        // 2. SERVER SCOPE (Inside server { ... })
        // -----------------------------------------------------------
        else if (inServer && !inLocation)
        {
            if (token == "}") {
                // End of server block
                // VALIDATION: Check mandatory fields
                if (currentServer.port == 0)
                    throw std::runtime_error("Config: Server block missing 'listen' port");
                
                // Check for duplicate host:port (Virtual Hosting check)
                for (size_t j = 0; j < servers.size(); ++j) {
                    if (servers[j].port == currentServer.port && servers[j].host == currentServer.host)
                        throw std::runtime_error("Config: Duplicate host:port detected. Virtual hosting is not supported.");
                }

                servers.push_back(currentServer);
                inServer = false;
                i++;
            }
            else if (token == "listen") {
                // Syntax: listen 8080;
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid listen syntax");
                checkValue(tokens[i+1], "listen");
                currentServer.port = toInt(tokens[i+1]);
                i += 3;
            }
            else if (token == "host") {
                // Syntax: host 127.0.0.1;
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid host syntax");
                checkValue(tokens[i+1], "host");
                currentServer.host = tokens[i+1];
                i += 3;
            }
            else if (token == "server_name") {
                // Syntax: server_name example.com;
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid server_name syntax");
                checkValue(tokens[i+1], "server_name");
                currentServer.server_name = tokens[i+1];
                i += 3;
            }
            else if (token == "error_page") {
                // Syntax: error_page 404 /404.html;
                if (i + 3 >= tokens.size() || tokens[i+3] != ";")
                    throw std::runtime_error("Config: Invalid error_page syntax");
                checkValue(tokens[i+1], "error_page");
                checkValue(tokens[i+2], "error_page");
                int code = toInt(tokens[i+1]);
                if (!isValidStatusCode(code))
                    throw std::runtime_error("Config: Invalid status code in error_page: " + tokens[i+1]);
                std::string path = tokens[i+2];
                currentServer.error_pages[code] = path;
                i += 4;
            }
            else if (token == "client_max_body_size") {
                // Syntax: client_max_body_size 1000000;
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid client_max_body_size syntax");
                checkValue(tokens[i+1], "client_max_body_size");
                currentServer.client_max_body_size = toSizeT(tokens[i+1]);
                i += 3;
            }
            else if (token == "location") {
                // Syntax: location /path {
                if (i + 2 >= tokens.size() || tokens[i+2] != "{")
                    throw std::runtime_error("Config: Invalid location syntax");
                
                checkValue(tokens[i+1], "location");
                inLocation = true;
                currentLocation = Location(); // Reset defaults
                currentLocation.uri = tokens[i+1];
                i += 3;
            }
            else {
                throw std::runtime_error("Config: Unknown directive in server block: " + token);
            }
        }
        // -----------------------------------------------------------
        // 3. LOCATION SCOPE (Inside location / { ... })
        // -----------------------------------------------------------
        else if (inLocation)
        {
            if (token == "}") {
                // End of location block
                // VALIDATION: Root or Return is mandatory (unless it's just for CGI, but typically we need root)
                if (currentLocation.root.empty() && currentLocation.return_code == 0)
                     throw std::runtime_error("Config: Location '" + currentLocation.uri + "' needs 'root' or 'return'");
                
                if (currentLocation.methods.empty())
                    currentLocation.methods.push_back("GET");

                currentServer.locations.push_back(currentLocation);
                inLocation = false;
                i++;
            }
            else if (token == "root") {
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid root");
                checkValue(tokens[i+1], "root");
                currentLocation.root = tokens[i+1];
                i += 3;
            }
            else if (token == "index") {
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid index");
                checkValue(tokens[i+1], "index");
                currentLocation.index = tokens[i+1];
                i += 3;
            }
            else if (token == "autoindex") {
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid autoindex");
                std::string val = tokens[i+1];
                checkValue(val, "autoindex");
                currentLocation.autoindex = (val == "on");
                i += 3;
            }
            else if (token == "allow_methods") {
                // Syntax: allow_methods GET POST; (Variable number of arguments!)
                // We consume until we see ';'
                i++; 
                while (i < tokens.size() && tokens[i] != ";")
                {
                    checkValue(tokens[i], "allow_methods");
                    if (!isValidMethod(tokens[i]))
                        throw std::runtime_error("Config: Invalid HTTP method in allow_methods: " + tokens[i]);
                    currentLocation.methods.push_back(tokens[i]);
                    i++;
                }
                if (i >= tokens.size())
                    throw std::runtime_error("Config: Missing ; after allow_methods");
                i++; // Skip ';'
            }
            else if (token == "return") {
                // Syntax: return 301 /url;
                if (i + 3 >= tokens.size() || tokens[i+3] != ";")
                    throw std::runtime_error("Config: Invalid return");
                checkValue(tokens[i+1], "return");
                checkValue(tokens[i+2], "return");
                int code = toInt(tokens[i+1]);
                if (!isValidStatusCode(code))
                    throw std::runtime_error("Config: Invalid status code in return: " + tokens[i+1]);
                currentLocation.return_code = toInt(tokens[i+1]);
                currentLocation.return_url = tokens[i+2];
                i += 4;
            }
            else if (token == "upload_store") {
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid upload_store");
                checkValue(tokens[i+1], "upload_store");
                currentLocation.upload_store = tokens[i+1];
                i += 3;
            }
            else if (token == "cgi_extension") {
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid cgi_extension");
                checkValue(tokens[i+1], "cgi_extension");
                currentLocation.cgi_extension = tokens[i+1];
                i += 3;
            }
            else if (token == "cgi_path") {
                if (i + 2 >= tokens.size() || tokens[i+2] != ";")
                    throw std::runtime_error("Config: Invalid cgi_path");
                checkValue(tokens[i+1], "cgi_path");
                currentLocation.cgi_path = tokens[i+1];
                i += 3;
            }
            else {
                throw std::runtime_error("Config: Unknown directive in location block: " + token);
            }
        }
    }

    if (inServer || inLocation)
        throw std::runtime_error("Config: Unexpected end of file (unclosed block)");
}
