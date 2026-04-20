#include <drogon/drogon.h>
#include <iostream>

int main() {
    std::cout << "Starting Drogon blog server..." << std::endl;
    
    //Load config file
    try {
        drogon::app().loadConfigFile("./config.json");
        std::cout << "Config loaded successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "Server starting on port 8090..." << std::endl;
    
    //Run HTTP framework,the method will block in the internal event loop
    drogon::app().run();
    
    return 0;
}
