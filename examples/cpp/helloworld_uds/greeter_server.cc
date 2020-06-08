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

#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

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
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::Service {
  Status SayHello(ServerContext* context, const HelloRequest* request,
                  HelloReply* reply) override {
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());
    return Status::OK;
  }
};

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

void RunServer() {
  std::string uds_path("/tmp/uds.sock");

  GreeterServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;

  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << uds_path << std::endl;

  struct sockaddr_un addr_remote;
  int addr_remote_size = sizeof(addr_remote);

  int uds_fd = create_uds(uds_path);

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

    grpc::AddInsecureChannelFromFd(server.get(), client_fd);
  }
}  

int main(int argc, char** argv) {
  RunServer();

  return 0;
}