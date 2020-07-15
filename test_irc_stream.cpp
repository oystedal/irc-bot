#include "net_stream.hpp"

#include <gtest/gtest.h>

#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ts/io_context.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <numeric>
#include <list>

using namespace std::chrono_literals;

class ManualExecutor
{
public:
    ManualExecutor(boost::asio::io_context& context)
        : context_(context)
    {}

    void run()
    {
        context_.run();
        context_.restart();
    }

private:
    boost::asio::io_context& context_;
};

struct fake_tcp {
    using endpoint = boost::asio::ip::tcp::endpoint;
};

class FakeResolver
{
public:
    using results_type = boost::asio::ip::tcp::resolver::results_type;
    using error_code = boost::system::error_code;
    using resolve_callback = std::function<void (const error_code&, results_type)>;

    FakeResolver(boost::asio::io_context& ioctx)
        : io_context(ioctx)
    {}

    struct resolve_request{
        std::string name;
        std::string port;
        resolve_callback callback;
        bool active;
        bool canceled;
    };

    void async_resolve(
        std::string name,
        std::string port,
        resolve_callback callback)
    {
        requests.emplace_back(
            resolve_request{
                std::move(name),
                std::move(port),
                std::move(callback),
                true,
                false,
            }
        );
    }

    void cancel()
    {
        std::for_each(
            std::begin(requests), std::end(requests),
            [] (auto& request) {
                request.canceled = true;
            }
        );
    }

    void simulate_resolve()
    {
        if (requests.empty())
            throw std::logic_error("Resolver requests is empty");

        auto request = std::move(requests[0]);
        requests.erase(requests.begin());

        std::initializer_list<boost::asio::ip::tcp::endpoint> eps = {
            boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("10.0.0.2"), 6667),
        };
        auto results = results_type::create(eps.begin(), eps.end(), "irc.hostname.org", "6667");

        boost::asio::post(
            io_context,
            [request, results] { request.callback(boost::system::error_code(), results); }
        );
    }

    void simulate_error()
    {
        if (requests.empty())
            throw std::logic_error("Resolver requests is empty");

        auto request = std::move(requests[0]);
        requests.erase(requests.begin());

        boost::asio::post(
            io_context,
            [request = std::move(request)] {
                request.callback(boost::asio::error::fault, {});
            }
        );
    }

    boost::asio::io_context& io_context;
    std::vector<resolve_request> requests;
};

class SocketListener
{
public:
    using connect_callback = std::function<void(boost::system::error_code)>;

    static SocketListener* instance()
    {
        static SocketListener listener;
        return &listener;
    }

    void reset()
    {
        pending_connects.clear();
    }

    struct pending_connect
    {
        boost::asio::ip::tcp::endpoint endpoint;
        connect_callback callback;
    };
    std::vector<pending_connect> pending_connects;
};

template <typename Executor>
class boost::asio::basic_socket<fake_tcp, Executor>
{
public:
    using native_handle_type = int;
    using connect_callback = std::function<void(boost::system::error_code)>;

    basic_socket<fake_tcp, Executor>(const Executor& executor)
        : executor_(executor)
    {
    }

    bool is_open()
    {
        return true;
    }

    void close(boost::system::error_code&)
    {
    }

    const Executor& get_executor() const
    {
        return executor_;
    }

    void async_connect(boost::asio::ip::tcp::endpoint endpoint, connect_callback&& callback)
    {
        fmt::print("{}\n", __func__);

        SocketListener::instance()->pending_connects.emplace_back(
            SocketListener::pending_connect{
                endpoint,
                callback
            }
        );
    }

private:
    const Executor& executor_;
};

class FakeTcpSocket : public boost::asio::basic_stream_socket<fake_tcp, boost::asio::io_context::executor_type>
{
public:
    using connect_callback = std::function<void (boost::system::error_code)>;

    FakeTcpSocket(const boost::asio::io_context::executor_type& exec)
        : boost::asio::basic_stream_socket<fake_tcp, boost::asio::io_context::executor_type>(exec)
    {}

