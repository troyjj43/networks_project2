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
#include <deque>

// Global variables
std::vector<int> clientSockets; // Store client socket descriptors
std::map<int, std::string> clients; // Map socket descriptor to client name
std::mutex clientListMutex; // Mutex for thread-safe access to the clientSockets and clients
std::atomic<bool> serverRunning(true); // Needed for shutting down server
std::deque<std::string> messageHistory; // Store last 2 messages
const size_t maxMessageHistory = 2; // Maximum number of messages to store in history
std::string userList;

// Message ID counter
int messageIdCounter = 1;

void handleClient(int clientSocket);
void broadcastMessage(const std::string& message, int excludeSocket);
void sendHistoryToClient(int clientSocket);

// Helper function to get the current date and time as a string
std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::string formattedTime = std::ctime(&now_c); 
    formattedTime.pop_back(); // Remove newline character added by ctime
    return formattedTime;
}

// Used to update which clients are connected
void updateUserList() {
    std::lock_guard<std::mutex> guard(clientListMutex);
    userList.clear();
    for (const auto& client : clients) {
        userList += client.second + "\n";
    }
}

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

            // Exit the server
            exit(0);
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
        std::thread clientThread(handleClient, clientSocket);
        clientThread.detach(); // Detach the thread 
    }

    // Wait for the shutdown listener to finish
    shutdownListener.join();

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}

void handleClient(int clientSocket) {
    // Read username
    char buffer[1024] = {0};
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
        clients[clientSocket] = username;
    }

    // Notify other clients that the user has joined
    {
        std::lock_guard<std::mutex> guard(clientListMutex);
        for (const auto& client : clients) {
            if (client.first != clientSocket) {
                std::string joinMsg = username + " has joined the chat.";
                send(client.first, joinMsg.c_str(), joinMsg.length(), 0);
            }
        }
    }

    // Send message history to the client
    sendHistoryToClient(clientSocket);

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
            // Remove the client from the global list and map
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                clientSockets.erase(std::remove(clientSockets.begin(), clientSockets.end(), clientSocket),	clientSockets.end());
                clients.erase(clientSocket);
            }

            // Notify other clients that the user has left
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                for (const auto& client : clients) {
                    std::string leaveMsg = username + " has left the chat.";
                    send(client.first, leaveMsg.c_str(), leaveMsg.length(), 0);
                }
            }
            break; // Exit the loop and close the client connection
        } else if (msg == "%users") {
            // Update the user list
            updateUserList();

            // Send the user list to the client
            send(clientSocket, userList.c_str(), userList.length(), 0);
        } else if (msg.find("%post") == 0) {
            // Extract the message content from the %post command
            std::string postContent = msg.substr(6); // Skip "%post "
            if (!postContent.empty()) {
                std::lock_guard<std::mutex> guard(clientListMutex);
                std::string postMsg = getCurrentTime() + " " + username + " posted: " + postContent;
                broadcastMessage(postMsg, clientSocket);
            }
        } else {
            // For simplicity, we'll assume all other messages are chat messages to be broadcasted
            std::string formattedMsg = getCurrentTime() + " " + username + ": " + msg;
            broadcastMessage(formattedMsg, clientSocket);
        }
    }

    // Remove the client from the global list and map
    {
        std::lock_guard<std::mutex> guard(clientListMutex);
        clientSockets.erase(std::remove(clientSockets.begin(), clientSockets.end(), clientSocket),	clientSockets.end());
        clients.erase(clientSocket);
    }

    // Notify other clients that the user has left
    {
        std::lock_guard<std::mutex> guard(clientListMutex);
        for (const auto& client : clients) {
            std::string leaveMsg = username + " has left the chat.";
            send(client.first, leaveMsg.c_str(), leaveMsg.length(), 0);
        }
    }

    // Close the socket
    close(clientSocket);
}


void sendHistoryToClient(int clientSocket) {
    std::lock_guard<std::mutex> guard(clientListMutex);
    int startIndex = std::max(0, static_cast<int>(messageHistory.size()) - 2);
    if (startIndex < 0) {
        return; // No need to send history if it's empty
    }
    for (int i = startIndex; i < messageHistory.size(); ++i) {
        std::string msg = messageHistory[i];
        // Exclude join messages from history
        if (msg.find("has joined the chat.") == std::string::npos) {
            ssize_t bytesSent = send(clientSocket, msg.c_str(), msg.length(), 0);
            if (bytesSent < 0) {
                std::cerr << "Failed to send message to client." << std::endl;
            } else {
                std::cout << "Sent message to client: " << msg << std::endl;
            }
        }
    }
}

void broadcastMessage(const std::string& message, int excludeSocket = -1) {
    std::lock_guard<std::mutex> guard(clientListMutex); // Ensure thread safety
    if (messageHistory.size() >= maxMessageHistory) {
        messageHistory.pop_front();
    }
    messageHistory.push_back(message);

    for (int socket : clientSockets) {
        if (socket != excludeSocket) {
            send(socket, message.c_str(), message.length(), 0);
        }
    }
}
