#ifndef CURL_ENGINE_HPP_INCLUDED
#define CURL_ENGINE_HPP_INCLUDED

#include <curl/curl.h>

#include <boost/asio.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <string>

extern std::string youtube_key;

struct request_data {
    std::string url;
    std::function<void(std::string)> callback;
};

int writer(char *data, size_t size, size_t nmemb, std::string *writerData);

class CurlEngine
{
public:
    CurlEngine(boost::asio::io_context& io_context);

    void execute(request_data&& request);
    void stop();
    void run();


private:
    CURL* init_request(std::string_view url);
    void perform_request(request_data&& request);

    std::mutex mutex;
    std::condition_variable cv;
    std::list<request_data> requests;
    std::atomic<bool> running;
    boost::asio::io_context& io_context_;
};

#endif
