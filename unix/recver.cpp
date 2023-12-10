// macOS Receiver Program
#include <iostream>
#include <fstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <vector>

std::vector<std::string> getLocalIPs() {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];
    std::vector<std::string> ips;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) { // Check for IPv4
            if (strcmp(ifa->ifa_name, "lo") != 0) {  // Exclude loopback interface
                int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                                    host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
                if (s != 0) {
                    std::cerr << "getnameinfo() failed: " << gai_strerror(s) << std::endl;
                    continue;
                }

                ips.push_back(std::string(host));
            }
        }
    }

    freeifaddrs(ifaddr);
    return ips;
}

int main(int argc, char const *argv[]) {
    if (argc != 3) {
        std::vector<std::string> localIPs = getLocalIPs();
        std::cerr << "Usage: " << argv[0] << " <port> <filename_to_save>" << std::endl;
        std::cerr << "Available IPv4 addresses:" << std::endl;
        for (const std::string &ip : localIPs) {
            std::cerr << " - " << ip << std::endl;
        }
        return 1;
    }

    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[1024] = {0};

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0) {
        std::cerr << "Socket creation error" << std::endl;
        return -1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(atoi(argv[1]));

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return -1;
    }

    if (listen(server_fd, 3) < 0) {
        std::cerr << "Listen failed" << std::endl;
        return -1;
    }

    new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
    if (new_socket < 0) {
        std::cerr << "Accept failed" << std::endl;
        return -1;
    }

    std::ofstream outFile(argv[2], std::ios::binary);

    while (int valread = read(new_socket, buffer, 1024)) {
        outFile.write(buffer, valread);
    }

    outFile.close();
    close(new_socket);
    close(server_fd);
    return 0;
}
