// net.cpp
#include "net.hpp"

#include <curl/curl.h>
#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

namespace tbx {
namespace {

std::once_flag g_curlInit;

size_t onWrite(char* data, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(data, size * nmemb);
    return size * nmemb;
}

std::string ua() {
    // 有些站源会挑 UA，太"裸"的会被拒
    return "Mozilla/5.0 (Nintendo Switch; tvbox-nx) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Version/4.0 Safari/537.36";
}

}  // namespace

HttpResult httpGet(const std::string& url, int timeoutSec) {
    HttpResult r;
    std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "curl_easy_init 失败";
        return r;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("User-Agent: " + ua()).c_str());
    headers = curl_slist_append(headers, "Accept: application/json,*/*");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)6);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // 让 curl 自己解 gzip/deflate

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        // 去掉可能存在的 BOM
        if (r.body.size() >= 3 && (unsigned char)r.body[0] == 0xEF &&
            (unsigned char)r.body[1] == 0xBB && (unsigned char)r.body[2] == 0xBF)
            r.body.erase(0, 3);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

bool readWholeFile(const std::string& path, std::string& out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace tbx
