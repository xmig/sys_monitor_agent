#include <sys/statvfs.h>
#include <string>
#include <unistd.h>
#include <cstdlib>

#include <iostream>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctime>

#include <fstream>
#include <sstream>
#include <utility>
#include <vector>
#include <algorithm>
#include <numeric>
#include <utility>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/unordered_map.hpp>
#include <thread>
#include <unordered_set>


#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

// #define USESSL
#ifdef USESSL
#include <openssl/rand.h>
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
namespace ssl = boost::asio::ssl;
#endif

namespace beast = boost::beast;
namespace http = beast::http;

typedef unsigned long int ulong;

std::string VERSION = "1.14.2 (free) tcp host dailyreport hostinfo iftop xtext kill_mem";
int DEBOUNCE_TIME_SEC = 10 * 60;

std::string APP_KEY = "NEWTESTKEY";
std::string CRYPTO_KEY = "";
std::string SLACK_PATH = "";
std::string SYSTEM_ROOT = "/";

std::string get_app_key() {
    return APP_KEY;
}

void signalHandler(int signal) {
    exit(EXIT_SUCCESS);
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0)        exit(EXIT_FAILURE);
    if (pid > 0)        exit(EXIT_SUCCESS);
    if (setsid() < 0)   exit(EXIT_FAILURE);

    pid = fork();
    if (pid < 0)        exit(EXIT_FAILURE);
    if (pid > 0)        exit(EXIT_SUCCESS);

    umask(0);

    if (chdir("/") < 0) {
        std::cerr << "Error: chdir failed." << std::endl;
        exit(EXIT_FAILURE);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
}

#ifdef USESSL
class SlackStatusSender {
private:
    boost::asio::io_context ioc_;
    ssl::context ctx_;
    boost::asio::ip::tcp::resolver resolver_;
    std::string hostname_;
    std::string target_;
    boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> host_resolved_;
public:
    SlackStatusSender(const std::string hostname, const std::string target)
            :ioc_{}
            ,ctx_{ssl::context::tlsv12_client}
            ,resolver_(ioc_)
            ,hostname_{hostname}
            ,target_{target} {
        ctx_.set_default_verify_paths();
        host_resolved_ = resolver_.resolve(hostname, "https");
    }

    void send(const std::string & message)  {

        beast::ssl_stream<beast::tcp_stream> stream_(ioc_, ctx_);
        auto _ = beast::get_lowest_layer(stream_).connect(host_resolved_);
        stream_.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, target_, 11};
        req.set(http::field::host, hostname_);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");

        req.body() = "{\"text\":" "\"" + message + "\"}";

        req.prepare_payload();

        http::write(stream_, req);

        beast::flat_buffer buffer;
        http::response<http::dynamic_body> res;
        http::read(stream_, buffer, res);

        beast::error_code ec;
        stream_.shutdown(ec);
    }
};
#endif



#ifdef CRYPTO_USESSL

#include <boost/crypto.hpp>
std::string aesEncrypt(const std::string& plaintext, const std::string& key, const std::string& iv) {
    namespace crypto = boost::crypto;

    // Ensure the key and IV are the correct size
    if (key.size() != crypto::aes::key_size<256>::value) {
        throw std::runtime_error("Key must be 256 bits (32 bytes).");
    }
    if (iv.size() != crypto::aes::block_size::value) {
        throw std::runtime_error("IV must be 16 bytes.");
    }

    // Create the AES encryption object in CBC mode
    crypto::aes::cbc_encryptor encryptor(
        std::vector<uint8_t>(key.begin(), key.end()),
        std::vector<uint8_t>(iv.begin(), iv.end())
    );

    // Perform encryption
    std::vector<uint8_t> input(plaintext.begin(), plaintext.end());
    std::vector<uint8_t> encrypted = encryptor.process(input);

    return std::string(encrypted.begin(), encrypted.end());
}

int main() {
    try {
        // Example input
        std::string plaintext = "Hello, AES encryption!";
        std::string key = "0123456789abcdef0123456789abcdef"; // 32-byte key (256-bit)
        std::string iv = "abcdef1234567890";                  // 16-byte IV

        // Encrypt the plaintext
        std::string encrypted = aesEncrypt(plaintext, key, iv);

        // Print encrypted data in hex format
        std::cout << "Encrypted: ";
        for (unsigned char c : encrypted) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
        std::cout << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
#endif

uint64_t time_current_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t time_current_milliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void
tick(boost::asio::steady_timer& timer, int seconds)
{
    timer.expires_after(std::chrono::seconds(seconds));
    std::cout << "tick:: " << time_current_milliseconds() << std::endl;
    timer.async_wait([&, seconds](auto) { tick(timer, seconds); });
}


std::string exec_command(const char * cmd) {
    std::string result;

    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd, "r"), static_cast<int(*)(FILE*)>(pclose));
    if (!pipe) {
        throw std::runtime_error("Cannot run command [" + std::string(cmd) + "]");
    }

    for (std::array<char, 1024> buffer{}; fgets(buffer.data(), buffer.size() - 1, pipe.get()) != nullptr;) {
        result += std::string(buffer.data());
    }
    return result;
}

