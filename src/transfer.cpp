#include "transfer.hpp"
#include "net.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ft {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t buffer_size = 64 * 1024;
constexpr std::array<unsigned char, 8> magic{'F', 'T', 'A', 'P', 'P', '0', '0', '1'};
enum class Status : std::uint32_t { ready = 0, complete = 1, rejected = 2 };

struct Options {
    std::vector<std::string> positional;
    std::string bind = "0.0.0.0";
    int timeout = 30;
    std::uint64_t max_size = std::numeric_limits<std::uint64_t>::max();
    bool force = false;
    bool quiet = false;
};

std::uint64_t number(const std::string& text, std::uint64_t maximum, const std::string& label) {
    if (text.empty()) throw std::invalid_argument(label + " requires an integer");
    std::uint64_t result = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') throw std::invalid_argument("Invalid " + label + ": " + text);
        const auto digit = static_cast<unsigned>(ch - '0');
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10))
            throw std::invalid_argument(label + " is out of range");
        result = result * 10 + digit;
    }
    return result;
}

void usage(bool sending, std::ostream& out) {
    out << (sending ? "Usage: sender <host> <port> <file> [options]\n"
                    : "Usage: recver <port> <output> [options]\n");
    out << "  --timeout <seconds>  Network inactivity / connection wait limit (1-86400; default 30)\n"
           "  --quiet              Suppress progress and success messages\n"
           "  --help               Show this help\n"
           "  --version            Show version\n"
           "  --                   Treat remaining arguments as positional\n";
    if (!sending) out << "  --bind <address>     Listen address (default 0.0.0.0; use :: for IPv6)\n"
                         "  --force              Replace an existing regular file after verification\n"
                         "  --max-size <bytes>   Reject files larger than this limit\n"
                         "  --list-ips           List local addresses (also shown with no arguments)\n";
}

Options parse(const std::vector<std::string>& args, bool sending) {
    Options options;
    bool positional_only = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto value = [&]() -> const std::string& {
            if (++i == args.size()) throw std::invalid_argument("Missing value for " + arg);
            return args[i];
        };
        if (positional_only) options.positional.push_back(arg);
        else if (arg == "--") positional_only = true;
        else if (arg == "--quiet") options.quiet = true;
        else if (arg == "--timeout") {
            options.timeout = static_cast<int>(number(value(), 86400, "timeout"));
            if (options.timeout == 0) throw std::invalid_argument("Timeout must be at least 1 second");
        } else if (!sending && arg == "--bind") options.bind = value();
        else if (!sending && arg == "--force") options.force = true;
        else if (!sending && arg == "--max-size")
            options.max_size = number(value(), std::numeric_limits<std::uint64_t>::max(), "maximum size");
        else if (!arg.empty() && arg[0] == '-') throw std::invalid_argument("Unknown option: " + arg);
        else options.positional.push_back(arg);
    }
    if (options.positional.size() != (sending ? 3U : 2U))
        throw std::invalid_argument("Incorrect number of arguments");
    const auto port = number(options.positional[sending ? 1 : 0], 65535, "port");
    if (port == 0) throw std::invalid_argument("Port must be between 1 and 65535");
    options.positional[sending ? 1 : 0] = std::to_string(port);
    return options;
}

// CRC-32/ISO-HDLC, compatible with Python zlib.crc32. This detects accidental corruption;
// it provides no authentication or protection against deliberate modification.
class Crc32 {
public:
    void update(const char* bytes, std::size_t size) {
        static const auto table = [] {
            std::array<std::uint32_t, 256> values{};
            for (std::uint32_t i = 0; i < values.size(); ++i) {
                auto value = i;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value >> 1) ^ ((value & 1U) ? 0xedb88320U : 0U);
                values[i] = value;
            }
            return values;
        }();
        for (std::size_t i = 0; i < size; ++i)
            state_ = table[(state_ ^ static_cast<unsigned char>(bytes[i])) & 0xffU] ^ (state_ >> 8);
    }
    std::uint32_t value() const { return state_ ^ 0xffffffffU; }
private:
    std::uint32_t state_ = 0xffffffffU;
};

void encode(unsigned char* output, std::uint64_t value, std::size_t size) {
    for (std::size_t i = size; i > 0; --i) {
        output[i - 1] = static_cast<unsigned char>(value & 0xffU);
        value >>= 8;
    }
}

std::uint64_t decode(const unsigned char* input, std::size_t size) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < size; ++i) result = (result << 8) | input[i];
    return result;
}

void send_status(const Socket& socket, Status status, int timeout) {
    std::array<unsigned char, 8> message{'F', 'T', 'A', 'K', 0, 0, 0, 0};
    encode(message.data() + 4, static_cast<std::uint32_t>(status), 4);
    socket.send_all(message.data(), message.size(), timeout);
}

