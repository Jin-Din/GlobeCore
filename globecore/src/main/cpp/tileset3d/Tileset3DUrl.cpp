#include "tileset3d/Tileset3DUrl.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace globecore {
namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string stripQueryAndFragment(const std::string &url) {
    const size_t query = url.find('?');
    const size_t fragment = url.find('#');
    size_t cut = std::string::npos;
    if (query != std::string::npos && fragment != std::string::npos) {
        cut = std::min(query, fragment);
    } else if (query != std::string::npos) {
        cut = query;
    } else {
        cut = fragment;
    }
    return cut == std::string::npos ? url : url.substr(0, cut);
}

std::string normalizePath(const std::string &path) {
    std::vector<std::string> segments;
    std::stringstream input(path);
    std::string item;
    while (std::getline(input, item, '/')) {
        if (item == ".") {
            continue; // @Jin 当前目录段不进入最终路径。
        }
        if (item == "..") {
            // @Jin 仅在存在普通目录时回退；站点根路径之前的 .. 保留为空段避免越界。
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            }
            continue;
        }
        segments.push_back(item);
    }

    std::string result;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) result.push_back('/');
        result += segments[i];
    }
    if (!path.empty() && path.back() == '/' && result.back() != '/') {
        result.push_back('/');
    }
    return result;
}

} // namespace

std::string Tileset3DUrl::resolve(const std::string &baseUrl, const std::string &reference) {
    if (reference.empty()) return baseUrl;
    if (reference.rfind("http://", 0) == 0 || reference.rfind("https://", 0) == 0) {
        return reference; // @Jin 绝对 URL 直接使用。
    }

    const std::string baseWithoutQuery = stripQueryAndFragment(baseUrl);
    const size_t schemePos = baseWithoutQuery.find("://");
    if (schemePos == std::string::npos) {
        return reference; // @Jin P0 只支持 http/https 服务地址，非 URL 输入不做磁盘路径猜测。
    }

    const size_t pathStart = baseWithoutQuery.find('/', schemePos + 3);
    const std::string origin = pathStart == std::string::npos
                                   ? baseWithoutQuery
                                   : baseWithoutQuery.substr(0, pathStart);
    const std::string basePath = pathStart == std::string::npos
                                     ? "/"
                                     : baseWithoutQuery.substr(pathStart);

    std::string refPath = reference;
    std::string suffix;
    const size_t queryPos = reference.find('?');
    const size_t hashPos = reference.find('#');
    size_t suffixPos = std::string::npos;
    if (queryPos != std::string::npos && hashPos != std::string::npos) {
        suffixPos = std::min(queryPos, hashPos);
    } else if (queryPos != std::string::npos) {
        suffixPos = queryPos;
    } else {
        suffixPos = hashPos;
    }
    if (suffixPos != std::string::npos) {
        refPath = reference.substr(0, suffixPos);
        suffix = reference.substr(suffixPos);
    }

    std::string combined;
    if (refPath.rfind("//", 0) == 0) {
        // @Jin 协议相对 URL（//host/path）优先继承 http/https 协议，不能误判为站点根路径。
        return baseWithoutQuery.substr(0, schemePos) + ":" + reference;
    } else if (!refPath.empty() && refPath.front() == '/') {
        combined = refPath; // @Jin 以单斜杠开头表示相对于站点 origin。
    } else {
        const size_t lastSlash = basePath.rfind('/');
        const std::string directory = lastSlash == std::string::npos ? "/" : basePath.substr(0, lastSlash + 1);
        combined = directory + refPath;
    }

    return origin + normalizePath(combined) + suffix;
}

bool Tileset3DUrl::isTilesetJson(const std::string &url) {
    const std::string clean = lowerCopy(stripQueryAndFragment(url));
    return clean.size() >= 5 && clean.compare(clean.size() - 5, 5, ".json") == 0;
}

bool Tileset3DUrl::isB3dm(const std::string &url) {
    const std::string clean = lowerCopy(stripQueryAndFragment(url));
    return clean.size() >= 5 && clean.compare(clean.size() - 5, 5, ".b3dm") == 0;
}

} // namespace globecore


