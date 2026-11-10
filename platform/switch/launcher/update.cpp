// See update.h.
#include "update.h"

#include <nlohmann/json.hpp>
#include <switch.h>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace CaneSwitch::Update
{
namespace
{
constexpr const char* kLatest = "https://api.github.com/repos/FridiNaTor1/SafeHouse/releases/latest";
constexpr const char* kDownloads = "https://github.com/FridiNaTor1/SafeHouse/releases/download/";
constexpr const char* kAsset = "SafeHouse.nro";
constexpr int kConnectMs = 10000, kIdleMs = 20000, kPollMs = 100;

std::mutex g_mutex;
Status g_status;
std::thread g_thread;
std::atomic<bool> g_cancel{false};
std::string g_self;
// the release's SafeHouse.nro
std::string g_url, g_sha256;
uint64_t g_size = 0;
// the services started here
bool g_nifm = false, g_sockets = false, g_ssl = false, g_contextOpen = false;
SslContext g_context{};

void SetState(State state, const std::string& message = {})
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.state = state;
    g_status.message = message;
}
std::string ResultText(Result rc)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return text;
}

// The network up (false: offline, or a service did not start).
bool StartNetwork()
{
    if (!g_nifm) g_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    if (!g_nifm || R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status)) ||
        status != NifmInternetConnectionStatus_Connected)
        return false;
    if (!g_sockets) g_sockets = R_SUCCEEDED(socketInitializeDefault());
    if (!g_ssl) g_ssl = R_SUCCEEDED(sslInitialize(1));
    if (g_ssl && !g_contextOpen) g_contextOpen = R_SUCCEEDED(sslCreateContext(&g_context, SslVersion_Auto));
    return g_sockets && g_contextOpen;
}
void StopNetwork()
{
    if (g_contextOpen) sslContextClose(&g_context);
    if (g_ssl) sslExit();
    if (g_sockets) socketExit();
    if (g_nifm) nifmExit();
    g_contextOpen = g_ssl = g_sockets = g_nifm = false;
}

// One TLS connection to a host's port 443, checked by the system against its certificate authorities.
class Https
{
public:
    Https() = default;
    Https(const Https&) = delete;
    Https& operator=(const Https&) = delete;
    ~Https()
    {
        if (m_open)
        {
            if (m_sslFd >= 0) close(m_sslFd);
            sslConnectionClose(&m_connection);
        }
        if (m_fd >= 0) close(m_fd);
    }

    // "" when connected, else why not.
    std::string Connect(const std::string& host)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(host.c_str(), "443", &hints, &list) != 0 || !list) return host + " was not found";
        for (addrinfo* a = list; a && m_fd < 0 && !g_cancel; a = a->ai_next)
        {
            const int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            // (the connection's wait bounded: non-blocking, then poll)
            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            bool connected = connect(fd, a->ai_addr, a->ai_addrlen) == 0;
            if (!connected && errno == EINPROGRESS)
            {
                pollfd p{fd, POLLOUT, 0};
                int error = 0;
                socklen_t length = sizeof(error);
                connected = poll(&p, 1, kConnectMs) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
            }
            fcntl(fd, F_SETFL, flags);
            if (connected) m_fd = fd;
            else close(fd);
        }
        freeaddrinfo(list);
        if (m_fd < 0) return "no connection to " + host;
        if (R_FAILED(sslContextCreateConnection(&g_context, &m_connection))) return "the TLS service refused a connection";
        m_open = true;
        m_sslFd = socketSslConnectionSetSocketDescriptor(&m_connection, m_fd);
        if (m_sslFd < 0 && errno != ENOENT) return "the TLS service refused the socket";
        Result rc = sslConnectionSetHostName(&m_connection, host.c_str(), u32(host.size()));
        if (R_SUCCEEDED(rc)) rc = sslConnectionSetVerifyOption(&m_connection, SslVerifyOption_PeerCa | SslVerifyOption_HostName);
        if (R_SUCCEEDED(rc)) rc = sslConnectionDoHandshake(&m_connection, nullptr, nullptr, nullptr, 0);
        if (R_FAILED(rc)) return "no secure connection to " + host + " (" + ResultText(rc) + ")";
        return {};
    }
    bool Send(const std::string& data)
    {
        for (size_t sent = 0; sent < data.size();)
        {
            u32 n = 0;
            if (R_FAILED(sslConnectionWrite(&m_connection, data.data() + sent, u32(data.size() - sent), &n)) || n == 0) return false;
            sent += n;
        }
        return true;
    }
    // Bytes into `buffer`: their count, 0 when the server closed the connection, -1 on failure, cancel or a long silence.
    int Read(char* buffer, size_t size)
    {
        for (int waited = 0;;)
        {
            if (g_cancel) return -1;
            s32 pending = 0;
            if (R_SUCCEEDED(sslConnectionPending(&m_connection, &pending)) && pending > 0) break;
            pollfd p{m_fd, POLLIN, 0};
            const int ready = poll(&p, 1, kPollMs);
            if (ready > 0) break;   // (data, or the end: the read says which)
            if (ready < 0 || (waited += kPollMs) >= kIdleMs) return -1;
        }
        u32 n = 0;
        if (R_FAILED(sslConnectionRead(&m_connection, buffer, u32(std::min<size_t>(size, 0x8000)), &n))) return -1;
        return int(n);
    }