    void simulate_error()
    {
        if (pending_connects.empty()) {
            throw std::logic_error("simulate_error with no pending connect requests");
        }

        auto& request = pending_connects.front();
        request.callback(boost::asio::error::connection_refused);
        pending_connects.erase(pending_connects.begin());
    }

    struct connect_request {
        boost::asio::ip::tcp::endpoint endpoint;
        connect_callback callback;
    };
    std::vector<connect_request> pending_connects;
};

class FakeSslStream
{
public:
    using executor_type = boost::asio::io_context::executor_type;
    static constexpr auto client = boost::asio::ssl::stream_base::client;

    FakeSslStream(const boost::asio::io_context::executor_type& exec)
        : executor(exec)
        , socket(exec)
    {}

    auto& lowest_layer() { return socket; }

    using handshake_callback = std::function<void(boost::system::error_code)>;
    void async_handshake(boost::asio::ssl::stream_base::handshake_type type, handshake_callback&& callback)
    {
        if (type != boost::asio::ssl::stream_base::client) {
            throw std::logic_error("Expected handshake type \"client\"");
        }
        pending_handshakes.emplace_back(std::move(callback));
    }

    using read_callback = std::function<void(boost::system::error_code, std::size_t)>;
    void async_read_some(const boost::asio::mutable_buffer& buffers, read_callback&& callback)
    {
        pending_reads.emplace_back(
            pending_read{
                buffers,
                std::move(callback),
            }
        );
    }

    using write_callback = std::function<void(boost::system::error_code, std::size_t)>;
    struct write_call {
        std::string data;
        write_callback callback;
    };
    std::vector<write_call> writes;
    void async_write_some(const boost::asio::const_buffer& buffer, write_callback&& callback)
    {
        writes.emplace_back(
            write_call{
                std::string((char*)buffer.data(), buffer.size()),
                std::move(callback),
            }
        );
    }

    void push(std::string_view msg)
    {
        if (pending_reads.empty()) {
            throw std::logic_error("push() with no pending read requests");
        }

        auto& read_request = pending_reads.front();

        if (read_request.buffers.size() < msg.size()) {
            throw std::runtime_error("Destination buffer smaller than message size");
        }

        std::size_t to_write = std::min(msg.size(), read_request.buffers.size());
        std::memcpy(read_request.buffers.data(), msg.data(), to_write);
        read_request.callback(boost::system::error_code(), to_write);

        pending_reads.pop_front();
    }

    void push(boost::system::error_code ec)
    {
        if (pending_reads.empty()) {
            throw std::logic_error("push() with no pending read requests");
        }

        auto& read_request = pending_reads.front();
        read_request.callback(ec, 0);

        pending_reads.pop_front();
    }

    void simulate_handshake()
    {
        if (pending_handshakes.empty()) {
            throw std::logic_error("simulate_handshake with no pending handshake requests");
        }

        auto& handshake = pending_handshakes.front();
        boost::asio::post(
            executor,
            [handshake = std::move(handshake)] { handshake(boost::system::error_code()); }
        );
        pending_handshakes.erase(pending_handshakes.begin());
    }

    void simulate_handshake_error()
    {
        if (pending_handshakes.empty()) {
            throw std::logic_error("simulate_error with no pending connect requests");
        }

        auto& handshake = pending_handshakes.front();
        boost::asio::post(
            executor,
            [handshake = std::move(handshake)] { handshake(boost::asio::error::fault); }
        );
        pending_handshakes.erase(pending_handshakes.begin());
    }

    std::vector<handshake_callback> pending_handshakes;

    boost::asio::io_context::executor_type executor;
    FakeTcpSocket socket;

    struct pending_read {
        const boost::asio::mutable_buffer& buffers;
        read_callback callback;
    };
    std::list<pending_read> pending_reads;

private:

};

class FakeTimer;

class ManualTimerEngine
{
public:
    static constexpr bool debug = true;

    using timer_type = FakeTimer;

    using timer_callback = std::function<void()>;
    using duration = std::chrono::milliseconds;
    using timer_id = std::size_t;

    FakeTimer create_timer();

