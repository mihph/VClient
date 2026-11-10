#include "auth.h"
#include "client.h"
#include "communicator.h"
#include "config.h"
#include "interface.h"
#include "reader.h"
#include "writer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

// ======================================================================
//  Тестовый каркас
// ======================================================================

struct TestFailure : std::runtime_error {
    explicit TestFailure(const std::string& m) : std::runtime_error(m) {}
};

struct TestCase {
    std::string group, title, input, expected;
    void (*fn)();
};

static std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* g, const char* t, const char* in, const char* ex, void (*f)()) {
        registry().push_back({g, t, in, ex, f});
    }
};

#define CONCAT2(a, b) a##b
#define CONCAT(a, b) CONCAT2(a, b)
#define TEST_CASE(group, title, input, expected)                                          \
    static void CONCAT(tc_, __LINE__)();                                                  \
    static const Registrar CONCAT(reg_, __LINE__)(group, title, input, expected,          \
                                                  &CONCAT(tc_, __LINE__));                \
    static void CONCAT(tc_, __LINE__)()

template <class T, class = void> struct Streamable : std::false_type {};
template <class T>
struct Streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T> std::string show(const T& v) {
    if constexpr (Streamable<T>::value) {
        std::ostringstream o;
        o << v;
        return o.str();
    } else {
        return "<значение>";
    }
}
template <class T> std::string show(const std::vector<T>& v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + show(v[i]);
    return s + "}";
}

static std::string where(const char* file, int line) {
    return std::string(file) + ":" + std::to_string(line) + ": ";
}

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) throw TestFailure(where(__FILE__, __LINE__) + "CHECK(" #cond ")");   \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        const auto& va_ = (a);                                                            \
        const auto& vb_ = (b);                                                            \
        if (!(va_ == vb_))                                                                \
            throw TestFailure(where(__FILE__, __LINE__) + #a " == " #b ": получено " +    \
                              show(va_) + ", ожидалось " + show(vb_));                    \
    } while (0)

#define CHECK_CONTAINS(text, part)                                                        \
    do {                                                                                  \
        const std::string t_ = (text);                                                    \
        const std::string p_ = (part);                                                    \
        if (t_.find(p_) == std::string::npos)                                             \
            throw TestFailure(where(__FILE__, __LINE__) + "в тексте \"" + t_ +            \
                              "\" не найдено \"" + p_ + "\"");                            \
    } while (0)

#define CHECK_NOT_CONTAINS(text, part)                                                    \
    do {                                                                                  \
        const std::string t_ = (text);                                                    \
        const std::string p_ = (part);                                                    \
        if (t_.find(p_) != std::string::npos)                                             \
            throw TestFailure(where(__FILE__, __LINE__) + "в тексте \"" + t_ +            \
                              "\" найдено лишнее \"" + p_ + "\"");                        \
    } while (0)

#define CHECK_NO_THROW(expr)                                                              \
    do {                                                                                  \
        try {                                                                             \
            expr;                                                                         \
        } catch (const std::exception& e_) {                                              \
            throw TestFailure(where(__FILE__, __LINE__) + #expr " бросило исключение: " + \
                              e_.what());                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_THROWS_MSG(expr, EX, MSG)                                                   \
    do {                                                                                  \
        bool ok_ = false;                                                                 \
        try {                                                                             \
            expr;                                                                         \
        } catch (const EX& e_) {                                                          \
            ok_ = true;                                                                   \
            if (std::string(e_.what()).find(MSG) == std::string::npos)                    \
                throw TestFailure(where(__FILE__, __LINE__) + "текст исключения \"" +     \
                                  e_.what() + "\" не содержит \"" + MSG + "\"");          \
        } catch (const std::exception& e_) {                                              \
            throw TestFailure(where(__FILE__, __LINE__) + "ожидалось " #EX ", получено: " + \
                              e_.what());                                                 \
        }                                                                                 \
        if (!ok_) throw TestFailure(where(__FILE__, __LINE__) + "исключение " #EX " не возникло"); \
    } while (0)

#define CHECK_THROWS(expr, EX) CHECK_THROWS_MSG(expr, EX, "")

// ======================================================================
//  Вспомогательные средства
// ======================================================================

class TempDir {
    std::string dir_;

public:
    TempDir() {
        char tmpl[] = "/tmp/vclient_test_XXXXXX";
        if (!mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
        dir_ = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string path(const std::string& name) const { return dir_ + "/" + name; }
    std::string write(const std::string& name, const std::string& content) const {
        std::ofstream f(path(name), std::ios::binary);
        f << content;
        return path(name);
    }
};

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool fileExists(const std::string& p) { return fs::exists(p); }

class Capture {
    std::ostream& s_;
    std::streambuf* old_;
    std::ostringstream buf_;

public:
    explicit Capture(std::ostream& s) : s_(s), old_(s.rdbuf(buf_.rdbuf())) {}
    ~Capture() { s_.rdbuf(old_); }
    std::string str() const { return buf_.str(); }
};

class EnvGuard {
    std::string name_, old_;
    bool had_;

public:
    explicit EnvGuard(const std::string& n) : name_(n) {
        const char* v = std::getenv(n.c_str());
        had_ = v != nullptr;
        if (v) old_ = v;
    }
    ~EnvGuard() {
        if (had_) setenv(name_.c_str(), old_.c_str(), 1);
        else unsetenv(name_.c_str());
    }
    void set(const std::string& v) { setenv(name_.c_str(), v.c_str(), 1); }
    void unset() { unsetenv(name_.c_str()); }
};

class Args {
    std::vector<std::string> s_;
    std::vector<char*> p_;

public:
    Args(std::initializer_list<std::string> a) {
        s_.push_back("./vclient");
        s_.insert(s_.end(), a.begin(), a.end());
        for (auto& x : s_) p_.push_back(&x[0]);
        p_.push_back(nullptr);
    }
    Args(const Args&) = delete;
    int argc() const { return static_cast<int>(s_.size()); }
    char** argv() { return p_.data(); }
};

static std::string be32(uint32_t v) {
    std::string s(4, '\0');
    s[0] = static_cast<char>(v >> 24);
    s[1] = static_cast<char>(v >> 16);
    s[2] = static_cast<char>(v >> 8);
    s[3] = static_cast<char>(v);
    return s;
}
static uint32_t fromBe32(const std::string& s, size_t off = 0) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(s[off])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[off + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[off + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(s[off + 3]));
}

class Conn {
public:
    int fd;
    explicit Conn(int f) : fd(f) {}
    std::string recvExact(size_t n) {
        std::string r(n, '\0');
        size_t got = 0;
        while (got < n) {
            ssize_t k = ::recv(fd, &r[got], n - got, 0);
            if (k <= 0) throw std::runtime_error("сервер: recv завершился ошибкой или по таймауту");
            got += static_cast<size_t>(k);
        }
        return r;
    }
    std::string recvSome(size_t max = 1024) {
        std::string b(max, '\0');
        ssize_t k = ::recv(fd, &b[0], max, 0);
        if (k < 0) throw std::runtime_error("сервер: recv по таймауту");
        b.resize(static_cast<size_t>(k));
        return b;
    }
    void sendStr(const std::string& s) {
        size_t sent = 0;
        while (sent < s.size()) {
            ssize_t k = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
            if (k <= 0) throw std::runtime_error("сервер: send завершился ошибкой");
            sent += static_cast<size_t>(k);
        }
    }
};

class FakeServer {
    int lfd_ = -1;
    int port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::string err_;

public:
    explicit FakeServer(std::function<void(Conn&)> handler) {
        lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd_ < 0) throw std::runtime_error("socket() не создан");
        int one = 1;
        setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd_, 1) != 0)
            throw std::runtime_error("bind/listen не удались");
        socklen_t len = sizeof a;
        getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);

        th_ = std::thread([this, handler]() {
            try {
                pollfd pfd{lfd_, POLLIN, 0};
                int waited = 0;
                for (;;) {
                    int r = ::poll(&pfd, 1, stop_ ? 0 : 50);
                    if (r > 0) break;
                    if (stop_ || waited >= 5000) return;
                    waited += 50;
                }
                int cfd = ::accept(lfd_, nullptr, nullptr);
                if (cfd < 0) return;
                timeval tv{5, 0};
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                Conn c(cfd);
                try {
                    handler(c);
                } catch (const std::exception& e) {
                    err_ = e.what();
                }
                ::close(cfd);
            } catch (...) {
            }
        });
    }
    FakeServer(const FakeServer&) = delete;
    int port() const { return port_; }
    void finish() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        if (!err_.empty()) throw TestFailure("тестовый сервер: " + err_);
    }
    ~FakeServer() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        if (lfd_ >= 0) ::close(lfd_);
    }
};

