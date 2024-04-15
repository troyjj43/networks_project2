#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

void handleServerResponses(int serverSocket);
void sendCommand(int serverSocket, const std::string& command, const std::string& args = "");

int main() {
    std::string serverIP = "127.0.0.1"; // Default server IP address
    int PORT = 12345; // Default server port

    int sock = 0;
    struct sockaddr_in serv_addr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cout << "\nSocket creation error\n";
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, serverIP.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cout << "\nInvalid address/Address not supported\n";
        return -1;
    }

    std::cout << "Client started. Use %connect [ip] [port] to connect to a server." << std::endl;

    // Client command loop
    bool joined = false; // Track if the client has joined the message board
    std::string inputLine;
    std::string username; // Username of the client
    while (true) {
        std::getline(std::cin, inputLine); // Read user input from console
        if (inputLine.empty()) continue;

        if (inputLine.find("%connect") == 0) {
            std::istringstream iss(inputLine);
            std::string cmd, newIP;
            int newPort;
            iss >> cmd >> newIP >> newPort;
            if (!newIP.empty() && newPort > 0) {
                serverIP = newIP;
                PORT = newPort;
                serv_addr.sin_port = htons(PORT);

                // Convert IPv4 and IPv6 addresses from text to binary form
                if (inet_pton(AF_INET, serverIP.c_str(), &serv_addr.sin_addr) <= 0) {
                    std::cout << "\nInvalid address/Address not supported\n";
                    continue;
                }

                if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                    std::cout << "\nConnection Failed\n";
                    continue;
                }

                std::cout << "Connected to the server at " << serverIP << ":" << PORT << std::endl;

                // Prompt for username and send it to the server
                std::cout << "Enter username: ";
                std::getline(std::cin, username);
                username += "\n"; // Ensure to append a newline character to signal end of input
                send(sock, username.c_str(), username.length(), 0); // Directly send the username

                break; // Exit the loop and continue with the rest of the client code
            } else {
                std::cout << "Invalid arguments for %connect command\n";
            }
        } else {
            std::cout << "You must connect to the server with %connect [ip] [port] before using other commands.\n";
        }
    }

    // Start a thread to handle server responses
    std::thread serverThread(handleServerResponses, sock);
    serverThread.detach(); // Don't need to join the thread, letting it run freely

    // Client command loop
    while (true) {
        std::getline(std::cin, inputLine); // Read user input from console
        if (inputLine.empty()) continue;
        std::cout << "Processing command: " << inputLine << std::endl;
        if (inputLine.find("%groupjoin ") == 0) {
            sendCommand(sock, inputLine);
            joined = true; // Update the joined status
        } else if (inputLine == "%groups") {
            sendCommand(sock, inputLine);
        } else if (!joined) {
            std::cout << "You must join a message board with %groupjoin before using other commands.\n";
        } else if (inputLine.find("%groupleave ") == 0) {
            sendCommand(sock, inputLine);
            joined = false; // Update the joined status
        } else if (inputLine == "%exit") {
            break; // Exit the loop and close the application
        } else if (inputLine.find("%post") == 0 || inputLine.find("%message") == 0 || inputLine == "%users") {
            // Handle post, message, and users commands
            sendCommand(sock, inputLine);
        } else {
            // Send the message to the server
            sendCommand(sock, "%message", inputLine);
        }
    }

    close(sock); // Close the socket before exiting
    std::cout << "Disconnected from the server." << std::endl;

    return 0;
}

void handleServerResponses(int serverSocket) {
    char buffer[1024];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytesReceived = read(serverSocket, buffer, sizeof(buffer) - 1);
        if (bytesReceived <= 0) {
            // Either an error occurred or the server closed the connection
            std::cerr << "Server disconnected or error receiving message." << std::endl;
            break;
        }

        
        // Display the message from the server
        std::string msg(buffer, bytesReceived);
        std::cout << "\n" << "Received message from server: " << std::endl;
        if (msg.find("Available Groups:") == 0) {
            // Directly display the groups list
            std::cout << msg << std::endl;
        } else if (msg.find("%message") == 0) {
            // Extract the message content
            std::string messageContent = msg.substr(9); // Skip "%message "
            std::cout << "Message: " << messageContent << std::endl;
        } else if (msg.find("%history") == 0) {
            // Handle message history
            size_t newlineIndex = msg.find('\n');
            std::string history = msg.substr(newlineIndex + 1);
            std::cout << "Message history:\n" << history << std::endl;
        } else if (msg.find("Joined group ") == 0) {
            // Extract the group name from the message
            std::string groupName = msg.substr(13, msg.find("\n") - 13);
            std::cout << "Successfully joined group: " << groupName << std::endl;
        } else {
            // Regular chat message
            std::cout << msg << std::endl;
        }
    }
}

void sendCommand(int serverSocket, const std::string& command, const std::string& args) {
    std::string fullCommand = command;
    if (!args.empty()) {
        fullCommand += " " + args; // Append arguments to the command if any
    }

    ssize_t bytesSent = write(serverSocket, fullCommand.c_str(), fullCommand.length());
    if (bytesSent < 0) {
        std::cerr << "Failed to send command to server." << std::endl;
    } else {
        std::cout << "Command sent: " << fullCommand << std::endl;
    }
}
