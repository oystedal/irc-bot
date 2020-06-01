#include "find_youtube_ids.hpp"

#include "ctre.hpp"

std::vector<std::string_view> find_youtube_ids(std::string_view str)
{
    std::vector<std::string_view> ids;

    const auto add_id = [&] (std::string_view id) {
        // the actual id is probably going to be shorter,
        // this just stops us from making crazy requests
        if (id.length() < 16) {
            ids.emplace_back(id);
        }
    };

    auto pos = str.find("youtu");

    while (pos != std::string_view::npos && pos < str.length()) {
        auto haystack = str.substr(pos, str.size() - pos);

        // youtu.be/...
        static constexpr auto youtu_be_pattern = ctll::fixed_string{ "(youtu\\.be/[A-Za-z0-9_\\-]+).*" };
        if (auto [match, url] = ctre::match<youtu_be_pattern>(haystack); match) {
            auto id = url.to_view();
            id.remove_prefix(id.find("/") + 1);
            add_id(id);
            pos += url.size();
            pos = str.find("youtu", pos);
            continue;
        }

        // youtube.com/watch?...v=...
        static constexpr auto youtube_pattern = ctll::fixed_string{ "(youtube\\.com/watch\\?\\S*?v=[A-Za-z0-9_\\-]+).*" };
        if (const auto [match, url] = ctre::match<youtube_pattern>(haystack); match) {
            auto id = url.to_view();
            id.remove_prefix(id.find("v=") + 2);
            add_id(id);
            pos += url.size();
            pos = str.find("youtu", pos);
            continue;
        }

        // youtube.com/embed/...
        static constexpr auto embed_pattern = ctll::fixed_string{ "(youtube\\.com/embed/[A-Za-z0-9_\\-]+).*" };
        if (const auto [match, url] = ctre::match<embed_pattern>(haystack); match) {
            auto id = url.to_view();
            id.remove_prefix(id.find("embed/") + 6);
            add_id(id);
            pos += url.size();
            pos = str.find("youtu", pos);
            continue;
        }

        ++pos;
        pos = str.find("youtu", pos);
    }

    return ids;
}