static int closedPort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    int p = ntohs(a.sin_port);
    ::close(fd);
    return p;
}

// ======================================================================
//  Названия таблиц (групп тестов)
// ======================================================================

static const char* G_AUTH = "Класс Authenticator: метод computeSHA256(const std::string&, const std::string&)";
static const char* G_CONF = "Класс ConfigManager: метод loadCredentials()";
static const char* G_READ = "Класс DataReader: метод readVectors()";
static const char* G_WRIT = "Класс DataWriter: метод saveResults(const std::vector<int32_t>&)";
static const char* G_COMM = "Класс Communicator: сетевой обмен с сервером (тестовый сервер на 127.0.0.1)";
static const char* G_UI   = "Классы UserInterface и Params: разбор параметров командной строки";
static const char* G_CLNT = "Класс Client: полный сценарий работы (тестовый сервер на 127.0.0.1)";

// ======================================================================
//  Таблица 1. Authenticator
// ======================================================================

TEST_CASE(G_AUTH, "Хеш пустых соли и пароля", "salt=\"\", pass=\"\"",
          "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855") {
    CHECK_EQ(Authenticator::computeSHA256("", ""),
             std::string("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
}

TEST_CASE(G_AUTH, "Эталонный вектор NIST «abc» (соль и пароль склеиваются)", "salt=\"ab\", pass=\"c\"",
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") {
    CHECK_EQ(Authenticator::computeSHA256("ab", "c"),
             std::string("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
}

TEST_CASE(G_AUTH, "Эталонный вектор NIST из двух блоков", "salt=\"abcdbcdecdefdefgefghfghighijhijk\", pass=\"ijkljklmklmnlmnomnopnopq\" (56 символов)",
          "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1") {
    CHECK_EQ(Authenticator::computeSHA256("abcdbcdecdefdefgefghfghighijhijk", "ijkljklmklmnlmnomnopnopq"),
             std::string("248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1"));
}

TEST_CASE(G_AUTH, "Эталонное значение SHA-256(salt + password)", "salt=\"A1B2C3D4E5F60789\", pass=\"TestPass123\"",
          "C7821BB369C931FB97EE4B47173160AF4B8BEB6956139705C6059D07E67F096E") {
    CHECK_EQ(Authenticator::computeSHA256("A1B2C3D4E5F60789", "TestPass123"),
             std::string("C7821BB369C931FB97EE4B47173160AF4B8BEB6956139705C6059D07E67F096E"));
}

TEST_CASE(G_AUTH, "Формат результата: 64 символа, верхний регистр HEX", "salt=\"s\", pass=\"p\"",
          "длина 64, символы 0-9A-F") {
    std::string h = Authenticator::computeSHA256("s", "p");
    CHECK_EQ(h.size(), static_cast<size_t>(64));
    for (char c : h) CHECK((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
}

TEST_CASE(G_AUTH, "Граница дополнения: 55 байт", "salt=\"\", pass=\"a\" × 55",
          "9F4390F8D30C2DD92EC9F095B65E2B9AE9B0A925A5258E241C9F1E910F734318") {
    CHECK_EQ(Authenticator::computeSHA256("", std::string(55, 'a')),
             std::string("9F4390F8D30C2DD92EC9F095B65E2B9AE9B0A925A5258E241C9F1E910F734318"));
}

TEST_CASE(G_AUTH, "Граница дополнения: 56 байт", "salt=\"a\" × 20, pass=\"a\" × 36",
          "B35439A4AC6F0948B6D6F9E3C6AF0F5F590CE20F1BDE7090EF7970686EC6738A") {
    CHECK_EQ(Authenticator::computeSHA256(std::string(20, 'a'), std::string(36, 'a')),
             std::string("B35439A4AC6F0948B6D6F9E3C6AF0F5F590CE20F1BDE7090EF7970686EC6738A"));
}

TEST_CASE(G_AUTH, "Граница дополнения: 64 байта (ровно один блок)", "salt=\"a\" × 64, pass=\"\"",
          "FFE054FE7AE0CB6DC65C3AF9B61D5209F439851DB43D0BA5997337DF154668EB") {
    CHECK_EQ(Authenticator::computeSHA256(std::string(64, 'a'), ""),
             std::string("FFE054FE7AE0CB6DC65C3AF9B61D5209F439851DB43D0BA5997337DF154668EB"));
}

TEST_CASE(G_AUTH, "Длинный вход (1 000 000 байт)", "salt=\"\", pass=\"a\" × 1 000 000",
          "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0") {
    CHECK_EQ(Authenticator::computeSHA256("", std::string(1000000, 'a')),
             std::string("CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0"));
}

TEST_CASE(G_AUTH, "Пустой пароль: хеш равен SHA-256 от одной соли", "salt=\"A1B2C3D4E5F60789\", pass=\"\"",
          "DB8BAD100CF710B197BC9149C2118C4FCD1CEEDBBAB560F3488C807F458570B3") {
    CHECK_EQ(Authenticator::computeSHA256("A1B2C3D4E5F60789", ""),
             std::string("DB8BAD100CF710B197BC9149C2118C4FCD1CEEDBBAB560F3488C807F458570B3"));
}

TEST_CASE(G_AUTH, "Порядок аргументов важен", "(\"12\", \"34\") и (\"34\", \"12\")",
          "хеши различаются: 03AC6742… и F23D089E…") {
    std::string a = Authenticator::computeSHA256("12", "34");
    std::string b = Authenticator::computeSHA256("34", "12");
    CHECK(a != b);
    CHECK_EQ(a, std::string("03AC674216F3E15C761EE1A5E255F067953623C8B388B4459E13F978D7C846F4"));
    CHECK_EQ(b, std::string("F23D089E7F2E05F6FE745BF632B49AA89D9B9ABB01BFE905A9C6F64A554AE57E"));
}

TEST_CASE(G_AUTH, "Детерминированность: одинаковые входы дают одинаковый хеш", "два вызова с salt=\"S\", pass=\"P\"",
          "результаты совпадают") {
    CHECK_EQ(Authenticator::computeSHA256("S", "P"), Authenticator::computeSHA256("S", "P"));
}

TEST_CASE(G_AUTH, "Многобайтовые символы UTF-8 в пароле", "salt=\"Соль\", pass=\"пароль\"",
          "753022D542A505DB6FEA0284FB346B48DAFC776BAE833ED3107C67F425395078") {
    CHECK_EQ(Authenticator::computeSHA256("Соль", "пароль"),
             std::string("753022D542A505DB6FEA0284FB346B48DAFC776BAE833ED3107C67F425395078"));
}

TEST_CASE(G_AUTH, "Нулевой байт внутри данных учитывается", "salt=\"a\", pass=\"\\0b\" (3 байта)",
          "59B271AE1BBCB1D31D41929817F4B16FB439EB4F31520B5AD1D5CE98920A7138") {
    CHECK_EQ(Authenticator::computeSHA256("a", std::string("\0b", 2)),
             std::string("59B271AE1BBCB1D31D41929817F4B16FB439EB4F31520B5AD1D5CE98920A7138"));
}

// ======================================================================
//  Таблица 2. ConfigManager
// ======================================================================

TEST_CASE(G_CONF, "Логин и пароль в одной строке", "Файл: \"user1 pass1\\n\"", "логин \"user1\", пароль \"pass1\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user1 pass1\n"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("user1"));
    CHECK_EQ(cr.second, std::string("pass1"));
}

TEST_CASE(G_CONF, "Логин и пароль на разных строках", "Файл: \"user1\\npass1\\n\"", "логин \"user1\", пароль \"pass1\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user1\npass1\n"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("user1"));
    CHECK_EQ(cr.second, std::string("pass1"));
}

TEST_CASE(G_CONF, "Файл без завершающего перевода строки", "Файл: \"user pass\"", "логин \"user\", пароль \"pass\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user pass"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("user"));
    CHECK_EQ(cr.second, std::string("pass"));
}

TEST_CASE(G_CONF, "Несколько пробелов и табуляция между полями", "Файл: \"  user \\t\\t pass  \\n\"", "логин \"user\", пароль \"pass\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "  user \t\t pass  \n"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("user"));
    CHECK_EQ(cr.second, std::string("pass"));
}

TEST_CASE(G_CONF, "Окончания строк CRLF", "Файл: \"user\\r\\npass\\r\\n\"", "пароль \"pass\" без символа \\r") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user\r\npass\r\n"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("user"));
    CHECK_EQ(cr.second, std::string("pass"));
}

TEST_CASE(G_CONF, "Лишние данные после пароля игнорируются", "Файл: \"u p extra more\\n\"", "логин \"u\", пароль \"p\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "u p extra more\n"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("u"));
    CHECK_EQ(cr.second, std::string("p"));
}

TEST_CASE(G_CONF, "Спецсимволы в пароле", "Файл: \"user P@ssW0rd!#$\"", "пароль \"P@ssW0rd!#$\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user P@ssW0rd!#$\n"));
    CHECK_EQ(c.loadCredentials().second, std::string("P@ssW0rd!#$"));
}

