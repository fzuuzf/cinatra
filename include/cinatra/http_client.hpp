#pragma once
#include <string>
#include <vector>
#include <tuple>
#include <future>
#include <atomic>
#include <fstream>
#include <mutex>
#include <deque>
#include <string_view>
#include "use_asio.hpp"
#include "uri.hpp"
#include "http_parser.hpp"
#include "itoa_jeaiii.hpp"

#ifdef CINATRA_ENABLE_SSL
#ifdef ASIO_STANDALONE
#include <asio/ssl.hpp>
#else
#include <boost/asio/ssl.hpp>
#endif
#endif

namespace cinatra {
    enum error_code {
        success = 0,
        invalid_uri = -1,
        content_out_of_range = -2,
        filepath_error = -3,
        request_timeout = -4,
        future_exception = -5,
        invalid_method = -6,
        create_file_failed = -7,
        filesize_error = -8,
        connect_timeout = -9,
        invalid_argument = -10,
    };

    template<typename... Args>
    inline void print(Args... args) {
        ((std::cout << args << ' '), ...);
        std::cout << "\n";
    }


    inline void print(std::tuple<error_code, int, std::string> tp) {
        auto& [code, status, result] = tp;
        std::cout <<"code: "<< code << ", status: " << status << ", result:\n" << result << "\n";
    }

    inline void print(const boost::system::error_code& ec) {
        print(ec.value(), ec.message());
    }

	struct callback_data {
        int status;
        std::string_view resp_body;
        std::pair<phr_header*, size_t> resp_headers;
    };

    using callback_t = std::function<void(boost::system::error_code, callback_data)>;
    class http_client : public std::enable_shared_from_this<http_client> {
    public:
        http_client(boost::asio::io_service& ios, size_t read_timout = 60) : 
            ios_(ios), resolver_(ios), socket_(ios), timer_(ios), timeout_seconds_(read_timout) {
        }
        
        ~http_client() {
            close();
        }

        bool connect(std::string uri, size_t seconds = 3) {
            auto promise = std::make_shared<std::promise<bool>>();
            std::weak_ptr<std::promise<bool>> weak(promise);
            auto code = async_connect(std::move(uri), weak);
            if (code != error_code::success) {
                promise->set_value(false);
            }

            try {
                auto future = promise->get_future();
                auto status = future.wait_for(std::chrono::seconds(seconds));
                if (status == std::future_status::timeout) {
                    promise->set_value(false);
                }
                return future.get();
            }
            catch (const std::exception&) {
                return false;
            }
        }

        void set_callback(callback_t cb) {
            if (promise_) {
                promise_ = nullptr;
            }
            cb_ = std::move(cb);
        }

        std::tuple<error_code, int, std::string> get(std::string uri, size_t seconds = 5, res_content_type type = res_content_type::json) {
            content_type_ = type;
            return request(http_method::GET, std::move(uri), "", seconds);
        }

        std::tuple<error_code, int, std::string> post(std::string uri, std::string body, size_t seconds = 5, res_content_type type = res_content_type::json) {
            content_type_ = type;
            return request(http_method::POST, std::move(uri), std::move(body), seconds);
        }

        error_code async_get(std::string uri, callback_t cb) {
            return async_request(http_method::GET, std::move(uri), std::move(cb));
        }

        error_code async_post(std::string uri, std::string body, callback_t cb) {
            return async_request(http_method::POST, std::move(uri), std::move(cb), std::move(body));
        }

        error_code async_request(http_method method, std::string uri, callback_t cb, std::string body="") {
            if (method != http_method::POST&&!body.empty()) {
                return error_code::invalid_method;
            }
            auto [code, u] = get_uri(uri);
            if (code != error_code::success) {
                return code;
            }
            cb_ = std::move(cb);
            context ctx(u, method, std::move(body));
            if (!has_connected_) {
                async_connect(std::move(ctx), {});
            }
            else {
                send_msg(ctx);
            }

            return error_code::success;
        }

