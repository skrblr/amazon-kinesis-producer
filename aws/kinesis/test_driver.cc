// Copyright 2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Amazon Software License (the "License").
// You may not use this file except in compliance with the License.
// A copy of the License is located at
//
//  http://aws.amazon.com/asl
//
// or in the "license" file accompanying this file. This file is distributed
// on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied. See the License for the specific language governing
// permissions and limitations under the License.

#include <stdlib.h>
#include <unistd.h>

#include <thread>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <glog/logging.h>

#include <gperftools/profiler.h>

#include <aws/kinesis/protobuf/messages.pb.h>
#include <aws/utils/utils.h>
#include <aws/auth/credentials.h>
#include <aws/kinesis/core/configuration.h>
#include <aws/kinesis/core/ipc_manager.h>
#include <aws/kinesis/core/kinesis_producer.h>
#include <aws/utils/io_service_executor.h>
#include <aws/http/io_service_socket.h>
#include <aws/metrics/metrics_manager.h>

int main(int argc, char** argv) {
  google::InitGoogleLogging("");
  FLAGS_logtostderr = 1;
  FLAGS_minloglevel = 0;

  //ProfilerStart("test_driver.prof");

  const char* to_child = "test_fifo2";
  const char* from_child = "test_fifo";
  const auto start = std::chrono::steady_clock::now();
  const uint64_t window = 10;
  //const uint64_t num_streams = 10;
  const uint64_t data_size = 0;
  // const uint64_t key_size = 64;
  const char* region = "us-west-2";
  const char* stream_name = "test_b";
  const uint64_t max_outstanding = 10000;
  //const uint64_t max_outstanding = 10000;

  auto config = std::make_shared<aws::kinesis::core::Configuration>();
  //config->record_max_buffered_time(0xFFFFFFF);
  config->record_ttl(0xFFFFFFF);
  //config->record_max_buffered_time(30000);
  //config->aggregation_max_size(1024);
  //config->metrics_granularity("stream");
  //config->metrics_level("summary");
  //config->verify_certificate(false);
  //config->custom_endpoint("54.183.89.5");
  config->add_additional_metrics_dims("GlobalCustomMetric", "global", "global");
  config->add_additional_metrics_dims("StreamCustomMetric", "stream", "stream");
  config->add_additional_metrics_dims("ShardCustomMetric", "shard", "shard");

  std::atomic<uint64_t> id(0);
  std::atomic<uint64_t> sent(0);
  std::atomic<uint64_t> success(0);
  std::atomic<uint64_t> fail(0);
  std::atomic<uint64_t> last_success(0);
  std::atomic<uint64_t> last_sent(0);

  auto ipc_channel =
      std::make_shared<aws::kinesis::core::detail::IpcChannel>(
          from_child,
          to_child);
  auto ipc = std::make_shared<aws::kinesis::core::IpcManager>(ipc_channel);

  auto executor = std::make_shared<aws::utils::IoServiceExecutor>(8);
  auto socket_factory = std::make_shared<aws::http::IoServiceSocketFactory>();
  auto ec2_metadata = std::make_shared<aws::http::Ec2Metadata>(executor,
                                                               socket_factory);
  auto provider =
      std::make_shared<aws::auth::DefaultAwsCredentialsProvider>(
          executor,
          ec2_metadata);

  aws::utils::sleep_for(std::chrono::milliseconds(150));
  if (!provider->get_credentials()) {
    throw std::runtime_error("Invalid credentials");
  }

  auto ipc_channel2 =
      std::make_shared<aws::kinesis::core::detail::IpcChannel>(
          to_child,
          from_child);
  auto ipc2 = std::make_shared<aws::kinesis::core::IpcManager>(ipc_channel2);

  aws::kinesis::core::KinesisProducer kp(
      ipc2,
      region,
      config,
      provider,
      executor,
      socket_factory);
      /*std::make_shared<aws::auth::BasicAwsCredentialsProvider>(
          "AKIAAAAAAAAAAAAAAAAA",
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));*/

  auto receiver = aws::thread([&] {
    std::string s;
    while (true) {
      if (ipc->try_take(s)) {
        aws::kinesis::protobuf::Message reply;
        reply.ParseFromString(s);

        if (reply.has_put_record_result()) {
          auto& prr = reply.put_record_result();
          if (prr.has_shard_id()) {
            success++;
          } else {
            auto a = prr.attempts(prr.attempts_size() - 1);
            LOG(ERROR) << a.error_code() << " : " << a.error_message();
            fail++;
          }
        } else if (reply.has_metrics_response()) {
          auto res = reply.metrics_response();
          std::stringstream ss;
          ss << "\n";
          for (auto i = 0; i < res.metrics_size(); i++) {
            auto& metric = res.metrics(i);

            /*bool print = true;
            for (auto j = 0; j < metric.dimensions_size(); j++) {
              if (metric.dimensions(j).key() == "ShardId") {
                print = false;
              }
            }
            if (!print) {
              continue;
            }*/

            ss << "[" << metric.seconds() << "] ["
               << metric.name() << "] [";
            for (auto j = 0; j < metric.dimensions_size(); j++) {
              auto& dim = metric.dimensions(j);
              ss << dim.key() << ": " << dim.value() << "; ";
            }
            ss << "] [";
            auto& stats = metric.stats();
            ss << "Count: " << stats.count() << "; "
               << "Min: " << stats.min() << "; "
               << "Max: " << stats.max() << "; "
               << "Sum: " << stats.sum() << "; "
               << "Mean: " << stats.mean() << "; "
               << "]\n";
          }
          // LOG(INFO) << ss.str();
        }
      } else {
        aws::utils::sleep_for(std::chrono::milliseconds(1));
      }
    }
  });

  aws::thread writer([&] {
    bool first = true;
    while (true) {
      if (sent - success - fail > max_outstanding) {
        aws::utils::sleep_for(std::chrono::milliseconds(1));;
        continue;
      }

      aws::kinesis::protobuf::Message m;
      m.set_id(id++);

      auto p = m.mutable_put_record();
      p->set_data(std::string(data_size, 'a'));
      //p->set_data(std::string(::rand() % (50 * 1024 + 1), 'a'));
      p->set_stream_name(stream_name);
      //p->set_stream_name("test" + std::to_string(::rand() % 4 + 2));
      p->set_partition_key(std::to_string(::rand()));

      ipc->put(m.SerializeAsString());
      sent++;

      if (first) {
        // allow shard map to update
        aws::utils::sleep_for(std::chrono::seconds(2));
        first = false;
      }
    }
  });

  aws::thread metrics([&] {
    while (true) {
      aws::kinesis::protobuf::Message m;
      m.set_id(id++);

      auto r = m.mutable_metrics_request();
      r->set_name("KinesisRecordsDataPut");
      r->set_seconds(10);

      ipc->put(m.SerializeAsString());

      aws::utils::sleep_for(std::chrono::seconds(5));
    }
  });

  aws::thread printer([&] {
    while (true) {
      double seconds = (double) aws::utils::millis_since(start) / 1000;
      double d_success = success - last_success;
      double d_sent = sent - last_sent;
      last_sent.store(sent);
      last_success.store(success);
      LOG(INFO) << seconds << ", "
                << success << " success ("
                << (d_success / window / 1000) << " Krps), "
                << fail << " fail, "
                << (sent - success) << " outstanding, "
                << sent << " attempted ("
                << (d_sent / window / 1000) << " Krps)"
                << "\n";
      aws::utils::sleep_for(std::chrono::seconds(window));
      //ProfilerFlush();
    }
  });

  aws::utils::sleep_for(std::chrono::hours(0xFFFFFFF));
  //ProfilerStop();
}
