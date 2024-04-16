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
#include <set>

// Global variables
std::vector<int> clientSockets; // Store client socket descriptors
std::map<int, std::string> clients; // Map socket descriptor to client name
std::mutex clientListMutex; // Mutex for thread-safe access to the clientSockets and clients
std::atomic<bool> serverRunning(true); // Needed for shutting down server
std::deque<std::string> messageHistory; // Store last 2 messages
const size_t maxMessageHistory = 2; // Maximum number of messages to store in history
std::vector<std::string> messageIDs;
std::string userList;

struct Group {
    int id; // Numeric ID for the group
    std::set<int> members; // Store client sockets that are members of the group
    std::vector<std::string> messages; // Messages posted to the group
    std::string name; // Name of the group
    std::vector<std::string> messageIDs; //Message history of each group
    int messageIDCounter = 1;
};
std::map<int, Group> groups; // Map group ID to Group structure
std::map<int, std::set<int>> userGroups; // Maps client sockets to a set of group IDs they are part of

// Initialize groups with IDs
void initializeGroups() {
    groups[1] = Group{1, std::set<int>(), std::vector<std::string>(), "group1", std::vector<std::string>()};
    groups[2] = Group{2, std::set<int>(), std::vector<std::string>(), "group2", std::vector<std::string>()};
    groups[3] = Group{3, std::set<int>(), std::vector<std::string>(), "group3", std::vector<std::string>()};
    groups[4] = Group{4, std::set<int>(), std::vector<std::string>(), "group4", std::vector<std::string>()};
    groups[5] = Group{5, std::set<int>(), std::vector<std::string>(), "group5", std::vector<std::string>()};
}

// Message ID counter
int messageIdCounter = 1;