        std::tuple<error_code, int, std::string> request(http_method method, std::string uri, std::string body = "", size_t seconds = 5) {
            if (timeout_seconds_>0&&seconds > timeout_seconds_) {
                return { error_code::invalid_argument, 404, "" };
            }

            //not thread safe
            promise_ = std::make_shared<std::promise<std::tuple<error_code, int, std::string>>>();
            if (cb_) {
                cb_ = nullptr;
            }

            if (!copy_headers_.empty()) {
                copy_headers_.clear();
            }

            if (!uri_.empty()&& uri_ != uri) {
                close();
                promise_->get_future().wait();
                promise_ = std::make_shared<std::promise<std::tuple<error_code, int, std::string>>>();
                reset_socket();
            }

            uri_ = uri;

            auto code = async_request(method, std::move(uri), nullptr, std::move(body));
            if (code != error_code::success) {
                return { code, 404, "" };
            }

            try {
                auto future = promise_->get_future();
                auto status = future.wait_for(std::chrono::seconds(seconds));
                if (status == std::future_status::timeout) {
                    promise_->set_value({ error_code::request_timeout, 404, "" });
                }

                auto tp = future.get();
                promise_ = nullptr;
                return tp;
            }
            catch (const std::exception&) {
                return { error_code::future_exception, 404, "" };
            }
        }

        error_code download(std::string src_file, std::string dest_file, callback_t cb) {
            //not thread safe
            auto parant_path = fs::path(dest_file).parent_path();
            std::error_code code;
            fs::create_directories(parant_path, code);
            if (code) {
                return error_code::filepath_error;
            }

            download_file_ = std::make_shared<std::ofstream>(dest_file, std::ios::binary);
            if (!download_file_->is_open()) {
                return error_code::create_file_failed;
            }

            return async_get(std::move(src_file), std::move(cb));
        }

        error_code download(std::string src_file, std::function<void(boost::system::error_code, std::string_view)> chunk) {
            //not thread safe
            on_chunk_ = std::move(chunk);
            return async_get(std::move(src_file), nullptr);
        }

        error_code upload(std::string uri, std::string filename, std::function<void(boost::system::error_code, std::string_view)> cb) {
            return upload(std::move(uri), std::move(filename), 0, std::move(cb));
        }

        error_code upload(std::string uri, std::string filename, size_t start, std::function<void(boost::system::error_code, std::string_view)> cb) {
            on_chunk_ = std::move(cb);
            if (!has_connected_) {
                bool r = connect(uri);
                if (!r) {
                    return error_code::connect_timeout;
                }
            }

            auto file = std::make_shared<std::ifstream>(filename, std::ios::binary);
            if (!file) {
                return error_code::filepath_error;
            }

            std::error_code ec;
            size_t size = fs::file_size(filename, ec);
            if (ec) {
                file->close();
                return error_code::filesize_error;
            }

            if (start > 0) {
                file->seekg(start);
            }

            auto left_file_size = size - start;

            uri_t u;
            u.parse_from(uri.data());
            context ctx(u, http_method::POST);
            header_str_.append("Content-Type: multipart/form-data; boundary=").append(BOUNDARY);
            auto multipart_str = multipart_file_start(fs::path(filename).filename().string());
            auto write_str = build_write_msg(ctx, total_multipart_size(left_file_size, multipart_str.size()));
            write_str.append(multipart_str);
            multipart_str_ = std::move(write_str);

            send_file_data(std::move(file));
            return error_code::success;
        }

        void add_header(std::string key, std::string val) {
            if (key.empty())
                return;

            if (key == "Host")
                return;

            headers_.emplace_back(std::move(key), std::move(val));
        }

        void add_header_str(std::string pair_str) {
            if (pair_str.empty())
                return;

            if (pair_str.find("Host:") != std::string::npos)
                return;

            header_str_.append(pair_str);
        }

        void clear_headers() {
            if (!headers_.empty()) {
                headers_.clear();
            }

            if (!header_str_.empty()) {
                header_str_.clear();
            }
        }

        std::pair<phr_header*, size_t> get_resp_headers() {
            if (!copy_headers_.empty())
                parser_.set_headers(copy_headers_);

            return parser_.get_headers();
        }

        std::string_view get_header_value(std::string_view key) {
            return parser_.get_header_value(key);
        }

        void send_msg(std::string write_msg) {
            std::unique_lock lock(write_mtx_);
            outbox_.emplace_back(std::move(write_msg));
            if (outbox_.size() > 1) {
                return;
            }

            write();
        }
    private:
        void callback(const boost::system::error_code& ec) {
            callback(ec, 404, "");
        }

        void callback(const boost::system::error_code& ec, int status) {
            callback(ec, status, "");
        }