/** string & json tools */
std::string trim(const std::string &str) {
    auto blanks = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(blanks);
    if (start == std::string::npos) {
        return "";
    }
    size_t end = str.find_last_not_of(blanks);
    return str.substr(start, end - start + 1);
}


std::string join(const std::vector<std::string> & parts, const std::string & delemiter) {
    return std::accumulate(std::next(parts.begin()), parts.end(), parts[0],
                           [&delemiter](std::string a, std::string b) {
                               return a + delemiter + b;
                           }
    );
}

std::vector<std::string> split(const std::string & line, const std::string & separator) {
    std::vector<std::string> result;
    boost::split(result, line, boost::is_any_of(separator), boost::token_compress_on);

    return result;
}

std::vector<std::string> split_trim(const std::string & line, const std::string & separator) {
    std::vector<std::string> result;
    boost::split(result, line, boost::is_any_of(separator), boost::token_compress_on);
    return result;
}

std::string as_json_string(std::vector<std::pair<std::string, std::string>> & fields) {
    std::vector<std::string> data;
    for (const auto& item : fields) {
        auto name = item.first;
        auto value = item.second;
        boost::replace_all(value, "\"", "\\\"");

        auto result_str = "\"" + name + "\"";
        result_str += ":";
        result_str += "\"" + value + "\"";
        data.push_back(result_str);
    }
    auto result =  join(data, ",");
    return "{" + result + "}";
}

std::string actual_hostname;
std::string get_hostname() {
    if (!actual_hostname.empty()) {
        return actual_hostname;
    }
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        actual_hostname = std::string(hostname);
    }
    return actual_hostname;
}


std::string inject_common_data(const std::string & action, const std::string & data) {
    return "{"
           "\"action\":   \"monitor-" + action + "\""
            ",\"appkey\":"  "\"" + get_app_key() + "\""
            ",\"host\":"  "\"" + get_hostname() + "\""
            ",\"time\":"  "\"" + std::to_string(time(0)) + "\""
            ",\"data\":" + data +
           "}";
}


struct NetworkStats {
    std::string name;
    unsigned long rx_bytes;
    unsigned long tx_bytes;
};


std::vector<NetworkStats> network_start_status;
uint64_t time_network_start;

std::vector<NetworkStats> getNetworkStats() {
    std::ifstream net_dev("/proc/net/dev");
    std::vector<NetworkStats> result;
    bool reade_for_process = false;
    std::string line;

    while (std::getline(net_dev, line)) {
        if (! reade_for_process) {
            reade_for_process = line.find("compressed") != std::string::npos;
            continue;
        }
        char name[31];
        unsigned long rx_bytes, tx_bytes;
        std::sscanf(line.c_str(), "%30s %lu %*s %*s %*s %*s %*s %*s %*s %lu", name, &rx_bytes, &tx_bytes);
        result.push_back(NetworkStats{name, rx_bytes, tx_bytes});
    }

    return result;
}