void expect_status(const Socket& socket, Status expected, int timeout) {
    std::array<unsigned char, 8> message{};
    socket.receive_all(message.data(), message.size(), timeout);
    if (std::memcmp(message.data(), "FTAK", 4) != 0) throw std::runtime_error("Invalid receiver response");
    const auto status = decode(message.data() + 4, 4);
    if (status == static_cast<std::uint32_t>(Status::rejected))
        throw std::runtime_error("Receiver rejected the transfer; check the receiver's error message");
    if (status != static_cast<std::uint32_t>(expected)) throw std::runtime_error("Unexpected receiver response");
}

class Progress {
public:
    Progress(std::uint64_t total, bool quiet) : total_(total), quiet_(quiet) {}
    ~Progress() { if (shown_) std::cerr << '\n'; }
    void update(std::uint64_t done, bool finished = false) {
        if (quiet_) return;
        const auto now = Clock::now();
        if (!finished && now - last_ < std::chrono::milliseconds(250)) return;
        const double seconds = std::max(0.001, std::chrono::duration<double>(now - start_).count());
        const double percent = total_ == 0 ? 100.0 : 100.0 * static_cast<double>(done) / static_cast<double>(total_);
        std::ostringstream line;
        line << '\r' << std::fixed << std::setprecision(1) << percent << "%  " << done << '/' << total_
             << " bytes  " << static_cast<double>(done) / (1024.0 * 1024.0 * seconds) << " MiB/s";
        std::cerr << line.str() << "          " << std::flush;
        shown_ = true;
        last_ = now;
        if (finished) { std::cerr << '\n'; shown_ = false; }
    }
private:
    std::uint64_t total_;
    bool quiet_;
    bool shown_ = false;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_ = start_;
};

std::runtime_error file_error(const std::string& operation) {
    const int code = errno;
    return std::runtime_error(operation + ": " + std::generic_category().message(code));
}

class OutputFile {
public:
    OutputFile(fs::path destination, bool force) : destination_(std::move(destination)), force_(force) {
        const auto status = fs::symlink_status(destination_);
        if (fs::exists(status)) {
            if (!force_) throw std::runtime_error("Output already exists; use --force to replace it");
            if (!fs::is_regular_file(status)) throw std::runtime_error("Output must be a regular file (not a directory or symlink)");
        }
        if (destination_.filename().empty()) throw std::runtime_error("Output must include a file name");
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::ostringstream name;
            name << ".file-transfer-" << std::hex << random() << random() << ".part";
            temporary_ = destination_.parent_path() / name.str();
#ifdef _WIN32
            int descriptor = -1;
            const int code = _wsopen_s(&descriptor, temporary_.c_str(),
                _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
            if (code != 0) errno = code;
#else
            const int descriptor = open(temporary_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
            if (descriptor < 0) {
                if (errno == EEXIST) continue;
                throw file_error("Cannot create temporary output");
            }
#ifdef _WIN32
            file_ = _fdopen(descriptor, "wb");
#else
            file_ = fdopen(descriptor, "wb");
#endif
            if (!file_) {
                const auto error = file_error("Cannot open output stream");
#ifdef _WIN32
                _close(descriptor);
#else
                close(descriptor);
#endif
                std::error_code ignored;
                fs::remove(temporary_, ignored);
                throw error;
            }
            return;
        }
        throw std::runtime_error("Cannot allocate a unique temporary file");
    }
    ~OutputFile() {
        if (file_) std::fclose(file_);
        std::error_code ignored;
        fs::remove(temporary_, ignored);
    }
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    void write(const char* data, std::size_t size) {
        if (std::fwrite(data, 1, size, file_) != size) throw file_error("Write output");
    }
    void commit() {
        check_cancelled();
        if (std::fflush(file_) != 0) throw file_error("Flush output");
#ifdef _WIN32
        if (_commit(_fileno(file_)) != 0) throw file_error("Sync output");
#else
        if (fsync(fileno(file_)) != 0) throw file_error("Sync output");
#endif
        const int result = std::fclose(file_);
        file_ = nullptr;
        if (result != 0) throw file_error("Close output");
        check_cancelled();
#ifdef _WIN32
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (force_ ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), flags))
            throw std::runtime_error("Publish output: " + std::system_category().message(static_cast<int>(GetLastError())));
#else
        // link() atomically refuses an existing name, including one created during the transfer.
        // Both paths are in the same directory. --force uses atomic replacement via rename().
        if (force_) {
            if (::rename(temporary_.c_str(), destination_.c_str()) != 0) throw file_error("Replace output");
        } else if (::link(temporary_.c_str(), destination_.c_str()) != 0) throw file_error("Publish output");
#endif
    }
private:
    fs::path destination_;
    fs::path temporary_;
    bool force_;
    std::FILE* file_ = nullptr;
};