TEST_CASE(G_CONF, "Двоеточие не является разделителем", "Файл: \"user:pass\"", "ошибка формата (в файле одно слово)") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user:pass\n"));
    CHECK_THROWS_MSG(c.loadCredentials(), std::runtime_error, "Ошибка формата файла конфигурации");
}

TEST_CASE(G_CONF, "Кириллица в логине и пароле", "Файл: \"логин пароль\"", "логин \"логин\", пароль \"пароль\"") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "логин пароль\n"));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first, std::string("логин"));
    CHECK_EQ(cr.second, std::string("пароль"));
}

TEST_CASE(G_CONF, "Пароль с пробелом обрезается по первому пробелу", "Файл: \"user my pass\"", "пароль \"my\" (ограничение формата)") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "user my pass\n"));
    CHECK_EQ(c.loadCredentials().second, std::string("my"));
}

TEST_CASE(G_CONF, "Очень длинные значения", "логин и пароль по 10 000 символов", "Значения прочитаны полностью") {
    TempDir d;
    std::string l(10000, 'L'), p(10000, 'P');
    ConfigManager c(d.write("a.conf", l + " " + p));
    auto cr = c.loadCredentials();
    CHECK_EQ(cr.first.size(), static_cast<size_t>(10000));
    CHECK_EQ(cr.second.size(), static_cast<size_t>(10000));
}

TEST_CASE(G_CONF, "Пустой файл", "Пустой файл", "std::runtime_error (ошибка формата)") {
    TempDir d;
    ConfigManager c(d.write("a.conf", ""));
    CHECK_THROWS_MSG(c.loadCredentials(), std::runtime_error, "Ошибка формата файла конфигурации");
}

TEST_CASE(G_CONF, "Файл только из пробельных символов", "Файл: \" \\n\\t \\n\"", "std::runtime_error (ошибка формата)") {
    TempDir d;
    ConfigManager c(d.write("a.conf", " \n\t \n"));
    CHECK_THROWS(c.loadCredentials(), std::runtime_error);
}

TEST_CASE(G_CONF, "Есть только логин", "Файл: \"onlylogin\\n\"", "std::runtime_error (ошибка формата)") {
    TempDir d;
    ConfigManager c(d.write("a.conf", "onlylogin\n"));
    CHECK_THROWS_MSG(c.loadCredentials(), std::runtime_error, "ожидается LOGIN и PASSWORD");
}

TEST_CASE(G_CONF, "Несуществующий файл", "Путь: \"/nonexistent_dir/vclient.conf\"", "std::runtime_error с путём в сообщении") {
    ConfigManager c("/nonexistent_dir/vclient.conf");
    CHECK_THROWS_MSG(c.loadCredentials(), std::runtime_error, "Не удалось открыть файл конфигурации: /nonexistent_dir/vclient.conf");
}

TEST_CASE(G_CONF, "Пустой путь к файлу", "Путь: \"\"", "std::runtime_error") {
    ConfigManager c("");
    CHECK_THROWS(c.loadCredentials(), std::runtime_error);
}

TEST_CASE(G_CONF, "Вместо файла указан каталог", "Путь: существующий каталог", "std::runtime_error") {
    TempDir d;
    ConfigManager c(d.path(""));
    CHECK_THROWS(c.loadCredentials(), std::runtime_error);
}

// ======================================================================
//  Таблица 3. DataReader
// ======================================================================

TEST_CASE(G_READ, "Два вектора", "Файл: \"2\\n3 1 2 3\\n2 -5 5\\n\"", "{{1, 2, 3}, {-5, 5}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "2\n3 1 2 3\n2 -5 5\n"));
    std::vector<std::vector<int32_t>> exp = {{1, 2, 3}, {-5, 5}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Один вектор из одного элемента", "Файл: \"1\\n1 42\"", "{{42}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n1 42"));
    std::vector<std::vector<int32_t>> exp = {{42}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Ноль векторов", "Файл: \"0\"", "Пустой результат") {
    TempDir d;
    DataReader r(d.write("in.txt", "0"));
    CHECK(r.readVectors().empty());
}

TEST_CASE(G_READ, "Вектор нулевой длины", "Файл: \"1\\n0\"", "{{}} (один пустой вектор)") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n0"));
    auto v = r.readVectors();
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK(v[0].empty());
}

TEST_CASE(G_READ, "Граничные значения int32", "Файл: \"1\\n4 2147483647 -2147483648 0 -1\"", "{{INT32_MAX, INT32_MIN, 0, -1}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n4 2147483647 -2147483648 0 -1"));
    std::vector<std::vector<int32_t>> exp = {{INT32_MAX, INT32_MIN, 0, -1}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Все числа в одной строке", "Файл: \"2 2 1 2 1 3\"", "{{1, 2}, {3}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "2 2 1 2 1 3"));
    std::vector<std::vector<int32_t>> exp = {{1, 2}, {3}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Окончания строк CRLF и лишние пробелы", "Файл: \"1\\r\\n  2 \\t 7\\r\\n 8 \\r\\n\"", "{{7, 8}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\r\n  2 \t 7\r\n 8 \r\n"));
    std::vector<std::vector<int32_t>> exp = {{7, 8}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Лишние данные в конце файла игнорируются", "Файл: \"1\\n1 5\\n999 888\"", "{{5}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n1 5\n999 888"));
    std::vector<std::vector<int32_t>> exp = {{5}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Знак «+» перед числом", "Файл: \"1\\n1 +15\"", "{{15}}") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n1 +15"));
    std::vector<std::vector<int32_t>> exp = {{15}};
    CHECK_EQ(r.readVectors(), exp);
}

TEST_CASE(G_READ, "Большой вектор", "1 вектор из 100 000 элементов 0…99 999", "Размер 100 000, последний элемент 99 999") {
    TempDir d;
    std::ostringstream s;
    s << "1\n100000\n";
    for (int i = 0; i < 100000; ++i) s << i << ' ';
    DataReader r(d.write("in.txt", s.str()));
    auto v = r.readVectors();
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK_EQ(v[0].size(), static_cast<size_t>(100000));
    CHECK_EQ(v[0].back(), 99999);
}

TEST_CASE(G_READ, "Много векторов", "1000 векторов по 3 элемента", "1000 векторов, содержимое сохранено") {
    TempDir d;
    std::ostringstream s;
    s << "1000\n";
    for (int i = 0; i < 1000; ++i) s << "3 " << i << ' ' << i + 1 << ' ' << i + 2 << '\n';
    DataReader r(d.write("in.txt", s.str()));
    auto v = r.readVectors();
    CHECK_EQ(v.size(), static_cast<size_t>(1000));
    std::vector<int32_t> last = {999, 1000, 1001};
    CHECK_EQ(v[999], last);
}

TEST_CASE(G_READ, "Несуществующий файл", "Путь: \"/nonexistent_dir/in.txt\"", "std::runtime_error «Ошибка открытия входного файла»") {
    DataReader r("/nonexistent_dir/in.txt");
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "Ошибка открытия входного файла: /nonexistent_dir/in.txt");
}

TEST_CASE(G_READ, "Пустой файл", "Пустой файл", "std::runtime_error «Ошибка чтения количества векторов»") {
    TempDir d;
    DataReader r(d.write("in.txt", ""));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "Ошибка чтения количества векторов");
}

TEST_CASE(G_READ, "Нечисловое количество векторов", "Файл: \"abc\"", "std::runtime_error «Ошибка чтения количества векторов»") {
    TempDir d;
    DataReader r(d.write("in.txt", "abc"));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "Ошибка чтения количества векторов");
}

