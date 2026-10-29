#include "net/HttpClient.h"

#include "util/Log.h"

#include <curl/curl.h>

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <mutex>

namespace globecore {

std::string HttpClient::caBundlePath_;

namespace {

std::once_flag g_initOnce;
bool g_initOk = false;

// @Jin 进程级网络诊断计数；只在请求线程做原子递增，不参与下载/缓存决策。
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_completed{0};
std::atomic<uint64_t> g_failures{0};
std::atomic<uint64_t> g_reusedConnections{0};
std::atomic<uint64_t> g_newConnections{0};
std::atomic<uint64_t> g_http2Responses{0};
std::atomic<uint64_t> g_http1Responses{0};

// @Jin libcurl 写回调上下文：除了收集字节，也在传输阶段强制响应大小上限。
struct WriteSink {
    std::vector<uint8_t> *out = nullptr;
    size_t maxBytes = 0;
    bool exceeded = false;
};

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *sink = static_cast<WriteSink *>(userdata);
    const size_t bytes = size * nmemb;
    if (sink == nullptr || sink->out == nullptr) return 0;
    if (sink->maxBytes > 0 && sink->out->size() + bytes > sink->maxBytes) {
        sink->exceeded = true;
        return 0; // @Jin 返回 0 触发 CURLE_WRITE_ERROR，及时停止超大响应。
    }
    sink->out->insert(sink->out->end(), ptr, ptr + bytes);
    return bytes;
}

// 传输进度回调：返回非 0 则中断传输。用于关机时让正在进行的 GET 尽快返回，避免 join 阻塞。
int abortProgress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto *flag = static_cast<const std::atomic<bool> *>(clientp);
    return (flag != nullptr && flag->load()) ? 1 : 0;
}

// Android 系统 CA 证书目录（OpenSSL 哈希命名的单个 PEM），供 CURLOPT_CAPATH 直接校验公有 CA
constexpr const char *const kAndroidSystemCaPath = "/system/etc/security/cacerts";

// 线程常驻 easy handle：libcurl 的连接缓存挂在 handle 上，请求结束不销毁 handle 即可跨瓦片
// 复用同 host 的 TCP+TLS 连接（HTTP/1.1 keep-alive）。此前每块瓦片新建/销毁 handle，每块都要
// 付 2~3 个 RTT 的握手代价，瓦片风暴（缩放/旋转到新区域数百块）下总时延成倍放大。
// 线程退出时随 thread_local 析构自动 cleanup。
struct ThreadCurl {
    CURL *h = nullptr;
    ThreadCurl() { h = curl_easy_init(); }
    ~ThreadCurl() {
        if (h != nullptr) curl_easy_cleanup(h);
    }
};

inline CURL *threadHandle() {
    static thread_local ThreadCurl tc;
    return tc.h;
}

// @Jin 日志只保留 URL 路径并将 query 替换为 ***，避免在线 GeoJSON token 经 logcat 泄露。
std::string redactUrlForLog(const std::string &url) {
    const size_t query = url.find('?');
    if (query == std::string::npos) return url;
    std::string out = url.substr(0, query);
    return out + "?***";
}

} // namespace

bool HttpClient::globalInit() {
    std::call_once(g_initOnce, []() {
        const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_initOk = (rc == CURLE_OK);
        if (g_initOk) {
            LOGI("curl_global_init ok, version=%s", curl_version());
        } else {
            LOGE("curl_global_init failed rc=%d", static_cast<int>(rc));
        }
    });
    return g_initOk;
}

void HttpClient::globalCleanup() {
    curl_global_cleanup();
}

std::string HttpClient::version() {
    const char *v = curl_version();
    return v != nullptr ? std::string(v) : std::string();
}

void HttpClient::setCaBundle(const std::string &caInfoPath) {
    caBundlePath_ = caInfoPath;
}

HttpClient::StatsSnapshot HttpClient::stats() {
    StatsSnapshot snapshot;
    snapshot.requests = g_requests.load();
    snapshot.completed = g_completed.load();
    snapshot.failures = g_failures.load();
    snapshot.reusedConnections = g_reusedConnections.load();
    snapshot.newConnections = g_newConnections.load();
    snapshot.http2Responses = g_http2Responses.load();
    snapshot.http1Responses = g_http1Responses.load();
    return snapshot;
}

