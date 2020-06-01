#include "CurlEngine.hpp"
#include "fmt/format.h"

#include "ctre.hpp"
#include "nlohmann/json.hpp"

char errorBuffer[CURL_ERROR_SIZE];
std::string curl_buffer;
std::string youtube_key;

int writer(char *data, size_t size, size_t nmemb, std::string *writerData)
{
    if(writerData == NULL)
        return 0;

    writerData->append(data, size*nmemb);

    return size * nmemb;
}

CurlEngine::CurlEngine(boost::asio::io_context& io_context)
    : running(false)
    , io_context_(io_context)
{}

CURL* CurlEngine::init_request(std::string_view url)
{
    CURLcode code;

    curl_buffer.clear();
    CURL* conn = curl_easy_init();

    if(conn == NULL) {
        fprintf(stderr, "Failed to create CURL connection\n");
        exit(EXIT_FAILURE);
    }

    code = curl_easy_setopt(conn, CURLOPT_ERRORBUFFER, errorBuffer);
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set error buffer [%d]\n", code);
        return nullptr;
    }

    code = curl_easy_setopt(conn, CURLOPT_URL, url.data());
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set URL [%s]\n", errorBuffer);
        return nullptr;
    }

    code = curl_easy_setopt(conn, CURLOPT_FOLLOWLOCATION, 1L);
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set redirect option [%s]\n", errorBuffer);
        return nullptr;
    }

    code = curl_easy_setopt(conn, CURLOPT_WRITEFUNCTION, writer);
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set writer [%s]\n", errorBuffer);
        return nullptr;
    }

    code = curl_easy_setopt(conn, CURLOPT_WRITEDATA, &curl_buffer);
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set write data [%s]\n", errorBuffer);
        return nullptr;
    }

    code = curl_easy_setopt(conn, CURLOPT_SSL_VERIFYPEER, 0L);
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set peer verification [%s]\n", errorBuffer);
        return nullptr;
    }

    code = curl_easy_setopt(conn, CURLOPT_SSL_VERIFYHOST, 0L);
    if(code != CURLE_OK) {
        fprintf(stderr, "Failed to set host verification [%s]\n", errorBuffer);
        return nullptr;
    }

    return conn;
}

void CurlEngine::execute(request_data&& request)
{
    std::unique_lock lg{mutex};
    requests.emplace_back(std::move(request));
    cv.notify_one();
}

void CurlEngine::stop()
{
    running = false;
}

void CurlEngine::run()
{
    running = true;
    while (running) {
        std::unique_lock lg{mutex};

        if (requests.empty()) {
            cv.wait(lg);
        }

        perform_request(std::move(requests.front()));
        requests.pop_front();
    }
}

void CurlEngine::perform_request(request_data&& request)
{
    fmt::print("Request: {}\n", request.url);
    CURL* conn = init_request(request.url);

    const CURLcode code = curl_easy_perform(conn);
    curl_easy_cleanup(conn);

    if(code != CURLE_OK) {
        fmt::print("Failed to get '{}' [{}]\n", request.url.data(), errorBuffer);
        return;
    }


    const auto parse_duration = [] (std::string str) -> std::string {
        static constexpr auto duration_hms = ctll::fixed_string{ "PT(\\d+)H(\\d+)M(\\d+)S" };
        if (auto [match, hours, minutes, seconds] = ctre::match<duration_hms>(str); match) {
            return fmt::format("{}h{}m{}s", hours, minutes, seconds);
        }

        static constexpr auto duration_ms = ctll::fixed_string{ "PT(\\d+)M(\\d+)S" };
        if (auto [match, minutes, seconds] = ctre::match<duration_ms>(str); match) {
            return fmt::format("{}m{}s", minutes, seconds);
        }

        static constexpr auto duration_s = ctll::fixed_string{ "PT(\\d+)S" };
        if (auto [match, seconds] = ctre::match<duration_s>(str); match) {
            return fmt::format("{}s", seconds);
        }
        return "";
    };

    std::optional<std::string> str;
    try {
        // TODO: Parse this a bit more sensibly
        nlohmann::json response = nlohmann::json::parse(curl_buffer);
        std::string title = response.at("items").at(0).at("snippet").at("title");
        std::string duration = parse_duration(response.at("items").at(0).at("contentDetails").at("duration"));

        if (!duration.empty()) {
            str = fmt::format("\x02youtube\x02: {} ({})", title, duration);
        } else {
            str = fmt::format("\x02youtube\x02: {}", title);
        }
    } catch (const nlohmann::json::exception& e) {
        fmt::print("json error: {}\n", e.what());
        fmt::print("curl buffer: >>>\n");
        fmt::print("{}", curl_buffer);
        fmt::print("\n<<<\n");
    }

    if (str) {
        boost::asio::post(
            io_context_,
            [str = *str, callback = request.callback] {
                callback(str);
            }
        );
    }
}
