/** This is a TcpServer's performance test.
  * It could be run in N different modes:
  * 1) Local throughput test:
  *     Clients and server created on the same machine and communicates through the loopback.
  *     This mode is designated to test server performance locally, on a single machine. And
  *     this mode is very limited because clients affects the server performance.
  * 2) Server mode:
  *     Application starts in server mode and accepts incomming connections. It can be termia
  *     ted using ^C signal. This mode can be used to test server performance in isolation.
  *     Also, this test mode takes network into account. Application should dump number of me
  *     ssages per second to stdout.
  * 3) Client mode:
  *     Application started in client mode can connect to server (the same application should
  *     be started on another node in server mode) and dump specified number of messages to
  *     the server.
  *
  * Parameters:
  * a) `mode` can accept three parameters - 'client', 'server' or 'local'.
  * b) `host` url or ip of the server if app was started in client mode.
  * c) `count` number of messages to send inf started in client mode.
  */
#include <iostream>
#include <thread>
#include <boost/thread/barrier.hpp>
#include <boost/program_options.hpp>

#include "tcp_server.h"
#include <sys/time.h>

using namespace Akumuli;
namespace po = boost::program_options;

struct DbMock : DbConnection {
    typedef std::tuple<aku_ParamId, aku_TimeStamp, double> ValueT;
    aku_ParamId idsum;
    aku_TimeStamp tssum;
    double valsum;

    DbMock() {
        idsum  = 0;
        tssum  = 0;
        valsum = 0;
    }

    void write_double(aku_ParamId param, aku_TimeStamp ts, double data) {
        idsum  += param;
        tssum  += ts;
        valsum += data;
    }
};

class Timer
{
public:
    Timer() { gettimeofday(&_start_time, nullptr); }
    void   restart() { gettimeofday(&_start_time, nullptr); }
    double elapsed() const {
        timeval curr;
        gettimeofday(&curr, nullptr);
        return double(curr.tv_sec - _start_time.tv_sec) +
               double(curr.tv_usec - _start_time.tv_usec)/1000000.0;
    }
private:
    timeval _start_time;
};


enum Mode {
    CLIENT,
    SERVER,
    LOCAL,
};


Mode str_to_mode(std::string str) {
    if (str == "client" || str == "CLIENT") {
        return CLIENT;
    } else if (str == "server" || str == "SERVER") {
        return SERVER;
    } else if (str == "local" || str == "LOCAL") {
        return LOCAL;
    }
    throw std::runtime_error("Bad mode value");
}

struct Server {

    Mode                                mode;
    std::shared_ptr<IngestionPipeline>  pline;
    std::shared_ptr<DbMock>             dbcon;
    std::shared_ptr<TcpServer>          serv;
    boost::asio::io_service             ioA;
    std::vector<IOService*>             iovec       = { &ioA };
    std::atomic<int>                    thread_cnt  = {0};
    std::mutex                          mutex;
    std::condition_variable             condvar;

    Server(Mode mode)
        : mode(mode)
    {
        dbcon = std::make_shared<DbMock>();
        pline = std::make_shared<IngestionPipeline>(dbcon, AKU_LINEAR_BACKOFF);
        int port = 4096;
        serv = std::make_shared<TcpServer>(iovec, port, pline);
    }

    void start() {
        pline->start();
        serv->start();

        // Run IO service
        Server* serv = this;
        auto iorun = [serv](IOService& io) {
            auto fn = [&]() {
                io.run();
                serv->thread_cnt--;
                serv->condvar.notify_one();
            };
            return fn;
        };

        for (auto io: iovec) {
            thread_cnt++;
            std::thread iothread(iorun(*io));
            iothread.detach();
        }
    }

