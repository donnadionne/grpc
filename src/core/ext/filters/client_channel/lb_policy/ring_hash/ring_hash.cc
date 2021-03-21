//
// Copyright 2018 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include <stdlib.h>
#include <string.h>

#include "absl/strings/str_cat.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <grpc/support/alloc.h>
#include "src/core/ext/filters/client_channel/lb_policy/subchannel_list.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/static_metadata.h"

namespace grpc_core {

const char* kRequestRingHashAttribute = "request_ring_hash";
TraceFlag grpc_lb_ring_hash_trace(false, "ring_hash_lb");

namespace {

constexpr char kRingHash[] = "ring_hash_experimental";

class RingHashLbConfig : public LoadBalancingPolicy::Config {
 public:
  RingHashLbConfig(size_t min_ring_size, size_t max_ring_size)
      : min_ring_size_(min_ring_size), max_ring_size_(max_ring_size) {}
  const char* name() const override { return kRingHash; }
  size_t min_ring_size() const { return min_ring_size_; }
  size_t max_ring_size() const { return max_ring_size_; }

 private:
  size_t min_ring_size_;
  size_t max_ring_size_;
};

//
// ring_hash LB policy
//
class RingHash : public LoadBalancingPolicy {
 public:
  explicit RingHash(Args args);

  const char* name() const override { return kRingHash; }

  void UpdateLocked(UpdateArgs args) override;
  void ResetBackoffLocked() override;

 private:
  ~RingHash() override;

  // Forward declaration.
  class RingHashSubchannelList;

  // Data for a particular subchannel in a subchannel list.
  // This subclass adds the following functionality:
  // - Tracks the previous connectivity state of the subchannel, so that
  //   we know how many subchannels are in each state.
  class RingHashSubchannelData
      : public SubchannelData<RingHashSubchannelList, RingHashSubchannelData> {
   public:
    RingHashSubchannelData(
        SubchannelList<RingHashSubchannelList, RingHashSubchannelData>*
            subchannel_list,
        const ServerAddress& address,
        RefCountedPtr<SubchannelInterface> subchannel)
        : SubchannelData(subchannel_list, address, std::move(subchannel)),
          address_(address) {}

    grpc_connectivity_state connectivity_state() const {
      return last_connectivity_state_;
    }
    const ServerAddress& address() const { return address_; }

    bool seen_failure_since_ready() const { return seen_failure_since_ready_; }

    // Performs connectivity state updates that need to be done both when we
    // first start watching and when a watcher notification is received.
    void UpdateConnectivityStateLocked(
        grpc_connectivity_state connectivity_state);

   private:
    // Performs connectivity state updates that need to be done only
    // after we have started watching.
    void ProcessConnectivityChangeLocked(
        grpc_connectivity_state connectivity_state) override;

    ServerAddress address_;
    grpc_connectivity_state last_connectivity_state_ = GRPC_CHANNEL_IDLE;
    bool seen_failure_since_ready_ = false;
  };

  // A list of subchannels.
  class RingHashSubchannelList
      : public SubchannelList<RingHashSubchannelList, RingHashSubchannelData> {
   public:
    RingHashSubchannelList(RingHash* policy, TraceFlag* tracer,
                           ServerAddressList addresses,
                           const grpc_channel_args& args)
        : SubchannelList(policy, tracer, std::move(addresses),
                         policy->channel_control_helper(), args) {
      // Need to maintain a ref to the LB policy as long as we maintain
      // any references to subchannels, since the subchannels'
      // pollset_sets will include the LB policy's pollset_set.
      policy->Ref(DEBUG_LOCATION, "subchannel_list").release();
    }

    ~RingHashSubchannelList() override {
      RingHash* p = static_cast<RingHash*>(policy());
      p->Unref(DEBUG_LOCATION, "subchannel_list");
    }

    // Starts watching the subchannels in this list.
    void StartWatchingLocked();

    // When the policy reports TRANSIENT_FAILURE, start reconnecting
    // and try to recover.
    void StartRecovery();

    // Updates the counters of subchannels in each state when a
    // subchannel transitions from old_state to new_state.
    void UpdateStateCountersLocked(grpc_connectivity_state old_state,
                                   grpc_connectivity_state new_state);

    // Updates the RH policy's connectivity state based on the
    // subchannel list's state counters, creating new picker and new ring.
    void UpdateRingHashConnectivityStateLocked();

   private:
    size_t num_ready_ = 0;
    size_t num_connecting_ = 0;
    size_t num_transient_failure_ = 0;
  };