TEST_CASE(G_READ, "Не хватает вектора (количество больше фактического)", "Файл: \"2\\n1 5\"", "std::runtime_error «Ошибка чтения размера вектора #1»") {
    TempDir d;
    DataReader r(d.write("in.txt", "2\n1 5"));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "Ошибка чтения размера вектора #1");
}

TEST_CASE(G_READ, "Не хватает элементов вектора", "Файл: \"1\\n3 1 2\"", "std::runtime_error «Ошибка чтения значения вектора #0, элемент 2»") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n3 1 2"));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "Ошибка чтения значения вектора #0, элемент 2");
}

TEST_CASE(G_READ, "Нечисловой элемент вектора", "Файл: \"1\\n3 1 x 3\"", "std::runtime_error «…вектора #0, элемент 1»") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n3 1 x 3"));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "вектора #0, элемент 1");
}

TEST_CASE(G_READ, "Значение вне диапазона int32", "Файл: \"1\\n1 2147483648\"", "std::runtime_error «Ошибка чтения значения»") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n1 2147483648"));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "Ошибка чтения значения вектора #0");
}

TEST_CASE(G_READ, "Дробное число в качестве элемента", "Файл: \"1\\n2 1.5 2\"", "std::runtime_error (дробная часть не читается как целое)") {
    TempDir d;
    DataReader r(d.write("in.txt", "1\n2 1.5 2"));
    CHECK_THROWS(r.readVectors(), std::runtime_error);
}

TEST_CASE(G_READ, "Ошибка во втором векторе указывает его номер", "Файл: \"2\\n1 5\\n2 7 oops\"", "std::runtime_error «…вектора #1, элемент 1»") {
    TempDir d;
    DataReader r(d.write("in.txt", "2\n1 5\n2 7 oops"));
    CHECK_THROWS_MSG(r.readVectors(), std::runtime_error, "вектора #1, элемент 1");
}

// ======================================================================
//  Таблица 4. DataWriter
// ======================================================================

TEST_CASE(G_WRIT, "Три результата", "{6, -4, 0}", "Файл: \"3 6 -4 0\\n\"") {
    TempDir d;
    std::string p = d.path("out.txt");
    DataWriter w(p);
    w.saveResults({6, -4, 0});
    CHECK_EQ(readFile(p), std::string("3 6 -4 0\n"));
}

TEST_CASE(G_WRIT, "Один результат", "{42}", "Файл: \"1 42\\n\"") {
    TempDir d;
    std::string p = d.path("out.txt");
    DataWriter w(p);
    w.saveResults({42});
    CHECK_EQ(readFile(p), std::string("1 42\n"));
}

TEST_CASE(G_WRIT, "Пустой список результатов", "{}", "Файл: \"0\\n\"") {
    TempDir d;
    std::string p = d.path("out.txt");
    DataWriter w(p);
    w.saveResults({});
    CHECK_EQ(readFile(p), std::string("0\n"));
}

TEST_CASE(G_WRIT, "Граничные значения int32", "{INT32_MIN, INT32_MAX}", "Файл: \"2 -2147483648 2147483647\\n\"") {
    TempDir d;
    std::string p = d.path("out.txt");
    DataWriter w(p);
    w.saveResults({INT32_MIN, INT32_MAX});
    CHECK_EQ(readFile(p), std::string("2 -2147483648 2147483647\n"));
}

TEST_CASE(G_WRIT, "Файл создаётся, если его не было", "Несуществующий файл в существующем каталоге", "Файл создан") {
    TempDir d;
    std::string p = d.path("new.txt");
    CHECK(!fileExists(p));
    DataWriter w(p);
    w.saveResults({1});
    CHECK(fileExists(p));
}

TEST_CASE(G_WRIT, "Прежнее содержимое файла перезаписывается", "Файл содержит длинный текст; saveResults({7})", "Файл: \"1 7\\n\"") {
    TempDir d;
    std::string p = d.write("out.txt", "OLD OLD OLD OLD OLD OLD OLD OLD\n");
    DataWriter w(p);
    w.saveResults({7});
    CHECK_EQ(readFile(p), std::string("1 7\n"));
}

TEST_CASE(G_WRIT, "Повторная запись тем же объектом", "saveResults({1}), затем saveResults({2, 3})", "Файл: \"2 2 3\\n\"") {
    TempDir d;
    std::string p = d.path("out.txt");
    DataWriter w(p);
    w.saveResults({1});
    w.saveResults({2, 3});
    CHECK_EQ(readFile(p), std::string("2 2 3\n"));
}

TEST_CASE(G_WRIT, "Большой список результатов", "10 000 значений 0…9 999", "Начинается с \"10000 0 1 2\", заканчивается \" 9999\\n\"") {
    TempDir d;
    std::string p = d.path("out.txt");
    std::vector<int32_t> v(10000);
    for (int i = 0; i < 10000; ++i) v[static_cast<size_t>(i)] = i;
    DataWriter w(p);
    w.saveResults(v);
    std::string s = readFile(p);
    CHECK_EQ(s.compare(0, 12, "10000 0 1 2 "), 0);
    CHECK_EQ(s.substr(s.size() - 6), std::string(" 9999\n"));
}

TEST_CASE(G_WRIT, "Недоступный путь", "Путь: \"/nonexistent_dir/out.txt\"", "std::runtime_error «Ошибка открытия выходного файла»") {
    DataWriter w("/nonexistent_dir/out.txt");
    CHECK_THROWS_MSG(w.saveResults({1}), std::runtime_error, "Ошибка открытия выходного файла: /nonexistent_dir/out.txt");
}

TEST_CASE(G_WRIT, "Вместо файла указан каталог", "Путь: существующий каталог", "std::runtime_error") {
    TempDir d;
    DataWriter w(d.path(""));
    CHECK_THROWS(w.saveResults({1}), std::runtime_error);
}

// ======================================================================
//  Таблица 5. Communicator
// ======================================================================

TEST_CASE(G_COMM, "Подключение к серверу", "127.0.0.1 и порт слушающего сокета", "Исключения нет, соединение установлено") {
    FakeServer srv([](Conn&) {});
    Communicator c;
    CHECK_NO_THROW(c.connectServer("127.0.0.1", srv.port()));
    c.closeConnection();
    srv.finish();
}

TEST_CASE(G_COMM, "Некорректный IP-адрес", "ip=\"999.1.1.1\"", "std::runtime_error «Неверный IP адрес»") {
    Communicator c;
    CHECK_THROWS_MSG(c.connectServer("999.1.1.1", 33333), std::runtime_error, "Неверный IP адрес: 999.1.1.1");
}

TEST_CASE(G_COMM, "Имя хоста вместо IP-адреса", "ip=\"localhost\"", "std::runtime_error «Неверный IP адрес» (DNS не поддерживается)") {
    Communicator c;
    CHECK_THROWS_MSG(c.connectServer("localhost", 33333), std::runtime_error, "Неверный IP адрес");
}

TEST_CASE(G_COMM, "Пустая строка вместо IP-адреса", "ip=\"\"", "std::runtime_error «Неверный IP адрес»") {
    Communicator c;
    CHECK_THROWS(c.connectServer("", 33333), std::runtime_error);
}

TEST_CASE(G_COMM, "Порт, на котором никто не слушает", "127.0.0.1 и закрытый порт", "std::system_error «connect() failed»") {
    Communicator c;
    CHECK_THROWS_MSG(c.connectServer("127.0.0.1", closedPort()), std::system_error, "connect() failed");
}

TEST_CASE(G_COMM, "sendLogin отправляет логин без изменений", "sendLogin(\"alice\")", "Сервер получил ровно \"alice\"") {
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvSome(); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendLogin("alice");
    srv.finish();
    CHECK_EQ(got, std::string("alice"));
}