private:
    int m_fd = -1, m_sslFd = -1;
    SslConnection m_connection{};
    bool m_open = false;
};

// A response's lines and bytes.
class Reader
{
public:
    explicit Reader(Https& https) : m_https(https), m_buffer(0x8000) {}
    bool Line(std::string& line)
    {
        line.clear();
        for (;;)
        {
            if (m_at == m_end && !Fill()) return false;
            const char c = m_buffer[m_at++];
            if (c == '\n')
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            if (line.size() >= 8192) return false;
            line += c;
        }
    }
    // Up to `most` bytes: where they are and their count (0: the connection ended or failed).
    size_t Bytes(const char*& data, size_t most)
    {
        if (m_at == m_end && !Fill()) return 0;
        const size_t n = std::min(most, m_end - m_at);
        data = m_buffer.data() + m_at;
        m_at += n;
        return n;
    }

private:
    bool Fill()
    {
        const int n = m_https.Read(m_buffer.data(), m_buffer.size());
        if (n <= 0) return false;
        m_at = 0;
        m_end = size_t(n);
        return true;
    }
    Https& m_https;
    std::vector<char> m_buffer;
    size_t m_at = 0, m_end = 0;
};

struct Url
{
    std::string host, path;
};
bool ParseUrl(const std::string& url, Url& out)
{
    if (url.rfind("https://", 0) != 0) return false;
    const size_t slash = url.find('/', 8);
    out.host = url.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    return !out.host.empty() && out.host.find_first_of(":@") == std::string::npos;
}
std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// An HTTPS GET's body into `sink` (redirects followed, HTTPS only). `start`: told the body's size when the server
// gives it (else 0), false refuses it. "" when the whole body came, else why not.
std::string Get(std::string url, bool api, const std::function<bool(uint64_t)>& start,
                const std::function<bool(const char*, size_t)>& sink)
{
    for (int redirects = 0; redirects <= 5; ++redirects)
    {
        Url u;
        if (!ParseUrl(url, u)) return "an unexpected address";
        Https https;
        if (const std::string error = https.Connect(u.host); !error.empty()) return g_cancel ? "cancelled" : error;
        std::string request = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + "\r\nUser-Agent: SafeHouse-Switch/" CANE_SWITCH_VERSION "\r\n";
        if (api) request += "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        request += "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
        if (!https.Send(request)) return "the request to " + u.host + " failed";
        Reader reader(https);
        std::string line;
        int status = 0;
        if (!reader.Line(line) || std::sscanf(line.c_str(), "HTTP/%*s %d", &status) != 1)
            return g_cancel ? "cancelled" : "no answer from " + u.host;
        int64_t length = -1;
        bool chunked = false;
        std::string location;
        for (;;)
        {
            if (!reader.Line(line)) return g_cancel ? "cancelled" : "the answer from " + u.host + " was cut";
            if (line.empty()) break;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = Lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            value.erase(0, std::min(value.size(), value.find_first_not_of(" \t")));
            if (name == "content-length") length = std::strtoll(value.c_str(), nullptr, 10);
            else if (name == "transfer-encoding") chunked = Lower(value).find("chunked") != std::string::npos;
            else if (name == "location") location = value;
        }
        if (status >= 300 && status < 400 && !location.empty())
        {
            url = location;
            api = false;
            continue;
        }
        if (status == 403 || status == 429) return "GitHub refused the request for now: try again later";
        if (status != 200) return "HTTP error " + std::to_string(status) + " from " + u.host;
        if (start && !start(length > 0 ? uint64_t(length) : 0)) return "the download is not the expected size";
        const char* data = nullptr;
        const auto cut = [] { return std::string(g_cancel ? "cancelled" : "the download was cut"); };
        if (chunked)
        {
            for (;;)
            {
                if (!reader.Line(line)) return cut();
                const uint64_t size = std::strtoull(line.c_str(), nullptr, 16);
                if (size == 0) return {};
                for (uint64_t left = size; left;)
                {
                    const size_t n = reader.Bytes(data, size_t(std::min<uint64_t>(left, 0x8000)));
                    if (n == 0) return cut();
                    if (!sink(data, n)) return "the download is not what was expected";
                    left -= n;
                }
                if (!reader.Line(line)) return cut();   // (the chunk's line end)
            }
        }
        for (uint64_t left = length >= 0 ? uint64_t(length) : UINT64_MAX; left;)
        {
            const size_t n = reader.Bytes(data, size_t(std::min<uint64_t>(left, 0x8000)));
            if (n == 0) return length < 0 && !g_cancel ? std::string() : cut();   // (no length: the end is the close)
            if (!sink(data, n)) return "the download is not what was expected";
            left -= n;
        }
        return {};
    }
    return "too many redirections";
}

