// Exercise the real socket/worker/FFmpeg lifecycle without an NVR or SDK login.
#define main bridge_application_main
#define wmain bridge_application_wmain
#include "../src/main.cpp"
#undef main
#undef wmain

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ConnectedSockets {
    socket_handle client{invalid_socket}, server{invalid_socket};
    ConnectedSockets() {
        Config config;
        config.port = 0;
        const auto listener = create_listener(config);
        sockaddr_in address{};
        socket_length size = sizeof(address);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
        client = create_socket(AF_INET, SOCK_STREAM, 0);
        require(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect failed");
        server = accept_client(listener, nullptr, nullptr);
#if !defined(_WIN32)
        ::fcntl(client, F_SETFD, FD_CLOEXEC);
        ::fcntl(server, F_SETFD, FD_CLOEXEC);
#endif
        close_socket(listener);
        require(server != invalid_socket, "accept failed");
    }
    socket_handle take_server() { const auto result = server; server = invalid_socket; return result; }
    ~ConnectedSockets() { close_socket(client); close_socket(server); }
};

int main(int argc, char** argv) {
    try {
        NetworkRuntime network;
        (void)network;
        {
            Config config;config.port=0;auto listener=create_listener(config);
            sockaddr_in address{};socket_length length=sizeof(address);::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length);
            config.port=ntohs(address.sin_port);bool rejected=false;
            try{auto duplicate=create_listener(config);close_socket(duplicate);}catch(const BridgeError& e){rejected=e.code=="BIND_FAILED";}
            close_socket(listener);require(rejected,"Second listener bound to occupied port");
            auto restarted=create_listener(config);close_socket(restarted);
            std::cout<<"PASS exclusive listener and rebind after close"<<std::endl;
        }
        {
            g_running.store(true);std::atomic_bool release{false};std::atomic_int entered{0};
            ConnectedSockets first,second,excess;
            ConnectionWorkers workers(2);
            auto hold=[&]{++entered;while(!release.load()&&g_running.load())std::this_thread::sleep_for(1ms);};
            require(workers.start(first.take_server(),hold),"First HTTP worker rejected");
            require(workers.start(second.take_server(),hold),"Second HTTP worker rejected");
            require(!workers.start(excess.take_server(),hold),"HTTP limit was bypassed");
            char response[512]{};int n=::recv(excess.client,response,sizeof(response),0);
            require(n>0&&std::string(response,n).find("503")!=std::string::npos,"Excess connection did not receive busy response");
            release.store(true);std::this_thread::sleep_for(50ms);workers.reap();
            struct ThrowOnMove {ThrowOnMove()=default;ThrowOnMove(ThrowOnMove&&){throw std::bad_alloc();}void operator()(){};};
            ConnectedSockets allocation_failure;ThrowOnMove handler;
            require(!workers.start(allocation_failure.take_server(),std::move(handler)),"Worker creation exception escaped or was not handled");
            ConnectedSockets recovered;require(workers.start(recovered.take_server(),[]{}),"Worker capacity did not recover");
            std::cout<<"PASS HTTP cap, busy response, allocation failure containment and recovery"<<std::endl;
        }
        // Exercise the public cleanup protocol without logging into a real device.
        {
            g_running.store(true);
            Config config;
            FfmpegHardwareAcceleration hardware;

            SessionRegistry sessions(16);
            SessionStatusRegistry statuses;
            VideoCodecCache cache(0, fs::path());
            auto call = [&](const std::string& method, const std::string& path, const std::string& headers = "") {
                ConnectedSockets sockets;
                std::thread server([&] {
                    handle_connection(sockets.server, config, hardware, sessions, statuses, cache);
#if defined(_WIN32)
                    ::shutdown(sockets.server, SD_SEND);
#else
                    ::shutdown(sockets.server, SHUT_WR);
#endif
                });
                std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + headers + "\r\n";
                ::send(sockets.client, request.data(), static_cast<int>(request.size()), 0);
                std::string response; char buffer[4096]; int count;
                while ((count = ::recv(sockets.client, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, count);
                server.join(); return response;
            };
            ConnectedSockets playing;
            const auto before = sessions.generation();
            auto lease = sessions.reserve("playing", playing.server, before, [&] { statuses.set_active("playing"); });
            require(call("GET", "/healthz").find("\"cleanSessions\":true") != std::string::npos, "health capability missing");
            require(sessions.active() == 1 && sessions.generation() == before, "health mutated sessions");
            require(call("GET", "/cleanSessions?apply=true").find("405") != std::string::npos, "GET allowed cleanup");
            require(call("POST", "/cleanSessions?apply=false").find("400") != std::string::npos, "false apply accepted");
            require(call("POST", "/cleanSessions").find("400") != std::string::npos, "missing apply accepted");
            require(call("OPTIONS", "/cleanSessions?apply=true", "Origin: http://another-site.test\r\n").find("Access-Control-Allow-Origin: *") != std::string::npos, "arbitrary origin preflight blocked");
            require(!lease->cancelled(), "rejected request cleaned sessions");
            require(call("OPTIONS", "/cleanSessions?apply=true", "Origin: http://example.test\r\n").find("204") != std::string::npos, "preflight failed");
            require(sessions.generation() == before, "preflight mutated sessions");
            ConnectedSockets starting;
            auto startup = sessions.reserve("starting", starting.server, before, [&] { statuses.set_active("starting"); });
            const auto result = call("POST", "/cleanSessions?apply=true", "Origin: http://another-site.test\r\nX-Hik-Cleanup: true\r\n");
            require(result.find("202") != std::string::npos && lease->cancelled(), "cleanup did not cancel active stream");
            require(result.find("\"affectedSessions\":2") != std::string::npos && startup->cancelled(), "cleanup missed starting stream");
            require(sessions.active() == 2 && sessions.cleaning() == 2, "capacity released before resources");
            require(statuses.get("playing")->end_reason == "manual_cleanup", "cleanup reason missing");
            statuses.set_ended("playing", StreamEndReason::ClientReplaced, "", 0, 0, 0);
            require(statuses.get("playing")->end_reason == "manual_cleanup", "late disconnect overwrote cleanup reason");
            for (const auto& token : {before, std::string()}) {
                bool rejected = false;
                try { auto old = sessions.reserve("retry", invalid_socket, token); }
                catch (const BridgeError& e) { rejected = e.code == "MANUAL_RESTART_REQUIRED"; }
                require(rejected, "stale or legacy retry bypassed cleanup");
            }
            lease.reset();
            startup.reset();
            require(sessions.active() == 0 && sessions.cleaning() == 0, "cleanup did not return capacity");
            auto manual = sessions.reserve("manual", invalid_socket, sessions.generation());
            require(manual != nullptr, "manual restart rejected");
            std::cout << "PASS cleanup API, origin, generation and resource accounting" << std::endl;
        }
#if !defined(_WIN32)
        ::signal(SIGPIPE, SIG_IGN);
#endif
        // A half-written request must not delay stop or outlive its dependencies.
        g_running.store(true);
        ConnectedSockets incomplete;
        std::atomic_bool exited{false};
        const auto started = std::chrono::steady_clock::now();
        {
            ConnectionWorkers workers;
            const auto fd = incomplete.take_server();
            workers.start(fd, [fd, &exited] {
                try { read_http_request(fd); } catch (...) {}
                exited.store(true);
            });
            const char partial[] = "GET /healthz HTTP/1.1\r\n";
            ::send(incomplete.client, partial, sizeof(partial) - 1, 0);
            std::this_thread::sleep_for(50ms);
            require(!exited.load(), "incomplete request unexpectedly completed");
        }
        require(exited.load(), "worker outlived its owner");
        require(std::chrono::steady_clock::now() - started < 2s, "header cancellation was too slow");
        std::cout << "PASS partial headers" << std::endl;

        // shutdown must also unblock a client that never reads a large response.
        g_running.store(true);
        ConnectedSockets blocked_writer;
        {
            ConnectionWorkers workers;
            const auto fd = blocked_writer.take_server();
            const int size = 4096;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
            workers.start(fd, [fd] {
                std::vector<uint8_t> payload(16 * 1024 * 1024);
                write_socket_all(fd, payload.data(), payload.size());
            });
            std::this_thread::sleep_for(50ms);
        }
        std::cout << "PASS blocked sends" << std::endl;

        // Backpressure must preserve response bytes when a slow client resumes.
        g_running.store(true);
        ConnectedSockets slow_reader;
        std::atomic_bool delivered{false};
        {
            ConnectionWorkers workers;
            const auto fd = slow_reader.take_server();
            const int size = 4096;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
            workers.start(fd, [fd, &delivered] {
                std::vector<uint8_t> payload(256 * 1024, 0x5a);
                delivered.store(write_socket_all(fd, payload.data(), payload.size()));
            });
            std::this_thread::sleep_for(50ms);
            size_t received = 0;
            uint8_t buffer[8192];
            while (received < 256 * 1024) {
                const int count = ::recv(slow_reader.client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
                require(count > 0, "backpressure truncated a response");
                for (int i = 0; i < count; ++i) require(buffer[i] == 0x5a, "response bytes changed");
                received += static_cast<size_t>(count);
            }
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (!delivered.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            require(delivered.load(), "response send failed after backpressure");
            workers.reap();
        }
        std::cout << "PASS backpressure preserves response" << std::endl;

        // Complete requests still parse normally.
        g_running.store(true);
        for (int i = 0; i < 32; ++i) {
            ConnectedSockets sockets;
            const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
            require(write_socket_all(sockets.client, reinterpret_cast<const uint8_t*>(request.data()), request.size()), "request write failed");
            const auto parsed = read_http_request(sockets.server);
            require(parsed.method == "GET" && parsed.path == "/healthz", "HTTP parsing changed");
        }
        std::cout << "PASS HTTP parsing" << std::endl;

        // A full registry rejects the next request, and exceptions return exactly one slot.
        SessionRegistry capacity(16);
        std::vector<std::unique_ptr<SessionRegistry::Lease>> leases;
        for (int i = 0; i < 16; ++i) leases.push_back(capacity.reserve("capacity-test"));
        require(capacity.active() == 16 && !capacity.reserve(), "session limit was not enforced");
        require(session_counts_json(capacity).find("\"availableSessions\":0") != std::string::npos, "full capacity was not reported");
        leases.pop_back();
        try { auto lease = capacity.reserve(); require(lease != nullptr, "released slot was not reusable"); throw std::runtime_error("expected"); } catch (const std::runtime_error&) {}
        require(capacity.active() == 15, "exception leaked or double released a slot");
        leases.clear(); require(capacity.active() == 0, "slots leaked");
        std::cout << "PASS capacity accounting" << std::endl;

        // A connected browser that stops consuming must not retain capacity forever.
        g_running.store(true);
        ConnectedSockets stalled;
        std::atomic_bool timed_out{false}, finished{false};
        {
            ConnectionWorkers workers;
            const auto fd = stalled.take_server();
            const int size = 4096;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
            workers.start(fd, [&, fd] {
                auto lease = capacity.reserve("stalled-writer");
                try { std::vector<uint8_t> payload(16 * 1024 * 1024); write_socket_all(fd, payload.data(), payload.size(), 200); }
                catch (const BridgeError& error) { timed_out.store(error.code == "CLIENT_WRITE_TIMEOUT"); }
                lease.reset(); finished.store(true);
            });
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!finished.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
            require(finished.load() && timed_out.load() && capacity.active() == 0, "blocked send retained its session");
        }
        std::cout << "PASS stalled client timeout releases capacity" << std::endl;

        if (argc > 1) {
            g_running.store(true);
            Config config;
            config.ffmpeg_path = fs::absolute(fs::u8path(argv[1]));
            StreamRequest request;
            request.speed = 1;
            request.sid = "lifecycle-test";
            FfmpegHardwareAcceleration hardware;
#if !defined(_WIN32)
            {
                Config listening=config;listening.port=0;auto fd=create_listener(listening);
                require((::fcntl(fd,F_GETFD)&FD_CLOEXEC)!=0,"Listener is inheritable");
                sockaddr_in address{};socket_length length=sizeof(address);::getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length);listening.port=ntohs(address.sin_port);
                ConnectedSockets sockets;
                require((::fcntl(sockets.server,F_GETFD)&FD_CLOEXEC)!=0,"Accepted client is inheritable");
                FfmpegProcess child(config,request,VideoOutputMode::Copy,hardware);
                close_socket(fd);auto replacement=create_listener(listening);close_socket(replacement);
                std::cout<<"PASS FFmpeg does not retain listener after parent closes it"<<std::endl;
            }
            {
                std::atomic_int ready{0},done{0};std::atomic_bool release{false},failed{false};std::vector<std::thread> children;
                for(int i=0;i<8;++i)children.emplace_back([&]{try{
                    {FfmpegProcess process(config,request,VideoOutputMode::Copy,hardware);++ready;while(!release.load())std::this_thread::sleep_for(1ms);}
                    ++done;
                }catch(...){failed.store(true);++ready;}});
                auto deadline=std::chrono::steady_clock::now()+5s;while(ready.load()<8&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(5ms);
                release.store(true);for(auto& child:children)child.join();
                require(!failed.load()&&done.load()==8,"Concurrent FFmpeg spawn/cleanup failed");
                std::cout<<"PASS concurrent posix_spawn and independent cleanup"<<std::endl;
            }
#endif
            const auto before_ffmpeg = std::chrono::steady_clock::now();
            {
                FfmpegProcess process(config, request, VideoOutputMode::Copy, hardware);
                std::atomic_bool reading{false};
                std::thread reader([&] {
                    uint8_t buffer[32];
                    reading.store(true);
                    try { process.read(buffer, sizeof(buffer)); } catch (...) {}
                });
                while (!reading.load()) std::this_thread::yield();
                std::this_thread::sleep_for(50ms);
                g_running.store(false);
                reader.join();
            }
            require(std::chrono::steady_clock::now() - before_ffmpeg < 3s, "FFmpeg read/cleanup did not cancel");

            // Exercise waiting for both initialization and ongoing output after browser close.
            for (bool initial : {true, false}) {
                g_running.store(true);
                ConnectedSockets sockets;
                auto lease = capacity.reserve("disconnected-ffmpeg");
                FfmpegProcess process(config, request, VideoOutputMode::Copy, hardware, false, [&] { check_video_client(sockets.server); });
                close_socket(sockets.client); sockets.client = invalid_socket;
                bool disconnected = false;
                const auto started_read = std::chrono::steady_clock::now();
                try { if (initial) process.read_initial(10000); else { uint8_t buffer[32]; process.read(buffer, sizeof(buffer)); } }
                catch (const ClientDisconnected&) { disconnected = true; }
                require(disconnected && std::chrono::steady_clock::now() - started_read < 1s, "browser close did not cancel FFmpeg wait");
            }
            require(capacity.active() == 0, "disconnected output retained capacity");
            g_running.store(true); config.output_stall_ms = 150;
            for (double speed : {1.0, 0.25}) {
                request.speed = speed;
                const auto started_read = std::chrono::steady_clock::now();
                bool timed = false;
                {
                    auto lease = capacity.reserve("no-ffmpeg-output");
                    FfmpegProcess process(config, request, VideoOutputMode::Copy, hardware);
                    try { uint8_t buffer[32]; process.read(buffer, sizeof(buffer)); }
                    catch (const BridgeError& error) { timed = error.code == "FFMPEG_OUTPUT_TIMEOUT"; }
                }
                require(timed && capacity.active() == 0, "output timeout did not release capacity");
                require(std::chrono::steady_clock::now() - started_read >= std::chrono::milliseconds(static_cast<int>(150 / speed)), "slow playback timeout was not scaled");
            }
            std::cout << "PASS client disconnect, FFmpeg stall, slow-play allowance" << std::endl;
        }
        std::cout << "PASS lifecycle" << (argc > 1 ? " including FFmpeg cancellation" : " (FFmpeg skipped: no path supplied)") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