bool HttpClient::get(const std::string &url, std::vector<uint8_t> &outBytes, long timeoutMs,
                     const std::atomic<bool> *abortFlag,
                     const std::vector<std::pair<std::string, std::string>> &headers,
                     size_t maxBytes, long *responseCode, bool *sizeLimitExceeded) {
    if (!globalInit()) return false;

    CURL *curl = threadHandle();
    if (curl == nullptr) {
        LOGE("curl_easy_init failed");
        return false;
    }
    curl_easy_reset(curl); // 选项清回默认（含校验/回调），但保留 handle 内连接缓存——keep-alive 复用不丢

    g_requests.fetch_add(1); // @Jin 统计真实进入 libcurl 的请求数。
    outBytes.clear();
    if (sizeLimitExceeded != nullptr) *sizeLimitExceeded = false; // @Jin 每次请求先复位超限输出。
    WriteSink sink{&outBytes, maxBytes, false}; // @Jin 在线 GeoJSON 可通过 maxBytes 限制响应体。
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);       // 多线程环境下必须，避免 alarm 信号
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // 跟随 3xx
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "globecore/0.2.8.1");
    // @Jin 单 host 瓦片源优先协商 HTTP/2，并保持 TCP keep-alive；旧版 libcurl 缺少常量时保持原协议行为。
#if LIBCURL_VERSION_NUM >= 0x072F00
    const curl_version_info_data *curlInfo = curl_version_info(CURLVERSION_NOW);
    if (curlInfo != nullptr && (curlInfo->features & CURL_VERSION_HTTP2) != 0) {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    }
#endif
#if LIBCURL_VERSION_NUM >= 0x071900
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
#endif
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 300L);

    // @Jin 宿主注入的请求头用于 Referer/Token 等数据源策略；请求结束统一释放，不污染线程常驻 handle。
    curl_slist *headerList = nullptr;
    for (const auto &header : headers) {
        const std::string line = header.first + ": " + header.second;
        curl_slist *next = curl_slist_append(headerList, line.c_str());
        if (next == nullptr) {
            curl_slist_free_all(headerList);
            headerList = nullptr;
            break;
        }
        headerList = next;
    }
    if (headerList != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    // 可中断传输：传入 abortFlag 时启用进度回调，标志置位则中断
    if (abortFlag != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abortFlag);
    }

    // HTTPS 证书校验：配置了 CA 包则用之；否则用 Android 系统证书目录（CAPATH）校验公有 CA
    if (!caBundlePath_.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath_.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CAPATH, kAndroidSystemCaPath);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    CURLcode rc = curl_easy_perform(curl);

    // 证书校验失败（部分设备系统证书目录不可用/不完整）时降级为不校验重试一次，保证瓦片可取
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE) {
        LOGW("curl TLS 校验失败(%s)，降级不校验重试 url=%s", curl_easy_strerror(rc), redactUrlForLog(url).c_str());
        outBytes.clear();
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        rc = curl_easy_perform(curl);
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (responseCode != nullptr) *responseCode = httpCode; // @Jin 在线 GeoJSON 用它生成 HTTP 状态错误。
    if (sizeLimitExceeded != nullptr) *sizeLimitExceeded = sink.exceeded; // @Jin 区分超大响应与普通网络失败。
    // @Jin 记录连接复用和协议版本，真机日志可直接确认请求慢是否仍出在重复 TCP/TLS 握手。
    long newConnections = 0;
    long httpVersion = 0;
    curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &newConnections);
#if LIBCURL_VERSION_NUM >= 0x073200
    curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &httpVersion);
#endif
    if (newConnections > 0) {
        g_newConnections.fetch_add(static_cast<uint64_t>(newConnections));
    } else if (rc == CURLE_OK && httpCode >= 200 && httpCode < 300) {
        g_reusedConnections.fetch_add(1);
    }
#if LIBCURL_VERSION_NUM >= 0x073200
    if (httpVersion >= CURL_HTTP_VERSION_2_0) {
        g_http2Responses.fetch_add(1);
    } else if (httpVersion == CURL_HTTP_VERSION_1_1 || httpVersion == CURL_HTTP_VERSION_1_0) {
        g_http1Responses.fetch_add(1);
    }
#endif
    // @Jin curl_slist 仅本请求有效；连接缓存属于 easy handle，不受 header list 释放影响。
    curl_slist_free_all(headerList);
    // 刻意不 cleanup：handle 线程常驻，连接缓存跨请求复用（线程退出由 ThreadCurl 析构回收）

    if (rc != CURLE_OK) {
        g_failures.fetch_add(1);
        // 主动中断（abort）属正常关机路径，降为 debug 级避免刷屏
        if (abortFlag != nullptr && abortFlag->load()) {
            outBytes.clear();
            return false;
        }
        LOGW("curl GET 失败 url=%s err=%s", redactUrlForLog(url).c_str(), curl_easy_strerror(rc));
        // @Jin 超限要保留已接收长度给上层生成明确中文错误；普通网络失败仍清空。
        if (!sink.exceeded) outBytes.clear();
        return false;
    }
    if (httpCode < 200 || httpCode >= 300) {
        g_failures.fetch_add(1);
        LOGW("curl GET 非 2xx（HTTP %ld）url=%s", httpCode, redactUrlForLog(url).c_str());
        outBytes.clear();
        return false;
    }
    g_completed.fetch_add(1); // @Jin 仅 HTTP 2xx 计为成功完成。
    return true;
}

} // namespace globecore
