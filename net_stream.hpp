#ifndef NET_STREAM_HPP
#define NET_STREAM_HPP

#include <boost/asio.hpp>

#include <fmt/format.h> // TODO: replace with some logger stuff

#include <string_view>
#include <list>
#include <deque>

template<
    typename Executor,
    typename Stream,
    typename Resolver,
    typename TimerEngine
>
class net_stream
{
public:
    net_stream(Executor& executor, Resolver& resolver, Stream& stream, TimerEngine& timer_engine)
        : executor_(executor)
        , resolver_(resolver)
        , stream_(stream)
        , connect_timer_(timer_engine.create_timer())
    {
    }

    void connect(std::string_view address)
    {
        resolver_.async_resolve(
            std::string(address),
            "6667",
            [this] (const boost::system::error_code & ec, boost::asio::ip::tcp::resolver::results_type results) {
                fmt::print("resolve callback\n");
                connect_timer_.cancel();

                if (ec) {
                    boost::asio::post(executor_, [this, ec] { error_callback_(ec); });
                    return;
                }

                connect(results);
            }
        );

        using namespace std::chrono_literals;
        set_timeout(10s);
    }

    using error_callback = std::function<void (boost::system::error_code)>;
    void on_error(error_callback&& callback)
    {
        error_callback_ = callback;
    }

    using connect_callback = std::function<void ()>;
    void on_connected(connect_callback&& callback)
    {
        // TODO: add tests
        on_connect_ = std::move(callback);
    }

    using read_callback = std::function<void (std::string_view str)>;
    void on_read(read_callback&& callback)
    {
        on_read_ = std::move(callback);
    }

    void write(std::string_view message)
    {
        const bool start_writing = message_queue_.empty();
        message_queue_.emplace_back(std::string(message));
        if (start_writing) {
            do_write();
        }
    }

    void do_write()
    {
        fmt::print("> ");
        std::for_each(
            std::begin(message_queue_.front()), std::end(message_queue_.front()),
            [] (char c) {
                if (c == '\n') {
                    fmt::print("\\n");
                } else if (c == '\r') {
                    fmt::print("\\r");
                } else {
                    fmt::print("{}", c);
                }
            }
        );
        fmt::print("\n");

        boost::asio::async_write(
            stream_,
            boost::asio::buffer(
                message_queue_.front().data(),
                message_queue_.front().length()),
            [this](boost::system::error_code ec, std::size_t /*length*/)
            {
                if (ec) {
                    boost::asio::post( executor_, [this, ec] { error_callback_(ec); });
                    return;
                }

                message_queue_.pop_front();
                if (!message_queue_.empty()) {
                    do_write();
                }
            }
        );
    }

private:
    void connect(boost::asio::ip::tcp::resolver::results_type results)
    {
        auto& tcp = stream_.lowest_layer();
        boost::asio::async_connect(
            tcp,
            std::begin(results),
            std::end(results),
            [this] (boost::system::error_code ec, auto) {
                fmt::print("Socket connected\n");
                connect_timer_.cancel();

                if (ec) {
                    boost::asio::post( executor_, [this, ec] { error_callback_(ec); });
                    return;
                }

                handshake();
            }
        );

        using namespace std::chrono_literals;
        set_timeout(10s);
    }

    void handshake()
    {
        fmt::print("handshake\n");
        stream_.async_handshake(
            Stream::client,
            [this] (boost::system::error_code ec) {
                fmt::print("handshake callback\n");
                connect_timer_.cancel();
                if (ec) {
                    fmt::print("err: {}\n", ec.message());
                    boost::asio::post(executor_, [this, ec] { error_callback_(ec); });
                    return;
                }

                do_read();

                if (on_connect_) {
                    executor_.post([this] { on_connect_(); });
                }

            }
        );

        using namespace std::chrono_literals;
        set_timeout(10s);
    }

    void do_read()
    {
        boost::asio::async_read_until(
            stream_, read_buffer_, "\n",
            [this] (boost::system::error_code ec, std::size_t transferred) {
                (void)transferred;
                // fmt::print("read callback, ec={} transfer={}\n", ec.message(), transferred);

                // TODO: check ec

                std::istream is(&read_buffer_);
                std::string str;
                std::getline(is, str);

                if (str.back() == '\r')
                    str.pop_back();

                if (!str.empty() && on_read_) {
                    executor_.post([this, str = std::move(str)] { on_read_(str); });
                }

                // TODO: Add test for this case
                if (!ec || ec != boost::asio::error::eof) {
                    do_read();
                }
            }
        );
    }

    template <typename Duration>
    void set_timeout(Duration duration)
    {
        connect_timer_.expires_after(duration);
        connect_timer_.async_wait(
            [this] (const boost::system::error_code & ec) {
                if (ec && ec == boost::asio::error::operation_aborted)
                    return;
                fmt::print("timer callback: err: {}\n", ec.message());

                resolver_.cancel();
                boost::asio::post(
                    executor_,
                    [this, ec] { error_callback_(ec); }
                );
            }
        );
    }

    Executor& executor_;
    Resolver& resolver_;
    Stream& stream_;
    typename TimerEngine::timer_type connect_timer_;
    boost::asio::streambuf read_buffer_;
    read_callback on_read_;
    connect_callback on_connect_;
    error_callback error_callback_;
    std::deque<std::string> message_queue_;

};

#endif