TEST_CASE(G_COMM, "sendLogin с пустым логином", "sendLogin(\"\")", "Исключения нет, по сети ничего не передаётся") {
    std::string got = "?";
    FakeServer srv([&](Conn& s) { got = s.recvSome(); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_NO_THROW(c.sendLogin(""));
    c.closeConnection();
    srv.finish();
    CHECK(got.empty());
}

TEST_CASE(G_COMM, "receiveSalt возвращает соль сервера", "Сервер отправляет \"A1B2C3D4E5F60789\"", "\"A1B2C3D4E5F60789\"") {
    FakeServer srv([](Conn& s) { s.sendStr("A1B2C3D4E5F60789"); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveSalt(), std::string("A1B2C3D4E5F60789"));
    srv.finish();
}

TEST_CASE(G_COMM, "Завершающие \\n и \\r\\n отбрасываются", "Сервер отправляет \"SALT\\r\\n\"", "\"SALT\"") {
    FakeServer srv([](Conn& s) { s.sendStr("SALT\r\n"); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveSalt(), std::string("SALT"));
    srv.finish();
}

TEST_CASE(G_COMM, "receiveAuthStatus: ответ OK", "Сервер отправляет \"OK\"", "\"OK\"") {
    FakeServer srv([](Conn& s) { s.sendStr("OK"); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveAuthStatus(), std::string("OK"));
    srv.finish();
}

TEST_CASE(G_COMM, "receiveAuthStatus: ответ ERR", "Сервер отправляет \"ERR\\n\"", "\"ERR\"") {
    FakeServer srv([](Conn& s) { s.sendStr("ERR\n"); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveAuthStatus(), std::string("ERR"));
    srv.finish();
}

TEST_CASE(G_COMM, "Слишком длинный текстовый ответ усекается", "Сервер отправляет 2000 символов «X»", "Возвращено 1023 символа (размер буфера приёма)") {
    FakeServer srv([](Conn& s) { s.sendStr(std::string(2000, 'X')); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveSalt().size(), static_cast<size_t>(1023));
    srv.finish();
}

TEST_CASE(G_COMM, "Сервер закрыл соединение до текстового ответа", "Сервер ничего не отправляет и закрывает сокет", "std::system_error «recvString() failed»") {
    FakeServer srv([](Conn&) {});
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_THROWS_MSG(c.receiveAuthStatus(), std::system_error, "recvString() failed");
    srv.finish();
}

TEST_CASE(G_COMM, "sendHash отправляет хеш без изменений", "sendHash(64-символьная строка)", "Сервер получил те же 64 символа") {
    std::string hash = "C7821BB369C931FB97EE4B47173160AF4B8BEB6956139705C6059D07E67F096E";
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(64); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendHash(hash);
    srv.finish();
    CHECK_EQ(got, hash);
}

TEST_CASE(G_COMM, "sendVectorCount: сетевой порядок байт", "sendVectorCount(3)", "Байты 00 00 00 03") {
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(4); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendVectorCount(3);
    srv.finish();
    CHECK_EQ(got, be32(3));
    CHECK_EQ(got, std::string("\x00\x00\x00\x03", 4));
}

TEST_CASE(G_COMM, "sendVectorCount: максимальное значение", "sendVectorCount(UINT32_MAX)", "Байты FF FF FF FF") {
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(4); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendVectorCount(UINT32_MAX);
    srv.finish();
    CHECK_EQ(fromBe32(got), UINT32_MAX);
}

TEST_CASE(G_COMM, "sendVector: размер и значения в сетевом порядке", "sendVector({1, -2, 3})", "16 байт: 3, 1, 0xFFFFFFFE, 3") {
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(16); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendVector({1, -2, 3});
    srv.finish();
    CHECK_EQ(fromBe32(got, 0), 3u);
    CHECK_EQ(fromBe32(got, 4), 1u);
    CHECK_EQ(fromBe32(got, 8), 0xFFFFFFFEu);
    CHECK_EQ(fromBe32(got, 12), 3u);
}

TEST_CASE(G_COMM, "sendVector: пустой вектор", "sendVector({})", "4 байта: размер 0") {
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(4); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendVector({});
    srv.finish();
    CHECK_EQ(got, be32(0));
}

TEST_CASE(G_COMM, "sendVector: граничные значения int32", "sendVector({INT32_MAX, INT32_MIN})", "0x7FFFFFFF и 0x80000000") {
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(12); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendVector({INT32_MAX, INT32_MIN});
    srv.finish();
    CHECK_EQ(fromBe32(got, 4), 0x7FFFFFFFu);
    CHECK_EQ(fromBe32(got, 8), 0x80000000u);
}

TEST_CASE(G_COMM, "sendVector: большой вектор", "sendVector из 100 000 элементов", "Сервер получил 400 004 байта; первый и последний элементы верны") {
    std::vector<int32_t> big(100000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<int32_t>(i);
    std::string got;
    FakeServer srv([&](Conn& s) { got = s.recvExact(4 + 400000); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.sendVector(big);
    srv.finish();
    CHECK_EQ(fromBe32(got, 0), 100000u);
    CHECK_EQ(fromBe32(got, 4), 0u);
    CHECK_EQ(fromBe32(got, got.size() - 4), 99999u);
}

TEST_CASE(G_COMM, "receiveResult: положительное значение", "Сервер отправляет байты 00 00 01 00", "256") {
    FakeServer srv([](Conn& s) { s.sendStr(be32(256)); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveResult(), 256);
    srv.finish();
}

TEST_CASE(G_COMM, "receiveResult: отрицательное значение", "Сервер отправляет байты FF FF FF FF", "-1") {
    FakeServer srv([](Conn& s) { s.sendStr(be32(0xFFFFFFFFu)); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveResult(), -1);
    srv.finish();
}

TEST_CASE(G_COMM, "receiveResult: граничные значения int32", "Сервер отправляет 0x80000000 и 0x7FFFFFFF", "INT32_MIN и INT32_MAX") {
    FakeServer srv([](Conn& s) { s.sendStr(be32(0x80000000u) + be32(0x7FFFFFFFu)); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveResult(), INT32_MIN);
    CHECK_EQ(c.receiveResult(), INT32_MAX);
    srv.finish();
}

TEST_CASE(G_COMM, "receiveResult: значение приходит по частям", "Сервер отправляет 1 байт, пауза 100 мс, затем 3 байта", "Значение 0x01020304 собрано корректно") {
    FakeServer srv([](Conn& s) {
        std::string b = be32(0x01020304u);
        s.sendStr(b.substr(0, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s.sendStr(b.substr(1));
    });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_EQ(c.receiveResult(), 0x01020304);
    srv.finish();
}

TEST_CASE(G_COMM, "receiveResult: соединение закрыто на середине значения", "Сервер отправляет 2 байта и закрывает сокет", "std::system_error «recv() failed or timeout»") {
    FakeServer srv([](Conn& s) { s.sendStr(std::string("\x00\x01", 2)); });
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_THROWS_MSG(c.receiveResult(), std::system_error, "recv() failed or timeout");
    srv.finish();
}

TEST_CASE(G_COMM, "Отправка после closeConnection()", "closeConnection(), затем sendLogin(\"x\")", "std::system_error «send() failed»") {
    FakeServer srv([](Conn&) {});
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    c.closeConnection();
    CHECK_THROWS_MSG(c.sendLogin("x"), std::system_error, "send() failed");
    srv.finish();
}

TEST_CASE(G_COMM, "Повторный вызов closeConnection()", "closeConnection() дважды подряд", "Исключений нет") {
    FakeServer srv([](Conn&) {});
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    CHECK_NO_THROW(c.closeConnection());
    CHECK_NO_THROW(c.closeConnection());
    srv.finish();
}

TEST_CASE(G_COMM, "closeConnection() для неподключённого объекта", "Новый Communicator, closeConnection()", "Исключения нет") {
    Communicator c;
    CHECK_NO_THROW(c.closeConnection());
}

TEST_CASE(G_COMM, "Деструктор закрывает сокет", "Объект Communicator выходит из области видимости", "Сервер получает признак закрытия соединения (EOF)") {
    std::string got = "?";
    FakeServer srv([&](Conn& s) { got = s.recvSome(); });
    {
        Communicator c;
        c.connectServer("127.0.0.1", srv.port());
    }
    srv.finish();
    CHECK(got.empty());
}

TEST_CASE(G_COMM, "Отправка в соединение, закрытое сервером", "Сервер закрыл сокет; клиент повторно вызывает sendVector", "std::system_error «send() failed» (SIGPIPE в тесте игнорируется)") {
    FakeServer srv([](Conn&) {});
    Communicator c;
    c.connectServer("127.0.0.1", srv.port());
    srv.finish();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<int32_t> big(20000, 7);
    bool thrown = false;
    for (int i = 0; i < 50 && !thrown; ++i) {
        try {
            c.sendVector(big);
        } catch (const std::system_error& e) {
            thrown = std::string(e.what()).find("send() failed") != std::string::npos;
        }
    }
    CHECK(thrown);
}

// ======================================================================
//  Таблица 6. UserInterface и Params
// ======================================================================

TEST_CASE(G_UI, "Params: значения по умолчанию", "Params p;", "строки пустые, serverPort = 33333") {
    Params p;
    CHECK(p.serverIp.empty());
    CHECK(p.inputFile.empty());
    CHECK(p.outputFile.empty());
    CHECK(p.configFile.empty());
    CHECK_EQ(p.serverPort, 33333);
}

TEST_CASE(G_UI, "getParams() до разбора аргументов", "UserInterface ui; ui.getParams()", "Пустые строки, порт 33333") {
    UserInterface ui;
    Params p = ui.getParams();
    CHECK(p.serverIp.empty());
    CHECK(p.configFile.empty());
    CHECK_EQ(p.serverPort, 33333);
}

TEST_CASE(G_UI, "Все параметры (короткие флаги)", "./vclient -a 127.0.0.1 -p 9090 -i in.txt -o out.txt -c my.conf", "ip=\"127.0.0.1\", port=9090, in=\"in.txt\", out=\"out.txt\", conf=\"my.conf\"") {
    UserInterface ui;
    Args a{"-a", "127.0.0.1", "-p", "9090", "-i", "in.txt", "-o", "out.txt", "-c", "my.conf"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    Params p = ui.getParams();
    CHECK_EQ(p.serverIp, std::string("127.0.0.1"));
    CHECK_EQ(p.serverPort, 9090);
    CHECK_EQ(p.inputFile, std::string("in.txt"));
    CHECK_EQ(p.outputFile, std::string("out.txt"));
    CHECK_EQ(p.configFile, std::string("my.conf"));
}

TEST_CASE(G_UI, "Все параметры (длинные флаги)", "./vclient --address 10.0.0.5 --port 44444 --input a.txt --output b.txt --config c.conf", "ip=\"10.0.0.5\", port=44444, in=\"a.txt\", out=\"b.txt\", conf=\"c.conf\"") {
    UserInterface ui;
    Args a{"--address", "10.0.0.5", "--port", "44444", "--input", "a.txt", "--output", "b.txt", "--config", "c.conf"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    Params p = ui.getParams();
    CHECK_EQ(p.serverIp, std::string("10.0.0.5"));
    CHECK_EQ(p.serverPort, 44444);
    CHECK_EQ(p.inputFile, std::string("a.txt"));
    CHECK_EQ(p.outputFile, std::string("b.txt"));
    CHECK_EQ(p.configFile, std::string("c.conf"));
}

TEST_CASE(G_UI, "Длинные флаги в форме --имя=значение", "./vclient --address=10.0.0.5 --port=8081 --input=a --output=b", "ip=\"10.0.0.5\", port=8081, in=\"a\", out=\"b\"") {
    UserInterface ui;
    Args a{"--address=10.0.0.5", "--port=8081", "--input=a", "--output=b"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    Params p = ui.getParams();
    CHECK_EQ(p.serverIp, std::string("10.0.0.5"));
    CHECK_EQ(p.serverPort, 8081);
    CHECK_EQ(p.inputFile, std::string("a"));
    CHECK_EQ(p.outputFile, std::string("b"));
}

TEST_CASE(G_UI, "Порт по умолчанию", "./vclient -a 1.2.3.4 -i i -o o", "port = 33333") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().serverPort, 33333);
}

TEST_CASE(G_UI, "Файл конфигурации по умолчанию", "HOME=/home/tester; параметр -c не задан", "conf=\"/home/tester/.config/vclient.conf\"") {
    EnvGuard home("HOME");
    home.set("/home/tester");
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().configFile, std::string("/home/tester/.config/vclient.conf"));
}

TEST_CASE(G_UI, "Короткий флаг справки", "./vclient -h", "parseArgs = false (выводится справка)") {
    Capture out(std::cout);
    UserInterface ui;
    Args a{"-h"};
    CHECK(!ui.parseArgs(a.argc(), a.argv()));
}

TEST_CASE(G_UI, "Длинный флаг справки", "./vclient --help", "parseArgs = false") {
    Capture out(std::cout);
    UserInterface ui;
    Args a{"--help"};
    CHECK(!ui.parseArgs(a.argc(), a.argv()));
}

TEST_CASE(G_UI, "Справка вместе с другими параметрами", "./vclient -a 1.2.3.4 -i i -o o -h", "parseArgs = false") {
    Capture out(std::cout);
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-h"};
    CHECK(!ui.parseArgs(a.argc(), a.argv()));
}

TEST_CASE(G_UI, "Текст справки", "showHelp()", "Вывод содержит «Allowed options» и все длинные имена параметров") {
    Capture out(std::cout);
    UserInterface ui;
    ui.showHelp();
    std::string s = out.str();
    CHECK_CONTAINS(s, "Allowed options");
    for (const char* n : {"--help", "--address", "--port", "--input", "--output", "--config"}) CHECK_CONTAINS(s, n);
}

TEST_CASE(G_UI, "Запуск без параметров", "./vclient", "po::error (не заданы обязательные параметры)") {
    UserInterface ui;
    Args a{};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Не указан адрес сервера", "./vclient -i i -o o", "po::error (требуется --address)") {
    UserInterface ui;
    Args a{"-i", "i", "-o", "o"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Не указан входной файл", "./vclient -a 1.2.3.4 -o o", "po::error (требуется --input)") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-o", "o"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Не указан выходной файл", "./vclient -a 1.2.3.4 -i i", "po::error (требуется --output)") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Неизвестный параметр", "./vclient --foo", "po::error") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "--foo"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Нечисловое значение порта", "./vclient … -p abc", "po::error") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "abc"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Значение порта не помещается в int", "./vclient … -p 99999999999", "po::error") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "99999999999"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Флаг порта без значения", "./vclient -a 1.2.3.4 -i i -o o -p", "po::error") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Параметр указан дважды", "./vclient … -p 1000 -p 2000", "po::error") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "1000", "-p", "2000"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Лишний позиционный аргумент", "./vclient … stray", "po::error") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "stray"};
    CHECK_THROWS(ui.parseArgs(a.argc(), a.argv()), po::error);
}

TEST_CASE(G_UI, "Порт 0 не проверяется на диапазон", "./vclient … -p 0", "true, port = 0 (проверка диапазона не реализована)") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "0"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().serverPort, 0);
}

TEST_CASE(G_UI, "Порт 70000 не проверяется на диапазон", "./vclient … -p 70000", "true, port = 70000 (проверка диапазона не реализована)") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "70000"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().serverPort, 70000);
}

TEST_CASE(G_UI, "Отрицательный порт не проверяется", "./vclient … --port=-5", "true, port = -5 (проверка диапазона не реализована)") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "--port=-5"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().serverPort, -5);
}

TEST_CASE(G_UI, "Допустимые границы порта 1 и 65535", "-p 1 и -p 65535", "port = 1 и port = 65535") {
    {
        UserInterface ui;
        Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "1"};
        CHECK(ui.parseArgs(a.argc(), a.argv()));
        CHECK_EQ(ui.getParams().serverPort, 1);
    }
    {
        UserInterface ui;
        Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-p", "65535"};
        CHECK(ui.parseArgs(a.argc(), a.argv()));
        CHECK_EQ(ui.getParams().serverPort, 65535);
    }
}

TEST_CASE(G_UI, "Адрес сервера не проверяется при разборе", "./vclient -a not_an_ip …", "true, ip = \"not_an_ip\" (проверка выполняется позже, в Communicator)") {
    UserInterface ui;
    Args a{"-a", "not_an_ip", "-i", "i", "-o", "o"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().serverIp, std::string("not_an_ip"));
}

TEST_CASE(G_UI, "Раскрытие «~» в пути к конфигурации", "HOME=/home/tester; -c ~/my.conf", "conf=\"/home/tester/my.conf\"") {
    EnvGuard home("HOME");
    home.set("/home/tester");
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-c", "~/my.conf"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().configFile, std::string("/home/tester/my.conf"));
}

TEST_CASE(G_UI, "Символ «~» отдельным значением", "HOME=/home/tester; -c ~", "conf=\"/home/tester\"") {
    EnvGuard home("HOME");
    home.set("/home/tester");
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-c", "~"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().configFile, std::string("/home/tester"));
}

TEST_CASE(G_UI, "Путь без «~» не изменяется", "-c cfg/vclient.conf", "conf=\"cfg/vclient.conf\"") {
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-c", "cfg/vclient.conf"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().configFile, std::string("cfg/vclient.conf"));
}

TEST_CASE(G_UI, "Символ «~» не в начале пути не раскрывается", "HOME=/home/tester; -c dir/~x.conf", "conf=\"dir/~x.conf\"") {
    EnvGuard home("HOME");
    home.set("/home/tester");
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-c", "dir/~x.conf"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().configFile, std::string("dir/~x.conf"));
}

TEST_CASE(G_UI, "Форма «~user/…» раскрывается некорректно", "HOME=/home/tester; -c ~bob/x.conf", "conf=\"/home/testerbob/x.conf\" (заменяется только «~», домашний каталог другого пользователя не определяется)") {
    EnvGuard home("HOME");
    home.set("/home/tester");
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-c", "~bob/x.conf"};
    CHECK(ui.parseArgs(a.argc(), a.argv()));
    CHECK_EQ(ui.getParams().configFile, std::string("/home/testerbob/x.conf"));
}

TEST_CASE(G_UI, "Раскрытие «~» при неопределённой переменной HOME", "HOME не задана; -c ~/my.conf", "std::runtime_error «Не удалось определить домашнюю директорию»") {
    EnvGuard home("HOME");
    home.unset();
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o", "-c", "~/my.conf"};
    CHECK_THROWS_MSG(ui.parseArgs(a.argc(), a.argv()), std::runtime_error, "Не удалось определить домашнюю директорию");
}

TEST_CASE(G_UI, "Конфигурация по умолчанию при неопределённой HOME", "HOME не задана; -c не указан", "std::runtime_error «Не удалось определить домашнюю директорию»") {
    EnvGuard home("HOME");
    home.unset();
    UserInterface ui;
    Args a{"-a", "1.2.3.4", "-i", "i", "-o", "o"};
    CHECK_THROWS_MSG(ui.parseArgs(a.argc(), a.argv()), std::runtime_error, "Не удалось определить домашнюю директорию");
}

TEST_CASE(G_UI, "Справка не требует обязательных параметров и HOME", "HOME не задана; ./vclient -h", "parseArgs = false без исключений") {
    EnvGuard home("HOME");
    home.unset();
    Capture out(std::cout);
    UserInterface ui;
    Args a{"-h"};
    CHECK_NO_THROW(CHECK(!ui.parseArgs(a.argc(), a.argv())));
}

// ======================================================================
//  Таблица 7. Client
// ======================================================================

namespace {

struct ServerScript {
    std::string saltReply = "A1B2C3D4E5F60789";  // ответ на LOGIN
    std::string authReply = "OK";                // ответ на хеш
    bool dropAfterFirstVector = false;           // оборвать соединение вместо первого результата
    std::function<int32_t(size_t, const std::vector<int32_t>&)> calc =
        [](size_t, const std::vector<int32_t>& v) {
            int64_t s = 0;
            for (int32_t x : v) s += x;
            return static_cast<int32_t>(s);
        };
    // Что получил сервер:
    std::string login, hash;
    uint32_t count = 0;
    std::vector<std::vector<int32_t>> vectors;
};

void runScript(Conn& c, ServerScript& s) {
    s.login = c.recvSome();
    c.sendStr(s.saltReply);
    if (s.saltReply == "ERR") return;
    s.hash = c.recvExact(64);
    c.sendStr(s.authReply);
    if (s.authReply != "OK") return;
    s.count = fromBe32(c.recvExact(4));
    for (uint32_t i = 0; i < s.count; ++i) {
        uint32_t n = fromBe32(c.recvExact(4));
        std::vector<int32_t> v;
        for (uint32_t j = 0; j < n; ++j) v.push_back(static_cast<int32_t>(fromBe32(c.recvExact(4))));
        s.vectors.push_back(v);
        if (s.dropAfterFirstVector) return;
        c.sendStr(be32(static_cast<uint32_t>(s.calc(i, v))));
    }
}

struct ClientEnv {
    TempDir dir;
    Params p;
    explicit ClientEnv(const std::string& input, const std::string& config = "alice TestPass123\n") {
        p.serverIp = "127.0.0.1";
        p.serverPort = 33333;
        p.configFile = dir.write("vclient.conf", config);
        p.inputFile = dir.write("input.txt", input);
        p.outputFile = dir.path("output.txt");
    }
};

} 

TEST_CASE(G_CLNT, "Успешный сценарий: три вектора", "Вход: {1,2,3}, {-5,5}, {}; сервер возвращает суммы", "start() = 0; в выходном файле \"3 6 0 0\"; в stdout сообщение об успехе") {
    ClientEnv env("3\n3 1 2 3\n2 -5 5\n0\n");
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 0);
    CHECK_EQ(readFile(env.p.outputFile), std::string("3 6 0 0\n"));
    CHECK_CONTAINS(out.str(), "Работа завершена успешно");
    CHECK(err.str().empty());
}

TEST_CASE(G_CLNT, "Сервер получает логин из файла конфигурации", "Конфигурация: \"alice TestPass123\"", "Сервер получил \"alice\"") {
    ClientEnv env("1\n1 5\n");
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(sc.login, std::string("alice"));
}

TEST_CASE(G_CLNT, "Клиент отправляет хеш SHA-256(соль + пароль)", "Соль \"A1B2C3D4E5F60789\", пароль \"TestPass123\"", "Сервер получил C7821BB3…096E") {
    ClientEnv env("1\n1 5\n");
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(sc.hash, std::string("C7821BB369C931FB97EE4B47173160AF4B8BEB6956139705C6059D07E67F096E"));
}

TEST_CASE(G_CLNT, "Соль с символом конца строки от сервера", "Сервер отправляет соль \"SALT\\n\", пароль \"secret\"", "Хеш вычислен от \"SALTsecret\" (52ED8103…98AE)") {
    ClientEnv env("1\n1 5\n", "alice secret\n");
    ServerScript sc;
    sc.saltReply = "SALT\n";
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(sc.hash, std::string("52ED81034E0E7FD346FF92A80DE0FB699F7A7A0CE425F4EA2E5783398B7F98AE"));
}

TEST_CASE(G_CLNT, "Серверу передаются все векторы без искажений", "Вход: {1,2,3}, {-5,5}, {}", "count = 3, векторы совпадают с входными") {
    ClientEnv env("3\n3 1 2 3\n2 -5 5\n0\n");
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(sc.count, 3u);
    std::vector<std::vector<int32_t>> exp = {{1, 2, 3}, {-5, 5}, {}};
    CHECK_EQ(sc.vectors, exp);
}

TEST_CASE(G_CLNT, "Ноль векторов во входном файле", "Вход: \"0\"", "start() = 0; в выходном файле \"0\\n\"") {
    ClientEnv env("0");
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(sc.count, 0u);
    CHECK_EQ(readFile(env.p.outputFile), std::string("0\n"));
}

TEST_CASE(G_CLNT, "Граничные значения результатов сервера", "Сервер возвращает INT32_MIN, INT32_MAX, -1", "Выходной файл: \"3 -2147483648 2147483647 -1\\n\"") {
    ClientEnv env("3\n1 1\n1 2\n1 3\n");
    ServerScript sc;
    sc.calc = [](size_t i, const std::vector<int32_t>&) {
        const int32_t r[3] = {INT32_MIN, INT32_MAX, -1};
        return r[i];
    };
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(readFile(env.p.outputFile), std::string("3 -2147483648 2147483647 -1\n"));
}

TEST_CASE(G_CLNT, "Порядок результатов совпадает с порядком векторов", "10 векторов {i}, i = 0…9; сервер возвращает суммы", "Выходной файл: \"10 0 1 2 3 4 5 6 7 8 9\\n\"") {
    std::ostringstream in;
    in << "10\n";
    for (int i = 0; i < 10; ++i) in << "1 " << i << '\n';
    ClientEnv env(in.str());
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 0);
    srv.finish();
    CHECK_EQ(readFile(env.p.outputFile), std::string("10 0 1 2 3 4 5 6 7 8 9\n"));
}

TEST_CASE(G_CLNT, "Сервер отклонил LOGIN", "Ответ на логин: \"ERR\"", "start() = 1; «Сервер отклонил LOGIN»; выходной файл не создан") {
    ClientEnv env("1\n1 5\n");
    ServerScript sc;
    sc.saltReply = "ERR";
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "Сервер отклонил LOGIN");
    CHECK(!fileExists(env.p.outputFile));
}

TEST_CASE(G_CLNT, "Сервер отклонил хеш пароля", "Ответ на хеш: \"ERR\"", "start() = 1; «Ошибка аутентификации»; выходной файл не создан") {
    ClientEnv env("1\n1 5\n");
    ServerScript sc;
    sc.authReply = "ERR";
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "Ошибка аутентификации");
    CHECK(!fileExists(env.p.outputFile));
}

TEST_CASE(G_CLNT, "Неожиданный статус аутентификации", "Ответ на хеш: \"HELLO\"", "start() = 1; «Неожиданный ответ сервера при аутентификации: HELLO»") {
    ClientEnv env("1\n1 5\n");
    ServerScript sc;
    sc.authReply = "HELLO";
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "Неожиданный ответ сервера при аутентификации: HELLO");
}

TEST_CASE(G_CLNT, "Статус аутентификации чувствителен к регистру", "Ответ на хеш: \"ok\"", "start() = 1 (строчное «ok» не принимается)") {
    ClientEnv env("1\n1 5\n");
    ServerScript sc;
    sc.authReply = "ok";
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "Неожиданный ответ сервера при аутентификации: ok");
}

