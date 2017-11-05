#include <waypositor/logger.hpp>

#include <cstdlib>

#include <optional>
#include <system_error>
#include <experimental/filesystem>

#include <boost/asio/io_service.hpp>
#include <boost/asio/local/stream_protocol.hpp>

namespace waypositor {
  namespace filesystem = std::experimental::filesystem;
  namespace asio = boost::asio;
  using Domain = asio::local::stream_protocol;

  class Listener final {
  private:
    Logger &mLog;
    Domain::acceptor mAcceptor;

    class Worker final {
    private:
      Listener *self;
    public:
      Worker(Listener &self_) : self{&self_} {}
      Worker(Worker const &);
      Worker &operator=(Worker const &);
      Worker(Worker &&other) noexcept : self{other.self} {
        other.self = nullptr;
      }
      Worker &operator=(Worker &&other) {
        if (this == &other) return *this;
        self = other.self;
        other.self = nullptr;
        return *this;
      }
      ~Worker() = default;

      void operator()(boost::system::error_code const &error = {}) {
        assert(*this);
        if (error) {
          self->mLog.error("ASIO error: ", error.message());
          return;
        }
      }

      explicit operator bool() const { return self != nullptr; }
    };

    struct Private {};
  public:
    void launch() { Worker{*this}(); }

    Listener(
      Private // effectively make this constructor private
    , Logger &log, asio::io_service &asio, filesystem::path const &path
    ) : mLog{log}, mAcceptor{asio, path.native()}
    {}

    template <typename Name>
    static std::optional<Listener> create(
      Logger &log, asio::io_service &asio, Name &&socket_name
    ) {
      char const *xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
      if (xdg_runtime == nullptr) {
        log.error("XDG_RUNTIME_DIR must be set");
        return std::nullopt;
      }
      filesystem::path socket{xdg_runtime};
      socket /= socket_name;

      if (filesystem::exists(socket)) {
        std::error_code error;
        filesystem::remove(socket, error);
        if (error) {
          log.error("Couldn't remove existing socket");
          return std::nullopt;
        }
      }

      log.info("Listening on ", socket);

      return std::make_optional<Listener>(Private{}, log, asio, socket);
    }
  };
}

int main() {
  using namespace waypositor;
  Logger log{"Main"};

  asio::io_service asio{};
  auto listener = Listener::create(log, asio, "wayland-0");
  if (!listener) return EXIT_FAILURE;
  listener->launch();
  asio.run();
  return EXIT_SUCCESS;
}