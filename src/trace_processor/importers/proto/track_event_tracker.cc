/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/proto/track_event_tracker.h"

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto {
namespace trace_processor {

// static
constexpr uint64_t TrackEventTracker::kDefaultDescriptorTrackUuid;

TrackEventTracker::TrackEventTracker(TraceProcessorContext* context)
    : source_key_(context->storage->InternString("source")),
      source_id_key_(context->storage->InternString("source_id")),
      parent_track_id_key_(context->storage->InternString("parent_track_id")),
      category_key_(context->storage->InternString("category")),
      descriptor_source_(context->storage->InternString("descriptor")),
      default_descriptor_track_name_(
          context->storage->InternString("Default Track")),
      context_(context) {}

void TrackEventTracker::ReserveDescriptorProcessTrack(uint64_t uuid,
                                                      StringId name,
                                                      uint32_t pid,
                                                      int64_t timestamp) {
  DescriptorTrackReservation reservation;
  reservation.min_timestamp = timestamp;
  reservation.pid = pid;
  reservation.name = name;

  std::map<uint64_t, DescriptorTrackReservation>::iterator it;
  bool inserted;
  std::tie(it, inserted) =
      reserved_descriptor_tracks_.insert(std::make_pair<>(uuid, reservation));

  if (inserted)
    return;

  if (!it->second.IsForSameTrack(reservation)) {
    // Process tracks should not be reassigned to a different pid later (neither
    // should the type of the track change).
    PERFETTO_DLOG("New track reservation for process track with uuid %" PRIu64
                  " doesn't match earlier one",
                  uuid);
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  it->second.min_timestamp = std::min(it->second.min_timestamp, timestamp);
}

void TrackEventTracker::ReserveDescriptorThreadTrack(uint64_t uuid,
                                                     uint64_t parent_uuid,
                                                     StringId name,
                                                     uint32_t pid,
                                                     uint32_t tid,
                                                     int64_t timestamp) {
  DescriptorTrackReservation reservation;
  reservation.min_timestamp = timestamp;
  reservation.parent_uuid = parent_uuid;
  reservation.pid = pid;
  reservation.tid = tid;
  reservation.name = name;

  std::map<uint64_t, DescriptorTrackReservation>::iterator it;
  bool inserted;
  std::tie(it, inserted) =
      reserved_descriptor_tracks_.insert(std::make_pair<>(uuid, reservation));

  if (inserted)
    return;

  if (!it->second.IsForSameTrack(reservation)) {
    // Thread tracks should not be reassigned to a different pid/tid later
    // (neither should the type of the track change).
    PERFETTO_DLOG("New track reservation for thread track with uuid %" PRIu64
                  " doesn't match earlier one",
                  uuid);
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  it->second.min_timestamp = std::min(it->second.min_timestamp, timestamp);
}

void TrackEventTracker::ReserveDescriptorCounterTrack(
    uint64_t uuid,
    uint64_t parent_uuid,
    StringId name,
    StringId category,
    int64_t unit_multiplier,
    bool is_incremental,
    uint32_t packet_sequence_id) {
  DescriptorTrackReservation reservation;
  reservation.parent_uuid = parent_uuid;
  reservation.is_counter = true;
  reservation.name = name;
  reservation.category = category;
  reservation.unit_multiplier = unit_multiplier;
  reservation.is_incremental = is_incremental;
  // Incrementally encoded counters are only valid on a single sequence.
  if (is_incremental)
    reservation.packet_sequence_id = packet_sequence_id;

  std::map<uint64_t, DescriptorTrackReservation>::iterator it;
  bool inserted;
  std::tie(it, inserted) =
      reserved_descriptor_tracks_.insert(std::make_pair<>(uuid, reservation));

  if (inserted || it->second.IsForSameTrack(reservation))
    return;

  // Counter tracks should not be reassigned to a different parent track later
  // (neither should the type of the track change).
  PERFETTO_DLOG("New track reservation for counter track with uuid %" PRIu64
                " doesn't match earlier one",
                uuid);
  context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
}

void TrackEventTracker::ReserveDescriptorChildTrack(uint64_t uuid,
                                                    uint64_t parent_uuid,
                                                    StringId name) {
  DescriptorTrackReservation reservation;
  reservation.parent_uuid = parent_uuid;
  reservation.name = name;

  std::map<uint64_t, DescriptorTrackReservation>::iterator it;
  bool inserted;
  std::tie(it, inserted) =
      reserved_descriptor_tracks_.insert(std::make_pair<>(uuid, reservation));

  if (inserted || it->second.IsForSameTrack(reservation))
    return;

  // Child tracks should not be reassigned to a different parent track later
  // (neither should the type of the track change).
  PERFETTO_DLOG("New track reservation for child track with uuid %" PRIu64
                " doesn't match earlier one",
                uuid);
  context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
}

base::Optional<TrackId> TrackEventTracker::GetDescriptorTrack(
    uint64_t uuid,
    StringId event_name) {
  base::Optional<TrackId> track_id = GetDescriptorTrackImpl(uuid);
  if (!track_id || event_name.is_null())
    return track_id;

  // Update the name of the track if unset and the track is not the primary
  // track of a process/thread or a counter track.
  auto* tracks = context_->storage->mutable_track_table();
  uint32_t row = *tracks->id().IndexOf(*track_id);
  if (!tracks->name()[row].is_null())
    return track_id;

  // Check reservation for track type.
  auto reservation_it = reserved_descriptor_tracks_.find(uuid);
  PERFETTO_CHECK(reservation_it != reserved_descriptor_tracks_.end());

  if (reservation_it->second.pid || reservation_it->second.tid ||
      reservation_it->second.is_counter) {
    return track_id;
  }
  tracks->mutable_name()->Set(row, event_name);
  return track_id;
}

base::Optional<TrackId> TrackEventTracker::GetDescriptorTrackImpl(
    uint64_t uuid,
    std::vector<uint64_t>* descendent_uuids) {
  auto it = resolved_descriptor_tracks_.find(uuid);
  if (it != resolved_descriptor_tracks_.end())
    return it->second;

  auto reservation_it = reserved_descriptor_tracks_.find(uuid);
  if (reservation_it == reserved_descriptor_tracks_.end())
    return base::nullopt;

  TrackId track_id =
      ResolveDescriptorTrack(uuid, reservation_it->second, descendent_uuids);
  resolved_descriptor_tracks_[uuid] = track_id;
  return track_id;
}

TrackId TrackEventTracker::ResolveDescriptorTrack(
    uint64_t uuid,
    const DescriptorTrackReservation& reservation,
    std::vector<uint64_t>* descendent_uuids) {
  auto set_track_name_and_return = [this, &reservation](TrackId track_id) {
    if (reservation.name.is_null())
      return track_id;

    // Initialize the track name here, so that, if a name was given in the
    // reservation, it is set immediately after resolution takes place.
    auto* tracks = context_->storage->mutable_track_table();
    tracks->mutable_name()->Set(*tracks->id().IndexOf(track_id),
                                reservation.name);
    return track_id;
  };

  static constexpr size_t kMaxAncestors = 10;

  // Try to resolve any parent tracks recursively, too.
  base::Optional<TrackId> parent_track_id;
  if (reservation.parent_uuid) {
    // Input data may contain loops or extremely long ancestor track chains. To
    // avoid stack overflow in these situations, we keep track of the ancestors
    // seen in the recursion.
    std::unique_ptr<std::vector<uint64_t>> owned_descendent_uuids;
    if (!descendent_uuids) {
      owned_descendent_uuids.reset(new std::vector<uint64_t>());
      descendent_uuids = owned_descendent_uuids.get();
    }
    descendent_uuids->push_back(uuid);

    if (descendent_uuids->size() > kMaxAncestors) {
      PERFETTO_ELOG(
          "Too many ancestors in parent_track_uuid hierarchy at track %" PRIu64
          " with parent %" PRIu64,
          uuid, reservation.parent_uuid);
    } else if (std::find(descendent_uuids->begin(), descendent_uuids->end(),
                         reservation.parent_uuid) != descendent_uuids->end()) {
      PERFETTO_ELOG(
          "Loop detected in parent_track_uuid hierarchy at track %" PRIu64
          " with parent %" PRIu64,
          uuid, reservation.parent_uuid);
    } else {
      parent_track_id =
          GetDescriptorTrackImpl(reservation.parent_uuid, descendent_uuids);
      if (!parent_track_id) {
        PERFETTO_ELOG("Unknown parent track %" PRIu64 " for track %" PRIu64,
                      reservation.parent_uuid, uuid);
      }
    }

    descendent_uuids->pop_back();
    if (owned_descendent_uuids)
      descendent_uuids = nullptr;
  }

  if (reservation.tid) {
    UniqueTid utid = context_->process_tracker->UpdateThread(*reservation.tid,
                                                             *reservation.pid);
    auto it_and_inserted =
        descriptor_uuids_by_utid_.insert(std::make_pair<>(utid, uuid));
    if (!it_and_inserted.second) {
      // We already saw a another track with a different uuid for this thread.
      // Since there should only be one descriptor track for each thread, we
      // assume that its tid was reused. So, start a new thread.
      uint64_t old_uuid = it_and_inserted.first->second;
      PERFETTO_DCHECK(old_uuid != uuid);  // Every track is only resolved once.

      PERFETTO_DLOG("Detected tid reuse (pid: %" PRIu32 " tid: %" PRIu32
                    ") from track descriptors (old uuid: %" PRIu64
                    " new uuid: %" PRIu64 " timestamp: %" PRId64 ")",
                    *reservation.pid, *reservation.tid, old_uuid, uuid,
                    reservation.min_timestamp);

      utid = context_->process_tracker->StartNewThread(base::nullopt,
                                                       *reservation.tid);

      // Associate the new thread with its process.
      PERFETTO_CHECK(context_->process_tracker->UpdateThread(
                         *reservation.tid, *reservation.pid) == utid);

      descriptor_uuids_by_utid_[utid] = uuid;
    }
    return set_track_name_and_return(
        context_->track_tracker->InternThreadTrack(utid));
  }

  if (reservation.pid) {
    UniquePid upid =
        context_->process_tracker->GetOrCreateProcess(*reservation.pid);
    auto it_and_inserted =
        descriptor_uuids_by_upid_.insert(std::make_pair<>(upid, uuid));
    if (!it_and_inserted.second) {
      // We already saw a another track with a different uuid for this process.
      // Since there should only be one descriptor track for each process, we
      // assume that its pid was reused. So, start a new process.
      uint64_t old_uuid = it_and_inserted.first->second;
      PERFETTO_DCHECK(old_uuid != uuid);  // Every track is only resolved once.

      PERFETTO_DLOG("Detected pid reuse (pid: %" PRIu32
                    ") from track descriptors (old uuid: %" PRIu64
                    " new uuid: %" PRIu64 " timestamp: %" PRId64 ")",
                    *reservation.pid, old_uuid, uuid,
                    reservation.min_timestamp);

      upid = context_->process_tracker->StartNewProcess(
          base::nullopt, base::nullopt, *reservation.pid, kNullStringId);

      descriptor_uuids_by_upid_[upid] = uuid;
    }
    return set_track_name_and_return(
        context_->track_tracker->InternProcessTrack(upid));
  }

  base::Optional<TrackId> track_id;
  if (parent_track_id) {
    // If parent is a thread track, create another thread-associated track.
    auto* thread_tracks = context_->storage->mutable_thread_track_table();
    base::Optional<uint32_t> thread_track_index =
        thread_tracks->id().IndexOf(*parent_track_id);
    if (thread_track_index) {
      if (reservation.is_counter) {
        // Thread counter track.
        auto* thread_counter_tracks =
            context_->storage->mutable_thread_counter_track_table();
        tables::ThreadCounterTrackTable::Row row;
        row.utid = thread_tracks->utid()[*thread_track_index];
        track_id = thread_counter_tracks->Insert(row).id;
      } else {
        // Thread slice track.
        tables::ThreadTrackTable::Row row;
        row.utid = thread_tracks->utid()[*thread_track_index];
        track_id = thread_tracks->Insert(row).id;
      }
    } else {
      // If parent is a process track, create another process-associated track.
      auto* process_tracks = context_->storage->mutable_process_track_table();
      base::Optional<uint32_t> process_track_index =
          process_tracks->id().IndexOf(*parent_track_id);
      if (process_track_index) {
        if (reservation.is_counter) {
          // Process counter track.
          auto* process_counter_tracks =
              context_->storage->mutable_process_counter_track_table();
          tables::ProcessCounterTrackTable::Row row;
          row.upid = process_tracks->upid()[*process_track_index];
          track_id = process_counter_tracks->Insert(row).id;
        } else {
          // Process slice track.
          tables::ProcessTrackTable::Row row;
          row.upid = process_tracks->upid()[*process_track_index];
          track_id = process_tracks->Insert(row).id;
        }
      }
    }
  }

  // Otherwise create a global track.
  if (!track_id) {
    if (reservation.is_counter) {
      // Global counter track.
      tables::CounterTrackTable::Row row;
      track_id =
          context_->storage->mutable_counter_track_table()->Insert(row).id;
    } else {
      // Global slice track.
      tables::TrackTable::Row row;
      track_id = context_->storage->mutable_track_table()->Insert(row).id;
    }
    // The global track with no uuid is the default global track (e.g. for
    // global instant events). Any other global tracks are considered children
    // of the default track.
    if (!parent_track_id && uuid) {
      // Detect loops where the default track has a parent that itself is a
      // global track (and thus should be parent of the default track).
      if (descendent_uuids &&
          std::find(descendent_uuids->begin(), descendent_uuids->end(),
                    kDefaultDescriptorTrackUuid) != descendent_uuids->end()) {
        PERFETTO_ELOG(
            "Loop detected in parent_track_uuid hierarchy at track %" PRIu64
            " with parent %" PRIu64,
            uuid, kDefaultDescriptorTrackUuid);
      } else {
        parent_track_id = GetOrCreateDefaultDescriptorTrack();
      }
    }
  }

  auto args = context_->args_tracker->AddArgsTo(*track_id);
  args.AddArg(source_key_, Variadic::String(descriptor_source_))
      .AddArg(source_id_key_, Variadic::Integer(static_cast<int64_t>(uuid)));
  if (parent_track_id) {
    args.AddArg(parent_track_id_key_,
                Variadic::Integer(parent_track_id->value));
  }
  if (reservation.category != kNullStringId) {
    args.AddArg(category_key_, Variadic::String(reservation.category));
  }
  return set_track_name_and_return(*track_id);
}

TrackId TrackEventTracker::GetOrCreateDefaultDescriptorTrack() {
  // If the default track was already reserved (e.g. because a producer emitted
  // a descriptor for it) or created, resolve and return it.
  base::Optional<TrackId> track_id =
      GetDescriptorTrack(kDefaultDescriptorTrackUuid);
  if (track_id)
    return *track_id;

  // Otherwise reserve a new track and resolve it.
  ReserveDescriptorChildTrack(kDefaultDescriptorTrackUuid, /*parent_uuid=*/0,
                              default_descriptor_track_name_);
  return *GetDescriptorTrack(kDefaultDescriptorTrackUuid);
}

base::Optional<int64_t> TrackEventTracker::ConvertToAbsoluteCounterValue(
    uint64_t counter_track_uuid,
    uint32_t packet_sequence_id,
    int64_t value) {
  auto reservation_it = reserved_descriptor_tracks_.find(counter_track_uuid);
  if (reservation_it == reserved_descriptor_tracks_.end()) {
    PERFETTO_DLOG("Unknown counter track with uuid %" PRIu64,
                  counter_track_uuid);
    return base::nullopt;
  }

  DescriptorTrackReservation& reservation = reservation_it->second;
  if (!reservation.is_counter) {
    PERFETTO_DLOG("Track with uuid %" PRIu64 " is not a counter track",
                  counter_track_uuid);
    return base::nullopt;
  }

  if (reservation.unit_multiplier > 0)
    value *= reservation.unit_multiplier;

  if (reservation.is_incremental) {
    if (reservation.packet_sequence_id != packet_sequence_id) {
      PERFETTO_DLOG(
          "Incremental counter track with uuid %" PRIu64
          " was updated from the wrong packet sequence (expected: %" PRIu32
          " got:%" PRIu32 ")",
          counter_track_uuid, reservation.packet_sequence_id,
          packet_sequence_id);
      return base::nullopt;
    }

    reservation.latest_value += value;
    value = reservation.latest_value;
  }

  return value;
}

void TrackEventTracker::OnIncrementalStateCleared(uint32_t packet_sequence_id) {
  // TODO(eseckler): Improve on the runtime complexity of this. At O(hundreds)
  // of packet sequences, incremental state clearing at O(trace second), and
  // total number of tracks in O(thousands), a linear scan through all tracks
  // here might not be fast enough.
  for (auto& entry : reserved_descriptor_tracks_) {
    DescriptorTrackReservation& reservation = entry.second;
    // Only consider incremental counter tracks for current sequence.
    if (!reservation.is_counter || !reservation.is_incremental ||
        reservation.packet_sequence_id != packet_sequence_id) {
      continue;
    }
    // Reset their value to 0, see CounterDescriptor's |is_incremental|.
    reservation.latest_value = 0;
  }
}

}  // namespace trace_processor
}  // namespace perfetto