TEST_CASE(G_CLNT, "Сервер закрыл соединение, не отправив соль", "Сервер принимает логин и закрывает сокет", "start() = 1; «recvString() failed»") {
    ClientEnv env("1\n1 5\n");
    FakeServer srv([&](Conn& c) { c.recvSome(); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "recvString() failed");
}

TEST_CASE(G_CLNT, "Обрыв соединения до получения результата", "Сервер закрывает сокет после первого вектора", "start() = 1; «recv() failed»; выходной файл не создан") {
    ClientEnv env("2\n1 5\n1 6\n");
    ServerScript sc;
    sc.dropAfterFirstVector = true;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "recv() failed");
    CHECK(!fileExists(env.p.outputFile));
}

TEST_CASE(G_CLNT, "Сервер недоступен", "Порт, на котором никто не слушает", "start() = 1; «connect() failed»") {
    ClientEnv env("1\n1 5\n");
    env.p.serverPort = closedPort();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    CHECK_CONTAINS(err.str(), "connect() failed");
}

TEST_CASE(G_CLNT, "Некорректный IP-адрес сервера", "ip=\"256.0.0.1\"", "start() = 1; «Неверный IP адрес»") {
    ClientEnv env("1\n1 5\n");
    env.p.serverIp = "256.0.0.1";
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    CHECK_CONTAINS(err.str(), "Неверный IP адрес");
}