void send_file(const Options& options) {
    const auto path = fs::u8path(options.positional[2]);
    if (!fs::is_regular_file(path)) throw std::runtime_error("Input must be an existing regular file");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open input file");
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("Cannot determine input size");
    const auto size = static_cast<std::uint64_t>(end);
    input.seekg(0);
    if (!input) throw std::runtime_error("Cannot seek input file");
    auto socket = connect_to(options.positional[0], options.positional[1], options.timeout);
    std::array<unsigned char, 16> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    encode(header.data() + 8, size, 8);
    socket.send_all(header.data(), header.size(), options.timeout);
    expect_status(socket, Status::ready, options.timeout);

    std::array<char, buffer_size> buffer{};
    Crc32 crc;
    Progress progress(size, options.quiet);
    std::uint64_t done = 0;
    while (done < size) {
        check_cancelled();
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - done));
        if (!input.read(buffer.data(), static_cast<std::streamsize>(count)))
            throw std::runtime_error("Cannot read input file; it may have changed during transfer");
        crc.update(buffer.data(), count);
        socket.send_all(buffer.data(), count, options.timeout);
        done += count;
        progress.update(done);
    }
    // A filebuf may retain bytes read ahead before another process truncates the file.
    // Check the current filesystem size as well as the stream, before sending the checksum.
    if (fs::file_size(path) != size || input.peek() != std::char_traits<char>::eof() || input.bad())
        throw std::runtime_error("Input file changed size or could not be read during transfer");
    std::array<unsigned char, 4> checksum{};
    encode(checksum.data(), crc.value(), checksum.size());
    socket.send_all(checksum.data(), checksum.size(), options.timeout);
    socket.finish_sending();
    try { expect_status(socket, Status::complete, options.timeout); }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) + "; delivery was not confirmed (check the receiver before retrying)");
    }
    progress.update(done, true);
    if (!options.quiet) std::cout << "Sent " << size << " bytes; receiver verified and saved the file.\n";
}

void receive_file(const Options& options) {
    OutputFile output(fs::u8path(options.positional[1]), options.force);
    auto listener = listen_on(options.bind, options.positional[0]);
    if (!options.quiet)
        std::cerr << "Listening on " << options.bind << ':' << options.positional[0] << " (timeout "
                  << options.timeout << "s)\n" << std::flush;
    auto socket = accept_one(listener, options.timeout);
    bool saved = false;
    try {
        std::array<unsigned char, 16> header{};
        socket.receive_all(header.data(), header.size(), options.timeout);
        if (!std::equal(magic.begin(), magic.end(), header.begin()))
            throw std::runtime_error("Unsupported transfer protocol; update both sender and receiver");
        const auto size = decode(header.data() + 8, 8);
        if (size > options.max_size) throw std::runtime_error("Incoming file exceeds --max-size");
        send_status(socket, Status::ready, options.timeout);
        std::array<char, buffer_size> buffer{};
        Crc32 crc;
        Progress progress(size, options.quiet);
        std::uint64_t done = 0;
        while (done < size) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - done));
            socket.receive_all(buffer.data(), count, options.timeout);
            output.write(buffer.data(), count);
            crc.update(buffer.data(), count);
            done += count;
            progress.update(done);
        }
        std::array<unsigned char, 4> checksum{};
        socket.receive_all(checksum.data(), checksum.size(), options.timeout);
        if (decode(checksum.data(), checksum.size()) != crc.value()) throw std::runtime_error("CRC32 checksum mismatch");
        socket.expect_end(options.timeout);
        output.commit();
        saved = true;
        send_status(socket, Status::complete, options.timeout);
        progress.update(done, true);
        if (!options.quiet) std::cout << "Received and verified " << size << " bytes: " << options.positional[1] << '\n';
    } catch (const std::exception& error) {
        if (saved) throw std::runtime_error(std::string("File saved, but sender acknowledgement failed: ") + error.what());
        // Rejection is best-effort: a disconnected/cancelled peer may no longer be writable.
        try { send_status(socket, Status::rejected, options.timeout); } catch (const std::exception&) {}
        throw;
    }
}
} // namespace

int run(const std::vector<std::string>& args, bool sending) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) { usage(sending, std::cout); return 0; }
    if (args.size() == 1 && args[0] == "--version") { std::cout << "file-transfer 1.0.0 (protocol 1)\n"; return 0; }
    if (!sending && (args.empty() || (args.size() == 1 && args[0] == "--list-ips"))) {
        Network network;
        if (args.empty()) usage(false, std::cout);
        std::cout << "Local IP addresses (including loopback):\n";
        for (const auto& address : local_addresses()) std::cout << "  " << address << '\n';
        return 0;
    }
    Options options;
    try { options = parse(args, sending); }
    catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << '\n';
        usage(sending, std::cerr);
        return 2;
    }
    Network network;
    if (sending) send_file(options);
    else receive_file(options);
    return 0;
}
} // namespace ft
