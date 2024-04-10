#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <ctime>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <atomic>


// Global variables
std::vector<int> clientSockets; // Store client socket descriptors
std::map<int, std::string> clients; // Map socket descriptor to client name
std::mutex clientListMutex; // Mutex for thread-safe access to the clientSockets and clients
std::atomic<bool> serverRunning(true); // Needed for shutting down server
int serverSocket; // Declare serverSocket globally

void handleClient(int clientSocket);
void broadcastMessage(const std::string& message, int excludeSocket = -1);

void listenForShutdownCommand() {
    std::string command;
    while (true) {
        std::getline(std::cin, command);
        if (command == "shutdown") {
            std::cout << "Shutdown command received. Shutting down server...\n";
            serverRunning = false; // Signal the server loop to stop

            // Close all client sockets to unblock any read/write operations
            clientListMutex.lock();
            for (int clientSocket : clientSockets) {
                close(clientSocket);
            }
            clientSockets.clear();
            clients.clear();
            clientListMutex.unlock();

            // Close the server socket
            close(serverSocket);
            break;
        }
    }
}

int main() {
    const int PORT = 12345;
    int serverSocket, newSocket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    std::cout << "Server started. Listening on port " << PORT << std::endl;

    // Start the thread that listens for the shutdown command
    std::thread shutdownListener(listenForShutdownCommand);
  
    // Creating socket file descriptor
    if ((serverSocket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return -1;
    }
  
    // Forcefully attaching socket to the port 12345
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "Setsockopt failed" << std::endl;
        return -1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Localhost
    address.sin_port = htons(PORT);
  
    // Forcefully attaching socket to the port 12345
    if (bind(serverSocket, (struct sockaddr *)&address, sizeof(address))<0) {
        std::cerr << "Bind failed" << std::endl;
        return -1;
    }
    if (listen(serverSocket, 3) < 0) { // Second parameter is the backlog (max queue of pending connections)
        std::cerr << "Listen failed" << std::endl;
        return -1;
    }
  
    while (serverRunning) {
        int clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket < 0) {
            if (!serverRunning) break; // Shutdown initiated
            std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            continue;
        }
      
        // Use a thread to handle the client
        std::thread clientThread(handleClient, newSocket);
        clientThread.detach(); // Detach the thread 
    }

    // Wait for the shutdown listener to finish
    shutdownListener.join();

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}

void handleClient(int clientSocket) {
    char buffer[1024] = {0};
    // Read username
    ssize_t readSize = read(clientSocket, buffer, sizeof(buffer) - 1);
    if (readSize <= 0) {
        std::cerr << "Failed to read username\n";
        close(clientSocket);
        return;
    }
    std::string username(buffer, readSize);

    // Add user to the map
    {
        std::lock_guard<std::mutex> guard(clientListMutex);
        for (const auto& client : clients) {
            std::string joinMsg = username + " has joined the group.";
            send(client.first, joinMsg.c_str(), joinMsg.length(), 0);
        }
        clients[clientSocket] = username;
    }

    // Listen for messages from the client
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        readSize = read(clientSocket, buffer, 1024);

        if (readSize <= 0) {
            // If readSize is 0 or negative, the client has disconnected
            break;
        }

        std::string msg(buffer, readSize);
        if (msg == "%leave") {
            // Handle leave command
            break;
        } else {
            // For simplicity, we'll assume all other messages are chat messages to be broadcasted
            std::string formattedMsg = username + ": " + msg;
            broadcastMessage(formattedMsg, clientSocket);
        }
    }

    // Remove the client from the global list and map
    {
        std::lock_guard<std::mutex> guard(clientListMutex);
        clientSockets.erase(std::remove(clientSockets.begin(), clientSockets.end(), clientSocket), clientSockets.end());
        clients.erase(clientSocket);
    }

    // Notify other clients that the user has left
    std::string leaveMsg = username + " has left the chat.";
    broadcastMessage(leaveMsg);

    // Close the socket
    close(clientSocket);
}

void broadcastMessage(const std::string& message, int excludeSocket) {
    std::lock_guard<std::mutex> guard(clientListMutex); // Ensure thread safety
    for (int socket : clientSockets) {
        if (socket != excludeSocket) { // Check to not send the message to the excluded socket
            send(socket, message.c_str(), message.length(), 0);
        }
    }
}

// Helper function to get the current date and time as a string
std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::string formattedTime = std::ctime(&now_c); 
    formattedTime.pop_back(); // Remove newline character added by ctime
    return formattedTime;
}