std::string network_usage() {
    std::vector<std::string>  result;

    if (network_start_status.empty()) {
        network_start_status = getNetworkStats();
        time_network_start = time_current_milliseconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    auto network_stop_status = getNetworkStats();
    auto time_network_stop = time_current_milliseconds();
    float duration = (time_network_stop - time_network_start) / 1000;

    time_network_start = time_network_stop;

    for (ulong i=0; i<network_start_status.size(); i++) {
        auto n1 = network_start_status[i];
        auto n2 = network_stop_status[i];
        auto name = n1.name;
        if (':' == name.back()) {
            name.pop_back();
        }
        result.push_back(
          "{ \"name\":\""       + n1.name + "\""
        + ", \"read\":"         + std::to_string((n2.rx_bytes - n1.rx_bytes) / duration)
        + ", \"transfer\":"     + std::to_string((n2.tx_bytes - n1.tx_bytes) / duration) + "}");
    }
    return "[" + join(result, ",") + "]";
}

std::string network_usage_report(std::string hostname) {
    auto net = network_usage();
    return inject_common_data("net", net);
}


std::string get_memory_info() {
    std::ifstream file("/proc/meminfo");
    int count_processed_lines = 20;
    std::vector<std::string> result;

    for (int i = 0; i < count_processed_lines; i++) {
        std::string line;
        std::getline(file, line);
        std::istringstream iss(line);

        std::string name;
        unsigned long long value;
        iss >> name >> value;
        if (':' == name.back()) {
            name.pop_back();
        }
        result.push_back("{\"" + name + "\":" + std::to_string(value) + "}");
    }
    return "[" + join(result, ",") + "]";
}

std::string memory_report(std::string hostname) {
    auto memory = get_memory_info();
    return inject_common_data("memory", memory);
}

std::string get_uptime() {
    std::ifstream file("/proc/uptime");
    std::string line;
    std::getline(file, line);
    std::istringstream iss(line);

    unsigned long long uptime, idletime;
    iss >> uptime >> idletime;
    return std::to_string(uptime);
}


struct CPUStats {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;
};


std::vector<CPUStats> read_CPU_stats(int nproc) {
    std::ifstream file("/proc/stat");
    std::vector<CPUStats> result;

    for (int i=0; i< nproc +1; i++) {   // first line which contains overall CPU stats
        std::string line;
        std::getline(file, line);
        std::istringstream iss(line);
        std::string cpu;
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        result.push_back(CPUStats{user, nice, system, idle, iowait, irq, softirq, steal});
    }
    return result;
}

float calculate_CPU_usage(const CPUStats& start, const CPUStats& end) {
    auto prevIdle = start.idle + start.iowait;
    auto idle = end.idle + end.iowait;
    auto prevNonIdle = start.user + start.nice + start.system + start.irq + start.softirq + start.steal;
    auto nonIdle = end.user + end.nice + end.system + end.irq + end.softirq + end.steal;

    auto prevTotal = prevIdle + prevNonIdle;
    auto total = idle + nonIdle;

    auto totald = total - prevTotal;
    auto idled = idle - prevIdle;

    float cpu_percentage = totald > 0 ? ((totald - idled) / (float)totald) * 100 : 0;
    return cpu_percentage;
}

static int nproc = 0;
std::vector<CPUStats> cpu_start_status;

std::string cpu_usage() {
    std::vector<std::string> result;

    if (nproc <=0) {
        nproc = atoi(exec_command("nproc").c_str());
        std::cout << "nproc:::" << "\n";
        nproc = 1;
    }
    nproc = nproc > 0 ? nproc : 1;
    if (cpu_start_status.empty()) {
        cpu_start_status = read_CPU_stats(nproc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    std::vector<CPUStats> cpu_stop_status = read_CPU_stats(nproc);

    for (int i=0; i<nproc+1; i++) {
        result.push_back(std::to_string(calculate_CPU_usage(cpu_start_status[i], cpu_stop_status[i])));
    }

    return "[" + join(result, ",") + "]";
}

std::string get_loadavg() {
    double ld[3];
    getloadavg(ld, 3);
    return "[" + std::to_string(ld[0]) + "," + std::to_string(ld[1]) + "," + std::to_string(ld[2]) + "]";
}

std::string cpu_usage_report(std::string & hostname) {
    auto cpu_data = cpu_usage();
    auto loadavg = get_loadavg();
    auto uptime = get_uptime();

    auto data = "{"
                        "\"cpu\":" + cpu_data + ""
                        ",\"uptime\":"    + uptime + ""
                        ",\"loadavg\":"   + loadavg
                     + "}";

    return inject_common_data("cpu", data);
}


std::string disk_space(std::string & mount_point_str) {
    std::vector<std::string> result;

    std::vector<std::string> mount_points = split_trim(mount_point_str.empty() ? "/" : mount_point_str, ";");

    for (auto path : mount_points) {
        struct statvfs buf;
        if (statvfs(path.c_str(), &buf) == 0) {
            auto total = buf.f_blocks * buf.f_frsize;
            auto free = buf.f_bfree * buf.f_frsize;
            auto used = total - free;
            auto usage = used * 100 / total;

            result.push_back("{"
                               "\"mounted\":" "\"" + path + "\""
                             + ",\"total\":" + std::to_string(total)
                             + ",\"free\":" + std::to_string(free)
                             + ",\"usage\":" + std::to_string(usage)
                             + "}");
        }
    }
    return inject_common_data("disk", "[" + join(result, ",") + "]");
}


std::vector<std::pair<std::string, std::string>>
parse_command_line_ps(const std::string & input, int marker, const std::vector<std::string> & names) {
    std::istringstream iss(input);
    std::string part;
    std::vector<std::string> parts;
    std::vector<std::string> command_line_parts;
    std::vector<std::pair<std::string, std::string>> param_values;

    for (int i=0; i<100; i++) {
        if (iss >> part) {
            if (i < marker) {
                auto value = part;
                param_values.push_back(std::pair<std::string, std::string>(names[i], part));

            } else {
                boost::replace_all(part, "\\", "\\\\");
                command_line_parts.push_back(part);
            }
        }
    }
    if (!command_line_parts.empty()) {
        auto cmd = join(command_line_parts, " ");
        param_values.push_back(std::pair<std::string, std::string>(names[marker], cmd));
    }

    return param_values;
}

std::vector<std::pair<std::string, int>>
command_line_blanks_posit(const std::string & input,
                          const std::vector<std::string> & names) {
    std::vector<std::pair<std::string, int>> result;

    for (auto name : names) {
        int idx = input.find(name);
        result.push_back(std::pair<std::string, int>(name, idx));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>>
command_line_blanks_posit_parse(const std::string & input, std::vector<std::pair<std::string, int>> & names) {
    std::vector<std::pair<std::string, std::string>> result;

    std::string param_name = "";
    int start = -1;

    for (auto item : names) {
        auto name = item.first;
        int idx = item.second;
        if (start != -1) {
            auto value = trim(input.substr (start, idx - start));
            result.push_back(std::pair<std::string, std::string>(param_name, value));
            param_name = name;
            start = idx;
            continue;
        }
        start = idx;
        param_name = name;
    }
    result.push_back(std::pair<std::string, std::string>(param_name, input.substr (start, input.size())));
    return result;
}

std::vector<std::string> file_as_lines(std::string & filename) {
    std::vector<std::string> result;

    std::ifstream file(filename);
    if (!file.is_open()) {
        return result;
    }
    std::string line;
    while (std::getline(file, line)) {
        result.push_back(line);
    }

    file.close();
    return result;
}

void kill_process(int current_pid) {
    auto command = "kill -9 " + std::to_string(current_pid);
    auto ps_info = exec_command(command.c_str());
}


class Agent {
private:
    boost::asio::ip::udp::endpoint endpoint_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::system_timer timer_;
    unsigned timeout_;
    bool print_sent_message_;
    std::string params_;
    std::string slack_app_url_;

    std::string message_;
    boost::unordered_map<std::string, uint64_t> times_;
    boost::unordered_map<std::string, uint64_t> timeouts_;
    boost::unordered_map<std::string, std::string> extra_params_;

#ifdef USESSL
    SlackStatusSender *  slackPost = nullptr;
#endif
    std::unordered_map<std::string, std::time_t> message_sent_map_;

public:
    Agent(boost::asio::io_context& ioc,
                           unsigned timeout_sec,
                           const boost::asio::ip::address& multicast_address,
                           short unsigned port,
                           const std::string & params,
                           const std::string & slack_app_url,
                           bool print_sent_message)
            : endpoint_{multicast_address, port}
            , socket_{ioc, endpoint_.protocol()}
            , timer_(ioc)
            , timeout_{timeout_sec}
            , slack_app_url_{slack_app_url}
            , print_sent_message_{print_sent_message} {

        if (params.find("--actions=") != std::string::npos) {
            params_ = params.substr(strlen("--actions="), 1000);
        }
        std::cout << "Version: " << VERSION << "\n";
        std::cout << "Statistics endpoint: " << multicast_address << ":" << port << "\n";
        std::cout << "Requested statistics:\n";

        for (auto & name_str: split_trim(params_, ",")) {
            auto param_val = split_trim(name_str, ":");
            std::string name;
            auto param_printed = false;
            if (param_val.size() == 2) {
                name = param_val[0];
                uint64_t value = param_val.size() > 1 ? atoi(param_val[1].c_str()) : timeout_;
                timeouts_[name] = value;
                times_[name] = 0;
                auto separator = name.size() > strlen("disk") ? "\t" : "\t\t";
                std::cout << "  " << name << ":" << separator << value << " seconds";
                param_printed = true;
            }
            if (name_str.find("[") != std::string::npos && name_str.find("]") != std::string::npos) {
                auto pozit = name_str.find("[");
                auto extra_params = name_str.substr(pozit + 1, name_str.size() - pozit -2);
                if (name.empty()) {
                    name = name_str.substr(0, pozit);
                }
                extra_params_[name] = extra_params;
                if (! param_printed) {
                    auto separator = name.size() > strlen("disk") ? "\t" : "\t\t";
                    auto comment = name == "dailyreport" ? "\t\t" : " seconds (default)";
                    std::cout << "  " << name << ":" << separator << timeout_ << comment;
                }
                std::cout << "\t[" << extra_params << "]";
            }
            std::cout << "\n";
        }

        send_message_and_next(compose_message("start"));
    }

    bool need_debounce_message(std::string & message, int interval) {
        if (message_sent_map_.find(message) != message_sent_map_.end()) {
            time_t now =  std::time(nullptr);
            time_t lastSent = message_sent_map_[message];
            auto real_interval = interval == -1 ? DEBOUNCE_TIME_SEC : interval;
            if (now - lastSent < real_interval) {
                return true;
            }
        }
        return false;
    }

    void register_message(std:: string & message) {
        message_sent_map_[message] = std::time(nullptr);
    }

#ifdef USESSL
    void send_message(SlackStatusSender * sender, std::string & message) {
        send_message(sender, message, -1);
    }

    void send_message(SlackStatusSender * sender, std::string & message, int interval) {
        if (!need_debounce_message(message, interval)) {
            sender->send(message);
            register_message(message);
        }
    }

    SlackStatusSender * getSlackInfraChannel() {
        if (slackPost == nullptr) {
            slackPost = new SlackStatusSender("hooks.slack.com", slack_app_url_);
        }
        return slackPost;
    }
#endif
    std::vector<std::string>
    process_lines(const std::vector<std::string> & lines, const std::vector<std::string> & names) {
        std::vector<std::string> result;
        auto idxs = command_line_blanks_posit(lines.at(0), names);
        for (ulong n = 1; n < lines.size(); n++) {
            auto & line = lines.at(n);
            if (!line.empty()) {
                auto data = command_line_blanks_posit_parse(line, idxs);
                result.emplace_back(as_json_string(data));
            }
        }
        return result;
    }

    std::vector<std::string>
    process_lines_by_blank(const std::vector<std::string> & lines, const std::vector<std::string> & names) {
        std::vector<std::string> result;

        for (const auto & line : lines) {
            if (!line.empty()) {
                auto values = split_trim(line, " ");
                std::vector<std::pair<std::string, std::string>> data;
                if (values.size() > names.size()) {
                    data.emplace_back(std::pair<std::string, std::string>("ERROR", line));
                    continue;
                }
                for (auto i = 0; i < values.size(); i++) {
                    auto value = values[i];
                    auto name = i < names.size() ? names[i] : "";
                    data.emplace_back(std::pair<std::string, std::string>(name, value));
                }
                result.push_back(as_json_string(data));
            }
        }
        return result;
    }

    std::string unpack_iftop_line( std::vector<std::string> & params,
                                   std::vector<std::string> & names,
                                   std::string direction) {
        std::vector<std::string> tmp;
        for (auto i=0; i< params.size(); i++) {

            if(i >= names.size()) {
                break;
            }
            auto & value = params[i];
            auto & name = names[i];
            if (name == "-") {
                continue;
            }
            if (name == "TXRX") {
                value = direction;
            }

            tmp.push_back("{\"" + name + "\":\"" + value + "\"}");
        }
        return join(tmp, ",");
    }

    std::string iftop_usage() {
        std::string filename = "iftop-result.txt";

        std::vector<std::string> result;
        std::vector<std::string> tx_names{"-", "-", "IP", "TXRX", "2sec", "10sec", "40sec", "cumulative"};
        std::vector<std::string> rx_names{"-", "IP", "TXRX", "2sec", "10sec", "40sec", "cumulative"};
        std::vector<std::string> total_names{"2sec", "10sec", "40sec"};

        auto started = 0;
        bool first_parsed_line = true;

        for (auto line: file_as_lines(filename)) {
            if(line.size() > 2  && line[0] == '=') {
                break;
            }
            if(line.size() > 2  && line[0] == '-') {
                started += 1;
                continue;
            }
            if (started == 0) {
                continue;
            }
            std::vector<std::string> params = split(line, " ");
            std::string item;
            if (started == 1) {
                auto item_regular = first_parsed_line
                        ? unpack_iftop_line(params, tx_names, std::string("TX"))
                        : unpack_iftop_line(params, rx_names, std::string("RX"));
                first_parsed_line = !first_parsed_line;
                item = "[" + item_regular + "]";
            }
            if (started >= 2) {
                std::vector<std::string> base_params = split(line, ":");
                if (base_params.size() > 1) {
                    params = split(base_params[1], " ");
                    auto item_total = unpack_iftop_line(params, total_names, std::string(""));
                    item = "{\"" + trim(base_params[0]) + "\":[" + item_total + "]}";
                }
            }
            result.push_back(item);
        }
        return inject_common_data("iftop", "[" + join(result, ",") + "]");
    }

    std::string compose_message(std::string message_type) {
        auto hostname = get_hostname();
        std::string action = message_type;
        const char * command = nullptr;

        times_[message_type] = time_current_seconds();

        if (message_type == "start") {
            return cpu_usage_report(hostname);
        }
        if (message_type == "cpu" ) {
            return cpu_usage_report(hostname);
        }
        if (message_type == "iftop") {
            return iftop_usage();
        }
        if (message_type == "net" ) {
            return network_usage_report(hostname);
        }
        if (message_type == "memory" ) {
            return memory_report(hostname);
        }
        if (message_type == "disk") {
            return disk_space(extra_params_[message_type]);
        }
        if (message_type == "hostinfo") {
            auto host_info = exec_command("hostnamectl");
            std::vector<std::pair<std::string, std::string>> data;
            if (!host_info.empty()) {
                for (const auto & line : split(host_info, "\n")) {
                    if (!line.empty()) {
                        auto params = split(line, ":");
                        if (params.size() >= 2) {
                            auto name = trim(params[0]);
                            auto value = trim(params[1]);
                            data.emplace_back(std::pair<std::string, std::string>(name, value));
                        }
                    }
                }
                auto result = as_json_string(data);
                return inject_common_data(action, result);
            }
        }
        if (message_type == "tcp") {
            auto posts_info = exec_command("ss  -tlnp"); // TCP ... UDP - -tulnp
            if (!posts_info.empty()) {
                std::vector<std::string> res;
                boost::replace_all(posts_info, " Address", ":Address");
                const auto names = split("State,Recv-Q,Send-Q,Local:Address:Port,Peer:Address:Port,Process,Unknown1,Unknown2m,Unknown3", ",");
                const auto lines = split(posts_info, "\n");

                auto result = process_lines_by_blank(lines, names);
                return inject_common_data(action, "[" + join(result, ",") + "]");
            }
        }
        if (message_type == "docker") {
            auto docker_info = exec_command("docker ps --no-trunc -a");
            if (!docker_info.empty()) {
                std::vector<std::string> res;
                boost::replace_all(docker_info, "CONTAINER ID", "CONTAINER_ID");
                const auto names = split("CONTAINER_ID,IMAGE,COMMAND,CREATED,STATUS,PORTS,NAMES", ",");
                const auto lines = split(docker_info, "\n");

                auto result = process_lines(lines, names);
                return inject_common_data(action, "[" + join(result, ",") + "]");
            }
        }
        if (message_type == "iostat") {
            // TBD
        }
        if (message_type == "ioping") {
            // TBD - used together with 'df'
        }
        if (message_type == "date") {
            std::vector<std::string> result;
            std::vector<std::pair<std::string, std::string>> date_status;

            std::string mount_point_str = extra_params_[message_type];
            std::vector<std::string> hosts = split_trim(mount_point_str, ";");
            auto const cmd = "date +'%Y-%m-%d %H:%M:%S.%4N %Z %:z'";
            auto resp = exec_command(cmd);
            resp.pop_back();   // remove \n
            return inject_common_data(action, "\"" + resp + "\"");
        }
        if (message_type == "host") {
            std::vector<std::string> result;
            std::vector<std::pair<std::string, std::string>> host_status;

            std::string mount_point_str = extra_params_[message_type];
            std::vector<std::string> hosts = split_trim(mount_point_str, ";");
            for (auto const & host : hosts) {
                auto const cmd = "netcat -zvw1 " + host + " 22 2>&1";
                auto host_resp = exec_command(cmd.c_str());
                std::string status = "OK";
#ifdef USESSL
                if (host_resp.find("succeeded") == std::string::npos) {
                    auto message = ":fire: :fire: :fire: *ERROR* HOST " + host + " *DOWN*";
                    send_message(getSlackInfraChannel(), message);
                    status = "ERROR";
                }
#endif
                host_status.push_back(std::pair<std::string, std::string>(host, status));
            }
            result.push_back(as_json_string(host_status));
            auto rs = result.empty() ? "" : join(result, ",");
            return inject_common_data(action, "[" + rs + "]");
        }
        if (message_type == "python") {
            command = "ps -eo pcpu,pmem,pid,ppid,user,etime,command | grep python";
            action = "python";
            message_type = "_ps";
        }
        if (message_type == "xtest") {
            command = "ps -eo pcpu,pmem,pid,ppid,user,etime,command | grep XTEST";
            action = "xtest";
            message_type = "_ps";
        }
        if (message_type == "ps") {
            command = "ps -eo pcpu,pmem,pid,ppid,user,etime,command";
            action = "ps";
            message_type = "_ps";
        }
        if (message_type == "_ps") {
            auto ps_info = exec_command(command);
            const auto names = split("PCPU,PMEM,PID,PPID,USER,ELAPSED,COMMAND", ",");
            const auto lines = split(ps_info, "\n");

            auto current_pid = -1;
            std::vector<std::string> result;
            for (ulong n = 0; n < lines.size(); n++) {
                auto & line = lines.at(n);
                if (!line.empty()) {
                    auto data = parse_command_line_ps(line, 6, names);
                    result.push_back(as_json_string(data));
                    if (action == "kill_cpu" || action == "kill_mem") {
                        for (auto &p: data) {
                            if (p.first == "PID") {
                                current_pid = atoi(p.second.c_str());
                                break;
                            }
                        }
                    }
                    if (action == "kill_cpu") {
                        auto max_cpu_bound = atoi(extra_params_["kill_cpu"].c_str());
                        if (max_cpu_bound > 1) { // minimal value just for avoiding error stopped
                            for (auto &p: data) {
                                if (p.first == "PCPU") {
                                    auto cpu_usage = atoi(p.second.c_str());
                                    if (cpu_usage > max_cpu_bound) {
                                        kill_process(current_pid);
                                        auto message =
                                                ":fire: *Attention* Python Process [" + std::to_string(current_pid)
                                                + "] was kill because CPU " + std::to_string(cpu_usage)
                                                + "% exceed the limit " + std::to_string(max_cpu_bound) +
                                                "%; Process info: " + line;
#ifdef USESSL
                                        send_message(getSlackInfraChannel(), message);
#endif
                                    }
                                }
                            }
                        }
                    }
                    if (action == "kill_mem") {
                        auto max_mem_bound = atoi(extra_params_["kill_mem"].c_str());
                        if (max_mem_bound > 1) {
                            for (auto &p: data) {
                                if (p.first == "PMEM") {
                                    auto mem_usage = atoi(p.second.c_str());
                                    if (mem_usage > max_mem_bound) {
                                        kill_process(current_pid);
                                        auto message =
                                                ":fire: *Attention* Python Process [" + std::to_string(current_pid)
                                                + "] was kill because MEMORY usage " + std::to_string(mem_usage)
                                                + "% exceed the limit " + std::to_string(max_mem_bound) +
                                                "%; Process info: " + line;
#ifdef USESSL
                                        send_message(getSlackInfraChannel(), message);
#endif

                                    }
                                }
                            }
                        }
                    }

                }
            }


            auto rs = result.empty() ? "" : join(result, ",");
            return inject_common_data(action, "[" + rs + "]");
        }
        if (message_type == "df") {
            auto df_info = exec_command("df -l");
            const auto lines = split(df_info, "\n");
            auto header_line = lines.at(0);
            boost::replace_all(header_line, "Mounted on", "Mounted_on");
            auto names = split_trim(header_line, " ");
            std::string mount_point_str = extra_params_[message_type];
            std::vector<std::string> mount_points = split_trim(mount_point_str, ";");

            std::vector<std::string> result;
            for (ulong i=1; i<lines.size(); i++) {
                if (lines.at(i).empty()) {
                    break;
                }

                std::vector<std::pair<std::string, std::string>> data;
                auto values = split_trim(lines.at(i), " ");
                if (!mount_points.empty()) {
                    auto & mount_point = values[5];
                    if (count(mount_points.begin(), mount_points.end(), mount_point) <= 0) {
                        continue;   // skip this disk
                    }
                }
                for (ulong j=0; j<names.size(); j++) {
                    data.push_back(std::pair<std::string, std::string>(names[j], values[j]));
                }
                result.push_back(as_json_string(data));
            }
            return inject_common_data(action, "[" + join(result, ",") + "]");
        }
        return "INVALID MODE";
    }

    void send_message(std::string message) {
        if (print_sent_message_) {
            std::cout << message << "\n\n";
        }
        socket_.async_send_to(boost::asio::buffer(message), endpoint_,
                              [](const boost::system::error_code& ec, size_t bytes_recvd){ handle_and_do_noting(ec, bytes_recvd); });
    }

    void send_message_and_next(std::string message) {
        std::cout << "start sending data....\n";
        if (print_sent_message_) {
            std::cout << message << "\n\n";
        }
        socket_.async_send_to(boost::asio::buffer(message), endpoint_,
                              [this](const boost::system::error_code& ec, size_t bytes_recvd){ handle_send_to(ec, bytes_recvd); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1 * 1000));
    }

    static void handle_and_do_noting(const boost::system::error_code& ec, size_t bytes_recvd) {
    }

    void handle_send_to(const boost::system::error_code& ec, size_t bytes_recvd) {
        if (!ec) {
            timer_.expires_after(std::chrono::seconds{timeout_});
            timer_.async_wait([this](const boost::system::error_code &ec) { handle_timeout(ec); });
        }
        else {
            std::cout << "ERROR (" << ec << ")\n";
        }
    }

    bool need_start(std::string action, uint64_t current_time) {
        if (timeouts_.find(action) != timeouts_.end()) {
            auto last_started = times_[action];
            auto timeout = timeouts_[action];
            return last_started == 0 || last_started + timeout  < current_time;
        }
        return false;
    }

    void check_send_daily_report() {
        std::string message = "dailyreport";
        if (extra_params_.find(message) != extra_params_.end()) {
            std::string dailyreport_str = extra_params_[message];
            std::vector<std::string> timeparts = split_trim(dailyreport_str, "-");
            int hour =  8;
            int min =  0;
            if (timeparts.size() == 2) {
                try {
                    hour = atoi(timeparts[0].c_str());
                    min  = atoi(timeparts[1].c_str());
                }
                catch (const std::exception &e) {
                }

                std::time_t current_time = std::time(nullptr);
                std::tm *utc_time = std::gmtime(&current_time);

                if (utc_time->tm_hour == hour && utc_time->tm_min >= min) {
                    auto hostname = get_hostname();
#ifdef USESSL
                    auto msg = std::string(":large_green_circle: *HOST AGENT OK* Host: [" + hostname + "]");
                    send_message(getSlackInfraChannel(), msg, 60 * 60 * 24);
#endif
                }
            }
        }
    }

    void handle_timeout(const boost::system::error_code& ec) {
        int cnt = 0;
        while (true) {
            uint64_t current_time = time_current_seconds();
            if (need_start("disk", current_time))       send_message(compose_message("disk"));
            if (need_start("docker", current_time))     send_message(compose_message("docker"));
            if (need_start("ps", current_time))         send_message(compose_message("ps"));
            if (need_start("df", current_time))         send_message(compose_message("df"));
            if (need_start("tcp", current_time))        send_message(compose_message("tcp"));
            if (need_start("net", current_time))        send_message(compose_message("net"));
            if (need_start("memory", current_time))     send_message(compose_message("memory"));
            if (need_start("hostinfo", current_time))   send_message(compose_message("hostinfo"));
            if (need_start("iftop", current_time))      send_message(compose_message("iftop"));
            if (need_start("host", current_time))       send_message(compose_message("host"));
            if (need_start("date", current_time))       send_message(compose_message("date"));
            if (need_start("python", current_time))     send_message(compose_message("python"));
            if (need_start("xtext", current_time))      send_message(compose_message("xtext"));
            if (need_start("cpu", current_time))        send_message(compose_message("cpu"));
            if (need_start("kill_cpu", current_time))   send_message(compose_message("kill_cpu"));
            if (need_start("kill_mem", current_time))   send_message(compose_message("kill_mem"));

            std::this_thread::sleep_for(std::chrono::milliseconds(1 * 500));
            if (++cnt > 10000000) {
                cnt = 0;
            }
            if (cnt % 120 == 0) {
                check_send_daily_report();
            }
        }
    }
};

void usage_massage(const char * pname, const char * example) {
    std::cerr << "VERSION: [" << VERSION  << "]\n";
    std::cerr << "USAGE:" << pname << " <multicast group / UDP IP > <port> <default_timeout_sec> [--actions=\"cpu:N,disk:N[<mount1;mount2;..>],df:N[<mount1;..>],ps:N,python:N,iftop:N,hostinfo:N,docker:N\", ...]  [-d | -p]\n";
    std::cerr << "FULL EXAMPLE: " << pname << " 127.0.0.1 6666 60 --actions=" << example << "\n";
}

int main(int argc, char* argv[]) {
    auto params = "\"cpu:5,disk:120[/],df:120[/;/data],dailyreport[7-00],ps:30,net:20,memory:30,python:30,tcp:30,hostinfo,docker:120,kill_mem:60[20]\"";
    auto apikey = "";
    auto securekey = "";
    auto slack_path = "";
    auto sysroot = "/";
    try {
        if (argc < 4) {
            usage_massage(argv[0], params);
            return EXIT_FAILURE;
        }
        auto address = argv[1];
        auto port = argv[2];
        auto timeout_sec = argv[3];
        bool need_daemonize = false;
        bool self_print = false;

        for (auto i=4; i<argc; i++) {
            if(std::string(argv[i]).find("--actions=") == 0) {
                params = argv[i];
                continue;
            }
            if(std::string(argv[i]).find("--apikey=") == 0) {
                apikey = argv[i] + strlen("--apikey=");
                APP_KEY = std::string(apikey);
                continue;
            }
            if(std::string(argv[i]).find("--securekey=") == 0) {
                securekey = argv[i] + strlen("--securekey=");
                CRYPTO_KEY = std::string(argv[i] + strlen("--securekey="));
                continue;
            }
            if(std::string(argv[i]).find("--slackpath=") == 0) {
                slack_path = argv[i] + strlen("--slackpath=");
                SLACK_PATH = std::string(slack_path);
                continue;
            }
            if(std::string(argv[i]).find("--sysroot=") == 0) {
                sysroot = argv[i] + strlen("--sysroot=");
                SYSTEM_ROOT = std::string(sysroot);
                continue;
            }

            if (std::string(argv[i]) == "-d") {
                need_daemonize = true;
                continue;
            }
            if (std::string(argv[i]) == "-p") {
                self_print = true;
                continue;
            }
            std::cout << "unrecognised key '" << std::string(argv[5]) << "'\n";
            usage_massage(argv[0], params);
            return EXIT_FAILURE;
        }

        std::cout << "\"" << argv[0] << "\" started. Version: " << VERSION << "\n";
        std::cout << "address: [" << address << "]   port: [" << port << "] default timeout: [" << timeout_sec << "] sec; " <<  params << "\n";

        if (need_daemonize) {
            std::cout << "Run as daemon...\n";
            daemonize();
        }

        boost::asio::io_context ioc;

        auto timer = boost::asio::steady_timer(ioc);
        tick(timer, 10);

        Agent agent(ioc,
                     atoi(timeout_sec),
                     boost::asio::ip::make_address(address),
                     (short unsigned)atoi(port),
                     params,
                     SLACK_PATH,
                     self_print);
        ioc.run();
    }
    catch (std::exception& e) {
        std::cerr << "std::exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/*
sudo apt-get update
sudo apt-get install libssl-dev
g++ -o https_post https_post.cpp -lboost_system -lboost_filesystem -lssl -lcrypto -lboost_thread -lpthread
*
**/
