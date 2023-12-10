#include <winsock2.h>
#include <iphlpapi.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

std::vector<std::string> getLocalIPs() {
    std::vector<std::string> ips;
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO *)malloc(sizeof(IP_ADAPTER_INFO));
    PIP_ADAPTER_INFO pAdapter = NULL;

    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)malloc(ulOutBufLen);
    }

    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == NO_ERROR) {
        pAdapter = pAdapterInfo;
        while (pAdapter) {
            ips.push_back(pAdapter->IpAddressList.IpAddress.String);
            pAdapter = pAdapter->Next;
        }
    }

    if (pAdapterInfo) {
        free(pAdapterInfo);
    }

    return ips;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::vector<std::string> localIPs = getLocalIPs();
        std::cerr << "Usage: " << argv[0] << " <port> <file_to_save>" << std::endl;
        std::cerr << "Available non-loopback IPv4 addresses:" << std::endl;
        for (const std::string &ip : localIPs) {
            std::cerr << " - " << ip << std::endl;
        }
        return 1;
    }

    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup failed: " << iResult << std::endl;
        return 1;
    }

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        std::cerr << "Socket creation failed" << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(atoi(argv[1]));

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    if (listen(server_fd, 3) < 0) {
        std::cerr << "Listen" << std::endl;
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    SOCKET new_socket;
    int addrlen = sizeof(address);
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
        std::cerr << "Accept" << std::endl;
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    std::ofstream outFile(argv[2], std::ios::binary);
    char buffer[1024] = {0};

    while (int valread = recv(new_socket, buffer, 1024, 0)) {
        outFile.write(buffer, valread);
    }

    outFile.close();
    closesocket(new_socket);
    closesocket(server_fd);
    WSACleanup();
    return 0;
}