void handleClient(int clientSocket);
void broadcastMessage(const std::string& message, int excludeSocket);
void sendHistoryToClient(int clientSocket);
void broadcastMessageToGroup(int groupID, std::string& message, std::string& messageContent,int excludeSocket);

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

    initializeGroups(); // Initialize the groups

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

    // Output list of groups when client connects
    {
        std::string availableGroups = "Available Groups:\n";
            for (const auto& group : groups) {
                availableGroups += "ID: " + std::to_string(group.second.id) + " - " + group.second.name + "\n";
            }
            send(clientSocket, availableGroups.c_str(), availableGroups.length(), 0);
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
        if (msg == "%groups") {
            std::string availableGroups = "Available Groups:\n";
            for (const auto& group : groups) {
                availableGroups += "ID: " + std::to_string(group.second.id) + " - " + group.second.name + "\n";
            }
            send(clientSocket, availableGroups.c_str(), availableGroups.length(), 0);
        }
        
        if (msg.find("%groupjoin ") == 0) {
            std::string groupIdentifier = msg.substr(11); // Get the rest of the string after %groupjoin 
            groupIdentifier.erase(std::remove_if(groupIdentifier.begin(), groupIdentifier.end(), isspace), groupIdentifier.end()); // Remove any extra spaces

            bool groupFound = false;
            // Try to join by ID first
            try {
                int groupId = std::stoi(groupIdentifier);
                auto it = groups.find(groupId);
                if (it != groups.end()) {
                    it->second.members.insert(clientSocket);
                    userGroups[clientSocket].insert(groupId); // Add group to user's list of groups
                    groupFound = true;
                    send(clientSocket, ("Joined group " + it->second.name + "\n").c_str(), ("Joined group " + it->second.name + "\n").length(), 0);
                }
            } catch (std::invalid_argument&) {
                // Not a number so treat it as a name
                for (auto& group : groups) {
                    if (group.second.name == groupIdentifier) {
                        group.second.members.insert(clientSocket);
                        userGroups[clientSocket].insert(group.first); // Add group to user's list of groups
                        groupFound = true;
                        send(clientSocket, ("Joined group " + group.second.name + "\n").c_str(), ("Joined group " + group.second.name + "\n").length(), 0);
                        break;
                    }
                }
            }

            if (!groupFound) {
                std::string errorMsg = "Group not found\n";
                send(clientSocket, errorMsg.c_str(), errorMsg.length(), 0);
            }
        }

        if (msg.find("%groupleave ") == 0) {
            std::string groupIdentifier = msg.substr(12); // Extract group identifier after command
            groupIdentifier.erase(std::remove_if(groupIdentifier.begin(), groupIdentifier.end(), isspace), groupIdentifier.end()); // Clean spaces

            bool groupFound = false;
            // Try to leave by ID
            try {
                int groupId = std::stoi(groupIdentifier);
                auto it = groups.find(groupId);
                if (it != groups.end() && userGroups[clientSocket].find(groupId) != userGroups[clientSocket].end()) {
                    it->second.members.erase(clientSocket);
                    userGroups[clientSocket].erase(groupId); // Remove group from user's list of groups
                    groupFound = true;
                    send(clientSocket, ("Left group " + it->second.name + "\n").c_str(), ("Left group " + it->second.name + "\n").length(), 0);
                }
            } catch (std::invalid_argument&) {
                // Leave by name if ID fails
                for (auto& group : groups) {
                    if (group.second.name == groupIdentifier && userGroups[clientSocket].find(group.first) != userGroups[clientSocket].end()) {
                        group.second.members.erase(clientSocket);
                        userGroups[clientSocket].erase(group.first); // Remove group from user's list of groups
                        groupFound = true;
                        send(clientSocket, ("Left group " + group.second.name + "\n").c_str(), ("Left group " + group.second.name + "\n").length(), 0);
                        break;
                    }
                }
            }

            if (!groupFound) {
                std::string errorMsg = "Group not found or not a member\n";
                send(clientSocket, errorMsg.c_str(), errorMsg.length(), 0);
            }
        }

        if (msg.find("%groupusers ") == 0) {
            std::string groupIdentifier = msg.substr(12); // Extract the group identifier
            groupIdentifier.erase(std::remove_if(groupIdentifier.begin(), groupIdentifier.end(), isspace), groupIdentifier.end()); // Clean spaces

            bool groupFound = false;
            // Try by ID
            try {
                int groupId = std::stoi(groupIdentifier);
                auto it = groups.find(groupId);
                if (it != groups.end() && userGroups[clientSocket].find(groupId) != userGroups[clientSocket].end()) {
                    // User is a member of the group, list users
                    std::string userList = "Users in " + it->second.name + ":\n";
                    for (int memberSocket : it->second.members) {
                        userList += clients[memberSocket] + "\n";
                    }
                    send(clientSocket, userList.c_str(), userList.length(), 0);
                    groupFound = true;
                }
            } catch (std::invalid_argument&) {
                // Not a number so use name
                for (auto& group : groups) {
                    if (group.second.name == groupIdentifier && userGroups[clientSocket].find(group.first) != userGroups[clientSocket].end()) {
                        // User is a member of the group, list users
                        std::string userList = "Users in " + group.second.name + ":\n";
                        for (int memberSocket : group.second.members) {
                            userList += clients[memberSocket] + "\n";
                        }
                        send(clientSocket, userList.c_str(), userList.length(), 0);
                        groupFound = true;
                        break;
                    }
                }
            }

            if (!groupFound) {
                std::string errorMsg = "Group not found or access denied\n";
                send(clientSocket, errorMsg.c_str(), errorMsg.length(), 0);
            }
        }

        if (msg.find("%grouppost ") == 0) {
            std::string extractedMessage = msg.substr(12); // Extract the users message from the command
            char extractedID = msg[11];
            try{
                int groupID = extractedID - '0'; // Extract ID from message as char
                auto it = groups.find(groupID);
                Group &group = it->second;
                if(std::find(group.members.begin(),group.members.end(), clientSocket)== group.members.end()){
                    std::string errorMessage = "Cannot send messages until you have joined the group";
                    send(clientSocket, errorMessage.c_str(), errorMessage.length(),0);
                }
                else{
                    std::string message = username + "posted to group " + extractedID + ": \n" + extractedMessage;
                    broadcastMessageToGroup(groupID, message, extractedMessage, clientSocket); 
                }
            } 
            catch (std::invalid_argument&) {
                std::string errorMessage = extractedID + "was not recognized as a group ID number, use format: %grouppost id message";
                send(clientSocket, errorMessage.c_str(),errorMessage.length(),0);
            }
        }
        if (msg.find("%groupmessage ") == 0){
            try{
                // Convert group and message IDs to integers
                int groupID = msg[14] -'0';
                int messageID = msg[16] -'0';
                auto it = groups.find(groupID);
                Group &group = it->second;
                if( it == groups.end() || std::find(group.members.begin(),group.members.end(), clientSocket)== group.members.end()){
                    std::string errorMessage = "You have not joined this group";
                    send(clientSocket, errorMessage.c_str(), errorMessage.length(),0);
                } else if(messageID > it->second.messageIDs.size()){
                    std::string erorrMessage = "Message ID does not exist";
                    send(clientSocket, erorrMessage.c_str(), erorrMessage.length(),0);
                } else{
                    std::string retrievedMessage = it->second.messageIDs[messageID - 1];
                    std:: string message = "Message: " + std::to_string(messageID) + " " + retrievedMessage;
                    send(clientSocket, message.c_str(), message.length(), 0);
                }
            }
            catch (std::invalid_argument&) {
                std::string errorMessage =  "Group or Message ID was not recognized";
                send(clientSocket, errorMessage.c_str(),errorMessage.length(),0);
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
                    std::string message = "Message" + std::to_string(messageIDNum) + ": " + messageIDs[messageIDNum - 1];
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


void broadcastMessageToGroup(int groupID, std::string& message, std::string& messageContent, int excludeSocket){
    auto it = groups.find(groupID);
    if (it != groups.end()) {
        std::string finalMessage = "Message: " + std::to_string(it->second.messageIDCounter) + "\n" + message;
        for (int memberSocket : it->second.members) {
            if (memberSocket != excludeSocket){
                send(memberSocket, message.c_str(),message.length(),0);
            }
        }
        it->second.messageIDs.push_back(messageContent);
        it->second.messageIDCounter++;  
    }
    else{
        std::string errorMessage = "Group ID not found, use %groups to see group IDs \n";
        send(excludeSocket, errorMessage.c_str(),errorMessage.length(),0);
    }
}










