#pragma once
#include "../sdk.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <iostream>
#include "../logger/logger.h"

namespace http_server {

namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;
using namespace std::literals;
namespace sys = boost::system;

class SessionBase {
protected:
    using HttpRequest = http::request<http::string_body>;
    ~SessionBase() = default;
    explicit SessionBase(tcp::socket&& socket)
        : stream_(std::move(socket)) {
    }

    template <typename Body, typename Fields>
    void Write(http::response<Body, Fields>&& response) {
        // Запись выполняется асинхронно, поэтому response перемещаем в область кучи
        auto safe_response = std::make_shared<http::response<Body, Fields>>(std::move(response));

        auto self = GetSharedThis();
        http::async_write(stream_, *safe_response,
          [safe_response, self](beast::error_code ec, std::size_t bytes_written) {
              self->OnWrite(safe_response->need_eof(), ec, bytes_written);
          });
    }
public:
    // Запрещаем копирование и присваивание объектов SessionBase и его наследников
    SessionBase(const SessionBase&) = delete;
    SessionBase& operator=(const SessionBase&) = delete;

    void Run();

private:
    // tcp_stream содержит внутри себя сокет и добавляет поддержку таймаутов
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    HttpRequest request_;

    virtual std::shared_ptr<SessionBase> GetSharedThis() = 0;

    void ReportError(beast::error_code ec, std::string_view what);
    void OnWrite(bool close, beast::error_code ec, [[maybe_unused]] std::size_t bytes_written);
    void Read();
    void OnRead(beast::error_code ec, [[maybe_unused]] std::size_t bytes_read);
    void Close();

    // Обработку запроса делегируем подклассу
    virtual void HandleRequest(HttpRequest&& request) = 0;

};

template <typename RequestHandler>
class Session : public SessionBase, public std::enable_shared_from_this<Session<RequestHandler>> {

public:
    template <typename Handler>
    Session(tcp::socket&& socket, Handler&& request_handler, const std::string& client_ip)
        : SessionBase(std::move(socket))
        , request_handler_(std::forward<Handler>(request_handler))
        , client_ip_(client_ip) {
    }

private:
    std::shared_ptr<SessionBase> GetSharedThis() override {
        return this->shared_from_this();
    }
    RequestHandler request_handler_;
    std::string client_ip_;

    void HandleRequest(HttpRequest&& request) override {
        // Захватываем умный указатель на текущий объект Session в лямбде,
        // чтобы продлить время жизни сессии до вызова лямбды.
        // Используется generic-лямбда функция, способная принять response произвольного типа
        request_handler_(std::move(request), client_ip_, [self = this->shared_from_this()](auto&& response) {
            self->Write(std::move(response));
        });
    }
};

template <typename RequestHandler>
class Listener : public std::enable_shared_from_this<Listener<RequestHandler>> {
public:
    template <typename Handler>
    Listener(net::io_context& ioc, const tcp::endpoint& endpoint, Handler&& request_handler)
        : ioc_(ioc)
        // Обработчики асинхронных операций acceptor_ будут вызываться в своём strand
        , acceptor_(net::make_strand(ioc))
        , request_handler_(std::forward<Handler>(request_handler)) {
        // Открываем acceptor, используя протокол (IPv4 или IPv6), указанный в endpoint
        acceptor_.open(endpoint.protocol());

        // После закрытия TCP-соединения сокет некоторое время может считаться занятым,
        // чтобы компьютеры могли обменяться завершающими пакетами данных.
        // Однако это может помешать повторно открыть сокет в полузакрытом состоянии.
        // Флаг reuse_address разрешает открыть сокет, когда он "наполовину закрыт"
        acceptor_.set_option(net::socket_base::reuse_address(true));
        // Привязываем acceptor к адресу и порту endpoint
        acceptor_.bind(endpoint);
        // Переводим acceptor в состояние, в котором он способен принимать новые соединения
        // Благодаря этому новые подключения будут помещаться в очередь ожидающих соединений
        acceptor_.listen(net::socket_base::max_listen_connections);
    }

    void Run() {
        DoAccept();
    }
private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    RequestHandler request_handler_;

    void ReportError(beast::error_code ec, std::string_view what) {
        json::value custom_data = json::object{
                {"code"s, ec.value()},
                {"text"s, ec.message()},
                {"where"s, what}
        };
        BOOST_LOG_TRIVIAL(error) << logging::add_value(additional_data, custom_data) << "error"sv;
    }

    void DoAccept() {
        acceptor_.async_accept(
            // Передаём последовательный исполнитель, в котором будут вызываться обработчики
            // асинхронных операций сокета
            net::make_strand(ioc_),
            // С помощью bind_front_handler создаём обработчик, привязанный к методу OnAccept
            // текущего объекта.
            // Так как Listener — шаблонный класс, нужно подсказать компилятору, что
            // shared_from_this — метод класса, а не свободная функция.
            // Для этого вызываем его, используя this
            // Этот вызов bind_front_handler аналогичен
            // namespace ph = std::placeholders;
            // std::bind(&Listener::OnAccept, this->shared_from_this(), ph::_1, ph::_2)
            beast::bind_front_handler(&Listener::OnAccept, this->shared_from_this()));
    }

    // Метод socket::async_accept создаст сокет и передаст его передан в OnAccept
    void OnAccept(beast::error_code ec, tcp::socket socket) {     //sys::error_code ec
        using namespace std::literals;

        if (ec) {
            return ReportError(ec, "accept"sv);
        }

        std::string client_ip = socket.remote_endpoint().address().to_string();

        // Асинхронно обрабатываем сессию
        AsyncRunSession(std::move(socket), client_ip);

        // Принимаем новое соединение
        DoAccept();
    }

    void AsyncRunSession(tcp::socket&& socket, const std::string& client_ip) {
        std::make_shared<Session<RequestHandler>>(std::move(socket), request_handler_, client_ip)->Run();
    }
};

template <typename RequestHandler>
void ServeHttp(net::io_context& ioc, const tcp::endpoint& endpoint, RequestHandler&& handler) {
    // При помощи decay_t исключим ссылки из типа RequestHandler,
    // чтобы Listener хранил RequestHandler по значению
    using MyListener = Listener<std::decay_t<RequestHandler>>;
    std::make_shared<MyListener>(ioc, endpoint, std::forward<RequestHandler>(handler))->Run();
}

}  // namespace http_server
