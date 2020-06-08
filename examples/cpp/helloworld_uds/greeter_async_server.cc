/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <memory>
#include <iostream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;

class ServerImpl final {
 public:
  ~ServerImpl() {
    server_->Shutdown();
    // Always shutdown the completion queue after the server.
    cq_->Shutdown();
  }

  int create_uds(std::string uds_path)
  {
    // unlink the uds if it exists
    unlink(uds_path.c_str());

    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (uds_fd < 0)
    {
      throw std::runtime_error("error creating uds");
    }

    struct sockaddr_un addr_info;
    memset(&addr_info, 0, sizeof(addr_info));
    addr_info.sun_family = AF_UNIX;

    snprintf(addr_info.sun_path, sizeof(addr_info.sun_path), "%s", uds_path.c_str());

    // bind on uds
    if (bind(uds_fd, (struct sockaddr*)&addr_info, sizeof(addr_info)) < 0)
    {
      throw std::runtime_error("error binding uds");
    }

    // listen on uds
    if (listen(uds_fd, 1) < 0)
    {
      throw std::runtime_error("error listening on uds");
    }

    // set uds ownership for current user
    if (chown(uds_path.c_str(), geteuid(), getegid()) != 0)
    {
      throw std::runtime_error("error setting uds ownership");
    }

    // set uds permission as read-write for current user & group
    if (chmod(uds_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0)
    {
      throw std::runtime_error("error setting uds permissions");
    }

    return uds_fd;
  }

  void set_nonblock_on_client_fd(int fd)
  {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
    {
      throw std::runtime_error("error getting flags for uds");
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      throw std::runtime_error("error setting non-blocking for uds");
    }
  }

  // There is no shutdown handling in this code.
  void Run() {
    std::string uds_path("/tmp/uds.sock");

    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    //builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service_" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *asynchronous* service.
    builder.RegisterService(&service_);
    // Get hold of the completion queue used for the asynchronous communication
    // with the gRPC runtime.
    cq_ = builder.AddCompletionQueue();
    // Finally assemble the server.
    server_ = builder.BuildAndStart();
    std::cout << "Server listening on " << uds_path << std::endl;

    struct sockaddr_un addr_remote;
    int addr_remote_size = sizeof(addr_remote);

    int uds_fd = create_uds(uds_path);

    new std::thread([&](){
      // Wait for the server to shutdown. Note that some other thread must be
      // responsible for shutting down the server for this call to ever return.
      while(true)
      {
        int client_fd = accept(uds_fd, (struct sockaddr *)&addr_remote, (socklen_t*)&addr_remote_size);

        if (client_fd < 0)
        {
          std::cout << errno << std::endl;
          throw std::runtime_error("error accepting on uds");
        }

        set_nonblock_on_client_fd(client_fd);

        grpc::AddInsecureChannelFromFd(server_.get(), client_fd);
      }
    });    

    // Proceed to the server's main loop.
    HandleRpcs();
  }

 private:
  // Class encompasing the state and logic needed to serve a request.
  class CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallData(Greeter::AsyncService* service, ServerCompletionQueue* cq)
        : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE) {
      // Invoke the serving logic right away.
      Proceed();
    }

    void Proceed() {
      if (status_ == CREATE) {
        // Make this instance progress to the PROCESS state.
        status_ = PROCESS;

        // As part of the initial CREATE state, we *request* that the system
        // start processing SayHello requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        service_->RequestSayHello(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
      } else if (status_ == PROCESS) {
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        new CallData(service_, cq_);

        // The actual processing.
        std::string prefix("Hello ");
        reply_.set_message(prefix + request_.name());

        // And we are done! Let the gRPC runtime know we've finished, using the
        // memory address of this instance as the uniquely identifying tag for
        // the event.
        status_ = FINISH;
        responder_.Finish(reply_, Status::OK, this);
      } else {
        GPR_ASSERT(status_ == FINISH);
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
      }
    }

   private:
    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    Greeter::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // What we get from the client.
    HelloRequest request_;
    // What we send back to the client.
    HelloReply reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<HelloReply> responder_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.
  };

  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    // Spawn a new CallData instance to serve new clients.
    new CallData(&service_, cq_.get());
    void* tag;  // uniquely identifies a request.
    bool ok;
    while (true) {
      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its tag, which in this case is the
      // memory address of a CallData instance.
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or cq_ is shutting down.
      GPR_ASSERT(cq_->Next(&tag, &ok));
      GPR_ASSERT(ok);
      static_cast<CallData*>(tag)->Proceed();
    }
  }

  std::unique_ptr<ServerCompletionQueue> cq_;
  Greeter::AsyncService service_;
  std::unique_ptr<Server> server_;
};

int main(int argc, char** argv) {
  ServerImpl server;
  server.Run();

  return 0;
}
