#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

void handleServerResponses(int serverSocket);
void sendCommand(int serverSocket, const std::string& command, const std::string& args = "");

int main() {
    const std::string serverIP = "127.0.0.1"; // Example server IP address
    const int PORT = 12345; // Example server port

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

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cout << "\nConnection Failed\n";
        return -1;
    }

    std::cout << "Connected to the server at " << serverIP << ":" << PORT << std::endl;

    // Prompt for username and send it to the server
    std::string username;
    std::cout << "Enter username to join the group: ";
    std::getline(std::cin, username);
    username += "\n"; // Ensure to append a newline character to signal end of input
    send(sock, username.c_str(), username.length(), 0); // Directly send the username

    // Start a thread to handle server responses
    std::thread serverThread(handleServerResponses, sock);
    serverThread.detach(); // Don't need to join the thread, letting it run freely

    // Client command loop
    std::string inputLine;
    while (true) {
        std::getline(std::cin, inputLine); // Read user input from console
        if (inputLine.empty()) continue;

        if (inputLine == "%leave") {
            sendCommand(sock, "%leave");
            break; // Exit the loop and close the application
        } else if (inputLine.find("%post") == 0) {
            // Handle post command
            sendCommand(sock, inputLine);
        } else if (inputLine == "%users") {
            sendCommand(sock, "%users");
        } else if (inputLine.find("%message") == 0) {
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
        if (msg.find("%message") == 0) {
            // Extract the message content
            std::string messageContent = msg.substr(9); // Skip "%message "
            std::cout << "Message: " << messageContent << std::endl;
        } else if (msg.find("%history") == 0) {
            // Handle message history
            size_t newlineIndex = msg.find('\n');
            std::string history = msg.substr(newlineIndex + 1);
            std::cout << "Message history:\n" << history << std::endl;
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
