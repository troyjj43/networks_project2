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
#include <arpa/inet.h>
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
std::vector<std::string> messageIDs;
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
        clientSockets.push_back(clientSocket);
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
        
        if (msg != "%join") {
            // Check if the client has joined the group
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                if (clients.find(clientSocket) == clients.end()) {
                    // Client has not joined the group yet
                    std::string joinMsg = "You must join the message board with %join before using other commands.";
                    send(clientSocket, joinMsg.c_str(), joinMsg.length(), 0);
                    continue;
                }
            }
        }

        if (msg == "%leave") {
            // Remove the client from the global list and map
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                clientSockets.erase(std::remove(clientSockets.begin(), clientSockets.end(), clientSocket), clientSockets.end());
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
        } else if (msg.find("%post") != std::string::npos) {
            // Extract the message content from the %post command
            std::string postContent = msg.substr(6); // Skip "%post "
            std::string currentmessageID = std::to_string(messageIdCounter);
            if (!postContent.empty()) {
                std::string postMsg = "Message ID: " + currentmessageID + "\n" + username + " posted: " + postContent + "\n";
                broadcastMessage(postMsg, clientSocket);
                messageIDs.push_back(postContent);
            }
        } else if (msg == "%exit") {
            // Notify other clients that the user has left
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                for (const auto& client : clients) {
                    std::string leaveMsg = username + " has left the chat.";
                    send(client.first, leaveMsg.c_str(), leaveMsg.length(), 0);
                }
            }

            // Remove the client from the global list and map
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                clientSockets.erase(std::remove(clientSockets.begin(), clientSockets.end(), clientSocket), clientSockets.end());
                clients.erase(clientSocket);
            }
            break; // Exit the loop and close the client connection
        } else if (msg == "%join") {
            // Notify other clients that the user has joined the group
            {
                std::lock_guard<std::mutex> guard(clientListMutex);
                for (const auto& client : clients) {
                    if (client.first != clientSocket && std::find(clientSockets.begin(), clientSockets.end(), client.first) != clientSockets.end()) {
                        std::string joinMsg = username + " has joined the group.";
                        send(client.first, joinMsg.c_str(), joinMsg.length(), 0);
                    }
                }
                // Add the client to the list of joined clients
                clients[clientSocket] = username;
            }
            // Update the user list
            updateUserList();
            std::string header = "Group Members:\n";
            std::string finalUserList = header + userList;  // Add header before the user list
            // Send the user list to the client
            send(clientSocket, finalUserList.c_str(), finalUserList.length(), 0);
        } else if (msg.find("%message") != std::string::npos) {
            std::lock_guard<std::mutex> guard(clientListMutex);
            if (messageIDs.empty()) //Send Message to client and return nothing if message history is empty
            {
                std::string emptyHistoryWarning = "There are no previous messages in this bulletin board";
                send(clientSocket,emptyHistoryWarning.c_str(), emptyHistoryWarning.length(), 0);
            }
            else{
                std::string messageIDInput = msg.substr(9); // Skip "%message"  
                messageIDInput.erase(std::remove_if(messageIDInput.begin(),messageIDInput.end(), ::isspace),messageIDInput.end()); //Remove any whitespace from user input
                char extractedIdNum = messageIDInput.back();
                int messageIDNum = messageIDInput.back() - '0';
                if (messageIDNum < 1 || messageIDNum-1 > messageIDs.size()){
                    std::string errorMessage = "The ID Number entered does not exist";
                    send(clientSocket,errorMessage.c_str(),errorMessage.length(),0);
                } else{
                    std::string message = "Message:" + std::to_string(messageIDNum) + "\n" + messageIDs[messageIDNum - 1];
                    send(clientSocket, message.c_str(),message.length(), 0);
                }
                
               }
            
        }
    }

    // Close the socket
    close(clientSocket);
}


void sendMessageToClients(const std::string& message) {
    for (int socket : clientSockets) {
        std::cout << "Sending message to socket " << socket << std::endl;
        ssize_t bytesSent = send(socket, message.c_str(), message.length(), 0);
        if (bytesSent < 0) {
            std::cerr << "Failed to send message to socket " << socket << std::endl;
        } else {
            std::cout << "Sent message to socket " << socket << ": " << message << std::endl;
        }
    }
}

void broadcastMessage(const std::string& message, int excludeSocket = -1) {
    // Copy the list of client sockets to avoid modifying it while iterating
    std::vector<int> socketsToSend;
    {
        std::lock_guard<std::mutex> guard(clientListMutex);
        for (int socket : clientSockets) {
            if (socket != excludeSocket) {
                socketsToSend.push_back(socket);
            }
        }
        //messageIDs.push_back(message);
        messageIdCounter++;
    }

    // Send messages outside the lock
    for (int socket : socketsToSend) {
        ssize_t bytesSent = send(socket, message.c_str(), message.length(), 0);
        if (bytesSent < 0) {
            std::cerr << "Failed to send message to socket " << socket << std::endl;
        }
    }

    std::cout << "Broadcasting message: " << message << std::endl;
}