// "v0.1.27" -> {0, 1, 27} (empty: not a version)
std::vector<int> VersionParts(std::string v)
{
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    for (size_t i = 0; i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]));)
    {
        int n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) n = std::min(n * 10 + (v[i++] - '0'), 1000000);
        parts.push_back(n);
        if (i >= v.size() || v[i] != '.') break;
        ++i;
    }
    return parts;
}
bool Newer(const std::string& a, const std::string& b)
{
    const std::vector<int> x = VersionParts(a), y = VersionParts(b);
    if (x.empty() || y.empty()) return false;
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i)
    {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q;
    }
    return false;
}
std::string Text(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void CheckThread()
{
    if (!StartNetwork())
    {
        SetState(State::Idle);
        return;
    }
    std::string body;
    const std::string error = Get(kLatest, true, nullptr, [&body](const char* data, size_t size) {
        if (body.size() + size > (4u << 20)) return false;
        body.append(data, size);
        return true;
    });
    if (!error.empty())
    {
        SetState(State::Idle);
        return;
    }
    const nlohmann::json release = nlohmann::json::parse(body, nullptr, false);
    const std::string tag = release.is_object() ? Text(release, "tag_name") : std::string();
    if (!Newer(tag, CANE_SWITCH_VERSION))
    {
        SetState(State::Idle);
        return;
    }
    // its SafeHouse.nro, with the SHA-256 GitHub computed when it was uploaded
    std::string url, digest;
    uint64_t size = 0;
    const auto assets = release.find("assets");
    if (assets != release.end() && assets->is_array())
        for (const nlohmann::json& asset : *assets)
        {
            if (!asset.is_object() || Text(asset, "name") != kAsset) continue;
            url = Text(asset, "browser_download_url");
            digest = Lower(Text(asset, "digest"));
            const auto bytes = asset.find("size");
            if (bytes != asset.end() && bytes->is_number_unsigned()) size = bytes->get<uint64_t>();
        }
    const std::string sha256 = digest.rfind("sha256:", 0) == 0 ? digest.substr(7) : std::string();
    const bool hex = sha256.size() == 64 && std::all_of(sha256.begin(), sha256.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
    if (url.rfind(kDownloads, 0) != 0 || size < (1u << 20) || size > (512u << 20) || !hex)
    {
        SetState(State::Idle);
        return;
    }
    g_url = url;
    g_sha256 = sha256;
    g_size = size;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.version = tag.substr(tag[0] == 'v' || tag[0] == 'V' ? 1 : 0);
    g_status.state = State::Available;
}

bool IsNro(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char header[0x20] = {};
    const bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header) && std::memcmp(header + 0x10, "NRO0", 4) == 0;
    std::fclose(file);
    return ok;
}
// The new SafeHouse.nro in the place of the running one, which is kept until then. "" or why not.
std::string Replace(const std::string& downloaded)
{
    const std::string previous = g_self + ".old";
    std::remove(previous.c_str());
    if (std::rename(g_self.c_str(), previous.c_str()) != 0) return "SafeHouse.nro could not be renamed";
    if (std::rename(downloaded.c_str(), g_self.c_str()) != 0)
    {
        std::rename(previous.c_str(), g_self.c_str());
        return "the new SafeHouse.nro could not be put in place";
    }
    std::remove(previous.c_str());
    return {};
}

void InstallThread()
{
    const std::string downloaded = g_self + ".update";
    std::string error;
    struct statvfs st{};
    if (statvfs("sdmc:/", &st) == 0 && uint64_t(st.f_bavail) * uint64_t(st.f_bsize) < g_size + (8u << 20))
        error = "there is not enough free space on the SD card";
    FILE* file = error.empty() ? std::fopen(downloaded.c_str(), "wb") : nullptr;
    if (error.empty() && !file) error = "the download cannot be written to the SD card";
    if (file)
    {
        Sha256Context sha;
        sha256ContextCreate(&sha);
        uint64_t done = 0;
        bool writeFailed = false;
        error = Get(g_url, false, [](uint64_t length) { return length == 0 || length == g_size; },
                    [&](const char* data, size_t size) {
                        if (done + size > g_size) return false;
                        if (std::fwrite(data, 1, size, file) != size)
                        {
                            writeFailed = true;
                            return false;
                        }
                        sha256ContextUpdate(&sha, data, size);
                        done += size;
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_status.done = done;
                        return true;
                    });
        if (std::fclose(file) != 0 || writeFailed) error = "the download could not be written to the SD card";
        if (error.empty() && done != g_size) error = "the download is incomplete";
        if (error.empty())
        {
            u8 hash[SHA256_HASH_SIZE];
            sha256ContextGetHash(&sha, hash);
            char hex[SHA256_HASH_SIZE * 2 + 1];
            for (size_t i = 0; i < SHA256_HASH_SIZE; ++i) std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
            if (g_sha256 != hex) error = "the download is damaged (its SHA-256 does not match)";
        }
        if (error.empty() && !IsNro(downloaded)) error = "the download is not a Switch program";
        if (error.empty()) error = Replace(downloaded);
    }
    if (!error.empty())
    {
        std::remove(downloaded.c_str());
        if (g_cancel) SetState(State::Available);
        else SetState(State::Failed, error);
        return;
    }
    SetState(State::Installed);
}

void Join()
{
    if (g_thread.joinable()) g_thread.join();
}
}  // namespace

void Check(const std::string& self)
{
    // (a development build has no release version: it never checks)
    if (self.empty() || CANE_SWITCH_VERSION[0] == '\0') return;
    Join();
    g_self = self;
    // (an earlier update's leftovers)
    std::remove((self + ".update").c_str());
    std::remove((self + ".old").c_str());
    g_cancel = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = Status{};
        g_status.state = State::Checking;
    }
    g_thread = std::thread(CheckThread);
}

Status Get()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void Install()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status.state != State::Available) return;
        g_status.state = State::Downloading;
        g_status.done = 0;
        g_status.total = g_size;
    }
    Join();
    g_cancel = false;
    g_thread = std::thread(InstallThread);
}

void Cancel() { g_cancel = true; }

void Stop()
{
    g_cancel = true;
    Join();
    StopNetwork();
}
}  // namespace CaneSwitch::Update
