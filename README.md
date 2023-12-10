# File Transfer Application

This project provides a simple implementation of a file transfer application. It includes separate implementations for Windows and Unix (macOS and Linux) systems. The application is capable of sending and receiving files over a network.

## Structure

- `windows/`: Contains the source code for the Windows version of the application.
- `unix/`: Contains the source code for the Unix (macOS and Linux) version of the application.

## Compilation

### Windows

Use MinGW or a similar GCC-based toolchain for Windows. The following commands can be used to compile the application:

```bash
g++ -O2 -o windows/sender.exe windows/sender.cpp -lws2_32 -liphlpapi
g++ -O2 -o windows/recver.exe windows/recver.cpp -lws2_32 -liphlpapi
```

### Unix (macOS and Linux)
Use GCC or Clang to compile the application. The following commands can be used:
```bash
g++ -O2 -o unix/sender unix/sender.cpp
g++ -O2 -o unix/recver unix/recver.cpp
```

## Usage

### Sender
Run the sender executable with the appropriate command-line arguments:

+ On Windows: ```sender.exe <server_ip> <port> <file_to_send>```

+ On Unix: ```./sender <server_ip> <port> <file_to_send>
Receiver```

### Receiver
Run the recver executable with the appropriate command-line arguments:

+ On Windows: ```recver.exe <port> <file_to_save>```

+ On Unix: ```./recver <port> <file_to_save>```

To simply show the IP adress of the current machine, use the following command:

+ On Windows: ```recver.exe```

+ On Unix: ```./recver```

## Features

+ (Currently Dealing With Bugs On Cross-Platforming) Cross-platform support: Works on Windows, macOS, and Linux.
+ Command-line interface: Easy to use commands for sending and receiving files.
+ Network communication: Uses TCP/IP for reliable data transfer.

## Contributing

Contributions to this project are welcome. Please ensure that any pull requests or issues are clear and descriptive.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.