  class Picker : public SubchannelPicker {
   public:
    struct RingEntry {
      uint64_t hash;
      RefCountedPtr<SubchannelInterface> subchannel;
    };

    Picker(RingHash* parent, RingHashSubchannelList* subchannel_list);

    PickResult Pick(PickArgs args) override;

   private:
    // Using pointer value only, no ref held -- do not dereference!
    RingHash* parent_;

    // A ring of subchannels.
    std::vector<RingEntry> ring_;
  };

  void ShutdownLocked() override;

  // Current config from resolver.
  RefCountedPtr<RingHashLbConfig> config_;

  // list of subchannels.
  OrphanablePtr<RingHashSubchannelList> subchannel_list_;
  // indicating if we are shutting down.
  bool shutdown_ = false;
};

//
// RingHash::Picker
//

RingHash::Picker::Picker(RingHash* parent,
                         RingHashSubchannelList* subchannel_list)
    : parent_(parent) {
  size_t num_subchannels = subchannel_list->num_subchannels();
  // Store the weights while finding the sum.
  struct AddressWeights {
    std::string address;
    uint32_t weight;
    double normalized_weight;
  };
  std::vector<AddressWeights> address_weights_info;
  size_t sum = 0;
  address_weights_info.reserve(num_subchannels);
  for (size_t i = 0; i < num_subchannels; ++i) {
    RingHashSubchannelData* sd = subchannel_list->subchannel(i);
    const ServerAddressWeightAttribute* weight_attribute = static_cast<
        const ServerAddressWeightAttribute*>(sd->address().GetAttribute(
        ServerAddressWeightAttribute::kServerAddressWeightAttributeKey));
    if (weight_attribute != nullptr) {
      GPR_ASSERT(weight_attribute->weight() != 0);
      AddressWeights address_weights;
      address_weights.address = sd->address().ToString();
      ;
      address_weights.weight = weight_attribute->weight();
      sum += weight_attribute->weight();
      gpr_log(GPR_INFO, "donna retrieve weight %d", weight_attribute->weight());
      address_weights_info.push_back(std::move(address_weights));
    } else {
      GPR_ASSERT(0);
    }
  }
  // Calculating normalized weights and find min and max.
  double min_normalized_weight = 1.0;
  double max_normalized_weight = 0.0;
  for (size_t i = 0; i < num_subchannels; ++i) {
    address_weights_info[i].normalized_weight =
        address_weights_info[i].weight / sum;
    if (address_weights_info[i].normalized_weight < min_normalized_weight) {
      min_normalized_weight = address_weights_info[i].normalized_weight;
    }
    if (address_weights_info[i].normalized_weight > max_normalized_weight) {
      max_normalized_weight = address_weights_info[i].normalized_weight;
    }
  }
  // Scale up the number of hashes per host such that the least-weighted host
  // gets a whole number of hashes on the ring. Other hosts might not end up
  // with whole numbers, and that's fine (the ring-building algorithm below can
  // handle this). This preserves the original implementation's behavior: when
  // weights aren't provided, all hosts should get an equal number of hashes. In
  // the case where this number exceeds the max_ring_size, it's scaled back down
  // to fit.
  const size_t min_ring_size = parent_->config_->min_ring_size();
  const size_t max_ring_size = parent_->config_->max_ring_size();
  const double scale = std::min(
      std::ceil(min_normalized_weight * min_ring_size) / min_normalized_weight,
      static_cast<double>(max_ring_size));
  // Reserve memory for the entire ring up front.
  const uint64_t ring_size = std::ceil(scale);
  ring_.reserve(ring_size);
  // Populate the hash ring by walking through the (host, weight) pairs in
  // normalized_host_weights, and generating (scale * weight) hashes for each
  // host. Since these aren't necessarily whole numbers, we maintain running
  // sums -- current_hashes and target_hashes -- which allows us to populate the
  // ring in a mostly stable way.
  absl::InlinedVector<char, 196> hash_key_buffer;
  double current_hashes = 0.0;
  double target_hashes = 0.0;
  uint64_t min_hashes_per_host = ring_size;
  uint64_t max_hashes_per_host = 0;
  for (size_t i = 0; i < num_subchannels; ++i) {
    const std::string& address_string = address_weights_info[i].address;
    hash_key_buffer.assign(address_string.begin(), address_string.end());
    hash_key_buffer.emplace_back('_');
    auto offset_start = hash_key_buffer.end();
    target_hashes += scale * address_weights_info[i].normalized_weight;
    size_t count = 0;
    while (current_hashes < target_hashes) {
      const std::string count_str = absl::StrCat("", count);
      hash_key_buffer.insert(offset_start, count_str.begin(), count_str.end());
      absl::string_view hash_key(static_cast<char*>(hash_key_buffer.data()),
                                 hash_key_buffer.size());
      const uint64_t hash = XXH64(hash_key.data(), hash_key.size(), 0);
      ring_.push_back(
          {hash, subchannel_list->subchannel(i)->subchannel()->Ref()});
      ++count;
      ++current_hashes;
      hash_key_buffer.erase(offset_start, hash_key_buffer.end());
    }
    min_hashes_per_host = std::min(i, min_hashes_per_host);
    max_hashes_per_host = std::max(i, max_hashes_per_host);
  }
  std::sort(ring_.begin(), ring_.end(),
            [](const RingEntry& lhs, const RingEntry& rhs) -> bool {
              return lhs.hash < rhs.hash;
            });
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(GPR_INFO,
            "[RH %p picker %p] created picker from subchannel_list=%p "
            "with %" PRIuPTR " subchannels" PRIuPTR,
            parent_, this, subchannel_list, ring_.size());
  }
}

RingHash::PickResult RingHash::Picker::Pick(PickArgs args) {
  auto hash =
      args.call_state->ExperimentalGetCallAttribute(kRequestRingHashAttribute);
  uint64_t h = std::stoul(std::string(hash));
  // Ported from https://github.com/RJ/ketama/blob/master/libketama/ketama.c
  // (ketama_get_server) I've generally kept the variable names to make the code
  // easier to compare. NOTE: The algorithm depends on using signed integers for
  // lowp, midp, and highp. Do not
  //       change them!
  int64_t lowp = 0;
  int64_t highp = ring_.size();
  int64_t midp = 0;
  while (true) {
    midp = (lowp + highp) / 2;
    if (midp == static_cast<int64_t>(ring_.size())) {
      midp = 0;
      break;
    }
    uint64_t midval = ring_[midp].hash;
    uint64_t midval1 = midp == 0 ? 0 : ring_[midp - 1].hash;
    if (h <= midval && h > midval1) {
      break;
    }
    if (midval < h) {
      lowp = midp + 1;
    } else {
      highp = midp - 1;
    }
    if (lowp > highp) {
      midp = 0;
      break;
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(GPR_INFO,
            "[RH %p picker %p] returning index %" PRIuPTR ", subchannel=%p",
            parent_, this, midp, ring_[midp].subchannel.get());
  }
  // Return pick result based on subchannel state, re-pick if necessary.
  grpc_connectivity_state state =
      ring_[midp].subchannel->CheckConnectivityState();
  PickResult result;
  switch (state) {
    case GRPC_CHANNEL_READY:
      result.type = PickResult::PICK_COMPLETE;
      result.subchannel = ring_[midp].subchannel;
      return result;
    case GRPC_CHANNEL_IDLE:
    case GRPC_CHANNEL_CONNECTING:
      result.type = PickResult::PICK_QUEUE;
      return result;
    case GRPC_CHANNEL_TRANSIENT_FAILURE:
      // Reconnect and find the next one. TODO (donnadionne): Reconnect
      midp = (midp == highp) ? 0 : midp + 1;
      state = ring_[midp].subchannel->CheckConnectivityState();
      switch (state) {
        case GRPC_CHANNEL_READY:
          result.type = PickResult::PICK_COMPLETE;
          result.subchannel = ring_[midp].subchannel;
          return result;
        case GRPC_CHANNEL_IDLE:
        case GRPC_CHANNEL_CONNECTING:
          result.type = PickResult::PICK_QUEUE;
          return result;
        case GRPC_CHANNEL_TRANSIENT_FAILURE:
          // Reconnect and find the next one. TODO (donnadionne): Reconnect
          midp = (midp == highp) ? 0 : midp + 1;
          state = ring_[midp].subchannel->CheckConnectivityState();
          switch (state) {
            case GRPC_CHANNEL_READY:
              result.type = PickResult::PICK_COMPLETE;
              result.subchannel = ring_[midp].subchannel;
              return result;
            case GRPC_CHANNEL_IDLE:
            case GRPC_CHANNEL_CONNECTING:
            case GRPC_CHANNEL_TRANSIENT_FAILURE:
              // TODO: Try all for a ready channel before sending back
              // PICK_FAILED.
              break;
            default:
              break;
          }
          result.type = PickResult::PICK_FAILED;
          return result;
        default:
          break;
      }
    default:
      break;
  }
  result.type = PickResult::PICK_FAILED;
  return result;
}

//
// RingHash
//

RingHash::RingHash(Args args) : LoadBalancingPolicy(std::move(args)) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(GPR_INFO, "[RH %p] Created", this);
  }
}

