#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include "net_stream.hpp"
#include "CurlEngine.hpp"
#include "find_youtube_ids.hpp"

#include "fmt/format.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <list>
#include <fstream>

#include <cstdlib>

using namespace std::literals;

class TimerEngine
{
public:
    using timer_type = boost::asio::steady_timer;

    TimerEngine(boost::asio::io_context& executor)
        : executor_(executor)
    {}

    timer_type create_timer()
    {
        return boost::asio::steady_timer(executor_);
    }
private:
    boost::asio::io_context& executor_;
};

nlohmann::json get_config()
{
    std::ifstream fstream;
    fstream.open("config.json");
    if (!fstream) {
        fmt::print("Unable to open config.json");
        exit(1);
    }

    try {
        return nlohmann::json::parse(fstream);
    } catch (const nlohmann::json::exception& e) {
        fmt::print("Failed to read config: {}\n", e.what());
        exit(1);
    }
}

int main(int, const char*[])
{
    const auto config = get_config();

    const std::string irc_server = config.at("irc").at("server");
    const std::string irc_channel = config.at("irc").at("channel");
    const std::string irc_nick = config.at("irc").at("nick");
    const std::string youtube_key = config.at("apis").at("youtube").at("key");
    // TODO: better error handling above...

    curl_global_init(CURL_GLOBAL_ALL);

    boost::asio::io_context io_context;
    boost::asio::ip::tcp::resolver resolver(io_context);
    boost::asio::ssl::context ssl_context(boost::asio::ssl::context_base::sslv23);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io_context, ssl_context);

    TimerEngine timer_engine(io_context);
    CurlEngine http_engine(io_context);
    std::thread thread{ [&] { http_engine.run(); } };

    net_stream<
        boost::asio::io_context,
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket>,
        boost::asio::ip::tcp::resolver,
        TimerEngine>
    irc(io_context, resolver, stream, timer_engine);

    irc.on_connected([&, irc_nick] {
        std::string connect_msg;
        connect_msg += fmt::format("NICK {}\r\n", irc_nick);
        connect_msg += fmt::format("USER {} remotehost remoteserver :Forkey Bot\r\n", irc_nick);

        irc.write(connect_msg);
    });

    irc.connect(irc_server);

    irc.on_error([] (boost::system::error_code ec) {
        throw boost::system::system_error(ec);
    });

    const auto is_ping_message = [] (std::string_view msg) {
        return msg.find("PING :") == 0;
    };

    const auto make_pong = [] (std::string_view ping) {
        ping.remove_prefix(ping.find(":"));
        return fmt::format("PONG {}\r\n", ping);
    };

    const auto is_mode_message = [irc_nick] (std::string_view msg) {
        static const std::string modeline = fmt::format("MODE {}", irc_nick);
        return msg.find(modeline) != std::string_view::npos;
    };

    std::function<void()> join_channel = [&] {
        static constexpr std::string_view join_msg = "C++ is a \x02great\x02 language";
        std::string msg = fmt::format("JOIN {}\r\n", irc_channel);
        msg += fmt::format("PRIVMSG {} :{}\r\n", irc_channel, join_msg);
        irc.write(msg);
    };

    const auto is_privmsg = [] (std::string_view msg) {
        auto space = msg.find(" ");
        if (space == std::string_view::npos)
            return false;
        auto privmsg = msg.find("PRIVMSG", space + 1);
        return privmsg != std::string_view::npos;
    };

    const auto is_hello = [] (std::string_view msg) {
        auto space = msg.find(" ");
        if (space == std::string_view::npos)
            return false;
        auto text = msg.find(":", space + 1);
        if (text == std::string_view::npos)
            return false;
        msg.remove_prefix(text);
        return msg.find(":.hello") == 0;
    };

    const auto get_nick = [] (std::string_view msg) -> std::optional<std::string_view> {
        auto exclamation = msg.find("!");
        if (exclamation == std::string_view::npos)
            return std::nullopt;
        msg.remove_suffix(msg.size() - exclamation);
        msg.remove_prefix(1);
        return msg;
    };

    irc.on_read([&] (std::string_view msg) {
        fmt::print("< {}\n", msg);
        if (is_ping_message(msg)) {
            irc.write(make_pong(msg));
        }

        if (is_mode_message(msg) && join_channel) {
            join_channel();
            join_channel = nullptr;
        }

        if (is_privmsg(msg) && is_hello(msg)) {
            if (auto nick = get_nick(msg)) {
                irc.write(fmt::format("PRIVMSG {} :hi {}\r\n", irc_channel, *nick));
            }
        }

        if (is_privmsg(msg)) {
            if (auto youtube_ids = find_youtube_ids(msg); !youtube_ids.empty()) {
                std::for_each(
                    std::begin(youtube_ids), std::end(youtube_ids),
                    [&] (const auto id) {
                        std::string url = fmt::format(
                            R"(https://www.googleapis.com/youtube/v3/videos?id={}&part=snippet,contentDetails&key={})",
                            id,
                            youtube_key
                        );

                        request_data request;
                        request.url = url;
                        request.callback = [&] (std::string title) {
                            irc.write(fmt::format("PRIVMSG {} :{}\r\n", irc_channel, title));
                        };

                        http_engine.execute(std::move(request));
                    }
                );
            }
        }
    });

    fmt::print("Starting executor\n");
    io_context.run();
    fmt::print("Executor stopped\n");

    http_engine.stop();
    thread.join();

    curl_global_cleanup();

    return 0;
}