        void callback(const boost::system::error_code& ec, int status, std::string_view result) {
            if (cb_) {
                cb_(ec, { status, result, get_resp_headers() });
            }

            if (on_chunk_) {
                on_chunk_(ec, result);
            }

            //for block request
            if (promise_) {
                if (ec || status >= 400) {
                    promise_->set_value({ (error_code)ec.value(), status, ec.message() });
                }
                else {
                    promise_->set_value({ error_code::success, status, std::string(result) });
                }
            }
        }

        std::pair<error_code, uri_t> get_uri(const std::string& uri) {
            uri_t u;
            if (!u.parse_from(uri.data())) {
                if (u.schema.empty())
                    return { error_code::invalid_uri, {} };

                auto new_uri = url_encode(uri);

                if (!u.parse_from(new_uri.data())) {
                    return { error_code::invalid_uri, {} };
                }
            }

            if (u.schema == "https"sv) {
#ifdef CINATRA_ENABLE_SSL
                upgrade_to_ssl(nullptr);
#else
                //please open CINATRA_ENABLE_SSL before request https!
                assert(false);
#endif
            }
            else {
#ifdef CINATRA_ENABLE_SSL
                if (ssl_stream_) {
                    boost::system::error_code ec;
                    ssl_stream_->shutdown(ec);
                    ssl_stream_ = nullptr;
                }
#endif
            }

            return { error_code::success, u };
        }

        /**************** connect *********************/
        error_code async_connect(std::string uri, std::weak_ptr<std::promise<bool>> weak) {
            const auto& [code, u] = get_uri(uri);
            if (code != error_code::success) {
                return code;
            }

            context ctx(u, http_method::UNKNOW);
            async_connect(std::move(ctx), weak);
            return error_code::success;
        }

        void async_connect(context ctx, std::weak_ptr<std::promise<bool>> weak) {
            boost::asio::ip::tcp::resolver::query query(ctx.host, ctx.port);
            resolver_.async_resolve(query, [this, self = this->shared_from_this(), weak, ctx = std::move(ctx)]
            (boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator& it) {
                if (ec) {
                    if (auto sp = weak.lock(); sp)
                        sp->set_value(false);
                    callback(ec);
                    return;
                }

                boost::asio::async_connect(socket_, it, [this, self = shared_from_this(), weak, ctx = std::move(ctx)]
                (boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator&) {
                    if (!ec) {
                        has_connected_ = true;
                        if (is_ssl()) {
                            handshake(std::move(ctx), weak);
                            return;
                        }

                        do_read(ctx);
                    }
                    else {
                        callback(ec);
                        close();
                    }

                    if (auto sp = weak.lock(); sp)
                        sp->set_value(has_connected_);
                });
            });
        }

        void do_read(const context& ctx) {            
            boost::system::error_code error_ignored;
            socket_.set_option(boost::asio::ip::tcp::no_delay(true), error_ignored);
            do_read();
            if (ctx.method != http_method::UNKNOW) {
                send_msg(ctx);
            }
        }

        void handshake(context ctx, std::weak_ptr<std::promise<bool>> weak) {
#ifdef CINATRA_ENABLE_SSL
            auto self = this->shared_from_this();
            ssl_stream_->async_handshake(boost::asio::ssl::stream_base::client,
                [this, self, weak, ctx = std::move(ctx)](const boost::system::error_code& ec) {
                if (!ec) {
                    do_read(ctx);
                }
                else {
                    callback(ec);
                    close();
                }

                if (auto sp = weak.lock(); sp)
                    sp->set_value(has_connected_);
            });
#endif
        }
        /**************** write *********************/
        std::string build_write_msg(const context& ctx, size_t content_len = 0) {
            std::string write_msg(method_name(ctx.method));
            //can be optimized here
            write_msg.append(" ").append(ctx.path);
            if (!ctx.query.empty()) {
                write_msg.append("?").append(ctx.query);
            }
            write_msg.append(" HTTP/1.1\r\nHost:").
                append(ctx.host).append("\r\n");

            build_content_type(content_type_);

            bool has_connection = false;
            //add user header
            if (!headers_.empty()) {
                for (auto& pair : headers_) {
                    if (pair.first == "Connection") {
                        has_connection = true;
                    }                        
                    write_msg.append(pair.first).append(": ").append(pair.second).append("\r\n");
                }
            }

            if (!header_str_.empty()) {
                if (header_str_.find("Connection")) {
                    has_connection = true;
                }
                write_msg.append(header_str_).append("\r\n");
            }

            //add content
            if (!ctx.body.empty()) {
                char buffer[20];
                auto p = i32toa_jeaiii((int)ctx.body.size(), buffer);

                write_msg.append("Content-Length: ").append(buffer, p - buffer).append("\r\n");
            }
            else {
                if (ctx.method == http_method::POST) {
                    if (content_len > 0) {
                        char buffer[20];
                        auto p = i32toa_jeaiii((int)content_len, buffer);

                        write_msg.append("Content-Length: ").append(buffer, p - buffer).append("\r\n");
                    }
                    else {
                        write_msg.append("Content-Length: 0\r\n");
                    }                    
                }
            }

            if (!has_connection) {
                write_msg.append("Connection: keep-alive\r\n");
            }
            
            write_msg.append("\r\n");
            
            if (!ctx.body.empty()) {
                write_msg.append(std::move(ctx.body));
            }

            return write_msg;
        }