    timer_id add_timer(duration wait_time, timer_callback callback)
    {
        auto id = next_timer_id++;
        timers.insert(std::pair(current_time_ + wait_time, timer{id, callback}));;
        if constexpr (debug)
            fmt::print("{}({}, ...) = {}\n", __func__, wait_time, id);
        return id;
    }

    void remove_timer(timer_id id)
    {
        if constexpr (debug)
            fmt::print("{}({})\n", __func__, id);
        auto timer = std::find_if(
            std::begin(timers), std::end(timers),
            [id] (const auto& timer) {
                return timer.second.id == id;
            }
        );
        if (timer != std::end(timers)) {
            auto work = timer->second.callback;
            timers.erase(timer);

        }
    }

    void advance_time(duration t, ManualExecutor& executor)
    {
        if constexpr (debug)
            fmt::print(">>>>>\n");

        if constexpr (debug)
            fmt::print("{}({})\n", __func__, t);

        auto end_time = current_time_ + t;

        while (true) {
            if constexpr (debug)
                fmt::print("current time: {}\n", current_time_);
            executor.run();

            if (timers.empty()) {
                if constexpr (debug)
                    fmt::print("no more timers\n");
                break;
            }

            auto& [next_timer, timer] = *timers.begin();
            if constexpr (debug)
                fmt::print("next timer is {} at {}\n", timer.id, next_timer);
            if (next_timer > end_time) {
                if constexpr (debug)
                    fmt::print("next timer is past end time\n");
                break;
            }

            fmt::format("Running callback for timer {}\n", timer.id);
            timer.callback();
            executor.run();

            timers.erase(timers.begin());
        }

        current_time_ = end_time;
        fmt::print("current time: {}\n", current_time_);

        if constexpr (debug)
            fmt::print("<<<<<\n");

    }

private:
    timer_id next_timer_id = 0;
    struct timer {
        timer_id id;
        timer_callback callback;
    };
    std::multimap<duration, timer> timers;
    duration current_time_;
};

class FakeTimer
{
public:
    using duration = std::chrono::milliseconds;

    FakeTimer(ManualTimerEngine& engine)
        : timer_engine(engine)
    {}

    void expires_after(duration t)
    {
        wait = t;
    }

    void async_wait(std::function<void (const boost::system::error_code&)> work)
    {
        id = timer_engine.add_timer(wait, [work] {
            fmt::print("internal callback\n");
            work(boost::system::error_code());
        });
        fmt::print("{} is waiting\n", *id);
        cancel_callback = [work] {
            fmt::print("cancel callback\n");
            work(boost::asio::error::operation_aborted);
        };
    }

    void cancel()
    {
        if (id) {
            fmt::print("{} is canceled\n", *id);
            timer_engine.remove_timer(*id);
            if (cancel_callback) {
                cancel_callback();
                cancel_callback = nullptr;
            }
        }
    }

private:
    ManualTimerEngine& timer_engine;
    std::optional<std::size_t> id;
    duration wait;
    std::function<void ()> cancel_callback;
};

FakeTimer ManualTimerEngine::create_timer()
{
    return FakeTimer(*this);
}

struct Fixture : public ::testing::Test
{
    Fixture()
        : executor(io_context)
        , timer_engine()
        , resolver(io_context)
        , stream(io_context.get_executor())
        , irc(io_context, resolver, stream, timer_engine)
    {
        irc.on_error([this] (auto) {
            fmt::print("fixture on_error");
            ++error_call_count;
        });
    }

    void TearDown() override
    {
        SocketListener::instance()->reset();
    }

    void run_executor()
    {
        io_context.run();
        io_context.restart();
    }

    void advance_time(std::chrono::milliseconds t)
    {
        timer_engine.advance_time(t, executor);
    }

    boost::asio::io_context io_context;
    ManualExecutor executor;
    ManualTimerEngine timer_engine;
    FakeResolver resolver;
    FakeSslStream stream;

    net_stream<boost::asio::io_context, FakeSslStream, FakeResolver, ManualTimerEngine> irc;

    std::size_t error_call_count = 0;
};