RingHash::~RingHash() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(GPR_INFO, "[RH %p] Destroying Ring Hash policy", this);
  }
  GPR_ASSERT(subchannel_list_ == nullptr);
}

void RingHash::ShutdownLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(GPR_INFO, "[RH %p] Shutting down", this);
  }
  shutdown_ = true;
  subchannel_list_.reset();
}

void RingHash::ResetBackoffLocked() { subchannel_list_->ResetBackoffLocked(); }

void RingHash::RingHashSubchannelList::StartWatchingLocked() {
  if (num_subchannels() == 0) return;
  // Start connectivity watch for each subchannel.
  for (size_t i = 0; i < num_subchannels(); i++) {
    if (subchannel(i)->subchannel() != nullptr) {
      subchannel(i)->StartConnectivityWatchLocked();
    }
  }
  RingHash* p = static_cast<RingHash*>(policy());
  // Sending up the initial picker while all subchannels are in IDLE state.
  p->channel_control_helper()->UpdateState(GRPC_CHANNEL_READY, absl::Status(),
                                           absl::make_unique<Picker>(p, this));
}

void RingHash::RingHashSubchannelList::StartRecovery() {
  if (num_subchannels() == 0) return;
  // Start another attempt on each subchannel while the watchers are still in
  // place.
  for (size_t i = 0; i < num_subchannels(); i++) {
    if (subchannel(i)->subchannel() != nullptr) {
      subchannel(i)->subchannel()->AttemptToConnect();
    }
  }
}

