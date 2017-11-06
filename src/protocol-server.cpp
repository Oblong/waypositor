#include <waypositor/logger.hpp>

#include <cstdlib>

#include <optional>
#include <system_error>
#include <unordered_map>
#include <experimental/filesystem>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>

namespace waypositor {
  namespace filesystem = std::experimental::filesystem;
  namespace asio = boost::asio;
  using Domain = asio::local::stream_protocol;

  class Parser {
  private:
    enum class State { OBJECT_ID, OPCODE, MESSAGE_SIZE, FINISHED };
    uint32_t mObjectId;
    uint16_t mOpcode;
    uint16_t mMessageSize;
    State mState{State::OBJECT_ID};
  public:
    template <typename Continuer>
    void resume(Logger &log, Continuer continuer) {
      switch (mState) {
      case State::OBJECT_ID:
        mState = State::OPCODE;
        continuer.async_read(asio::buffer(&mObjectId, sizeof(uint32_t)));
        return;
      case State::OPCODE:
        mState = State::MESSAGE_SIZE;
        continuer.async_read(asio::buffer(&mOpcode, sizeof(uint16_t)));
        return;
      case State::MESSAGE_SIZE:
        mState = State::FINISHED;
        continuer.async_read(asio::buffer(&mMessageSize, sizeof(uint16_t)));
        return;
      case State::FINISHED:
        log.info("Finished parsing header");
        log.info("Object ID: ", mObjectId);
        log.info("Message Size: ", mMessageSize);
        log.info("Opcode: ", mOpcode);
        mState = State::OBJECT_ID;
        continuer.suspend();
        return;
      }
    }
  };

  // This is pretty complex in order to support shutting down the server
  // cleanly. There might be a simpler way. It's currently thread safe, but I
  // don't see why we'd ever need more than one thread for this.
  class Registry final {
  private:
    class Connection final {
    private:
      Logger &mLog;
      asio::io_service &mAsio;
      Registry &mOwner;
      std::size_t mId;
      std::mutex mMutex;
      std::optional<Domain::socket> mSocket;
      Parser mParser;

      class Continuer final {
      private:
        std::shared_ptr<Connection> self;
      public:
        template <typename Buffers>
        void async_read(Buffers &&buffers) {
          asio::async_read(*self->mSocket, buffers, Worker{std::move(self)});
        }

        void suspend() {
          self->mAsio.post(Worker{std::move(self)});
        }

        Continuer(std::shared_ptr<Connection> &&self_)
          : self{std::move(self_)}
        {}
      };

      class Worker final {
      private:
        std::shared_ptr<Connection> self;
      public:
        Worker(Worker const &);
        Worker &operator=(Worker const &);
        Worker(Worker &&other) = default;
        Worker &operator=(Worker &&other) = default;
        ~Worker() {
          if (self) self->mOwner.mLookup.erase(self->mId);
        }

        Worker(std::shared_ptr<Connection> self_) : self{std::move(self_)} {}

        void operator()(
          boost::system::error_code const &error = {}, std::size_t = 0
        ) {
          if (error) {
            self->mLog.error("ASIO: ", error.message());
            return;
          }
          auto lock = std::lock_guard(self->mMutex);
          if (!self->mSocket) {
            self->mLog.info(
              "Connection worker exiting due to connection closure"
            );
            return;
          }

          self->mParser.resume(self->mLog, Continuer{std::move(self)});
        }

        explicit operator bool() const { return self != nullptr; }
      };

      struct Private {};
    public:
      Connection(
        Private // make it effectively private
      , Logger &log, asio::io_service &asio
      , Registry &owner, std::size_t id, Domain::socket socket
      ) : mLog{log}, mAsio{asio}, mOwner{owner}, mId{id}
        , mMutex{}, mSocket{std::move(socket)}
        , mParser{}
      {}
      ~Connection() { mLog.info("Connection ", mId, " destroyed"); }

      void close() {
        auto lock = std::lock_guard(mMutex);
        mSocket = std::nullopt;
      }

      class Handle {
      private:
        std::shared_ptr<Connection> mHandle;
      public:
        // These should be deleted, but there was a bug in gcc:
        // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80654
        Handle(Handle const &);
        Handle &operator=(Handle const &);
        Handle(Handle &&) = default;
        Handle &operator=(Handle &&) = default;
        ~Handle() { if (mHandle) mHandle->close(); }

        Handle(std::shared_ptr<Connection> handle)
          : mHandle{std::move(handle)}
        {}
      };

      static Handle create(
        Logger &log, asio::io_service &asio
      , Registry &owner, std::size_t id, Domain::socket socket
      ) {
        auto pointer = std::make_shared<Connection>(
          Private{}, log, asio, owner, id, std::move(socket)
        );
        Worker{pointer}();
        return {std::move(pointer)};
      }
    };

    std::unordered_map<std::size_t, Connection::Handle> mLookup{};
    std::size_t mCurrentId{0};
  public:
    void connect(
      Logger &log, asio::io_service &asio, Domain::socket socket
    ) {
      auto handle = Connection::create(
        log, asio, *this, mCurrentId, std::move(socket)
      );
      mLookup.emplace(mCurrentId, std::move(handle));
      log.info("Connection ", mCurrentId, " accepted");
      mCurrentId++;
    }
  };

  class Listener final {
  private:
    enum class State { STOPPED, LISTENING, ACCEPTED };
    Logger &mLog;
    asio::io_service &mAsio;
    Domain::acceptor mAcceptor;
    Domain::socket mSocket;
    std::optional<Registry> mConnections;
    State mState;

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
          self->mLog.error("ASIO: ", error.message());
          return;
        }

        switch (self->mState) {
        case State::STOPPED:
          self->mLog.info("Socket listener stopped by request");
          return;
        case State::LISTENING:
          self->mState = State::ACCEPTED;
          self->mAcceptor.async_accept(self->mSocket, std::move(*this));
          return;
        case State::ACCEPTED:
          self->mConnections->connect(
            self->mLog, self->mAsio, std::move(self->mSocket)
          );
          self->mState = State::LISTENING;
          self->mAsio.post(std::move(*this));
          return;
        }
      }

      explicit operator bool() const { return self != nullptr && *self; }
    };

    struct Private {};
  public:
    void launch() { Worker{*this}(); }

    void stop() {
      mState = State::STOPPED;
      mConnections = std::nullopt;

      boost::system::error_code error;
      mAcceptor.cancel(error);
      if (error) {
        mLog.error("ASIO: ", error.message());
      }
    }

    explicit operator bool() const {
      return mState == State::STOPPED || static_cast<bool>(mConnections);
    }

    Listener(
      Private // effectively make this constructor private
    , Logger &log, asio::io_service &asio, filesystem::path const &path
    ) : mLog{log}, mAsio{asio}
      , mAcceptor{asio, path.native()}, mSocket{asio}
      , mConnections{std::make_optional<Registry>()}
      , mState{State::LISTENING}
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

  asio::signal_set signals{asio, SIGINT, SIGTERM};
  signals.async_wait([&](
    boost::system::error_code const &error, int /*signal*/
  ) {
    if (error) {
      log.error("ASIO: ", error.message());
      return;
    }
    listener->stop();
  });

  asio.run();
  return EXIT_SUCCESS;
}