TEST_CASE(G_CLNT, "Отсутствует файл конфигурации", "Путь к конфигурации не существует", "start() = 1; «Не удалось открыть файл конфигурации»; соединение не устанавливается") {
    ClientEnv env("1\n1 5\n");
    env.p.configFile = env.dir.path("missing.conf");
    FakeServer srv([](Conn&) {});
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    CHECK_CONTAINS(err.str(), "Не удалось открыть файл конфигурации");
    CHECK(!fileExists(env.p.outputFile));
}

TEST_CASE(G_CLNT, "Отсутствует входной файл", "Путь ко входному файлу не существует", "start() = 1; «Ошибка открытия входного файла»") {
    ClientEnv env("1\n1 5\n");
    env.p.inputFile = env.dir.path("missing.txt");
    FakeServer srv([](Conn&) {});
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    CHECK_CONTAINS(err.str(), "Ошибка открытия входного файла");
}

TEST_CASE(G_CLNT, "Некорректный формат входного файла", "Вход: \"2\\n1 5\\n\" (второй вектор отсутствует)", "start() = 1; «Ошибка чтения размера вектора #1»") {
    ClientEnv env("2\n1 5\n");
    FakeServer srv([](Conn&) {});
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    CHECK_CONTAINS(err.str(), "Ошибка чтения размера вектора #1");
}