TEST_F(Fixture, test_connect_starts_name_resolution)
{
    irc.connect("irc.hostname.org");

    ASSERT_EQ(1, resolver.requests.size());
    EXPECT_EQ("irc.hostname.org", resolver.requests[0].name);
    EXPECT_EQ("6667", resolver.requests[0].port);
}

TEST_F(Fixture, test_resolution_failure_calls_error_callback)
{
    irc.connect("irc.hostname.org");

    ASSERT_EQ(0, error_call_count);

    resolver.simulate_error();
    executor.run();

    EXPECT_EQ(1, error_call_count);

    advance_time(10s);
    executor.run();

    EXPECT_EQ(1, error_call_count);
}

TEST_F(Fixture, test_resolution_timeout_calls_error_callback)
{
    irc.connect("irc.hostname.org");

    advance_time(10s - 1ms);
    EXPECT_EQ(0, error_call_count);

    advance_time(1ms);
    EXPECT_EQ(1, error_call_count);
    EXPECT_EQ(true, resolver.requests[0].canceled);
}

struct TcpConnect : public Fixture
{
    TcpConnect()
        : Fixture()
    {
        irc.connect("irc.hostname.org");
        resolver.simulate_resolve();
        executor.run();
    }
};

TEST_F(TcpConnect, test_name_resolution_success_connects_to_host)
{
    ASSERT_EQ(1, SocketListener::instance()->pending_connects.size());
    ASSERT_EQ(0, error_call_count);
}

TEST_F(TcpConnect, test_connect_to_host_times_out_calls_error_callback)
{
    advance_time(10s - 1ms);
    ASSERT_EQ(0, error_call_count);

    advance_time(1ms);
    ASSERT_EQ(1, error_call_count);
}

TEST_F(TcpConnect, test_connect_to_host_error_calls_error_callback)
{
    boost::system::error_code ec = boost::asio::error::connection_refused;
    SocketListener::instance()->pending_connects.at(0).callback(ec);
    executor.run();
    ASSERT_EQ(1, error_call_count);
}

TEST_F(TcpConnect, test_connect_to_host_error_stops_connect_timer)
{
    boost::system::error_code ec = boost::asio::error::connection_refused;
    SocketListener::instance()->pending_connects.at(0).callback(ec);
    executor.run();
    advance_time(10s);
    ASSERT_EQ(1, error_call_count);
}

struct Handshake : public TcpConnect
{
    Handshake()
        : TcpConnect()
    {
        SocketListener::instance()->pending_connects[0].callback(boost::system::error_code());
        executor.run();
    }
};

TEST_F(Handshake, test_start_ssl_handshake_after_tcp_connect)
{
    ASSERT_EQ(1, stream.pending_handshakes.size());
}

TEST_F(Handshake, test_connect_to_host_error_calls_error_callback)
{
    stream.simulate_handshake_error();
    executor.run();

    ASSERT_EQ(1, error_call_count);
}

TEST_F(Handshake, test_timeout_if_handshake_does_not_complete_within_10_seconds)
{
    advance_time(10s - 1ms);
    ASSERT_EQ(0, error_call_count);

    advance_time(1ms);
    ASSERT_EQ(1, error_call_count);
}

TEST_F(Handshake, test_no_timeout_after_handshake_has_completed)
{
    stream.simulate_handshake();
    executor.run();

    ASSERT_EQ(0, error_call_count);

    advance_time(10s);

    ASSERT_EQ(0, error_call_count);
}

struct Connected : public Handshake
{
    Connected()
        : Handshake()
    {
        stream.simulate_handshake();
        executor.run();
    }
};


TEST_F(Connected, test_is_reading_stream_after_handshake)
{
    ASSERT_EQ(1, stream.pending_reads.size());
}

TEST_F(Connected, test_read_until_crlf)
{
    std::vector<std::string> lines;
    irc.on_read([&] (std::string_view str) {
        lines.emplace_back(std::string(str));
    });

    stream.push("asdf\r\nfoo");
    executor.run();

    ASSERT_EQ(1, lines.size());
    ASSERT_EQ("asdf", lines[0]);
}