        void build_content_type(res_content_type type) {
            if (type != res_content_type::none) {
                auto iter = res_mime_map.find(type);
                if (iter != res_mime_map.end()) {
                    if (type == res_content_type::multipart) {
                        return;
                    }
                    
                    add_header("Content-Type", std::string(iter->second));
                }
            }
        }

        void send_msg(const context& ctx) {
            std::string write_msg = build_write_msg(ctx);

            std::unique_lock lock(write_mtx_);
            outbox_.emplace_back(std::move(write_msg));
            if (outbox_.size() > 1) {
                return;
            }

            write();
        }

        void write() {
            auto& msg = outbox_[0];
            async_write(msg, [this, self = shared_from_this()](const boost::system::error_code& ec, const size_t length) {
                if (ec) {
                    //print(ec);
                    close();
                    return;
                }

                std::unique_lock lock(write_mtx_);
                if (outbox_.empty()) {
                    return;
                }

                outbox_.pop_front();

                if (!outbox_.empty()) {
                    // more messages to send
                    write();
                }
            });
        }

        /**************** read *********************/
        
        void do_read() {
            reset_timer();
            async_read_until(TWO_CRCF, [this, self = shared_from_this()](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    //parse header
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    size_t buf_size = read_buf_.size();
                    int ret = parser_.parse_response(data_ptr, size, 0);
                    read_buf_.consume(size);
                    if (ret < 0) {
                        callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404, 
                            "http response parse error");
                        if (buf_size > size) {
                            read_buf_.consume(buf_size - size);
                        }

                        read_or_close(parser_.keep_alive());
                        return;
                    }

                    bool is_chunked = parser_.is_chunked();
                    
                    if (is_chunked) {
                        copy_headers();
                        //read_chunk_header
                        read_chunk_head(parser_.keep_alive());
                    }
                    else {
                        if (parser_.body_len() == 0) {
                            callback(ec, parser_.status());

                            read_or_close(parser_.keep_alive());
                            return;
                        }
                        
                        size_t content_len = (size_t)parser_.body_len();
                        if ((size_t)parser_.total_len() <= buf_size) {
                            callback(ec, parser_.status(), { data_ptr + parser_.header_len(), content_len });
                            read_buf_.consume(content_len);

                            read_or_close(parser_.keep_alive());
                            return;
                        }

                        size_t size_to_read = content_len - read_buf_.size();
                        if ((unsigned)parser_.total_len() > read_buf_.max_size()) {
                            copy_headers();
                        }
                        do_read_body(parser_.keep_alive(), parser_.status(), size_to_read);
                    }
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void do_read_body(bool keep_alive, int status, size_t size_to_read) {
            reset_timer();
            async_read(size_to_read, [this, self = shared_from_this(), keep_alive, status](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    size_t data_size = read_buf_.size();
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());

                    callback(ec, status, { data_ptr, data_size });

                    read_buf_.consume(data_size);

                    read_or_close(keep_alive);
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void read_or_close(bool keep_alive) {
            if (keep_alive) {                
                do_read();
            }
            else {
                close();
            }
        }

        void read_chunk_head(bool keep_alive) {
            reset_timer();
            async_read_until(CRCF, [this, self = shared_from_this(), keep_alive](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    //size_t buf_size = read_buf_.size();
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    std::string_view size_str(data_ptr, size - CRCF.size());
                    auto chunk_size = hex_to_int(size_str);
                    if (chunk_size < 0) {                        
                        callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404,
                            "invalid chunk size");
                        read_or_close(keep_alive);
                        return;
                    }

                    read_buf_.consume(size);

                    if (chunk_size == 0) {
                        if (read_buf_.size() < CRCF.size()) {
                            read_buf_.consume(read_buf_.size());
                            read_chunk_body(keep_alive, CRCF.size() - read_buf_.size());
                        }
                        else {
                            read_buf_.consume(CRCF.size());
                            read_chunk_body(keep_alive, 0);
                        }
                        return;
                    }

                    if ((size_t)chunk_size <= read_buf_.size()) {
                        const char* data = boost::asio::buffer_cast<const char*>(read_buf_.data());
                        append_chunk(std::string_view( data, chunk_size ));
                        read_buf_.consume(chunk_size + CRCF.size());
                        read_chunk_head(keep_alive);
                        return;
                    }

                    size_t extra_size = read_buf_.size();
                    size_t size_to_read = chunk_size - extra_size;
                    const char* data = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    append_chunk({ data, extra_size });
                    read_buf_.consume(extra_size);

                    read_chunk_body(keep_alive, size_to_read + CRCF.size());
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void read_chunk_body(bool keep_alive, size_t size_to_read) {
            reset_timer();
            async_read(size_to_read, [this, self = shared_from_this(), keep_alive](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    if (size <= CRCF.size()) {
                        //finish all chunked
                        read_buf_.consume(size);
                        callback(ec, 200, chunked_result_);
                        clear_chunk_buffer();
                        do_read();
                        return;
                    }

                    //size_t buf_size = read_buf_.size();
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    append_chunk({ data_ptr, size - CRCF.size() });
                    read_buf_.consume(size);
                    read_chunk_head(keep_alive);
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void append_chunk(std::string_view chunk) {
            if (on_chunk_) {
                on_chunk_({}, chunk);
                return;
            }

            if (download_file_) {
                download_file_->write(chunk.data(), chunk.size());
            }
            else {
                chunked_result_.append(chunk);
            }            
        }

        void clear_chunk_buffer() {
            if (download_file_) {
                download_file_->close();
            }
            else {
                if (!chunked_result_.empty()) {
                    chunked_result_.clear();
                }
            }
        }

        template<typename Handler>
        void async_read(size_t size_to_read, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_read(*ssl_stream_, read_buf_, boost::asio::transfer_exactly(size_to_read), std::move(handler));
#endif
            }
            else {
                boost::asio::async_read(socket_, read_buf_, boost::asio::transfer_exactly(size_to_read), std::move(handler));
            }            
        }

        template<typename Handler>
        void async_read_until(const std::string& delim, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_read_until(*ssl_stream_, read_buf_, delim, std::move(handler));
#endif
            }
            else {
                boost::asio::async_read_until(socket_, read_buf_, delim, std::move(handler));
            }
        }

        template<typename Handler>
        void async_write(const std::string& msg, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_write(*ssl_stream_, boost::asio::buffer(msg), std::move(handler));
#endif
            }
            else {
                boost::asio::async_write(socket_, boost::asio::buffer(msg), std::move(handler));
            }
        }

        void close(bool close_ssl = true) {
            boost::system::error_code ec;
            if (close_ssl) {
#ifdef CINATRA_ENABLE_SSL
                if (ssl_stream_) {
                    ssl_stream_->shutdown(ec);
                    ssl_stream_ = nullptr;
                }
#endif
            }

            if (!has_connected_)
                return;

            has_connected_ = false;            
            timer_.cancel(ec);
            socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }

        void reset_timer() {
            if (!cb_) {
                return; //just for async request
            }

            if (timeout_seconds_ == 0) {
                return;
            }

            auto self(this->shared_from_this());
            timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
            timer_.async_wait([this, self](const boost::system::error_code& ec) {
                if (ec) {
                    return;
                }

                callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::timed_out), 404, "read timeout");
                close(false); //don't close ssl now, close ssl when read/write error
                if (download_file_) {
                    download_file_->close();
                }
            });
        }

        void cancel_timer() {
            if (!cb_) {
                return; //just for async request
            }

            if (timeout_seconds_ == 0) {
                return;
            }

            timer_.cancel();
        }

        bool is_ssl() const {
#ifdef CINATRA_ENABLE_SSL
            return ssl_stream_ != nullptr;
#else
            return false;
#endif
        }

#ifdef CINATRA_ENABLE_SSL
        void upgrade_to_ssl(std::function<void(boost::asio::ssl::context&)> ssl_context_callback) {
            if (ssl_stream_)
                return;

            boost::asio::ssl::context ssl_context(boost::asio::ssl::context::sslv23);
            ssl_context.set_default_verify_paths();
            boost::system::error_code ec;
            ssl_context.set_options(boost::asio::ssl::context::default_workarounds, ec);
            if (ssl_context_callback) {
                ssl_context_callback(ssl_context);
            }
            ssl_stream_ = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(socket_, ssl_context);
            //verify peer TODO
        }
#endif

        void send_file_data(std::shared_ptr<std::ifstream> file) {
            auto eof = make_upload_data(*file);
            if (eof) {
                return;
            }

            auto self = this->shared_from_this();
            async_write(multipart_str_, [this, self, file = std::move(file)](boost::system::error_code ec, std::size_t length) mutable {
                if (!ec) {
                    multipart_str_.clear();
                    send_file_data(std::move(file));
                }
                else {
                    on_chunk_(ec, "send failed");
                    close();
                }
            });
        }

        std::string multipart_file_start(std::string filename) {
            std::string multipart_start;
            multipart_start.append("--" + BOUNDARY + CRCF);
            multipart_start.append("Content-Disposition: form-data; name=\"" + std::string("test") + "\"; filename=\"" + std::move(filename) + "\"" + CRCF);
            multipart_start.append(CRCF);
            return multipart_start;
        }

        size_t total_multipart_size(size_t left_file_size, size_t multipart_start_size) {
            return left_file_size + multipart_start_size + MULTIPART_END.size();
        }

        bool make_upload_data(std::ifstream& file) {
            bool eof = file.peek() == EOF;
            if (eof) {
                file.close();
                return true;//finish all file
            }

            std::string content;
            constexpr int64_t size = 3 * 1024 * 1024;
            content.resize(size);
            file.read(&content[0], size);
            int64_t read_len = (int64_t)file.gcount();
            assert(read_len > 0);
            eof = file.peek() == EOF;

            if (read_len < size) {
                content.resize(read_len);
            }

            multipart_str_.append(content);
            if (eof) {
                multipart_str_.append(MULTIPART_END);
            }

            return false;
        }

        void copy_headers() {
            if (!copy_headers_.empty()) {
                copy_headers_.clear();
            }
            auto[headers, num_headers] = parser_.get_headers();
            for (size_t i = 0; i < num_headers; i++) {
                copy_headers_.emplace_back(std::string(headers[i].name, headers[i].name_len), 
                    std::string(headers[i].value, headers[i].value_len));
            }
        }

        void reset_socket() {
            boost::system::error_code igored_ec;
            socket_.close(igored_ec);
            socket_ = decltype(socket_)(ios_);
            if (!socket_.is_open()) {
                socket_.open(boost::asio::ip::tcp::v4(), igored_ec);
            }
        }

    private:
        std::atomic_bool has_connected_ = false;
        callback_t cb_;

        boost::asio::io_service& ios_;
        boost::asio::ip::tcp::resolver resolver_;
        boost::asio::ip::tcp::socket socket_;
#ifdef CINATRA_ENABLE_SSL
        std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> ssl_stream_;
#endif
        boost::asio::steady_timer timer_;
        std::size_t timeout_seconds_ = 15;
        boost::asio::streambuf read_buf_;

        http_parser parser_;
        std::vector<std::pair<std::string, std::string>> copy_headers_;

        std::string uri_;

        std::string header_str_;
        std::vector<std::pair<std::string, std::string>> headers_;
        res_content_type content_type_ = res_content_type::json;

        std::mutex write_mtx_;
        std::deque<std::string> outbox_;

        std::shared_ptr<std::promise<std::tuple<error_code, int, std::string>>> promise_ = nullptr;

        std::string chunked_result_;
        std::shared_ptr<std::ofstream> download_file_ = nullptr;
        std::function<void(boost::system::error_code, std::string_view)> on_chunk_ = nullptr;

        std::string multipart_str_;
    };
}