TEST_CASE(G_CLNT, "Некорректный формат файла конфигурации", "Конфигурация: \"onlylogin\"", "start() = 1; «Ошибка формата файла конфигурации»") {
    ClientEnv env("1\n1 5\n", "onlylogin\n");
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    CHECK_CONTAINS(err.str(), "Ошибка формата файла конфигурации");
}

TEST_CASE(G_CLNT, "Невозможно записать выходной файл", "Выходной путь в несуществующем каталоге", "start() = 1; «Ошибка открытия выходного файла»") {
    ClientEnv env("1\n1 5\n");
    env.p.outputFile = "/nonexistent_dir/output.txt";
    ServerScript sc;
    FakeServer srv([&](Conn& c) { runScript(c, sc); });
    env.p.serverPort = srv.port();
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    int rc = cl.start(0, nullptr);
    srv.finish();
    CHECK_EQ(rc, 1);
    CHECK_CONTAINS(err.str(), "Ошибка открытия выходного файла");
}

TEST_CASE(G_CLNT, "Формат сообщения об ошибке (handleError)", "Любая ошибка выполнения, например отсутствие файла конфигурации", "stderr содержит блок «=== ОШИБКА ===» с полями «Функция/Модуль», «Контекст/Параметры», «Тип ошибки»") {
    ClientEnv env("1\n1 5\n");
    env.p.configFile = env.dir.path("missing.conf");
    Capture out(std::cout), err(std::cerr);
    Client cl(env.p);
    CHECK_EQ(cl.start(0, nullptr), 1);
    std::string e = err.str();
    CHECK_CONTAINS(e, "=== ОШИБКА ===");
    CHECK_CONTAINS(e, "Функция/Модуль: Client::run");
    CHECK_CONTAINS(e, "Контекст/Параметры: ");
    CHECK_CONTAINS(e, "Тип ошибки: ");
    CHECK_CONTAINS(e, "===============");
}

TEST_CASE(G_CLNT, "Client::run() пробрасывает исключение наружу", "run() при отсутствующем файле конфигурации", "std::runtime_error (без перехвата внутри run)") {
    ClientEnv env("1\n1 5\n");
    env.p.configFile = env.dir.path("missing.conf");
    Client cl(env.p);
    CHECK_THROWS(cl.run(), std::runtime_error);
}

// ======================================================================
//  Запуск тестов
// ======================================================================

static std::string tsvSafe(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN); 
    alarm(180);               

    bool tsv = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--tsv") tsv = true;

    std::cout << "=== Модульные тесты VClient ===" << std::endl;
    auto t0 = std::chrono::steady_clock::now();

    struct Result {
        bool ok;
        std::string msg;
    };
    std::vector<Result> results;
    int passed = 0, failed = 0, n = 0;
    for (const auto& tc : registry()) {
        ++n;
        Result r{true, ""};
        try {
            tc.fn();
        } catch (const TestFailure& e) {
            r = {false, e.what()};
        } catch (const std::exception& e) {
            r = {false, std::string("неожиданное исключение: ") + e.what()};
        } catch (...) {
            r = {false, "неизвестное исключение"};
        }
        r.ok ? ++passed : ++failed;
        std::cout << (r.ok ? "[ OK ]   " : "[ FAIL ] ") << "T" << n << "  " << tc.title << std::endl;
        if (!r.ok) std::cout << "         " << r.msg << std::endl;
        results.push_back(r);
    }

    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char tbuf[32];
    snprintf(tbuf, sizeof tbuf, "%.3f", sec);

    std::cout << "------------------------------------------" << std::endl;
    std::cout << "Количество выполненных тестов: " << n << std::endl;
    std::cout << "Успешно пройдено:              " << passed << std::endl;
    std::cout << "Завершено с ошибкой:           " << failed << std::endl;
    std::cout << "Затраченное время:             " << tbuf << " сек" << std::endl;

    if (tsv) {
        std::vector<std::string> groups;
        std::cout << "#SUMMARY\t" << n << '\t' << passed << '\t' << failed << '\t' << tbuf << std::endl;
        int i = 0;
        for (const auto& tc : registry()) {
            size_t g = 0;
            while (g < groups.size() && groups[g] != tc.group) ++g;
            if (g == groups.size()) {
                groups.push_back(tc.group);
                std::cout << "#GROUP\t" << g + 1 << '\t' << tsvSafe(tc.group) << std::endl;
            }
            std::cout << "#TEST\t" << i + 1 << '\t' << g + 1 << '\t' << tsvSafe(tc.title) << '\t'
                      << tsvSafe(tc.input) << '\t' << tsvSafe(tc.expected) << '\t'
                      << (results[static_cast<size_t>(i)].ok ? "PASS" : "FAIL") << '\t'
                      << tsvSafe(results[static_cast<size_t>(i)].msg) << std::endl;
            ++i;
        }
    }
    return failed == 0 ? 0 : 1;
}