TEST_F(Connected, test_read_remaining_characters_on_next_crlf)
{
    std::vector<std::string> lines;
    irc.on_read([&] (std::string_view str) {
        lines.emplace_back(std::string(str));
    });

    stream.push("asdf\r\nfoo");
    executor.run();
    stream.push("bar\r\n");
    executor.run();

    ASSERT_EQ(2, lines.size());
    EXPECT_EQ("asdf", lines[0]);
    EXPECT_EQ("foobar", lines[1]);
}

TEST_F(Connected, test_skip_empty_lines)
{
    std::vector<std::string> lines;
    irc.on_read([&] (std::string_view str) {
        lines.emplace_back(std::string(str));
    });

    stream.push("asdf\r\n");
    stream.push("\r\n");
    stream.push("foo\r\n");
    executor.run();

    ASSERT_EQ(2, lines.size());
    EXPECT_EQ("asdf", lines[0]);
    EXPECT_EQ("foo", lines[1]);
}

TEST_F(Connected, test_read_until_eof)
{
    std::vector<std::string> lines;
    irc.on_read([&] (std::string_view str) {
        lines.emplace_back(std::string(str));
    });

    irc.on_error([this] (auto) {
        ++error_call_count;
    });

    stream.push("asdf\r\n");
    executor.run();

    ASSERT_EQ(1, lines.size());
    EXPECT_EQ("asdf", lines[0]);
    EXPECT_EQ(0, error_call_count);
    ASSERT_EQ(1, stream.pending_reads.size());

    stream.push(boost::asio::error::eof);
    executor.run();

    EXPECT_EQ(1, error_call_count);
    ASSERT_EQ(0, stream.pending_reads.size());
}

TEST_F(Connected, test_single_write_call_writes_to_stream)
{
    irc.write("line 1\r\n");
    executor.run();

    ASSERT_EQ(1, stream.writes.size());
    EXPECT_EQ("line 1\r\n", stream.writes[0].data);
}

TEST_F(Connected, test_non_consecutive_writes)
{
    irc.write("line 1\r\n");
    boost::asio::post(
        stream.executor,
        [this] {
            stream.writes[0].callback(boost::system::error_code(), 0);
        }
    );
    executor.run();
    irc.write("line 2\r\n");

    ASSERT_EQ(2, stream.writes.size());
    EXPECT_EQ("line 1\r\n", stream.writes[0].data);
    EXPECT_EQ("line 2\r\n", stream.writes[1].data);
}

TEST_F(Connected, test_consecutive_write_to_stream_is_buffered)
{
    irc.write("line 1\r\n");
    irc.write("line 2\r\n");
    executor.run();

    ASSERT_EQ(1, stream.writes.size());
    EXPECT_EQ("line 1\r\n", stream.writes[0].data);
}

TEST_F(Connected, test_write_buffered_string_after_callback)
{
    irc.write("line 1\r\n");
    irc.write("line 2\r\n");
    executor.run();
    ASSERT_EQ(1, stream.writes.size());

    boost::asio::post(
        stream.executor,
        [this] {
            stream.writes[0].callback(boost::system::error_code(), 0);
        }
    );
    executor.run();

    ASSERT_EQ(2, stream.writes.size());
    EXPECT_EQ("line 1\r\n", stream.writes[0].data);
    EXPECT_EQ("line 2\r\n", stream.writes[1].data);
}

TEST_F(Connected, test_no_more_writes_after_both_strings_have_finished_writing)
{
    irc.write("line 1\r\n");
    irc.write("line 2\r\n");
    executor.run();
    ASSERT_EQ(1, stream.writes.size());

    boost::asio::post(
        stream.executor,
        [this] {
            stream.writes[0].callback(boost::system::error_code(), 0);
        }
    );
    executor.run();

    ASSERT_EQ(2, stream.writes.size());
    EXPECT_EQ("line 1\r\n", stream.writes[0].data);
    EXPECT_EQ("line 2\r\n", stream.writes[1].data);

    boost::asio::post(
        stream.executor,
        [this] {
            stream.writes[1].callback(boost::system::error_code(), 0);
        }
    );
    executor.run();

    ASSERT_EQ(2, stream.writes.size());
}
