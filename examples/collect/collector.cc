#include <examples/collect/collect.pb.h>
#include <examples/collect/ProcFs.h>

#include <muduo/base/LogFile.h>
#include <muduo/base/Logging.h>
#include <muduo/net/inspect/Inspector.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/protobuf/ProtobufCodecLite.h>
#include <muduo/protorpc2/RpcServer.h>
#include <google/malloc_hook.h>

#include <signal.h>

using namespace muduo;

namespace collect
{

extern const char tag[] = "SYS0";
typedef muduo::net::ProtobufCodecLiteT<SystemInfo, tag> Codec;

class CollectLogger : muduo::noncopyable
{
 public:
  CollectLogger(muduo::net::EventLoop* loop, const string& filename)
    : loop_(loop),
      codec_(std::bind(&CollectLogger::onMessage, this, _1, _2, _3)),
      file_(filename, 100*1000*1000, false, 60, 30) // flush every 60s, check every 30 writes
  {
  }

  void start()
  {
    if (proc_.fill(SnapshotRequest_Level_kSystemInfoInitialSnapshot, &info_))
    {
      printf("%s\n", info_.DebugString().c_str());
      save();
      file_.flush();
      info_.Clear();
    }
    loop_->runEvery(1.0, std::bind(&CollectLogger::snapshot, this));
  }

  bool fill(SnapshotRequest_Level level, SystemInfo* info)
  {
    return proc_.fill(level, info);
  }

  void snapshot()
  {
    if (proc_.fill(SnapshotRequest_Level_kSystemInfoAndThreads, &info_))
    {
      save();
      info_.Clear();
    }
  }

  void flush()
  {
    file_.flush();
  }

  void roll()
  {
    file_.rollFile();
  }

 private:
  void onMessage(const muduo::net::TcpConnectionPtr&,
                 const SystemInfoPtr& messagePtr,
                 muduo::Timestamp receiveTime)
  {
  }

  void save()
  {
    codec_.fillEmptyBuffer(&output_, info_);
    LOG_DEBUG << output_.readableBytes();
    file_.append(output_.peek(), static_cast<int>(output_.readableBytes()));
    output_.retrieveAll();
  }

  muduo::net::EventLoop* loop_;
  ProcFs proc_;
  Codec codec_;
  muduo::LogFile file_;
  muduo::net::Buffer output_;  // for scratch
  SystemInfo info_;  // for scratch
};

class CollectServiceImpl : public CollectService
{
 public:
  CollectServiceImpl(CollectLogger* logger)
    : logger_(logger)
  {
  }

  virtual void getSnapshot(const ::collect::SnapshotRequestPtr& request,
                           const ::collect::SystemInfo* responsePrototype,
                           const ::muduo::net::RpcDoneCallback& done) // override
  {
    SystemInfo response;
    logger_->fill(request->level(), &response);
    done(&response);
  }

  virtual void flush(const ::rpc2::EmptyPtr& request,
                     const ::rpc2::Empty* responsePrototype,
                     const ::muduo::net::RpcDoneCallback& done) // override
  {
    logger_->flush();
    ::rpc2::Empty response;
    done(&response);
  }

  virtual void roll(const ::rpc2::EmptyPtr& request,
                    const ::rpc2::Empty* responsePrototype,
                    const ::muduo::net::RpcDoneCallback& done) // override
  {
    logger_->roll();
    ::rpc2::Empty response;
    done(&response);
  }

 private:
  CollectLogger* logger_;
};
}

using namespace muduo::net;
EventLoop* g_loop;

void sighandler(int)
{
  g_loop->quit();
}

void NewHook(const void* ptr, size_t size)
{
  printf("alloc %zd %p\n", size, ptr);
}

int main(int argc, char* argv[])
{
  EventLoop loop;
  g_loop = &loop;
  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);
  // MallocHook::SetNewHook(&NewHook);

  collect::CollectLogger logger(&loop, "collector");
  logger.start();

  collect::CollectServiceImpl impl(&logger);
  std::unique_ptr<RpcServer> server;
  if (argc > 1)
  {
    uint16_t port = static_cast<uint16_t>(atoi(argv[1]));
    server.reset(new RpcServer(&loop, InetAddress(port)));
    server->registerService(&impl);
    server->start();
  }

  std::unique_ptr<Inspector> ins;
  if (argc > 2)
  {
    uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
    ins.reset(new Inspector(&loop, InetAddress(port), "collector"));
    ins->remove("pprof", "profile"); // remove 30s blocking
  }

  loop.loop();
  google::protobuf::ShutdownProtobufLibrary();
}