void RingHash::RingHashSubchannelList::UpdateStateCountersLocked(
    grpc_connectivity_state old_state, grpc_connectivity_state new_state) {
  GPR_ASSERT(old_state != GRPC_CHANNEL_SHUTDOWN);
  GPR_ASSERT(new_state != GRPC_CHANNEL_SHUTDOWN);
  if (old_state == GRPC_CHANNEL_READY) {
    GPR_ASSERT(num_ready_ > 0);
    --num_ready_;
  } else if (old_state == GRPC_CHANNEL_CONNECTING) {
    GPR_ASSERT(num_connecting_ > 0);
    --num_connecting_;
  } else if (old_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    GPR_ASSERT(num_transient_failure_ > 0);
    --num_transient_failure_;
  }
  if (new_state == GRPC_CHANNEL_READY) {
    ++num_ready_;
  } else if (new_state == GRPC_CHANNEL_CONNECTING) {
    ++num_connecting_;
  } else if (new_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    ++num_transient_failure_;
  }
}

// Sets the RH policy's connectivity state and generates a new picker based
// on the current subchannel list.
void RingHash::RingHashSubchannelList::UpdateRingHashConnectivityStateLocked() {
  RingHash* p = static_cast<RingHash*>(policy());
  // Only set connectivity state if this is the current subchannel list.
  if (p->subchannel_list_.get() != this) return;
  if (num_transient_failure_ < 2) {
    /* READY */
    p->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_READY, absl::Status(), absl::make_unique<Picker>(p, this));
  } else if (num_ready_ == 0 && num_connecting_ == 0) {
    /* TRANSIENT_FAILURE >= 2 and everything else is IDLE */
    grpc_error* error =
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                               "connections to all backends failing"),
                           GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE);
    p->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_TRANSIENT_FAILURE, grpc_error_to_absl_status(error),
        absl::make_unique<TransientFailurePicker>(error));
    // While the ring_hash policy is reporting TRANSIENT_FAILURE, it will
    // not be getting any pick requests from the priority policy.
    // However, because the ring_hash policy does not attempt to
    // reconnect to subchannels unless it is getting pick requests,
    // it will need special handling to ensure that it will eventually
    // recover from TRANSIENT_FAILURE state once the problem is resolved.
    // Specifically, it will make sure that it is attempting to connect to
    // at least one subchannel at any given time.  After a given subchannel
    // fails a connection attempt, it will move on to the next subchannel
    // in the ring.  It will keep doing this until one of the subchannels
    // successfully connects, at which point it will report READY and stop
    // proactively trying to connect.  The policy will remain in
    // TRANSIENT_FAILURE until at least one subchannel becomes connected,
    // even if subchannels are in state CONNECTING during that time.
    StartRecovery();
  }
}