    void stop() {
        serv->stop();
        std::cout << "TcpServer stopped" << std::endl;

        // No need to joint I/O threads, just wait until they completes.
        // Waiting procedure
        std::unique_lock<std::mutex> lock(mutex);
        while(true) {
            condvar.wait(lock);
            int val = thread_cnt.load();
            if (val == 0) {
                break;
            } else if (val < 0) {
                throw std::runtime_error("Error in server wait proc. Invariant broken.");
            }
        }
        std::cout << "I/O threads stopped" << std::endl;

        pline->stop();
        std::cout << "Pipeline stopped" << std::endl;

        for (auto io: iovec) {
            io->stop();
        }
        std::cout << "I/O service stopped" << std::endl;

        std::cout << dbcon->idsum << " messages received" << std::endl;
    }
};

int main(int argc, char *argv[]) {
    std::cout << "Tcp server performance test" << std::endl;
    po::options_description desc("Allowed options");
    /*
     * Parameters:
     * a) `mode` can accept three parameters - 'client', 'server' or 'local'.
     * b) `host` url or ip of the server if app was started in client mode.
     * c) `count` number of messages to send inf started in client mode.
     */
    std::string mode;
    std::string host;
    int num_messages;
    desc.add_options()
        ("help",                                                                "produce help message")
        ("mode",    po::value<std::string>(&mode)->default_value("local"),      "test mode")
        ("host",    po::value<std::string>(&host)->default_value("localhost"),  "server host in client mode")
        ("count",   po::value<int>(&num_messages)->default_value(1000000),      "number of messages to send")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    // Create mock pipeline
    auto dbcon = std::make_shared<DbMock>();
    auto pline = std::make_shared<IngestionPipeline>(dbcon, AKU_LINEAR_BACKOFF);
    pline->start();

    // Run server
    // boost::asio::io_service ioA, ioB, ioC;  // Several io-services version
    boost::asio::io_service ioA;
    //std::vector<IOService*> iovec = { &ioA , &ioB, &ioC };  // Several io-services version
    std::vector<IOService*> iovec = { &ioA };
    int port = 4096;
    auto serv = std::make_shared<TcpServer>(iovec, port, pline);
    serv->start();

    // Run IO service
    auto iorun = [](IOService& io) {
        auto fn = [&]() {
            io.run();
        };
        return fn;
    };

    std::thread iothreadA(iorun(ioA));
    std::thread iothreadB(iorun(ioA));
    std::thread iothreadC(iorun(ioA));

    std::this_thread::sleep_for(std::chrono::seconds(1));

    boost::barrier barrier(4);

    // Push data to server
    auto push = [&]() {
        IOService io;
        const int COUNT = 2500000;  // 25M
        TcpSocket socket(io);
        auto loopback = boost::asio::ip::address_v4::loopback();
        boost::asio::ip::tcp::endpoint peer(loopback, 4096);
        socket.connect(peer);

        sleep(1);
        barrier.wait();

        boost::asio::streambuf stream;
        std::ostream os(&stream);
        for (int i = COUNT; i --> 0; ) {
            os << ":1\r\n" ":2\r\n" "+3.14\r\n";
            size_t n = socket.send(stream.data());
            stream.consume(n);
        }
        socket.shutdown(TcpSocket::shutdown_both);
        std::cout << "Push process completed" << std::endl;
    };

    Timer tm;
    std::thread pusherA(push);
    std::thread pusherB(push);
    std::thread pusherC(push);
    std::thread pusherD(push);

    pusherA.join();
    pusherB.join();
    pusherC.join();
    pusherD.join();

    serv->stop();
    std::cout << "TcpServer stopped" << std::endl;

    iothreadA.join();
    iothreadB.join();
    iothreadC.join();
    std::cout << "I/O thread stopped" << std::endl;

    pline->stop();
    std::cout << "Pipeline stopped" << std::endl;

    ioA.stop();
    //ioB.stop();
    //ioC.stop();
    std::cout << "I/O service stopped" << std::endl;

    std::cout << dbcon->idsum << " messages received" << std::endl;

    // Every message is processed here
    double elapsed = tm.elapsed();
    std::cout << "100M sent in " << elapsed << "s" << std::endl;

    return 0;
}
