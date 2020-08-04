/*
 *
 * Copyright 2020 gRPC authors.
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

#include <unistd.h>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "examples/protos/account.grpc.pb.h"

using account::Account;
using account::GetUserInfoRequest;
using account::GetUserInfoResponse;
using account::MembershipType;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

class AccountServiceImpl final : public Account::Service {
 public:
  void SetHostName(const std::string& hostname) { hostname_ = hostname; }

 private:
  Status GetUserInfo(ServerContext* context, const GetUserInfoRequest* request,
                     GetUserInfoResponse* response) override {
    std::string token = request->token();
    context->AddInitialMetadata("hostname", hostname_);
    if (token == "2bd806c9") {
      response->set_name("Alice");
      response->set_membership(MembershipType::PREMIUM);
    } else if (token == "81b637d8") {
      response->set_name("Bob");
      response->set_membership(MembershipType::NORMAL);
    } else {
      std::cout << "Default to Bob" << std::endl;
      response->set_name("Bob");
      response->set_membership(MembershipType::NORMAL);
    }
    return Status::OK;
  }

  std::string hostname_;
};

void RunServer(const std::string& port, const std::string& hostname_suffix) {
  char base_hostname[256];
  if (gethostname(base_hostname, 256) != 0) {
    std::cout << "unable to get host name" << std::endl;
    return;
  }
  std::string hostname(base_hostname);
  hostname += hostname_suffix;
  std::string server_address("0.0.0.0:");
  server_address += port;
  AccountServiceImpl service;
  service.SetHostName(hostname);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Account server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  std::string port = "50053";
  std::string hostname_suffix = "";
  std::string arg_str_port("--port");
  std::string arg_str_hostname_suffix("--hostname_suffix");

  for (int i = 1; i < argc; ++i) {
    std::string arg_val = argv[i];
    std::cout << "arg " << i << " is " << arg_val << std::endl;
    size_t start_pos = arg_val.find(arg_str_port);
    if (start_pos != std::string::npos) {
      start_pos += arg_str_port.size();
      if (arg_val[start_pos] == '=') {
        port = arg_val.substr(start_pos + 1);
        continue;
      } else {
        std::cout << "The only correct argument syntax is --port=" << std::endl;
        return 0;
      }
    }
    start_pos = arg_val.find(arg_str_hostname_suffix);
    if (start_pos != std::string::npos) {
      start_pos += arg_str_hostname_suffix.size();
      if (arg_val[start_pos] == '=') {
        hostname_suffix = arg_val.substr(start_pos + 1);
        continue;
      } else {
        std::cout << "The only correct argument syntax is --hostname_suffix="
                  << std::endl;
        return 0;
      }
    }
  }

  std::cout << "port: " << port << ", hostname_suffix: " << hostname_suffix
            << std::endl;
  std::cout << "==========" << std::endl;
  RunServer(port, hostname_suffix);

  return 0;
}