void RingHash::RingHashSubchannelData::UpdateConnectivityStateLocked(
    grpc_connectivity_state connectivity_state) {
  RingHash* p = static_cast<RingHash*>(subchannel_list()->policy());
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(
        GPR_INFO,
        "[RR %p] connectivity changed for subchannel %p, subchannel_list %p "
        "(index %" PRIuPTR " of %" PRIuPTR "): prev_state=%s new_state=%s",
        p, subchannel(), subchannel_list(), Index(),
        subchannel_list()->num_subchannels(),
        ConnectivityStateName(last_connectivity_state_),
        ConnectivityStateName(connectivity_state));
  }
  // Decide what state to report for aggregation purposes.
  // If we haven't seen a failure since the last time we were in state
  // READY, then we report the state change as-is.  However, once we do see
  // a failure, we report TRANSIENT_FAILURE and do not report any subsequent
  // state changes until we go back into state READY.
  if (!seen_failure_since_ready_) {
    if (connectivity_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
      seen_failure_since_ready_ = true;
    }
    subchannel_list()->UpdateStateCountersLocked(last_connectivity_state_,
                                                 connectivity_state);
  } else {
    if (connectivity_state == GRPC_CHANNEL_READY) {
      seen_failure_since_ready_ = false;
      subchannel_list()->UpdateStateCountersLocked(
          GRPC_CHANNEL_TRANSIENT_FAILURE, connectivity_state);
    }
  }
  // Record last seen connectivity state.
  last_connectivity_state_ = connectivity_state;
}

void RingHash::RingHashSubchannelData::ProcessConnectivityChangeLocked(
    grpc_connectivity_state connectivity_state) {
  RingHash* p = static_cast<RingHash*>(subchannel_list()->policy());
  GPR_ASSERT(subchannel() != nullptr);
  // If the new state is TRANSIENT_FAILURE, re-resolve.
  // Only do this if we've started watching, not at startup time.
  // Otherwise, if the subchannel was already in state TRANSIENT_FAILURE
  // when the subchannel list was created, we'd wind up in a constant
  // loop of re-resolution.
  // Also attempt to reconnect.
  if (connectivity_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
      gpr_log(GPR_INFO,
              "[RR %p] Subchannel %p has gone into TRANSIENT_FAILURE. "
              "Requesting re-resolution",
              p, subchannel());
    }
    p->channel_control_helper()->RequestReresolution();
    // This is not needed anymore: subchannel()->AttemptToConnect();
  }

  // Update state counters.
  UpdateConnectivityStateLocked(connectivity_state);
  // Update the RH policy's connectivity state, creating new picker and new
  // ring.
  subchannel_list()->UpdateRingHashConnectivityStateLocked();
}

void RingHash::UpdateLocked(UpdateArgs args) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_ring_hash_trace)) {
    gpr_log(GPR_INFO, "[RR %p] received update with %" PRIuPTR " addresses",
            this, args.addresses.size());
  }
  config_ = std::move(args.config);
  subchannel_list_ = MakeOrphanable<RingHashSubchannelList>(
      this, &grpc_lb_ring_hash_trace, std::move(args.addresses), *args.args);
  if (subchannel_list_->num_subchannels() == 0) {
    // If the new list is empty, immediately transition to TRANSIENT_FAILURE.
    grpc_error* error =
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING("Empty update"),
                           GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE);
    channel_control_helper()->UpdateState(
        GRPC_CHANNEL_TRANSIENT_FAILURE, grpc_error_to_absl_status(error),
        absl::make_unique<TransientFailurePicker>(error));
  } else {
    // Start watching the new list.
    subchannel_list_->StartWatchingLocked();
  }
}

//
// factory
//

class RingHashFactory : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    return MakeOrphanable<RingHash>(std::move(args));
  }

  const char* name() const override { return kRingHash; }

  RefCountedPtr<LoadBalancingPolicy::Config> ParseLoadBalancingConfig(
      const Json& json, grpc_error** error) const override {
    if (json.type() != Json::Type::OBJECT) {
      *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "ring_hash_experimental should be of type object");
      return nullptr;
    }
    std::vector<grpc_error*> error_list;
    const Json::Object& ring_hash = json.object_value();
    size_t min_ring_size = 1024;
    size_t max_ring_size = 8388608;
    auto ring_hash_it = ring_hash.find("min_ring_size");
    if (ring_hash_it == ring_hash.end()) {
      error_list.push_back(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("field:min_ring_size missing"));
    } else if (ring_hash_it->second.type() != Json::Type::NUMBER) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:min_ring_size error: should be of "
          "number"));
    } else {
      min_ring_size = gpr_parse_nonnegative_int(
          ring_hash_it->second.string_value().c_str());
    }
    ring_hash_it = ring_hash.find("max_ring_size");
    if (ring_hash_it == ring_hash.end()) {
      error_list.push_back(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("field:max_ring_size missing"));
    } else if (ring_hash_it->second.type() != Json::Type::NUMBER) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:max_ring_size error: should be of "
          "number"));
    } else {
      max_ring_size = gpr_parse_nonnegative_int(
          ring_hash_it->second.string_value().c_str());
    }
    if (min_ring_size <= 0 || min_ring_size > 8388608 || max_ring_size <= 0 ||
        max_ring_size > 8388608 || min_ring_size > max_ring_size) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:max_ring_size and or min_ring_size error: "
          "values need to be in the range of 1 to 8388608 "
          "and max_ring_size cannot be smaller than "
          "min_ring_size"));
    }
    if (error_list.empty()) {
      return MakeRefCounted<RingHashLbConfig>(min_ring_size, max_ring_size);
    } else {
      *error = GRPC_ERROR_CREATE_FROM_VECTOR(
          "ring_hash_experimental LB policy config", &error_list);
      return nullptr;
    }
  }
};

}  // namespace

}  // namespace grpc_core

void grpc_lb_policy_ring_hash_init() {
  grpc_core::LoadBalancingPolicyRegistry::Builder::
      RegisterLoadBalancingPolicyFactory(
          absl::make_unique<grpc_core::RingHashFactory>());
}

void grpc_lb_policy_ring_hash_shutdown() {}
